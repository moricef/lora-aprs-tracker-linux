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
#include <deque>
#include <mutex>
#include <thread>
#include <condition_variable>

#include "pmtiles.hpp"
#include "vtzero/vector_tile.hpp"
#include "vtzero/geometry.hpp"

extern const lv_font_t lv_font_montserrat_12;
extern const lv_font_t lv_font_montserrat_14;

namespace MapVector {

#define EXTENT    4096
#define TILE_SIZE 256

// ---- Glyph rendering from LVGL fonts (worker-thread safe) ------------------
static int textWidth(const char *text, const lv_font_t *font) {
    int w = 0;
    while (*text) {
        lv_font_glyph_dsc_t g;
        if (lv_font_get_glyph_dsc(font, &g, (uint8_t)*text, (uint8_t)*(text+1)))
            w += g.adv_w;
        text++;
    }
    return w;
}

static void drawTextLabel(uint8_t *buf, int bufW, int bufH,
                          int x, int y, const char *text,
                          const lv_font_t *font, uint8_t r, uint8_t g, uint8_t b) {
    lv_draw_buf_t tmpDrawBuf{};
    int penX = x;
    while (*text) {
        uint8_t c = (uint8_t)*text;
        uint8_t cn = (uint8_t)*(text + 1);
        lv_font_glyph_dsc_t gd;
        if (!lv_font_get_glyph_dsc(font, &gd, c, cn)) { text++; continue; }
        const uint8_t *src = (const uint8_t *)lv_font_get_glyph_bitmap(&gd, &tmpDrawBuf);
        if (!src) { text++; penX += gd.adv_w; continue; }
        int gx = penX + gd.ofs_x;
        int gy = y + gd.ofs_y;
        int stride = gd.stride ? (int)gd.stride : (int)gd.box_w;
        for (int row = 0; row < (int)gd.box_h; row++) {
            for (int col = 0; col < (int)gd.box_w; col++) {
                int px = gx + col, py = gy + row;
                if (px < 0 || px >= bufW || py < 0 || py >= bufH) continue;
                uint8_t alpha = 0;
                switch (gd.format) {
                case LV_FONT_GLYPH_FORMAT_A8:
                    alpha = src[row * stride + col]; break;
                case LV_FONT_GLYPH_FORMAT_A4: {
                    uint8_t v = src[row * stride + col / 2];
                    alpha = (col & 1) ? (v & 0x0F) << 4 : (v & 0xF0);
                    break;
                }
                case LV_FONT_GLYPH_FORMAT_A2: {
                    uint8_t v = src[row * stride + col / 4];
                    alpha = ((v >> (6 - (col & 3) * 2)) & 0x03) * 0x55;
                    break;
                }
                case LV_FONT_GLYPH_FORMAT_A1:
                    if ((src[row * stride + col / 8] >> (7 - (col & 7))) & 1)
                        alpha = 0xFF;
                    break;
                default: continue;
                }
                if (alpha == 0) continue;
                uint8_t *p = buf + (py * bufW + px) * 4;
                p[0] = ((int)b * alpha + (int)p[0] * (255 - alpha)) / 255;
                p[1] = ((int)g * alpha + (int)p[1] * (255 - alpha)) / 255;
                p[2] = ((int)r * alpha + (int)p[2] * (255 - alpha)) / 255;
            }
        }
        penX += gd.adv_w;
        text++;
    }
}

// ---- Label placement (collision avoidance) ---------------------------------
struct LabelCandidate {
    int x, y, w, h, priority;
    const char *text;
    uint8_t r, g, b;
    const lv_font_t *font;
};

static std::vector<LabelCandidate> s_labels;

static void addLabel(int px, int py, const char *text, int priority,
                     const lv_font_t *font, uint8_t r, uint8_t g, uint8_t b) {
    if (!text || !text[0]) return;
    int tw = textWidth(text, font);
    if (tw <= 0 || tw > TILE_SIZE - 4) return;
    int fh = font->line_height > 0 ? font->line_height : font->base_line;
    s_labels.push_back({px - tw/2, py - fh/2, tw, fh, priority, text, r, g, b, font});
}

static void placeLabels(uint8_t *buf, int sz) {
    struct Placed { int x, y, w, h; };
    std::vector<Placed> placed;
    std::sort(s_labels.begin(), s_labels.end(),
              [](const LabelCandidate &a, const LabelCandidate &b) {
                  return a.priority < b.priority;
              });
    for (auto &l : s_labels) {
        int lx = l.x, ly = l.y;
        if (lx < 1) lx = 1;
        if (ly < 1) ly = 1;
        if (lx + l.w > sz - 1) lx = sz - 1 - l.w;
        if (ly + l.h > sz - 1) ly = sz - 1 - l.h;
        bool overlap = false;
        for (auto &p : placed) {
            if (lx + l.w + 2 <= p.x || p.x + p.w + 2 <= lx ||
                ly + l.h + 2 <= p.y || p.y + p.h + 2 <= ly) continue;
            overlap = true; break;
        }
        if (overlap) continue;
        drawTextLabel(buf, sz, sz, lx, ly + l.h, l.text, l.font, l.r, l.g, l.b);
        placed.push_back({lx, ly, l.w, l.h});
    }
    s_labels.clear();
}

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

        // Check if any pixel was actually drawn (not just background)
        bool hasData = false;
        for (int i = 0; i < sz * sz; i++) {
            uint8_t *p = tmp + i * 4;
            if (p[0] != 0xD6 || p[1] != 0xEA || p[2] != 0xF0) { hasData = true; break; }
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

// VTzero → pixel coords
static inline int toPx(int v, int sz) { return (int)((int64_t)v * sz / EXTENT); }

// ---- Collecteurs geometrie -----------------------------------------------

struct LineCollector {
    std::vector<int> px, py;
    int sz;
    uint8_t r, g, b;
    uint8_t *buf;
    int w, h;
    void linestring_begin(uint32_t) { px.clear(); py.clear(); }
    void linestring_point(vtzero::point p) { px.push_back(toPx(p.x, sz)); py.push_back(toPx(p.y, sz)); }
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
    void ring_point(vtzero::point p) { px.push_back(toPx(p.x, sz)); py.push_back(toPx(p.y, sz)); }
    void ring_end(vtzero::ring_type rt) {
        if (rt == vtzero::ring_type::outer && px.size() >= 3)
            fillPoly(buf, w, h, px, py, r, g, b);
    }
    void points_begin(uint32_t) {} void points_point(vtzero::point) {} void points_end() {}
    void linestring_begin(uint32_t) {} void linestring_point(vtzero::point) {} void linestring_end() {}
};

// ---- renderTileBuf (core — renders to any ARGB8888 buffer) ----------------
static void renderTileBuf(uint8_t *buf, int sz, int z, int x, int y) {
    int w = sz, h = sz;

    // Beige background
    for (int i = 0; i < w * h; i++) {
        uint8_t *p = buf + i * 4;
        p[0] = 0xD6; p[1] = 0xEA; p[2] = 0xF0; p[3] = 0xFF;
    }

    auto [off, len] = pmtiles::get_tile(gunzip, (const char*)s_mapped, z, x, y);
    if (len == 0) return;

    std::string raw((const char*)s_mapped + off, len);
    std::string mvt = (s_header.tile_compression == pmtiles::COMPRESSION_GZIP)
                      ? gunzip(raw, 0) : raw;
    if (mvt.empty()) return;

    auto layerName = [](vtzero::layer &l) { return std::string(l.name()); };

    vtzero::vector_tile t1{mvt};
    while (auto lay = t1.next_layer()) {
        std::string name = layerName(lay);
        PolyCollector pc; pc.sz = sz; pc.buf = buf; pc.w = w; pc.h = h;
        if      (name == "landcover") { pc.r = 0xCD; pc.g = 0xE0; pc.b = 0xB3; }
        else if (name == "landuse")   { pc.r = 0xE8; pc.g = 0xD8; pc.b = 0xC0; }
        else if (name == "water")     { pc.r = 0x6E; pc.g = 0xA8; pc.b = 0xE0; }
        else continue;
        while (auto feat = lay.next_feature())
            if (feat.geometry_type() == vtzero::GeomType::POLYGON)
                vtzero::decode_polygon_geometry(feat.geometry(), pc);
    }

    vtzero::vector_tile t2{mvt};
    while (auto lay = t2.next_layer()) {
        std::string name = layerName(lay);
        LineCollector lc; lc.sz = sz; lc.buf = buf; lc.w = w; lc.h = h;
        if      (name == "waterway")       { lc.r = 0x4A; lc.g = 0x7A; lc.b = 0xB0; }
        else if (name == "transportation") { lc.r = 0x40; lc.g = 0x40; lc.b = 0x40; }
        else continue;
        while (auto feat = lay.next_feature())
            if (feat.geometry_type() == vtzero::GeomType::LINESTRING)
                vtzero::decode_linestring_geometry(feat.geometry(), lc);
    }

    // ---- Pass 3: collecter les labels (place, waterway, water) ----------------
    vtzero::vector_tile t3{mvt};
    while (auto lay = t3.next_layer()) {
        std::string name = layerName(lay);
        bool isPlace = (name == "place");
        bool isWater = (name == "water");
        bool isWaterway = (name == "waterway");
        if (!isPlace && !isWater && !isWaterway) continue;

        while (auto feat = lay.next_feature()) {
            // Read feature name
            const char *labelText = nullptr;
            while (auto prop = feat.next_property()) {
                if (prop.key() == "name" && prop.value().type() == vtzero::property_value_type::string_value) {
                    labelText = prop.value().string_value().data();
                    break;
                }
            }
            if (!labelText) continue;

            auto geom = feat.geometry_type();
            if (geom == vtzero::GeomType::POLYGON || geom == vtzero::GeomType::POINT) {
                // Centroid from first ring bounding box
                struct LabelPos {
                    int minX = 0, minY = 0, maxX = 0, maxY = 0;
                    int ptCount = 0;
                    void ring_begin(uint32_t) { minX = minY = 99999; maxX = maxY = -99999; ptCount = 0; }
                    void ring_point(vtzero::point p) {
                        int px = toPx(p.x, tileSz), py = toPx(p.y, tileSz);
                        if (px < minX) minX = px; if (px > maxX) maxX = px;
                        if (py < minY) minY = py; if (py > maxY) maxY = py;
                        ptCount++;
                    }
                    void ring_end(vtzero::ring_type) {}
                    void points_begin(uint32_t) {} void points_point(vtzero::point) {} void points_end() {}
                    void linestring_begin(uint32_t) {} void linestring_point(vtzero::point) {} void linestring_end() {}
                    int tileSz;
                } lp;
                lp.tileSz = sz;
                if (geom == vtzero::GeomType::POLYGON)
                    vtzero::decode_polygon_geometry(feat.geometry(), lp);
                else
                    vtzero::decode_point_geometry(feat.geometry(), lp);

                if (lp.ptCount > 0) {
                    int cx = (lp.minX + lp.maxX) / 2;
                    int cy = (lp.minY + lp.maxY) / 2;
                    if (cx > 4 && cx < sz - 4 && cy > 4 && cy < sz - 4) {
                        if (isPlace) {
                            // Priority by place type
                            int prio = 50; // default
                            feat.reset_property();
                            while (auto prop = feat.next_property()) {
                                if (prop.key() != "class") continue;
                                auto v = std::string(prop.value().string_value());
                                if (v == "city") prio = 10;
                                else if (v == "town") prio = 20;
                                else if (v == "village") prio = 30;
                                else if (v == "suburb" || v == "hamlet") prio = 40;
                                break;
                            }
                            addLabel(cx, cy, labelText, prio,
                                     &lv_font_montserrat_14, 0x33, 0x22, 0x21);
                        } else if (isWater) {
                            addLabel(cx, cy, labelText, 70,
                                     &lv_font_montserrat_12, 0x4A, 0x7A, 0xB0);
                        }
                    }
                }
            } else if (geom == vtzero::GeomType::LINESTRING && isWaterway) {
                // Linestring midpoint
                struct LineMid {
                    int tileSz;
                    std::vector<int> px, py;
                    void linestring_begin(uint32_t) { px.clear(); py.clear(); }
                    void linestring_point(vtzero::point p) { px.push_back(toPx(p.x, tileSz)); py.push_back(toPx(p.y, tileSz)); }
                    void linestring_end() {}
                    void points_begin(uint32_t) {} void points_point(vtzero::point) {} void points_end() {}
                    void ring_begin(uint32_t) {} void ring_point(vtzero::point) {} void ring_end(vtzero::ring_type) {}
                } lm;
                lm.tileSz = sz;
                vtzero::decode_linestring_geometry(feat.geometry(), lm);
                if (lm.px.size() >= 2) {
                    int mid = (int)lm.px.size() / 2;
                    int cx = lm.px[mid], cy = lm.py[mid];
                    if (cx > 4 && cx < sz - 4 && cy > 4 && cy < sz - 4)
                        addLabel(cx, cy, labelText, 60,
                                 &lv_font_montserrat_12, 0x4A, 0x7A, 0xB0);
                }
            }
        }
    }

    placeLabels(buf, sz);
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
