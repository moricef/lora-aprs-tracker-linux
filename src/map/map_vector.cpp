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

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#include "pmtiles.hpp"
#pragma GCC diagnostic pop
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
// Place labels are sized by importance (OSM differentiates by size, one colour),
// so we keep a FreeType font per pixel size. Indexed by px (8..18).
static lv_font_t *s_placeFont[19] = {};

static const lv_font_t *rtFont(const lv_font_t *fallback) {
    if (fallback == &lv_font_montserrat_14 && s_font14) return s_font14;
    if (fallback == &lv_font_montserrat_12 && s_font12) return s_font12;
    return fallback;
}

const lv_font_t *stationLabelFont() { return s_font12; }
const lv_font_t *stationOverlayFont() { return s_font14; }

static const lv_font_t *placeFont(int px) {
    if (px < 8) px = 8; if (px > 18) px = 18;
    if (s_placeFont[px]) return s_placeFont[px];
    return s_font12 ? s_font12 : &lv_font_montserrat_12;
}

// Per-zoom label size; score splits the city band (large vs medium importance).
static int placeSizePx(const std::string &cls, int z, long score) {
    if (cls == "city") {
        if (score >= 400000) return z <= 9 ? 13 : (z == 10 ? 14 : 15);
        return z <= 8 ? 10 : (z == 9 ? 12 : (z == 10 ? 13 : (z <= 13 ? 14 : 15)));
    }
    if (cls == "town")    return z <= 10 ? 10 : (z == 11 ? 11 : (z <= 13 ? 13 : 15));
    if (cls == "suburb")  return z <= 12 ? 11 : (z == 13 ? 12 : (z <= 15 ? 14 : 15));
    if (cls == "village") return z <= 12 ? 10 : (z == 13 ? 11 : (z == 14 ? 13 : (z == 15 ? 14 : 15)));
    if (cls == "quarter") return z <= 14 ? 11 : (z == 15 ? 12 : 14);
    if (cls == "hamlet")  return z <= 14 ? 10 : (z == 15 ? 11 : 12);
    if (cls == "country") return z <= 3 ? 10 : (z == 4 ? 11 : (z <= 6 ? 12 : (z <= 8 ? 13 : (z == 9 ? 14 : 15))));
    if (cls == "state" || cls == "province")
                          return z <= 6 ? 10 : (z <= 8 ? 11 : (z == 9 ? 12 : (z <= 11 ? 13 : 15)));
    return 11;
}

// 7-inch ~169 DPI panel: lift every place label by a fixed step so the smallest
// stay legible; relative sizes unchanged.
static constexpr int kPlaceSizeBoost = 3;

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
    // Place label sizes (OSM range 10-15). Pre-created here so getTileLabels
    // (worker thread) never has to allocate a font.
    for (int px : {10, 11, 12, 13, 14, 15, 16, 17, 18})
        s_placeFont[px] = lv_freetype_font_create(found, LV_FREETYPE_FONT_RENDER_MODE_BITMAP, px, LV_FREETYPE_FONT_STYLE_NORMAL);
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
double centerLat() { return s_header.center_lat_e7 / 10000000.0; }
double centerLon() { return s_header.center_lon_e7 / 10000000.0; }

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

// Even-odd fill across every ring of one polygon feature at once: a point inside
// an odd number of rings is filled, so inner rings (islands/holes) punch through.
static void fillPolyMulti(uint8_t *buf, int w, int h,
                          const std::vector<std::vector<int>> &X,
                          const std::vector<std::vector<int>> &Y,
                          uint8_t r, uint8_t g, uint8_t b) {
    int minY = h, maxY = 0;
    for (auto &c : Y) for (int v : c) { if (v < minY) minY = v; if (v > maxY) maxY = v; }
    if (minY < 0) minY = 0;
    if (maxY >= h) maxY = h - 1;
    std::vector<int> xs;
    for (int y = minY; y <= maxY; y++) {
        xs.clear();
        for (size_t c = 0; c < X.size(); c++) {
            const auto &px = X[c]; const auto &py = Y[c];
            int n = (int)px.size();
            for (int i = 0; i < n; i++) {
                int j = (i + 1) % n;
                if ((py[i] <= y && py[j] > y) || (py[j] <= y && py[i] > y))
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
    std::vector<std::vector<int>> ringsX, ringsY;  // all rings of the current feature
    void ring_begin(uint32_t) { px.clear(); py.clear(); }
    void ring_point(vtzero::point p) { px.push_back(toPxX(p.x, sz)); py.push_back(toPxY(p.y, sz)); }
    void ring_end(vtzero::ring_type) {
        if (px.size() >= 3) { ringsX.push_back(px); ringsY.push_back(py); }
    }
    // Fill outer + inner rings together (even-odd) so islands stay land.
    void flush() {
        if (!ringsX.empty()) fillPolyMulti(buf, w, h, ringsX, ringsY, r, g, b);
        ringsX.clear(); ringsY.clear();
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
    {"wood",      7, 0xAD,0xD1,0x9E},
    {"grass",     9, 0xCE,0xEC,0xB1},
    {"farmland",  9, 0xEE,0xF0,0xD6},
    {"wetland",   9, 0xAC,0xD2,0xBF},
    {"sand",     10, 0xF5,0xE9,0xC6},
    {"rock",      7, 0xEE,0xE6,0xDD},
    {"ice",      10, 0xDE,0xED,0xED},
};
// landuse classes from landuseKeys in process.lua
static const StyleRule kLanduse[] = {
    {"residential",  7, 0xE1,0xE0,0xE0},
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
    {"parking",     13, 0xEE,0xEE,0xEE},
    {"parking_space",13,0xEE,0xEE,0xEE},
};
// park layer: only national_park and nature_reserve (process.lua)
static const StyleRule kPark[] = {
    {"national_park",  7, 0xF2,0xEF,0xE9},
    {"nature_reserve", 9, 0xF2,0xEF,0xE9},
};
// aerodrome = background (drawn before grass so lawns between runways show);
// the rest sits on top of grass.
static const StyleRule kAerowayBg[] = {
    {"aerodrome", 10, 0xE7,0xE6,0xDE},
};
static const StyleRule kAerowayFg[] = {
    {"apron",   12, 0xE7,0xE6,0xDE},
    {"runway",  12, 0xB2,0xB5,0xD1}, {"taxiway", 12, 0xB2,0xB5,0xD1},
    {"helipad", 11, 0xE7,0xE6,0xDE}, {"hangar",  12, 0xD9,0xD0,0xC9},
};
static const StyleRule kWater[] = {
    {"ocean",     6, 0xAA,0xD2,0xDF},
    {"water",     7, 0xAA,0xD2,0xDF}, {"bay",        7, 0xAA,0xD2,0xDF},
    {"river",     7, 0xAA,0xD2,0xDF}, {"canal",     10, 0xAA,0xD2,0xDF},
    {"lake",      7, 0xAA,0xD2,0xDF}, {"reservoir",  7, 0xAA,0xD2,0xDF},
    {"pond",     12, 0xAA,0xD2,0xDF}, {"basin",     11, 0xAA,0xD2,0xDF},
    {"dock",     12, 0xAA,0xD2,0xDF}, {"riverbank",  7, 0xAA,0xD2,0xDF},
};

// Road classification — colour + min zoom + line width multiplier
struct RoadStyle { const char *cls; int minZ; uint8_t r,g,b; float wMul; };
static const RoadStyle kRoads[] = {
    {"motorway",      6, 0xE8,0x92,0xA2, 3.0f},
    {"trunk",         6, 0xF9,0xB2,0x9C, 2.5f},
    {"primary",       7, 0xFC,0xD6,0xA4, 2.0f},
    {"secondary",     9, 0xF7,0xFA,0xBF, 1.5f},
    {"tertiary",     12, 0xFF,0xFF,0xFF, 1.2f},
    {"unclassified", 14, 0xFF,0xFF,0xFF, 1.0f},
    {"residential",  14, 0xFF,0xFF,0xFF, 1.0f},
    {"living_street",14, 0xED,0xED,0xED, 1.0f},
    {"pedestrian",   14, 0xDD,0xDD,0xE8, 1.0f},
    {"motorway_link",10, 0xE8,0x92,0xA2, 2.0f},
    {"trunk_link",   10, 0xF9,0xB2,0x9C, 1.8f},
    {"primary_link", 11, 0xFC,0xD6,0xA4, 1.5f},
    {"secondary_link",12,0xF7,0xFA,0xBF, 1.2f},
    {"tertiary_link",12, 0xFF,0xFF,0xFF, 1.0f},
    {"service",      15, 0xFF,0xFF,0xFF, 0.8f},
    {"track",        14, 0x99,0x66,0x00, 0.7f},
    {"construction", 13, 0xAA,0xAA,0xAA, 1.0f},
    {"path",         15, 0x88,0x88,0x88, 0.4f},
    {"footway",      16, 0x88,0x88,0x88, 0.3f},
    {"cycleway",     16, 0x88,0x88,0x88, 0.3f},
    {"steps",        17, 0x88,0x88,0x88, 0.3f},
    {"bridleway",    16, 0x88,0x88,0x88, 0.3f},
};
static const RoadStyle *findRoad(const std::string &cls) {
    if (cls.empty()) return nullptr;
    for (auto &r : kRoads) if (cls == r.cls) return &r;
    return nullptr;
}

// Per-zoom full line width in px, porté de OSM-carto roads.mss (report avant
// -> zoom suivant ; -1 = non dessiné). Toute case s'écartant de roads.mss est
// validée par l'utilisateur et marquée en commentaire inline.
static float roadWidthForZoom(const std::string &cls, int z) {
    struct WZ { const char *cls; float z[14]; }; // index 0=z6 .. 13=z19
    static const WZ T[] = {
        //                  z6   z7   z8   z9  z10  z11  z12  z13  z14  z15  z16  z17  z18  z19
        {"motorway",      {0.4f,0.8f,   1,1.4f,1.9f,   2,3.5f,   6,   6,  10,  10,  18,  21,  27}},
        {"motorway_link", {  -1,  -1,  -1,  -1,  -1,  -1,1.5f,   4,   4,7.8f,7.8f,  12,  13,  16}},
        {"trunk",         {0.4f,0.6f,   1,1.4f,1.9f,1.9f,3.5f,   6,   6,  10,  10,  18,  21,  27}},
        {"trunk_link",    {  -1,  -1,  -1,  -1,  -1,  -1,1.5f,   4,   4,7.8f,7.8f,  12,  13,  16}},
        {"primary",       {  -1,  -1,   1,1.4f,1.8f,1.8f,3.5f,   5,   5,  10,  10,  18,  21,  27}},
        {"primary_link",  {  -1,  -1,  -1,  -1,  -1,  -1,1.5f,   4,   4,7.8f,7.8f,  12,  13,  16}},
        {"secondary",     {  -1,  -1,  -1,   1,1.1f,1.1f,3.5f,   5,   5,   9,  10,  18,  21,  27}},
        {"secondary_link",{  -1,  -1,  -1,  -1,  -1,  -1,1.5f,   4,   4,   7,   7,  12,  13,  16}},
        {"tertiary",      {  -1,  -1,  -1,  -1,0.7f,0.7f,2.5f,   4,   5,   9,  10,  18,  21,  27}},
        {"tertiary_link", {  -1,  -1,  -1,  -1,  -1,  -1,1.5f,   3,   3,   7,   7,  12,  13,  16}},
        {"pedestrian",    {  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,   3,   5,   6,  12,  13,  17}},
        {"residential",   {  -1,  -1,  -1,  -1,  -1,  -1,0.5f,2.5f,   3,   5,   6,  12,  13,  17}},
        {"living_street", {  -1,  -1,  -1,  -1,  -1,  -1,  -1,   2,   3,   5,   6,  12,  13,  17}},
        {"unclassified",  {  -1,  -1,  -1,  -1,  -1,  -1,  -1,2.5f,   3,   5,   6,  12,  13,  17}},
        {"service",       {  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,   2,   2,3.5f,   7,8.5f,  11}},
        {"track",         {  -1,  -1,  -1,  -1,  -1,  -1,  -1,0.5f,0.5f,1.5f,1.5f,1.5f,1.5f,1.5f}},
        {"path",          {  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,0.7f,   1,1.3f,1.3f,1.3f,1.6f}},
        {"footway",       {  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,0.7f,   1,1.3f,1.3f,1.3f,1.6f}},
        {"cycleway",      {  -1,  -1,  -1,  -1,  -1,  -1,  -1,0.7f,0.7f,0.9f,0.9f,0.9f,   1,1.3f}},
        {"bridleway",     {  -1,  -1,  -1,  -1,  -1,  -1,  -1,0.3f,0.3f,1.2f,1.2f,1.2f,1.2f,1.2f}},
        {"steps",         {  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,0.7f,   3,   3,   3,   3,   3}},
        // rail : largeur du trait « dark » OSM ; le sur-tracé pointillé blanc n'est pas rendu (trait plein)
        {"rail",          {  -1,  -1,0.8f,0.8f,0.8f,0.8f,   2,   3,   3,   3,   3,   3,   4,   4}},
    };
    if (z < 6) z = 6;
    if (z > 19) z = 19;
    for (auto &w : T) if (cls == w.cls) return w.z[z-6];
    return 2; // défaut routes mineures non tabulées
}

// Road outline (per-side) by zoom: major = motorway/trunk/primary, plus a
// thinner ladder for secondary. Keeps the casing from over-fattening roads at
// low zoom, where a flat width reads far too wide.
static float roadCasingW(const std::string &k, int z) {
    if (k == "secondary" || k == "secondary_link") {
        if (z <= 12) return 0.3f;
        if (z <= 14) return 0.35f;
        if (z <= 16) return 0.7f;
        return 1.0f;
    }
    if (z <= 11) return 0.3f;
    if (z <= 13) return 0.5f;
    if (z == 14) return 0.6f;
    if (z <= 16) return 0.7f;
    return 1.0f;
}

// Black bridge casing: the fill is inset by this width on each side, so the
// casing showing past it equals this per-side amount. Two ladders by zoom:
// motorway/trunk/primary vs everything else.
static float bridgeCasingW(const std::string &cls, int z) {
    bool major = (cls == "motorway" || cls == "trunk" || cls == "primary");
    if (major) {
        if (z <= 13) return 0.5f;
        if (z == 14) return 0.6f;
        if (z <= 16) return 0.75f;
        return 1.0f;            // z17+
    }
    if (z <= 14) return 0.5f;
    if (z <= 16) return 0.75f;
    return 0.8f;               // z17+
}

// Waterway classification
static const StyleRule kWaterway[] = {
    {"river",   8, 0xAA,0xD2,0xDF}, {"canal", 13, 0xAA,0xD2,0xDF},
    {"stream", 13, 0xAA,0xD2,0xDF}, {"ditch", 13, 0xAA,0xD2,0xDF},
    {"drain",  13, 0xAA,0xD2,0xDF}, {"dam",   12, 0xAD,0xAD,0xAD},
    {"weir",   12, 0xAA,0xAA,0xAA},
};

// Waterway line width per zoom. river starts z8, stream/canal/ditch/drain z13;
// returns -1 below. canal = stream*1.4+0.6, stream/ditch/drain = stream+0.6.
static float waterwayWidthForZoom(const std::string &cls, int z) {
    if (cls == "river") {
        switch (z) {
            case 8: return 0.7f;  case 9: return 1.2f;  case 10: return 1.5f;
            case 11: return 1.8f; case 12: return 2.3f; case 13: return 3.0f;
            case 14: return 4.5f; case 15: return 6.0f; case 16: return 8.0f;
            case 17: return 10.0f;
            default: return z >= 18 ? 12.0f : -1.0f;
        }
    }
    if (cls == "canal") {
        switch (z) {
            case 13: return 2.6f; case 14: return 3.4f; case 15: return 4.1f;
            case 16: return 4.8f;
            default: return z >= 17 ? 5.5f : -1.0f;
        }
    }
    if (cls == "stream" || cls == "ditch" || cls == "drain") {
        switch (z) {
            case 13: return 2.0f; case 14: return 2.6f; case 15: return 3.1f;
            case 16: return 3.6f;
            default: return z >= 17 ? 4.1f : -1.0f;
        }
    }
    if (cls == "dam" || cls == "weir") return z >= 12 ? 2.0f : -1.0f;
    return -1.0f;
}
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
    {6, 8, 0x8D,0x61,0x8B, 1},
};

// Place label styling: class → {minZoom, priority, font, r, g, b}
struct PlaceStyle { const char *cls; int minZ; int prio; int sizePx; uint8_t r,g,b; };
// Place label styles: priority + size + colour. minZ=0 everywhere — zoom
// filtering is delegated to tilemaker (process-aprs.lua).
// Settlements are differentiated by SIZE, all one dark colour (#222) — the OSM
// way. Bigger place = bigger font (city 15 > town 13 > village/suburb 12 >
// hamlet/quarter 11). Priority: lower = drawn first / wins collision.
static const PlaceStyle kPlaces[] = {
    {"city",      0, 10, 15, 0x22,0x22,0x22},
    {"town",      0, 20, 13, 0x22,0x22,0x22},
    {"village",   0, 30, 12, 0x22,0x22,0x22},
    {"borough",   0, 33, 12, 0x22,0x22,0x22},
    {"suburb",    0, 35, 12, 0x22,0x22,0x22},
    {"neighbourhood",0,38,11, 0x22,0x22,0x22},
    {"hamlet",    0, 40, 11, 0x22,0x22,0x22},
    {"quarter",   0, 45, 11, 0x22,0x22,0x22},
    {"locality",  0, 45, 11, 0x22,0x22,0x22},
    {"islet",     0, 50, 11, 0x22,0x22,0x22},
    {"isolated_dwelling",0,55,11, 0x22,0x22,0x22},
    {"farm",      0, 55, 11, 0x22,0x22,0x22},
    // Admin regions: prio above every settlement so a region is placed last and
    // never displaces a city; the label collision also keeps them clear.
    {"state",     0, 60, 14, 0x55,0x44,0x43},
    {"country",   0, 62, 15, 0x33,0x22,0x21},
};

// ---- Path drawing with width ------------------------------------------------
static void drawWideLine(uint8_t *buf, int w, int h, int x0, int y0, int x1, int y1,
                         float width, uint8_t r, uint8_t g, uint8_t b) {
    if (width <= 1) { drawLine(buf,w,h,x0,y0,x1,y1,r,g,b); return; }
    int dx = abs(x1-x0), sx = x0<x1 ? 1 : -1;
    int dy = -abs(y1-y0), sy = y0<y1 ? 1 : -1;
    int err = dx+dy;
    int rad = (int)(width / 2.0f + 0.5f);
    while (true) {
        for (int dy2 = -rad; dy2 <= rad; dy2++)
            for (int dx2 = -rad; dx2 <= rad; dx2++)
                if (dx2*dx2 + dy2*dy2 <= rad*rad)
                    { int px=x0+dx2, py=y0+dy2; if(px>=0&&px<w&&py>=0&&py<h)setPx(buf,w*4,px,py,r,g,b); }
        if (x0==x1 && y0==y1) break;
        int e2=2*err; if (e2>=dy) { err+=dy; x0+=sx; } if (e2<=dx) { err+=dx; y0+=sy; }
    }
}

// Thick polyline with flat (butt) caps at the two free ends and round joins at
// interior vertices. Used as a casing/fill underlay where round caps would poke a
// halo past the span ends — the flat end is what makes a bridge read as a bridge.
static void drawThickPolyButt(uint8_t *buf, int w, int h,
                              const std::vector<int> &px, const std::vector<int> &py,
                              float width, uint8_t r, uint8_t g, uint8_t b,
                              float endTrim = 0, bool joinDiscs = true) {
    float hw = width * 0.5f;
    int stride = w * 4;
    int last = (int)px.size() - 1;
    for (size_t i = 1; i < px.size(); i++) {
        float ax = px[i-1], ay = py[i-1], bx = px[i], by = py[i];
        float ddx = bx-ax, ddy = by-ay, L = sqrtf(ddx*ddx + ddy*ddy);
        if (L < 0.01f) continue;
        float ux = ddx/L, uy = ddy/L;     // along segment
        // Recess only the two free ends of the polyline, so the (untrimmed)
        // fill drawn over this casing reaches flush and leaves no black tab
        // across the end; interior segment ends are bridged by the join discs.
        float t0 = (i == 1) ? endTrim : 0.0f;
        float t1 = ((int)i == last) ? endTrim : 0.0f;
        int minx = (int)floorf(fminf(ax,bx)-hw-1), maxx = (int)ceilf(fmaxf(ax,bx)+hw+1);
        int miny = (int)floorf(fminf(ay,by)-hw-1), maxy = (int)ceilf(fmaxf(ay,by)+hw+1);
        if (minx<0) minx=0; if (miny<0) miny=0; if (maxx>w-1) maxx=w-1; if (maxy>h-1) maxy=h-1;
        for (int y = miny; y <= maxy; y++)
            for (int x = minx; x <= maxx; x++) {
                float rx = x-ax, ry = y-ay;
                float t = rx*ux + ry*uy;
                if (t < t0 || t > L - t1) continue;    // butt cap, ends recessed
                float d = rx*(-uy) + ry*ux;
                if (d < -hw || d > hw) continue;
                setPx(buf, stride, x, y, r, g, b);
            }
    }
    if (!joinDiscs) return;
    int rad = (int)(hw + 0.5f);
    for (size_t i = 1; i + 1 < px.size(); i++)         // round joins, not endpoints
        for (int dy = -rad; dy <= rad; dy++)
            for (int dx = -rad; dx <= rad; dx++)
                if (dx*dx + dy*dy <= rad*rad) {
                    int X = px[i]+dx, Y = py[i]+dy;
                    if (X>=0 && X<w && Y>=0 && Y<h) setPx(buf, stride, X, Y, r, g, b);
                }
}

// Thick polyline with butt (square) ends and bevel joins. Each segment is filled
// as its own rectangle and each interior corner as a small triangle, so nothing
// relies on a single self-intersecting polygon (which carved triangle holes at
// bends under even-odd fill). Overlaps overdraw the same colour, harmless.
static void drawThickPolyMiter(uint8_t *buf, int w, int h,
                               const std::vector<int> &px, const std::vector<int> &py,
                               float width, uint8_t r, uint8_t g, uint8_t b) {
    int n = (int)px.size();
    if (n < 2) return;
    float hw = width * 0.5f;
    auto leftNormal = [&](int i, float &nx, float &ny) {   // left normal of segment i -> i+1, scaled to hw
        float dx = px[i+1]-px[i], dy = py[i+1]-py[i];
        float L = sqrtf(dx*dx+dy*dy); if (L < 1e-4f) L = 1e-4f;
        nx = -dy/L * hw; ny = dx/L * hw;
    };
    for (int i = 0; i + 1 < n; i++) {                      // one rectangle per segment
        float ox, oy; leftNormal(i, ox, oy);
        std::vector<int> qx = { (int)lroundf(px[i]+ox), (int)lroundf(px[i+1]+ox),
                                (int)lroundf(px[i+1]-ox), (int)lroundf(px[i]-ox) };
        std::vector<int> qy = { (int)lroundf(py[i]+oy), (int)lroundf(py[i+1]+oy),
                                (int)lroundf(py[i+1]-oy), (int)lroundf(py[i]-oy) };
        fillPoly(buf, w, h, qx, qy, r, g, b);
    }
    for (int i = 1; i + 1 < n; i++) {                      // bevel corner at interior vertices
        float ax, ay, bx, by; leftNormal(i-1, ax, ay); leftNormal(i, bx, by);
        int vx = px[i], vy = py[i];
        std::vector<int> t1x = { vx, (int)lroundf(vx+ax), (int)lroundf(vx+bx) };
        std::vector<int> t1y = { vy, (int)lroundf(vy+ay), (int)lroundf(vy+by) };
        fillPoly(buf, w, h, t1x, t1y, r, g, b);
        std::vector<int> t2x = { vx, (int)lroundf(vx-ax), (int)lroundf(vx-bx) };
        std::vector<int> t2y = { vy, (int)lroundf(vy-ay), (int)lroundf(vy-by) };
        fillPoly(buf, w, h, t2x, t2y, r, g, b);
    }
}

// ---- Line collector with per-feature class lookup ----------------------------
struct StyledLineCollector {
    std::vector<int> px, py;
    int sz, w, h; float width;
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
    int scale = 1;  // ×1 normal, ×2 SSAA — so line widths match buffer resolution
    float dashOn = 0, dashGap = 0;  // dash pattern in px (pre-scale); 0 = solid
    bool buttCap = false;           // flat ends + round joins (bridge casing/fill underlay)
    float buttEndTrim = 0;          // recess casing ends so the fill leaves no black tab
    bool buttJoinDiscs = true;      // round-join discs (off for casing: would blob the ends)
    bool miterJoin = false;         // butt ends + miter joins (filled polygon) instead of discs
    void linestring_end() {
        float lw = width * scale;
        if (buttCap && dashGap <= 0) {
            if (miterJoin) drawThickPolyMiter(buf,w,h,px,py,lw,r,g,b);
            else           drawThickPolyButt(buf,w,h,px,py,lw,r,g,b,buttEndTrim*scale,buttJoinDiscs);
            return;
        }
        if (dashGap <= 0) {
            for (size_t i = 1; i < px.size(); i++)
                drawWideLine(buf,w,h,px[i-1],py[i-1],px[i],py[i],lw,r,g,b);
            return;
        }
        // Dashed: walk the polyline by arc length, drawing only the "on" runs.
        // Phase is continuous across segments so dashes don't restart at vertices.
        // double accumulation is required: with a small dash, a float pos/phase
        // can stop advancing near a cycle boundary (step < ULP) and spin forever.
        double on = dashOn * scale, gap = dashGap * scale, cycle = on + gap;
        double phase = 0;
        for (size_t i = 1; i < px.size(); i++) {
            double ax = px[i-1], ay = py[i-1];
            double ddx = px[i]-ax, ddy = py[i]-ay;
            double seg = sqrt(ddx*ddx + ddy*ddy);
            if (seg < 0.5) continue;
            double ux = ddx/seg, uy = ddy/seg, pos = 0;
            while (pos < seg) {
                double inCycle = fmod(phase, cycle);
                if (inCycle < on) {
                    double dl = fmin(on - inCycle, seg - pos);
                    drawWideLine(buf,w,h, (int)(ax+ux*pos),(int)(ay+uy*pos),
                                 (int)(ax+ux*(pos+dl)),(int)(ay+uy*(pos+dl)), lw,r,g,b);
                    pos += dl; phase += dl;
                } else {
                    double sk = fmin(cycle - inCycle, seg - pos);
                    pos += sk; phase += sk;
                }
            }
        }
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
    int wScale = sz / TILE_SIZE;  // 1=normal, 2=SSAA — line widths must match buffer resolution

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
        if (cls == "path")  return sub.empty() ? std::string("path") : sub;
        return cls;
    };
    // Lit class, subclass et ramp (les bretelles = ramp=1, repliées dans la classe
    // parente). Pour une bretelle on prend la largeur du variant _link (plus fin).
    auto readRoad = [](vtzero::feature &f, std::string &cls, std::string &sub,
                       bool &ramp, bool &bridge, int &layer) {
        cls.clear(); sub.clear(); ramp = false; bridge = false; layer = 0;
        while (auto p = f.next_property()) {
            auto t = p.value().type();
            if (p.key() == "ramp") { ramp = true; continue; }
            if (p.key() == "layer") {
                if (t == vtzero::property_value_type::int_value)       layer = (int)p.value().int_value();
                else if (t == vtzero::property_value_type::sint_value) layer = (int)p.value().sint_value();
                else if (t == vtzero::property_value_type::uint_value) layer = (int)p.value().uint_value();
                continue;
            }
            if (t != vtzero::property_value_type::string_value) continue;
            auto v = p.value().string_value();
            if (p.key() == "class")    cls.assign(v.data(), v.size());
            else if (p.key() == "subclass") sub.assign(v.data(), v.size());
            else if (p.key() == "brunnel" && std::string(v.data(), v.size()) == "bridge")
                bridge = true;
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
                pc.flush();
            }
        }
    };
    // Render order optimised for tracker readability:
    // farmland/grass background → urban areas visible → forests on top
    static const StyleRule kLandcoverBg[] = {
        {"farmland", 9, 0xEE,0xF0,0xD6},
        {"grass",    9, 0xCE,0xEC,0xB1},
        {"sand",    10, 0xF5,0xE9,0xC6},
        {"rock",     7, 0xEE,0xE6,0xDD},
        {"ice",     10, 0xDE,0xED,0xED},
    };
    static const StyleRule kLandcoverFg[] = {
        {"wood",    7, 0xAD,0xD1,0x9E},
        {"wetland", 9, 0xAC,0xD2,0xBF},
    };
    // Draw order matters: a park / hippodrome lawn is often INSIDE a larger
    // landuse=residential polygon. If landuse painted after landcover, the
    // residential gray hid the inner grass. Paint landuse FIRST (urban
    // background), then landcover (grass / farmland) ON TOP.
    renderPolyLayer("park",      kPark,         STYLE_LEN(kPark));       // national_park, nature_reserve
    renderPolyLayer("landuse",   kLanduse,      STYLE_LEN(kLanduse));     // residential / commercial / industrial background
    renderPolyLayer("aeroway",   kAerowayBg,    STYLE_LEN(kAerowayBg));    // aerodrome fond, AVANT l'herbe
    renderPolyLayer("landcover", kLandcoverBg,  STYLE_LEN(kLandcoverBg)); // grass / farmland on top of urban
    renderPolyLayer("aeroway",   kAerowayFg,    STYLE_LEN(kAerowayFg));    // pistes/taxiways/apron au-dessus de l'herbe
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
                        if (prop.value().type() == vtzero::property_value_type::int_value)
                            admin_level = (int)prop.value().int_value();
                        else if (prop.value().type() == vtzero::property_value_type::sint_value)
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
                lc.scale = wScale;
                int bw = as->width;
                // Thinner country border at low zoom (avoids a z7 border as fat
                // as z8).
                if (as->level == 2 && z <= 7) bw = 1;
                lc.r=as->r; lc.g=as->g; lc.b=as->b; lc.width=bw;
                lc.zoom=z; lc.table=nullptr; lc.tableLen=0; lc.roadTable=nullptr;
                vtzero::decode_linestring_geometry(feat.geometry(), lc);
            }
        }
    }

    // Pass 3: waterway lines
    {
        StyledLineCollector lc;
        lc.scale = wScale; lc.sz=sz; lc.buf=buf; lc.w=w; lc.h=h; lc.zoom=z; lc.width=1;
        lc.setTable(kWaterway, STYLE_LEN(kWaterway));
        vtzero::vector_tile t{mvt};
        while (auto lay = t.next_layer()) {
            if (std::string(lay.name()) != "waterway") continue;
            while (auto feat = lay.next_feature()) {
                if (feat.geometry_type() != vtzero::GeomType::LINESTRING) continue;
                std::string cls = readClass(feat);
                if (cls.empty()) continue;
                float ww = waterwayWidthForZoom(cls, z);
                if (ww < 0) continue;
                lc.r=lc.g=lc.b=0; lc.applyClass(cls);
                if (lc.r==0 && lc.g==0 && lc.b==0) continue;
                lc.width = ww;
                vtzero::decode_linestring_geometry(feat.geometry(), lc);
            }
        }
    }

    // Pass 4: railway lines
    {
        StyledLineCollector lc;
        lc.scale = wScale; lc.sz=sz; lc.buf=buf; lc.w=w; lc.h=h; lc.zoom=z; lc.width=1;
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
        lc.scale = wScale; lc.sz=sz; lc.buf=buf; lc.w=w; lc.h=h; lc.zoom=z; lc.width=1;
        lc.setRoads();
        // Outline pass (liseré fin, fill+2) — uniquement routes MAJEURES
        // (motorway/trunk/primary/secondary, wMul>=1.5). Tertiary et en dessous
        // n'ont pas de liseré (sinon ça surépaissit residential/living_street).
        vtzero::vector_tile t1{mvt};
        while (auto lay = t1.next_layer()) {
            if (std::string(lay.name()) != "transportation") continue;
            while (auto feat = lay.next_feature()) {
                if (feat.geometry_type() != vtzero::GeomType::LINESTRING) continue;
                std::string cls, sub; bool ramp, bridge; int lyr; readRoad(feat, cls, sub, ramp, bridge, lyr);
                if (bridge && z >= 13) continue; // bridges handled by the dedicated pass below
                std::string key = roadKey(cls, sub);
                if (key.empty()) continue;
                auto *rd = findRoad(key);
                if (!rd) continue;
                if (rd->wMul < 1.5f) continue;   // pas de liseré sur tertiary et en dessous
                float fw = roadWidthForZoom(widthKey(key, ramp), z);
                if (fw < 3) continue;            // ni si trop fine à ce zoom
                // Liseré = couleur de la voie légèrement assombrie (×0.85, même teinte),
                // pas du noir — discret sur le 7" 1024x600.
                lc.r=(uint8_t)(rd->r*0.85f); lc.g=(uint8_t)(rd->g*0.85f); lc.b=(uint8_t)(rd->b*0.85f);
                lc.width = fw + 2.0f*roadCasingW(key, z);
                vtzero::decode_linestring_geometry(feat.geometry(), lc);
            }
        }
        // Fill pass (couleur de route, largeur de la table)
        vtzero::vector_tile t2{mvt};
        while (auto lay = t2.next_layer()) {
            if (std::string(lay.name()) != "transportation") continue;
            while (auto feat = lay.next_feature()) {
                if (feat.geometry_type() != vtzero::GeomType::LINESTRING) continue;
                std::string cls, sub; bool ramp, bridge; int lyr; readRoad(feat, cls, sub, ramp, bridge, lyr);
                if (bridge && z >= 13) continue; // bridges handled by the dedicated pass below
                std::string key = roadKey(cls, sub);
                if (key.empty()) continue;       // chemins non-pedestrian non rendus
                auto *rd = findRoad(key);
                if (!rd) continue;
                float fw = roadWidthForZoom(widthKey(key, ramp), z);
                if (fw < 0) continue;            // route pas visible à ce zoom
                lc.r=rd->r; lc.g=rd->g; lc.b=rd->b;
                lc.width = fw;
                // Tracks are dashed (4 on / 2 gap); everything else solid.
                bool dashed = (key == "track");
                lc.dashOn = dashed ? 4.0f : 0.0f;
                lc.dashGap = dashed ? 2.0f : 0.0f;
                vtzero::decode_linestring_geometry(feat.geometry(), lc);
            }
        }

        // Bridges as a dedicated layer on top: all black butt-cap casings, then
        // all butt-cap fills. Drawing every casing before any fill stops a span
        // from streaking black across the fill of the span it crosses.
        if (z >= 13) {
            lc.buttCap = true; lc.dashOn = lc.dashGap = 0;
            // Distinct bridge layers, ascending: a higher deck (e.g. a tertiary
            // crossing a motorway) must be drawn last so it spans on top intact.
            std::vector<int> layers;
            { vtzero::vector_tile ts{mvt};
              while (auto lay = ts.next_layer()) {
                  if (std::string(lay.name()) != "transportation") continue;
                  while (auto feat = lay.next_feature()) {
                      if (feat.geometry_type() != vtzero::GeomType::LINESTRING) continue;
                      std::string cls, sub; bool ramp, bridge; int lyr; readRoad(feat, cls, sub, ramp, bridge, lyr);
                      if (bridge) layers.push_back(lyr);
                  }
              }
            }
            std::sort(layers.begin(), layers.end());
            layers.erase(std::unique(layers.begin(), layers.end()), layers.end());
            for (int pass = 0; pass < 2; pass++)
              for (int L : layers) {
                vtzero::vector_tile tb{mvt};
                while (auto lay = tb.next_layer()) {
                    if (std::string(lay.name()) != "transportation") continue;
                    while (auto feat = lay.next_feature()) {
                        if (feat.geometry_type() != vtzero::GeomType::LINESTRING) continue;
                        std::string cls, sub; bool ramp, bridge; int lyr; readRoad(feat, cls, sub, ramp, bridge, lyr);
                        if (!bridge || lyr != L) continue;
                        std::string key = roadKey(cls, sub);
                        if (key.empty()) continue;
                        auto *rd = findRoad(key);
                        if (!rd) continue;
                        float fw = roadWidthForZoom(widthKey(key, ramp), z);
                        if (fw < 0) continue;
                        if (pass == 0) { lc.r=lc.g=lc.b=0; lc.width = fw + 2.0f*bridgeCasingW(cls, z); lc.buttEndTrim = bridgeCasingW(cls, z); lc.buttJoinDiscs = false; }
                        else           { lc.r=rd->r; lc.g=rd->g; lc.b=rd->b; lc.width = fw; lc.buttEndTrim = 0; lc.buttJoinDiscs = true; }
                        vtzero::decode_linestring_geometry(feat.geometry(), lc);
                    }
                }
              }
            lc.buttCap = false;
        }

        // Aeroway lines (runways/taxiways) drawn AFTER roads so service/minor roads
        // don't paint over them. Centrelines sized to true ground width (constant
        // grey aerodrome margin at every zoom); square ends + bevel joins.
        if (z >= 12) {
            StyledLineCollector la;
            la.scale = wScale; la.sz=sz; la.buf=buf; la.w=w; la.h=h; la.zoom=z;
            la.table=nullptr; la.tableLen=0; la.roadTable=nullptr;
            la.r=0xB2; la.g=0xB5; la.b=0xD1;   // #b2b5d1
            la.buttCap = true; la.miterJoin = true;
            double mPerPx = 113369.0 / (double)(1 << z);   // metres/px, lat ~43.6
            vtzero::vector_tile ta2{mvt};
            while (auto lay = ta2.next_layer()) {
                if (std::string(lay.name()) != "aeroway") continue;
                while (auto feat = lay.next_feature()) {
                    if (feat.geometry_type() != vtzero::GeomType::LINESTRING) continue;
                    std::string cls = readClass(feat);
                    if (cls == "runway")       la.width = (float)(45.0 / mPerPx);
                    else if (cls == "taxiway") la.width = (float)(23.0 / mPerPx);
                    else continue;
                    vtzero::decode_linestring_geometry(feat.geometry(), la);
                }
            }
        }

        // Aerialways on top of roads/bridges (aerialways.mss, z>=12) : grey base
        // line + sparse black dash = pylon ticks. Drawn last so cable cars read
        // as crossing above the road network.
        if (z >= 12) {
            lc.buttCap = false;
            vtzero::vector_tile ta{mvt};
            while (auto lay = ta.next_layer()) {
                if (std::string(lay.name()) != "transportation") continue;
                while (auto feat = lay.next_feature()) {
                    if (feat.geometry_type() != vtzero::GeomType::LINESTRING) continue;
                    if (readClass(feat) != "aerialway") continue;
                    lc.r=0x80; lc.g=0x80; lc.b=0x80; lc.width=1; lc.dashOn=0; lc.dashGap=0;
                    vtzero::decode_linestring_geometry(feat.geometry(), lc);
                    lc.r=lc.g=lc.b=0; lc.width=3; lc.dashOn=0.4f; lc.dashGap=13.0f;
                    vtzero::decode_linestring_geometry(feat.geometry(), lc);
                }
            }
            lc.dashOn=0; lc.dashGap=0;
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
            long score = 0;
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
                } else if (prop.key() == "score") {
                    if (pt == vtzero::property_value_type::int_value)       score = (long)prop.value().int_value();
                    else if (pt == vtzero::property_value_type::sint_value) score = (long)prop.value().sint_value();
                    else if (pt == vtzero::property_value_type::uint_value) score = (long)prop.value().uint_value();
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
                        if(px<minX)minX=px;
                        if(px>maxX)maxX=px;
                        if(py<minY)minY=py;
                        if(py>maxY)maxY=py;
                        n++;}
                    void ring_begin(uint32_t){} void ring_point(vtzero::point p){add(p);} void ring_end(vtzero::ring_type){}
                    void points_begin(uint32_t){} void points_point(vtzero::point p){add(p);} void points_end(){}
                    void linestring_begin(uint32_t){}void linestring_point(vtzero::point){}void linestring_end(){}
                } lp; lp.ts=sz;
                if (geom == vtzero::GeomType::POLYGON) vtzero::decode_polygon_geometry(feat.geometry(),lp);
                else vtzero::decode_point_geometry(feat.geometry(),lp);
                if (lp.n>0) {
                    int cx=(lp.minX+lp.maxX)/2, cy=(lp.minY+lp.maxY)/2;
                    if (isPlace) {
                        long sc = score > 0 ? score : (long)pop;
                        for (auto &ps : kPlaces) if (cls==ps.cls && z>=ps.minZ) {
                            bool region = (cls=="state" || cls=="country");
                            // OSM: font grows with zoom; collision tiebreak by score.
                            push(cx,cy,labelText,ps.prio,placeFont(placeSizePx(cls,z,sc)+kPlaceSizeBoost),ps.r,ps.g,ps.b,0,false,false,(int)sc,region); break; }
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
            // z14+ : all D roads + tertiary get a shield; below z14 only A/N/D1000-1999
            // on motorway/trunk/primary/secondary.
            else if (isRoadName && geom == vtzero::GeomType::LINESTRING) {
                if (ref.empty()) continue;
                // Ref shields per class, at the OSM-carto shield zooms. No shield
                // for unclassified/residential/etc — OSM doesn't ref those.
                int minZ;
                if      (cls=="motorway")                minZ = 10;
                else if (cls=="trunk" || cls=="primary") minZ = 11;
                else if (cls=="secondary")               minZ = 12;
                else if (cls=="tertiary")                minZ = 13;
                else continue;
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
