/* map_vector.cpp — fast vector tile rendering (direct ARGB8888 buffer)
 * Bypasses LVGL draw API for speed — writes pixels directly into canvas buffer.
 */
#include "map_vector.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <zlib.h>
#include <string>
#include <vector>
#include <algorithm>
#include <map>
#include <set>
#include <deque>
#include <mutex>
#include <thread>
#include <condition_variable>

#include "pmtiles.hpp"
#include "vtzero/vector_tile.hpp"
#include "vtzero/geometry.hpp"

#include "libs/freetype/lv_freetype.h"

extern const lv_font_t lv_font_montserrat_12;
extern const lv_font_t lv_font_montserrat_14;

namespace MapVector {

// Label fonts loaded at runtime from OpenSans-Bold.ttf. Required because the
// built-in montserrat fonts are ASCII-only and drop French accents (é è à ô ç…).
// Falls back to montserrat when the TTF is missing.
static lv_font_t *s_font12 = nullptr;
static lv_font_t *s_font14 = nullptr;

static const lv_font_t *rtFont(const lv_font_t *fallback) {
    if (fallback == &lv_font_montserrat_14 && s_font14) return s_font14;
    if (fallback == &lv_font_montserrat_12 && s_font12) return s_font12;
    return fallback;
}

bool initLabelFonts() {
    if (s_font12 && s_font14) return true;
    static const char *paths[] = {
        "/usr/share/fonts/truetype/open-sans/OpenSans-Bold.ttf",
        "fonts/OpenSans-Bold.ttf",
        "/data/LoRa_Tracker/fonts/OpenSans-Bold.ttf",
        nullptr };
    const char *found = nullptr;
    for (int i = 0; paths[i]; i++)
        if (access(paths[i], R_OK) == 0) { found = paths[i]; break; }
    if (!found) {
        printf("[map] OpenSans-Bold.ttf introuvable — labels en montserrat (accents coupés)\n");
        return false;
    }
    // FreeType is already initialized by lv_init() (LV_USE_FREETYPE=1); don't re-init.
    s_font12 = lv_freetype_font_create(found, LV_FREETYPE_FONT_RENDER_MODE_BITMAP, 14, LV_FREETYPE_FONT_STYLE_NORMAL);
    s_font14 = lv_freetype_font_create(found, LV_FREETYPE_FONT_RENDER_MODE_BITMAP, 16, LV_FREETYPE_FONT_STYLE_NORMAL);
    printf("[map] police labels: %s (%s)\n", found, (s_font12 && s_font14) ? "OK" : "partiel");
    return s_font12 && s_font14;
}

#define EXTENT    4096
#define TILE_SIZE 256


static int    s_fd       = -1;
static void  *s_mapped   = nullptr;
static size_t s_mapSize  = 0;
static pmtiles::headerv3 s_header{};

// ---- gzip --------------------------------------------------------------
static std::string gunzip(const std::string& in, uint8_t) {
    z_stream zs{};
    zs.next_in  = (Bytef*)in.data();
    zs.avail_in = (uInt)in.size();
    if (inflateInit2(&zs, 15 + 32) != Z_OK) return {};
    std::string out; out.reserve(in.size() * 4);
    char buf[16384];
    int ret;
    do {
        zs.next_out  = (Bytef*)buf; zs.avail_out = sizeof(buf);
        ret = inflate(&zs, Z_NO_FLUSH);
        if (ret < 0) { inflateEnd(&zs); return {}; }
        out.append(buf, sizeof(buf) - zs.avail_out);
    } while (ret != Z_STREAM_END);
    inflateEnd(&zs);
    return out;
}

// ---- Tile cache (LRU, max 64 tiles = 16 MB) -------------------------------
#define CACHE_MAX 64
struct CacheEntry {
    uint8_t *buf;           // ARGB8888 buffer (TILE_SIZE*TILE_SIZE*4)
    uint32_t lastAccess;
    int z, x, y;
    bool allocated;         // buffer was malloc'd (not pre-allocated by canvas)
};
static std::map<uint64_t, CacheEntry> s_cache;
static uint32_t s_cacheCounter = 0;

static uint64_t tileKey(int z, int x, int y) {
    return ((uint64_t)z << 40) | ((uint64_t)(x & 0xFFFFF) << 20) | (uint64_t)(y & 0xFFFFF);
}

static uint8_t* cacheGet(int z, int x, int y) {
    auto it = s_cache.find(tileKey(z, x, y));
    if (it != s_cache.end()) {
        it->second.lastAccess = ++s_cacheCounter;
        return it->second.buf;
    }
    return nullptr;
}

static void cachePut(int z, int x, int y, uint8_t *buf, bool allocated) {
    uint64_t key = tileKey(z, x, y);
    if (s_cache.size() >= CACHE_MAX) {
        // Evict LRU
        auto oldest = s_cache.begin();
        for (auto it = s_cache.begin(); it != s_cache.end(); ++it)
            if (it->second.lastAccess < oldest->second.lastAccess) oldest = it;
        if (oldest->second.allocated) lv_free(oldest->second.buf);
        s_cache.erase(oldest);
    }
    s_cache[key] = {buf, ++s_cacheCounter, z, x, y, allocated};
}

static void renderTileBuf(uint8_t *buf, int sz, int z, int x, int y); // fwd

// ---- Async render thread --------------------------------------------------
struct RenderReq { int z, x, y; };
static std::deque<RenderReq> s_queue;
static std::mutex s_queueMutex;
static std::condition_variable s_queueCV;
static std::thread s_worker;
static bool s_workerRunning = false;

static void workerLoop() {
    while (s_workerRunning) {
        RenderReq req;
        {
            std::unique_lock<std::mutex> lk(s_queueMutex);
            if (s_queue.empty()) {
                s_queueCV.wait_for(lk, std::chrono::milliseconds(200));
                if (s_queue.empty()) continue;
            }
            req = s_queue.front(); s_queue.pop_front();
        }

        // Skip if already cached (processed by another request)
        if (cacheGet(req.z, req.x, req.y)) continue;

        // Render to temp buffer
        int sz = TILE_SIZE;
        uint8_t *tmp = (uint8_t *)lv_malloc(sz * sz * 4);
        if (!tmp) continue;
        renderTileBuf(tmp, sz, req.z, req.x, req.y);

        // Check if any pixel was actually drawn (bg = F2EFE9 in ARGB8888 LE = B,G,R)
        bool hasData = false;
        for (int i = 0; i < sz * sz; i++) {
            uint8_t *p = tmp + i * 4;
            if (p[0] != 0xE9 || p[1] != 0xEF || p[2] != 0xF2) { hasData = true; break; }
        }
        if (hasData) {
            cachePut(req.z, req.x, req.y, tmp, true);
        } else {
            lv_free(tmp);
        }
    }
}

// ---- Public API -----------------------------------------------------------

bool open(const char *pmtilesPath) {
    s_fd = ::open(pmtilesPath, O_RDONLY);
    if (s_fd < 0) { fprintf(stderr, "MapVector: open %s failed\n", pmtilesPath); return false; }
    struct stat st; fstat(s_fd, &st);
    s_mapSize = st.st_size;
    s_mapped = mmap(nullptr, s_mapSize, PROT_READ, MAP_PRIVATE, s_fd, 0);
    if (s_mapped == MAP_FAILED) { ::close(s_fd); s_fd = -1; return false; }
    std::string hstr((const char*)s_mapped, 127);
    s_header = pmtiles::deserialize_header(hstr);
    fprintf(stderr, "MapVector: open %s OK z%d-%d\n",
            pmtilesPath, s_header.min_zoom, s_header.max_zoom);
    return true;
}

bool isOpen() { return s_mapped != nullptr; }
int  minZoom() { return s_header.min_zoom; }
int  maxZoom() { return s_header.max_zoom; }

void close() {
    if (s_mapped) { munmap(s_mapped, s_mapSize); s_mapped = nullptr; }
    if (s_fd >= 0) { ::close(s_fd); s_fd = -1; }
}

// ---- Rendu direct buffer (sans API LVGL) -----------------------------------

static inline void setPx(uint8_t *buf, int stride, int x, int y,
                          uint8_t r, uint8_t g, uint8_t b) {
    uint8_t *p = buf + (y * stride) + (x * 4);
    p[0] = b; p[1] = g; p[2] = r; p[3] = 0xFF;
}

// Bresenham line
static void drawLine(uint8_t *buf, int w, int h, int x0, int y0, int x1, int y1,
                      uint8_t r, uint8_t g, uint8_t b) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (true) {
        if (x0 >= 0 && x0 < w && y0 >= 0 && y0 < h) setPx(buf, w*4, x0, y0, r, g, b);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// Scanline polygon fill (fan-triangulation + scanline)
static void fillPoly(uint8_t *buf, int w, int h,
                      const std::vector<int>& px, const std::vector<int>& py,
                      uint8_t r, uint8_t g, uint8_t b) {
    int n = (int)px.size();
    if (n < 3) return;
    // Find y range
    int minY = h, maxY = 0;
    for (int i = 0; i < n; i++) {
        if (py[i] < minY) minY = py[i];
        if (py[i] > maxY) maxY = py[i];
    }
    if (minY < 0) minY = 0;
    if (maxY >= h) maxY = h - 1;
    for (int y = minY; y <= maxY; y++) {
        std::vector<int> xs;
        for (int i = 0; i < n; i++) {
            int j = (i + 1) % n;
            if ((py[i] <= y && py[j] > y) || (py[j] <= y && py[i] > y)) {
                if (py[j] != py[i])
                    xs.push_back(px[i] + (y - py[i]) * (px[j] - px[i]) / (py[j] - py[i]));
            }
        }
        std::sort(xs.begin(), xs.end());
        for (size_t k = 0; k + 1 < xs.size(); k += 2) {
            int x0 = xs[k], x1 = xs[k+1];
            if (x0 < 0) x0 = 0;
            if (x1 >= w) x1 = w - 1;
            for (int x = x0; x <= x1; x++) setPx(buf, w*4, x, y, r, g, b);
        }
    }
}

// VTzero → pixel coords, with optional overzoom transform.
// When rendering at a screen zoom higher than the pmtiles max zoom, the
// source tile covers a 2^delta × 2^delta region of target tiles. We pick
// one sub-tile (s_ozSubX, s_ozSubY) inside the source and scale its
// portion of the MVT extent up to the full target sz.
//
// target_pixel = (mvt_coord * factor * sz / EXTENT) - sub_offset_in_target_pixels
//
// When no overzoom (factor=1, sub=0) the formulas collapse to the original
// `v * sz / EXTENT`.
static thread_local int s_ozFactor  = 1;
static thread_local int s_ozSubX    = 0;
static thread_local int s_ozSubY    = 0;
// Screen zoom for style lookups under overzoom. 0 = no overzoom, the
// renderer falls back to the pmtiles source zoom (line widths, MinZoom
// gates, road styles all keyed off s_ozStyleZ when non-zero).
static thread_local int s_ozStyleZ  = 0;

static inline int toPxX(int v, int sz) {
    return (int)((int64_t)v * sz * s_ozFactor / EXTENT) - s_ozSubX * sz;
}
static inline int toPxY(int v, int sz) {
    return (int)((int64_t)v * sz * s_ozFactor / EXTENT) - s_ozSubY * sz;
}

// ---- Collecteurs geometrie -----------------------------------------------

struct LineCollector {
    std::vector<int> px, py;
    int sz;
    uint8_t r, g, b;
    uint8_t *buf;
    int w, h;
    void linestring_begin(uint32_t) { px.clear(); py.clear(); }
    void linestring_point(vtzero::point p) { px.push_back(toPxX(p.x, sz)); py.push_back(toPxY(p.y, sz)); }
    void linestring_end() {
        for (size_t i = 1; i < px.size(); i++)
            drawLine(buf, w, h, px[i-1], py[i-1], px[i], py[i], r, g, b);
    }
    void points_begin(uint32_t) {} void points_point(vtzero::point) {} void points_end() {}
    void ring_begin(uint32_t) {}   void ring_point(vtzero::point)   {} void ring_end(vtzero::ring_type) {}
};

struct PolyCollector {
    std::vector<int> px, py;
    int sz;
    uint8_t r, g, b;
    uint8_t *buf;
    int w, h;
    void ring_begin(uint32_t) { px.clear(); py.clear(); }
    void ring_point(vtzero::point p) { px.push_back(toPxX(p.x, sz)); py.push_back(toPxY(p.y, sz)); }
    void ring_end(vtzero::ring_type rt) {
        // On remplit TOUS les rings (outer ET inner) en plein : le winding des
        // tuiles bas-zoom est peu fiable (gros polygones classés inner à tort).
        // Les vrais trous du landcover sont négligeables visuellement.
        (void)rt;
        if (px.size() >= 3)
            fillPoly(buf, w, h, px, py, r, g, b);
    }
    void points_begin(uint32_t) {} void points_point(vtzero::point) {} void points_end() {}
    void linestring_begin(uint32_t) {} void linestring_point(vtzero::point) {} void linestring_end() {}
};

// ---- Style tables (from Tile-Generator-Pack features.json) -------------------
struct StyleRule { const char *key; int minZoom; uint8_t r, g, b; };
#define STYLE_LEN(a) (int)(sizeof(a)/sizeof(a[0]))

static bool matchStyle(const StyleRule *t, int n, const std::string &cls, int zoom,
                       uint8_t &r, uint8_t &g, uint8_t &b) {
    if (cls.empty()) return false;
    for (int i = 0; i < n; i++) {
        if (zoom < t[i].minZoom) continue;
        if (cls == t[i].key) { r = t[i].r; g = t[i].g; b = t[i].b; return true; }
    }
    return false;
}

// Style tables aligned with tilemaker process.lua output classes
// landcover classes: wood, grass, farmland, wetland, sand, rock, ice
static const StyleRule kLandcover[] = {
    {"wood",      8, 0xAD,0xD1,0x9E},
    {"grass",     9, 0xCE,0xEC,0xB1},
    {"farmland",  9, 0xEE,0xF0,0xD6},
    {"wetland",   9, 0xAC,0xD2,0xBF},
    {"sand",     10, 0xF5,0xE9,0xC6},
    {"rock",      8, 0xEE,0xE6,0xDD},
    {"ice",      10, 0xDE,0xED,0xED},
};
// landuse classes from landuseKeys in process.lua
static const StyleRule kLanduse[] = {
    {"residential",  8, 0xE1,0xE0,0xE0},
    {"commercial",  10, 0xF2,0xDA,0xD9},
    {"industrial",  10, 0xEB,0xDB,0xE8},
    {"retail",      10, 0xFF,0xD6,0xD1},
    {"military",    10, 0xF5,0xBF,0xBF},
    {"cemetery",    11, 0xAA,0xCB,0xAF},
    {"railway",     11, 0xEB,0xDB,0xE8},
    {"stadium",     11, 0xD6,0xFF,0xDA},
    {"pitch",       13, 0xA9,0xE0,0xCB},
    {"playground",  12, 0xD6,0xFF,0xDA},
    {"school",      13, 0xFF,0xFF,0xE5},
    {"university",  12, 0xFF,0xFF,0xE5},
    {"college",     12, 0xFF,0xFF,0xE5},
    {"kindergarten",13, 0xFF,0xFF,0xE5},
    {"hospital",    12, 0xFF,0xFF,0xE5},
    {"theme_park",  12, 0xD6,0xFF,0xDA},
    {"zoo",         12, 0xD6,0xFF,0xDA},
};
// park layer: only national_park and nature_reserve (process.lua)
static const StyleRule kPark[] = {
    {"national_park",  8, 0xF2,0xEF,0xE9},
    {"nature_reserve", 9, 0xF2,0xEF,0xE9},
};
static const StyleRule kAeroway[] = {
    {"aerodrome", 10, 0xE7,0xE6,0xDE}, {"apron",   12, 0xE7,0xE6,0xDE},
    {"runway",    12, 0xB2,0xB5,0xD1}, {"taxiway", 12, 0xB2,0xB5,0xD1},
    {"helipad",   11, 0xE7,0xE6,0xDE}, {"hangar",  12, 0xD9,0xD0,0xC9},
};
static const StyleRule kWater[] = {
    {"ocean",     6, 0xAA,0xD2,0xDF},
    {"water",     8, 0xAA,0xD2,0xDF}, {"bay",        8, 0xAA,0xD2,0xDF},
    {"river",     8, 0xAA,0xD2,0xDF}, {"canal",     10, 0xAA,0xD2,0xDF},
    {"lake",      8, 0xAA,0xD2,0xDF}, {"reservoir",  8, 0xAA,0xD2,0xDF},
    {"pond",     12, 0xAA,0xD2,0xDF}, {"basin",     11, 0xAA,0xD2,0xDF},
    {"dock",     12, 0xAA,0xD2,0xDF}, {"riverbank",  8, 0xAA,0xD2,0xDF},
};

// Road classification — colour + min zoom + line width multiplier
struct RoadStyle { const char *cls; int minZ; uint8_t r,g,b; float wMul; };
static const RoadStyle kRoads[] = {
    {"motorway",      6, 0xE8,0x92,0xA2, 3.0f},
    {"trunk",         6, 0xF9,0xB2,0x9C, 2.5f},
    {"primary",       7, 0xFC,0xD6,0xA4, 2.0f},
    {"secondary",     9, 0xF7,0xFA,0xBF, 1.5f},
    {"tertiary",     12, 0xFF,0xFF,0xFF, 1.2f},
    {"unclassified", 13, 0xFF,0xFF,0xFF, 1.0f},
    {"residential",  13, 0xFF,0xFF,0xFF, 1.0f},
    {"living_street",13, 0xED,0xED,0xED, 1.0f},
    {"pedestrian",   13, 0xDD,0xDD,0xE8, 1.0f},
    {"motorway_link",10, 0xE8,0x92,0xA2, 2.0f},
    {"trunk_link",   10, 0xF9,0xB2,0x9C, 1.8f},
    {"primary_link", 11, 0xFC,0xD6,0xA4, 1.5f},
    {"secondary_link",12,0xF7,0xFA,0xBF, 1.2f},
    {"tertiary_link",12, 0xFF,0xFF,0xFF, 1.0f},
    {"service",      15, 0xFF,0xFF,0xFF, 0.8f},
    {"track",        14, 0x99,0x66,0x00, 0.7f},
    {"construction", 13, 0xAA,0xAA,0xAA, 1.0f},
    {"path",         14, 0x88,0x88,0x88, 0.5f},
    {"footway",      14, 0x88,0x88,0x88, 0.5f},
    {"cycleway",     14, 0x88,0x88,0x88, 0.5f},
    {"steps",        14, 0x88,0x88,0x88, 0.5f},
    {"bridleway",    14, 0x88,0x88,0x88, 0.5f},
};
static const RoadStyle *findRoad(const std::string &cls) {
    if (cls.empty()) return nullptr;
    for (auto &r : kRoads) if (cls == r.cls) return &r;
    return nullptr;
}

// Largeur de route par zoom — transcription de constants.hpp line_width_per_zoom
// (Tile-Generator-Pack). Valeur = largeur pleine en px. -1 = non visible à ce zoom.
static int roadWidthForZoom(const std::string &cls, int z) {
    struct WZ { const char *cls; int z[14]; }; // index 0=z6 .. 13=z19
    // -1 = absent (route pas dessinée à ce zoom)
    static const WZ T[] = {
        //                 z6 z7 z8 z9 z10 z11 z12 z13 z14 z15 z16 z17 z18 z19
        {"motorway",      { 2, 2, 2, 2,  3,  3,  4,  5,  6,  7, 10, 18, 22, 28}},
        {"motorway_link", {-1,-1,-1,-1,  2,  2,  2,  3,  3,  5,  8, 14, 14, 16}},
        {"trunk",         { 2, 2, 2, 2,  3,  3,  4,  5,  6,  7, 10, 18, 22, 28}},
        {"trunk_link",    {-1,-1,-1,-1,  2,  2,  2,  3,  3,  5,  8, 14, 14, 16}},
        {"primary",       {-1,-1, 2, 2,  3,  3,  3,  4,  5,  6, 10, 16, 22, 28}},
        {"primary_link",  {-1,-1,-1,-1,  2,  2,  2,  3,  3,  4,  8, 12, 14, 16}},
        {"secondary",     {-1,-1,-1,-1,  2,  2,  3,  3,  4,  5, 10, 14, 22, 28}},
        {"secondary_link",{-1,-1,-1,-1,  2,  2,  2,  3,  2,  3,  8, 10, 14, 16}},
        {"tertiary",      {-1,-1,-1,-1, -1,  2,  2,  3,  3,  4, 10, 12, 19, 28}},
        {"tertiary_link", {-1,-1,-1,-1, -1, -1,  2,  2,  2,  3,  8, 10, 12, 16}},
        {"pedestrian",    {-1,-1,-1,-1, -1, -1, -1,  2,  2,  3,  8, 12, 15, 18}},
        {"residential",   {-1,-1,-1,-1, -1, -1, -1,  2,  2,  3,  5,  8, 11, 18}},
        {"living_street", {-1,-1,-1,-1, -1, -1, -1,  2,  2,  3,  5,  8, 11, 18}},
        {"unclassified",  {-1,-1,-1,-1, -1, -1,  2,  2,  3,  3,  5, 12, 11, 18}},
        {"service",       {-1,-1,-1,-1, -1, -1, -1,  2,  2,  2,  3,  6,  8, 10}},
        {"track",         {-1,-1,-1,-1, -1, -1, -1, -1, -1,  2,  2,  4,  4,  6}},
        {"rail",          {-1,-1,-1, 2,  2,  2,  2,  2,  2,  2,  3,  3,  4,  6}},
    };
    if (z < 6) z = 6; if (z > 19) z = 19;
    for (auto &w : T) if (cls == w.cls) { int v = w.z[z-6]; return v; }
    return 2; // défaut routes mineures non tabulées
}

// Waterway classification
static const StyleRule kWaterway[] = {
    {"river",  10, 0xAA,0xD2,0xDF}, {"canal", 10, 0xAA,0xD2,0xDF},
    {"stream", 13, 0xAA,0xD2,0xDF}, {"ditch", 13, 0xAA,0xD2,0xDF},
    {"drain",  13, 0xAA,0xD2,0xDF}, {"dam",   12, 0xAD,0xAD,0xAD},
    {"weir",   12, 0xAA,0xAA,0xAA},
};
static const StyleRule kRailway[] = {
    {"rail",        10, 0x88,0x88,0x88}, {"tram",        12, 0x88,0x88,0x88},
    {"abandoned",   12, 0x77,0x77,0x77}, {"disused",     12, 0x88,0x88,0x88},
    {"funicular",   11, 0x88,0x88,0x88}, {"subway",      12, 0x88,0x88,0x88},
    {"light_rail",  12, 0x88,0x88,0x88}, {"narrow_gauge",11, 0x88,0x88,0x88},
};
// admin_level thresholds for boundary rendering (integer attribute)
struct AdminStyle { int level; int minZ; uint8_t r,g,b; int width; };
static const AdminStyle kAdmin[] = {
    {2, 6, 0x8D,0x61,0x8B, 2},
    {4, 7, 0x8D,0x61,0x8B, 1},
    {6, 9, 0x8D,0x61,0x8B, 1},
};

// Place label styling: class → {minZoom, priority, font, r, g, b}
struct PlaceStyle { const char *cls; int minZ; int prio; const lv_font_t *font; uint8_t r,g,b; };
// Place label styles: priority + color + font. minZ=0 everywhere — zoom
// filtering is delegated to tilemaker (process-aprs.lua). Duplicating it
// here produced empty zoom bands or abrupt gaps instead of a smooth progression.
// Priority: lower = drawn first / wins collision.
// Colors: #555555 city/town, #666666 village/suburb, #777777 hamlet.
static const PlaceStyle kPlaces[] = {
    {"city",      0, 10, &lv_font_montserrat_14, 0x55,0x55,0x55},
    {"town",      0, 20, &lv_font_montserrat_14, 0x55,0x55,0x55},
    {"village",   0, 30, &lv_font_montserrat_12, 0x66,0x66,0x66},
    {"borough",   0, 33, &lv_font_montserrat_12, 0x66,0x66,0x66},
    {"suburb",    0, 35, &lv_font_montserrat_12, 0x66,0x66,0x66},
    {"neighbourhood",0,38,&lv_font_montserrat_12,0x66,0x66,0x66},
    {"hamlet",    0, 40, &lv_font_montserrat_12, 0x77,0x77,0x77},
    {"quarter",   0, 45, &lv_font_montserrat_12, 0x77,0x77,0x77},
    {"locality",  0, 45, &lv_font_montserrat_12, 0x77,0x77,0x77},
    {"islet",     0, 50, &lv_font_montserrat_12, 0x77,0x77,0x77},
    {"isolated_dwelling",0,55,&lv_font_montserrat_12,0x77,0x77,0x77},
    {"farm",      0, 55, &lv_font_montserrat_12, 0x77,0x77,0x77},
    // Admin regions: prio above every settlement so a region is placed last and
    // never displaces a city; the label collision also keeps them clear.
    {"state",     0, 60, &lv_font_montserrat_14, 0x55,0x44,0x43},
    {"country",   0, 62, &lv_font_montserrat_14, 0x33,0x22,0x21},
};

// ---- Path drawing with width ------------------------------------------------
static void drawWideLine(uint8_t *buf, int w, int h, int x0, int y0, int x1, int y1,
                         int width, uint8_t r, uint8_t g, uint8_t b) {
    if (width <= 1) { drawLine(buf,w,h,x0,y0,x1,y1,r,g,b); return; }
    int dx = abs(x1-x0), sx = x0<x1 ? 1 : -1;
    int dy = -abs(y1-y0), sy = y0<y1 ? 1 : -1;
    int err = dx+dy, rad = width/2;
    while (true) {
        for (int dy2 = -rad; dy2 <= rad; dy2++)
            for (int dx2 = -rad; dx2 <= rad; dx2++)
                if (dx2*dx2 + dy2*dy2 <= rad*rad)
                    { int px=x0+dx2, py=y0+dy2; if(px>=0&&px<w&&py>=0&&py<h)setPx(buf,w*4,px,py,r,g,b); }
        if (x0==x1 && y0==y1) break;
        int e2=2*err; if (e2>=dy) { err+=dy; x0+=sx; } if (e2<=dx) { err+=dx; y0+=sy; }
    }
}

// ---- Line collector with per-feature class lookup ----------------------------
struct StyledLineCollector {
    std::vector<int> px, py;
    int sz, w, h, width;
    uint8_t *buf;
    uint8_t r, g, b;
    int zoom;
    const char *propName;   // "class" for most layers
    const StyleRule *table; int tableLen;
    const RoadStyle *roadTable;
    void setTable(const StyleRule *t, int n) { table = t; tableLen = n; roadTable = nullptr; }
    void setRoads() { table = nullptr; tableLen = 0; roadTable = kRoads; }
    void linestring_begin(uint32_t) { px.clear(); py.clear(); }
    void linestring_point(vtzero::point p) { px.push_back(toPxX(p.x, sz)); py.push_back(toPxY(p.y, sz)); }
    void linestring_end() {
        for (size_t i = 1; i < px.size(); i++)
            drawWideLine(buf,w,h,px[i-1],py[i-1],px[i],py[i],width,r,g,b);
    }
    void points_begin(uint32_t) {} void points_point(vtzero::point) {} void points_end() {}
    void ring_begin(uint32_t) {}   void ring_point(vtzero::point)   {} void ring_end(vtzero::ring_type) {}
    void applyClass(const std::string &cls) {
        if (roadTable) {
            auto *rd = findRoad(cls);
            if (rd && zoom >= rd->minZ) { r=rd->r; g=rd->g; b=rd->b; width = (int)(rd->wMul*1.2f); }
            else { r=g=b=0; }
        } else if (table) {
            if (!matchStyle(table, tableLen, cls, zoom, r, g, b)) r=g=b=0;
            width = 1;
        }
    }
};

// ---- renderTileBufCore : rend la tuile à la taille sz (utilisé en 2x par le SSAA)
// `srcZ/x/y` = source tile coords in the pmtiles. When overzoom is active
// (s_ozStyleZ != 0), style decisions use the screen zoom instead of the
// source zoom — road widths, MinZoom gates etc.
static void renderTileBufCore(uint8_t *buf, int sz, int srcZ, int x, int y) {
    int z = s_ozStyleZ ? s_ozStyleZ : srcZ;
    int w = sz, h = sz;

    // Background — LAND_BG_COLOR du générateur (#f2efe9), octets B,G,R,A
    for (int i = 0; i < w * h; i++) {
        uint8_t *p = buf + i * 4;
        p[0] = 0xE9; p[1] = 0xEF; p[2] = 0xF2; p[3] = 0xFF;
    }

    auto [off, len] = pmtiles::get_tile(gunzip, (const char*)s_mapped, srcZ, x, y);
    if (len == 0) return;
    std::string raw((const char*)s_mapped + off, len);
    std::string mvt = (s_header.tile_compression == pmtiles::COMPRESSION_GZIP)
                      ? gunzip(raw, 0) : raw;
    if (mvt.empty()) return;

    auto layerName = [](vtzero::layer &l) { return std::string(l.name()); };
    auto readClass = [](vtzero::feature &f) -> std::string {
        while (auto p = f.next_property()) {
            if (p.key() == "class" && p.value().type() == vtzero::property_value_type::string_value) {
                auto v = p.value().string_value();
                return std::string(v.data(), v.size());
            }
        }
        return {};
    };
    // Lit class + subclass d'un coup. Le schéma tilemaker regroupe les petites
    // rues sous class="minor" (subclass=residential/living_street/…) et les
    // chemins sous class="path" (subclass=footway/pedestrian/…).
    auto readClassSub = [](vtzero::feature &f, std::string &cls, std::string &sub) {
        cls.clear(); sub.clear();
        while (auto p = f.next_property()) {
            if (p.value().type() != vtzero::property_value_type::string_value) continue;
            auto v = p.value().string_value();
            if (p.key() == "class")    cls.assign(v.data(), v.size());
            else if (p.key() == "subclass") sub.assign(v.data(), v.size());
        }
    };
    // Clé de style routier : pour minor/path on prend le subclass (vrai tag OSM).
    // Retourne "" si la route ne doit pas être rendue (chemins non-pedestrian).
    auto roadKey = [](const std::string &cls, const std::string &sub) -> std::string {
        if (cls == "minor") return sub.empty() ? std::string("residential") : sub;
        if (cls == "path")  return (sub == "pedestrian") ? std::string("pedestrian") : std::string();
        return cls;
    };
    // Lit class, subclass et ramp (les bretelles = ramp=1, repliées dans la classe
    // parente). Pour une bretelle on prend la largeur du variant _link (plus fin).
    auto readRoad = [](vtzero::feature &f, std::string &cls, std::string &sub, bool &ramp) {
        cls.clear(); sub.clear(); ramp = false;
        while (auto p = f.next_property()) {
            auto t = p.value().type();
            if (p.key() == "ramp") { ramp = true; continue; }
            if (t != vtzero::property_value_type::string_value) continue;
            auto v = p.value().string_value();
            if (p.key() == "class")    cls.assign(v.data(), v.size());
            else if (p.key() == "subclass") sub.assign(v.data(), v.size());
        }
    };
    auto widthKey = [](const std::string &key, bool ramp) -> std::string {
        return (ramp && (key=="motorway"||key=="trunk"||key=="primary"||
                         key=="secondary"||key=="tertiary")) ? key + "_link" : key;
    };

    // Couleurs fines landcover par subclass (features.json) — override la couleur
    // de classe (sinon heath/scrub/fell tombent tous sur le vert "grass").
    struct SubColor { const char *sub; uint8_t r,g,b; };
    static const SubColor kLandcoverSub[] = {
        {"heath",     0xD6,0xD9,0xA0}, {"scrub",     0xC9,0xD7,0xAC},
        {"shrubbery", 0xC9,0xD7,0xAC}, {"fell",      0xD6,0xD9,0x9F},
    };
    // Pass 1: landcover / landuse / water / park / aeroway / building polygons
    auto renderPolyLayer = [&](const char *layerName, const StyleRule *tbl, int n) {
        bool isLandcover = (std::string(layerName) == "landcover");
        vtzero::vector_tile t{mvt};
        while (auto lay = t.next_layer()) {
            if (layerName != std::string(lay.name())) continue;
            while (auto feat = lay.next_feature()) {
                if (feat.geometry_type() != vtzero::GeomType::POLYGON) continue;
                std::string cls, sub; readClassSub(feat, cls, sub);
                if (cls.empty()) continue;
                uint8_t r,g,b;
                if (!matchStyle(tbl, n, cls, z, r, g, b)) continue;
                if (isLandcover && !sub.empty())
                    for (auto &sc : kLandcoverSub) if (sub == sc.sub) { r=sc.r; g=sc.g; b=sc.b; break; }
                PolyCollector pc; pc.sz=sz; pc.buf=buf; pc.w=w; pc.h=h; pc.r=r; pc.g=g; pc.b=b;
                vtzero::decode_polygon_geometry(feat.geometry(), pc);
            }
        }
    };
    // Render order optimised for tracker readability:
    // farmland/grass background → urban areas visible → forests on top
    static const StyleRule kLandcoverBg[] = {
        {"farmland", 9, 0xEE,0xF0,0xD6},
        {"grass",    9, 0xCE,0xEC,0xB1},
        {"sand",    10, 0xF5,0xE9,0xC6},
        {"rock",     8, 0xEE,0xE6,0xDD},
        {"ice",     10, 0xDE,0xED,0xED},
    };
    static const StyleRule kLandcoverFg[] = {
        {"wood",    8, 0xAD,0xD1,0x9E},
        {"wetland", 9, 0xAC,0xD2,0xBF},
    };
    // Draw order matters: a park / hippodrome lawn is often INSIDE a larger
    // landuse=residential polygon. If landuse painted after landcover, the
    // residential gray hid the inner grass. Paint landuse FIRST (urban
    // background), then landcover (grass / farmland) ON TOP.
    renderPolyLayer("park",      kPark,         STYLE_LEN(kPark));       // national_park, nature_reserve
    renderPolyLayer("landuse",   kLanduse,      STYLE_LEN(kLanduse));     // residential / commercial / industrial background
    renderPolyLayer("landcover", kLandcoverBg,  STYLE_LEN(kLandcoverBg)); // grass / farmland on top of urban
    renderPolyLayer("aeroway",   kAeroway,      STYLE_LEN(kAeroway));
    renderPolyLayer("landcover", kLandcoverFg,  STYLE_LEN(kLandcoverFg)); // forest/wetland on top
    if (z >= 13) {
        vtzero::vector_tile t{mvt};
        while (auto lay = t.next_layer()) {
            if (std::string(lay.name()) != "building") continue;
            PolyCollector pc; pc.sz=sz; pc.buf=buf; pc.w=w; pc.h=h; pc.r=0xD9; pc.g=0xD0; pc.b=0xC9;
            while (auto feat = lay.next_feature())
                if (feat.geometry_type() == vtzero::GeomType::POLYGON)
                    vtzero::decode_polygon_geometry(feat.geometry(), pc);
        }
    }
    renderPolyLayer("water",     kWater,     STYLE_LEN(kWater));     // p8: last polygon layer

    // Pass 2: boundary lines — admin_level is an integer attribute, not "class"
    {
        vtzero::vector_tile tb{mvt};
        while (auto lay = tb.next_layer()) {
            if (std::string(lay.name()) != "boundary") continue;
            while (auto feat = lay.next_feature()) {
                if (feat.geometry_type() != vtzero::GeomType::LINESTRING) continue;
                int admin_level = 0;
                while (auto prop = feat.next_property()) {
                    if (prop.key() == "admin_level") {
                        if (prop.value().type() == vtzero::property_value_type::sint_value)
                            admin_level = (int)prop.value().sint_value();
                        else if (prop.value().type() == vtzero::property_value_type::uint_value)
                            admin_level = (int)prop.value().uint_value();
                        break;
                    }
                }
                if (admin_level <= 0) continue;
                const AdminStyle *as = nullptr;
                for (auto &a : kAdmin) if (a.level == admin_level && z >= a.minZ) { as = &a; break; }
                if (!as) continue;
                StyledLineCollector lc; lc.sz=sz; lc.buf=buf; lc.w=w; lc.h=h;
                lc.r=as->r; lc.g=as->g; lc.b=as->b; lc.width=as->width;
                lc.zoom=z; lc.table=nullptr; lc.tableLen=0; lc.roadTable=nullptr;
                vtzero::decode_linestring_geometry(feat.geometry(), lc);
            }
        }
    }

    // Pass 3: waterway lines
    {
        StyledLineCollector lc;
        lc.sz=sz; lc.buf=buf; lc.w=w; lc.h=h; lc.zoom=z; lc.width=1;
        lc.setTable(kWaterway, STYLE_LEN(kWaterway));
        vtzero::vector_tile t{mvt};
        while (auto lay = t.next_layer()) {
            if (std::string(lay.name()) != "waterway") continue;
            while (auto feat = lay.next_feature()) {
                if (feat.geometry_type() != vtzero::GeomType::LINESTRING) continue;
                std::string cls = readClass(feat);
                if (cls.empty()) continue;
                lc.r=lc.g=lc.b=0; lc.applyClass(cls);
                if (lc.r==0 && lc.g==0 && lc.b==0) continue;
                vtzero::decode_linestring_geometry(feat.geometry(), lc);
            }
        }
    }

    // Pass 3b: aeroway lines — runways and taxiways are usually LINESTRING and are
    // not covered by the polygon pass. Drawn from z12 up.
    if (z >= 12) {
        StyledLineCollector lc;
        lc.sz=sz; lc.buf=buf; lc.w=w; lc.h=h; lc.zoom=z;
        lc.table=nullptr; lc.tableLen=0; lc.roadTable=nullptr;
        lc.r=0xB2; lc.g=0xB5; lc.b=0xD1;   // #b2b5d1
        vtzero::vector_tile t{mvt};
        while (auto lay = t.next_layer()) {
            if (std::string(lay.name()) != "aeroway") continue;
            while (auto feat = lay.next_feature()) {
                if (feat.geometry_type() != vtzero::GeomType::LINESTRING) continue;
                std::string cls = readClass(feat);
                if (cls == "runway")       lc.width = (z>=14)?7:5;
                else if (cls == "taxiway") lc.width = (z>=14)?3:2;
                else continue;
                vtzero::decode_linestring_geometry(feat.geometry(), lc);
            }
        }
    }

    // Pass 4: railway lines
    {
        StyledLineCollector lc;
        lc.sz=sz; lc.buf=buf; lc.w=w; lc.h=h; lc.zoom=z; lc.width=1;
        lc.setTable(kRailway, STYLE_LEN(kRailway));
        vtzero::vector_tile t{mvt};
        while (auto lay = t.next_layer()) {
            if (std::string(lay.name()) != "transportation") continue;
            while (auto feat = lay.next_feature()) {
                if (feat.geometry_type() != vtzero::GeomType::LINESTRING) continue;
                std::string cls = readClass(feat);
                if (cls.empty()) continue;
                lc.r=lc.g=lc.b=0; lc.applyClass(cls);
                if (lc.r==0 && lc.g==0 && lc.b==0) continue;
                vtzero::decode_linestring_geometry(feat.geometry(), lc);
            }
        }
    }

    // Pass 5: roads
    {
        StyledLineCollector lc;
        lc.sz=sz; lc.buf=buf; lc.w=w; lc.h=h; lc.zoom=z; lc.width=1;
        lc.setRoads();
        // Outline pass (liseré fin, fill+2) — uniquement routes MAJEURES
        // (motorway/trunk/primary/secondary, wMul>=1.5). Tertiary et en dessous
        // n'ont pas de liseré (sinon ça surépaissit residential/living_street).
        vtzero::vector_tile t1{mvt};
        while (auto lay = t1.next_layer()) {
            if (std::string(lay.name()) != "transportation") continue;
            while (auto feat = lay.next_feature()) {
                if (feat.geometry_type() != vtzero::GeomType::LINESTRING) continue;
                std::string cls, sub; bool ramp; readRoad(feat, cls, sub, ramp);
                std::string key = roadKey(cls, sub);
                if (key.empty()) continue;
                auto *rd = findRoad(key);
                if (!rd) continue;
                if (rd->wMul < 1.5f) continue;   // pas de liseré sur tertiary et en dessous
                int fw = roadWidthForZoom(widthKey(key, ramp), z);
                if (fw < 3) continue;            // ni si trop fine à ce zoom
                // Liseré = couleur de la voie légèrement assombrie (×0.85, même teinte),
                // pas du noir — discret sur le 7" 1024x600.
                lc.r=(uint8_t)(rd->r*0.85f); lc.g=(uint8_t)(rd->g*0.85f); lc.b=(uint8_t)(rd->b*0.85f);
                lc.width = fw + 2;
                vtzero::decode_linestring_geometry(feat.geometry(), lc);
            }
        }
        // Fill pass (couleur de route, largeur de la table)
        vtzero::vector_tile t2{mvt};
        while (auto lay = t2.next_layer()) {
            if (std::string(lay.name()) != "transportation") continue;
            while (auto feat = lay.next_feature()) {
                if (feat.geometry_type() != vtzero::GeomType::LINESTRING) continue;
                std::string cls, sub; bool ramp; readRoad(feat, cls, sub, ramp);
                std::string key = roadKey(cls, sub);
                if (key.empty()) continue;       // chemins non-pedestrian non rendus
                auto *rd = findRoad(key);
                if (!rd) continue;
                int fw = roadWidthForZoom(widthKey(key, ramp), z);
                if (fw < 0) continue;            // route pas visible à ce zoom
                lc.r=rd->r; lc.g=rd->g; lc.b=rd->b;
                lc.width = fw;
                vtzero::decode_linestring_geometry(feat.geometry(), lc);
            }
        }
    }

}

// ---- SSAA : sous-échantillonnage 2x2 (moyenne de boîte) → anti-aliasing ------
static void downsample2x(const uint8_t *src, int ssz, uint8_t *dst, int dsz) {
    for (int y = 0; y < dsz; y++) {
        for (int x = 0; x < dsz; x++) {
            const uint8_t *s0 = src + ((y*2)   * ssz + x*2) * 4;
            const uint8_t *s1 = s0 + 4;
            const uint8_t *s2 = src + ((y*2+1) * ssz + x*2) * 4;
            const uint8_t *s3 = s2 + 4;
            uint8_t *d = dst + (y * dsz + x) * 4;
            d[0] = (uint8_t)((s0[0]+s1[0]+s2[0]+s3[0]) >> 2);
            d[1] = (uint8_t)((s0[1]+s1[1]+s2[1]+s3[1]) >> 2);
            d[2] = (uint8_t)((s0[2]+s1[2]+s2[2]+s3[2]) >> 2);
            d[3] = 0xFF;
        }
    }
}

// Resolve overzoom for a target tile (screen zoom may exceed pmtiles max).
// Sets thread_local s_oz* and returns the source (srcZ, srcX, srcY) to fetch
// from pmtiles. Caller must reset via ozReset() when done.
struct OZResolved { int srcZ, srcX, srcY; };
static OZResolved ozSetup(int z, int x, int y) {
    if (!s_mapped) return {z, x, y};
    int maxZ = s_header.max_zoom;
    if (z <= maxZ) return {z, x, y};
    int delta = z - maxZ;
    int factor = 1 << delta;
    s_ozFactor = factor;
    s_ozSubX   = x & (factor - 1);
    s_ozSubY   = y & (factor - 1);
    s_ozStyleZ = z;
    return { maxZ, x >> delta, y >> delta };
}
static inline void ozReset() {
    s_ozFactor = 1; s_ozSubX = 0; s_ozSubY = 0; s_ozStyleZ = 0;
}

// ---- renderTileBuf : SSAA ×2 (rend en 2× puis réduit → bords lissés) ---------
static void renderTileBuf(uint8_t *buf, int sz, int z, int x, int y) {
    auto src = ozSetup(z, x, y);
    const int SS = 2;
    int ssz = sz * SS;
    uint8_t *big = (uint8_t *)malloc((size_t)ssz * ssz * 4);
    if (!big) { renderTileBufCore(buf, sz, src.srcZ, src.srcX, src.srcY); ozReset(); return; }
    renderTileBufCore(big, ssz, src.srcZ, src.srcX, src.srcY);
    downsample2x(big, ssz, buf, sz);
    free(big);
    ozReset();
}

// ---- Per-tile label cache. Tiles never change, so a (z,x,y)'s labels are decoded
// once and reused. Avoids re-decoding the 25 visible tiles on every pan step.
static std::map<uint64_t, std::vector<Label>> s_labelCache;
static std::deque<uint64_t> s_labelCacheOrder;
static const size_t LABEL_CACHE_MAX = 80;
static inline uint64_t labelKey(int z, int x, int y) {
    return ((uint64_t)(z & 0x1F) << 48) | ((uint64_t)(x & 0xFFFFFF) << 24) | (uint64_t)(y & 0xFFFFFF);
}

// ---- getTileLabels: collect a tile's labels in tile-local coords (size sz) without
// drawing. Global placement/collision is done by map_view (LVGL widgets).
void getTileLabels(int z, int x, int y, std::vector<Label> &out) {
    if (!s_mapped) return;
    uint64_t lk = labelKey(z, x, y);
    auto cit = s_labelCache.find(lk);
    if (cit != s_labelCache.end()) { out = cit->second; return; }
    auto src = ozSetup(z, x, y);
    auto [off, len] = pmtiles::get_tile(gunzip, (const char*)s_mapped, src.srcZ, src.srcX, src.srcY);
    if (len == 0) { ozReset(); return; }
    std::string raw((const char*)s_mapped + off, len);
    std::string mvt = (s_header.tile_compression == pmtiles::COMPRESSION_GZIP)
                      ? gunzip(raw, 0) : raw;
    if (mvt.empty()) { ozReset(); return; }
    const int sz = TILE_SIZE;
    auto push = [&](int px, int py, const std::string &t, int prio,
                    const lv_font_t *f, uint8_t r, uint8_t g, uint8_t b,
                    int angle = 0, bool followLine = false, bool shield = false,
                    int population = 0, bool isRegion = false) {
        // Keep labels whose anchor lies in the (sub-)tile. Margin tightened
        // to zero so labels near a sub-tile boundary aren't dropped from
        // every sibling sub-tile under overzoom — global collision in
        // map_labels handles the actual edge clipping.
        if (px>=0 && px<sz && py>=0 && py<sz) {
            MapVector::Label lab;
            lab.px = px; lab.py = py; lab.priority = prio; lab.text = t;
            lab.r = r; lab.g = g; lab.b = b; lab.font = f;
            lab.angle = angle; lab.followLine = followLine; lab.shield = shield;
            lab.population = population;
            lab.isRegion = isRegion;
            out.push_back(std::move(lab));
        }
    };
    // Milieu d'une polyligne (waterway / transportation_name) en coords tuile-locales.
    struct LineMid { int ts; std::vector<int> px,py;
        void linestring_begin(uint32_t){px.clear();py.clear();}
        void linestring_point(vtzero::point p){px.push_back(toPxX(p.x, ts));py.push_back(toPxY(p.y, ts));}
        void linestring_end(){} void points_begin(uint32_t){}void points_point(vtzero::point){}void points_end(){}
        void ring_begin(uint32_t){}void ring_point(vtzero::point){}void ring_end(vtzero::ring_type){}
    };
    vtzero::vector_tile tv{mvt};
    while (auto lay = tv.next_layer()) {
        std::string name(lay.name());
        // Labels: places, waterways and major road refs (the A/N/D number, not the
        // street name). Water names live in water_name (rivers) and water_name_detail
        // (canals + lakes); min_zoom is handled when the tiles are built.
        bool isPlace = (name == "place");
        bool isWaterName = (name == "water_name");
        bool isWaterDetail = (name == "water_name_detail");
        bool isRoadName = (name == "transportation_name");
        bool isPeak = (name == "mountain_peak");
        if (!isPlace && !isWaterName && !isWaterDetail && !isRoadName && !isPeak) continue;
        while (auto feat = lay.next_feature()) {
            std::string labelText, nameLatin, cls, ref;
            int pop = 0;
            int ele = 0;
            while (auto prop = feat.next_property()) {
                auto pt = prop.value().type();
                if (pt == vtzero::property_value_type::string_value) {
                    auto v = prop.value().string_value(); std::string val(v.data(), v.size());
                    if (prop.key() == "name")            labelText = val;
                    else if (prop.key() == "name:latin") nameLatin = val;
                    else if (prop.key() == "class")      cls = val;
                    else if (prop.key() == "ref")        ref = val;
                } else if (prop.key() == "population") {
                    if (pt == vtzero::property_value_type::int_value)       pop = (int)prop.value().int_value();
                    else if (pt == vtzero::property_value_type::sint_value) pop = (int)prop.value().sint_value();
                    else if (pt == vtzero::property_value_type::uint_value) pop = (int)prop.value().uint_value();
                } else if (prop.key() == "ele") {
                    if (pt == vtzero::property_value_type::int_value)       ele = (int)prop.value().int_value();
                    else if (pt == vtzero::property_value_type::sint_value) ele = (int)prop.value().sint_value();
                    else if (pt == vtzero::property_value_type::uint_value) ele = (int)prop.value().uint_value();
                    else if (pt == vtzero::property_value_type::float_value) ele = (int)prop.value().float_value();
                    else if (pt == vtzero::property_value_type::double_value) ele = (int)prop.value().double_value();
                }
            }
            auto geom = feat.geometry_type();
            bool isLake = isWaterDetail && cls == "lake";

            // --- Places + lake: polygon/point centroid ---
            if ((isPlace || isLake) && (geom == vtzero::GeomType::POLYGON || geom == vtzero::GeomType::POINT)) {
                if (labelText.empty()) labelText = nameLatin;
                if (labelText.empty()) continue;
                struct LabelPos { int minX=99999,minY=99999,maxX=-99999,maxY=-99999,n=0,ts=0;
                    void add(vtzero::point p){int px=toPxX(p.x, ts),py=toPxY(p.y, ts);
                        if(px<minX)minX=px;if(px>maxX)maxX=px;if(py<minY)minY=py;if(py>maxY)maxY=py;n++;}
                    void ring_begin(uint32_t){} void ring_point(vtzero::point p){add(p);} void ring_end(vtzero::ring_type){}
                    void points_begin(uint32_t){} void points_point(vtzero::point p){add(p);} void points_end(){}
                    void linestring_begin(uint32_t){}void linestring_point(vtzero::point){}void linestring_end(){}
                } lp; lp.ts=sz;
                if (geom == vtzero::GeomType::POLYGON) vtzero::decode_polygon_geometry(feat.geometry(),lp);
                else vtzero::decode_point_geometry(feat.geometry(),lp);
                if (lp.n>0) {
                    int cx=(lp.minX+lp.maxX)/2, cy=(lp.minY+lp.maxY)/2;
                    if (isPlace) {
                        for (auto &ps : kPlaces) if (cls==ps.cls && z>=ps.minZ) {
                            bool region = (cls=="state" || cls=="country");
                            push(cx,cy,labelText,ps.prio,rtFont(ps.font),ps.r,ps.g,ps.b,0,false,false,pop,region); break; }
                    } else push(cx,cy,labelText,70,rtFont(&lv_font_montserrat_12),0x4A,0x7A,0xB0);
                }
            }
            // --- Mountain peak / volcano: triangle marker from Z12 with name + ele
            // mountain_peak layer (natural=peak), min zoom 12.
            else if (isPeak && geom == vtzero::GeomType::POINT) {
                if (z < 12) continue;
                struct PointAt { int ts; int px=-1, py=-1;
                    void points_begin(uint32_t){} void points_point(vtzero::point p){ px=toPxX(p.x,ts); py=toPxY(p.y,ts); } void points_end(){}
                    void linestring_begin(uint32_t){} void linestring_point(vtzero::point){} void linestring_end(){}
                    void ring_begin(uint32_t){} void ring_point(vtzero::point){} void ring_end(vtzero::ring_type){}
                } pa; pa.ts=sz;
                vtzero::decode_point_geometry(feat.geometry(), pa);
                if (pa.px<0 || pa.py<0 || pa.px>=sz || pa.py>=sz) continue;
                Label lab;
                lab.px = pa.px; lab.py = pa.py;
                lab.priority = 40;             // between roads (25-35) and obscure places (50+)
                lab.text = labelText.empty() ? nameLatin : labelText;
                lab.r = 0xD0; lab.g = 0x80; lab.b = 0x50; // #d08050
                lab.font = rtFont(&lv_font_montserrat_12);
                lab.isPeak = true;
                lab.elevation = ele;
                out.push_back(std::move(lab));
            }
            // --- Waterways (river in water_name, canal in water_name_detail): name
            // oriented along the path, no background. min_zoom handled by the tiles.
            else if ((isWaterName || (isWaterDetail && cls == "canal"))
                     && geom == vtzero::GeomType::LINESTRING) {
                if (labelText.empty()) labelText = nameLatin;
                if (labelText.empty()) continue;
                LineMid lm; lm.ts=sz;
                vtzero::decode_linestring_geometry(feat.geometry(),lm);
                if (lm.px.size()>=2) {
                    // Anchor at the polyline mid-index for dedup/hysteresis.
                    // The actual rendering walks the full path glyph by glyph
                    // thanks to Label::path populated below.
                    int m = (int)lm.px.size() / 2;
                    int px = lm.px[m], py = lm.py[m];
                    if (px>=0 && px<sz && py>=0 && py<sz) {
                        Label lab;
                        lab.px = px; lab.py = py;
                        lab.priority = 60;
                        lab.text = labelText;
                        lab.r = 0x4A; lab.g = 0x7A; lab.b = 0xB0;
                        lab.font = rtFont(&lv_font_montserrat_12);
                        lab.angle = 0;             // unused once path is set
                        lab.followLine = true;
                        lab.shield = false;
                        lab.isPlace = false;
                        lab.path.reserve(lm.px.size());
                        for (size_t i = 0; i < lm.px.size(); i++) {
                            lv_point_t p; p.x = lm.px[i]; p.y = lm.py[i];
                            lab.path.push_back(p);
                        }
                        out.push_back(std::move(lab));
                    }
                }
            }
            // --- Major road refs: the A/N/D number, not the street name ---
            else if (isRoadName && geom == vtzero::GeomType::LINESTRING) {
                if (ref.empty()) continue;
                if (cls!="motorway"&&cls!="trunk"&&cls!="primary"&&cls!="secondary") continue;
                bool wanted = (ref[0]=='A' || ref[0]=='N');
                if (!wanted && ref[0]=='D') {        // reclassified D roads reuse the 1000-1999 range
                    try { int n=std::stoi(ref.substr(1)); wanted = (n>=1000 && n<=1999); } catch(...) {}
                }
                if (!wanted) continue;
                int minZ = (cls=="motorway"||cls=="trunk") ? 10 : (cls=="primary" ? 11 : 12);
                if (z < minZ) continue;
                int prio = (cls=="motorway"||cls=="trunk") ? 25 : (cls=="primary" ? 35 : 45);
                // Pass the road BASE color; the shield (light bg + dark text + border)
                // is built at display time.
                const RoadStyle *rd = findRoad(cls);
                uint8_t rr=0x88,rg=0x88,rb=0x88;
                if (rd) { rr=rd->r; rg=rd->g; rb=rd->b; }
                LineMid lm; lm.ts=sz;
                vtzero::decode_linestring_geometry(feat.geometry(),lm);
                if (lm.px.size()>=2){int m=(int)lm.px.size()/2; push(lm.px[m],lm.py[m],ref,prio,rtFont(&lv_font_montserrat_12),rr,rg,rb,0,false,true);}
            }
        }
    }
    s_labelCache[lk] = out;
    s_labelCacheOrder.push_back(lk);
    if (s_labelCacheOrder.size() > LABEL_CACHE_MAX) {
        s_labelCache.erase(s_labelCacheOrder.front());
        s_labelCacheOrder.pop_front();
    }
    ozReset();
}

// ---- renderTileRaw : remplit un buffer ARGB8888 (B,G,R,A) sans cache ni LVGL
// Utilisé par l'outil de debug PNG pour obtenir exactement les pixels d'une tuile.
bool renderTileRaw(uint8_t *buf, int sz, int z, int x, int y) {
    if (!s_mapped || !buf) return false;
    renderTileBuf(buf, sz, z, x, y);
    return true;
}

// ---- renderTileCached : raw buffer + render cache (for compositing) --------

bool renderTileCached(uint8_t *buf, int sz, int z, int x, int y) {
    if (!s_mapped || !buf) return false;

    uint8_t *cached = cacheGet(z, x, y);
    if (cached) {
        memcpy(buf, cached, sz * sz * 4);
        return true;
    }

    renderTileBuf(buf, sz, z, x, y);

    uint8_t *copy = (uint8_t *)lv_malloc(sz * sz * 4);
    if (copy) {
        memcpy(copy, buf, sz * sz * 4);
        cachePut(z, x, y, copy, true);
    }
    return true;
}

// ---- renderTile public (canvas version, with cache) ------------------------

bool renderTile(lv_obj_t *canvas, int z, int x, int y, int canvasSize) {
    if (!s_mapped || !canvas) return false;

    uint8_t *cached = cacheGet(z, x, y);
    if (cached) {
        lv_draw_buf_t *dbuf = lv_canvas_get_draw_buf(canvas);
        if (dbuf && dbuf->data) {
            memcpy(dbuf->data, cached, canvasSize * canvasSize * 4);
            lv_obj_invalidate(canvas);
            return true;
        }
    }

    lv_draw_buf_t *dbuf = lv_canvas_get_draw_buf(canvas);
    if (!dbuf || !dbuf->data) return false;
    renderTileBuf((uint8_t*)dbuf->data, canvasSize, z, x, y);

    uint8_t *copy = (uint8_t *)lv_malloc(canvasSize * canvasSize * 4);
    if (copy) {
        memcpy(copy, dbuf->data, canvasSize * canvasSize * 4);
        cachePut(z, x, y, copy, true);
    }
    lv_obj_invalidate(canvas);
    return true;
}

// ---- Async API -------------------------------------------------------------

void startWorker() {
    if (s_workerRunning) return;
    s_workerRunning = true;
    s_worker = std::thread(workerLoop);
}

void requestTile(int z, int x, int y) {
    // Don't queue if already cached or already in queue
    if (cacheGet(z, x, y)) return;
    {
        std::lock_guard<std::mutex> lk(s_queueMutex);
        for (auto &r : s_queue)
            if (r.z == z && r.x == x && r.y == y) return;
        s_queue.push_back({z, x, y});
    }
    s_queueCV.notify_one();
}

void stopWorker() {
    s_workerRunning = false;
    s_queueCV.notify_all();
    if (s_worker.joinable()) s_worker.join();
    // Clear queue
    {
        std::lock_guard<std::mutex> lk(s_queueMutex);
        s_queue.clear();
    }
}

}  // namespace MapVector
