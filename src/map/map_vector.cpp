/* map_vector.cpp — rendu vectoriel PMTiles + VTzero sur lv_canvas
 * Décodage MVT puis dessin via lv_canvas_draw_line / lv_canvas_draw_polygon.
 * Backend software, indépendant du GPU (Mali-450 OK).
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
#include <functional>

#include "pmtiles.hpp"
#include "vtzero/vector_tile.hpp"
#include "vtzero/geometry.hpp"

namespace MapVector {

#define EXTENT 4096

static int    s_fd       = -1;
static void  *s_mapped   = nullptr;
static size_t s_mapSize  = 0;
static pmtiles::headerv3 s_header{};

// Décompression gzip (PMTiles + payload MVT)
static std::string gunzip(const std::string& in, uint8_t /*compression*/) {
    z_stream zs{};
    zs.next_in  = (Bytef*)in.data();
    zs.avail_in = (uInt)in.size();
    if (inflateInit2(&zs, 15 + 32) != Z_OK) return {};
    std::string out; out.reserve(in.size() * 4);
    char buf[16384];
    int ret;
    do {
        zs.next_out  = (Bytef*)buf;
        zs.avail_out = sizeof(buf);
        ret = inflate(&zs, Z_NO_FLUSH);
        if (ret < 0) { inflateEnd(&zs); return {}; }
        out.append(buf, sizeof(buf) - zs.avail_out);
    } while (ret != Z_STREAM_END);
    inflateEnd(&zs);
    return out;
}

bool open(const char *pmtilesPath) {
    s_fd = ::open(pmtilesPath, O_RDONLY);
    if (s_fd < 0) { fprintf(stderr, "MapVector: open %s failed\n", pmtilesPath); return false; }
    struct stat st;
    if (fstat(s_fd, &st) != 0) { ::close(s_fd); s_fd = -1; return false; }
    s_mapSize = st.st_size;
    s_mapped = mmap(nullptr, s_mapSize, PROT_READ, MAP_PRIVATE, s_fd, 0);
    if (s_mapped == MAP_FAILED) { ::close(s_fd); s_fd = -1; s_mapped = nullptr; return false; }
    std::string hstr((const char*)s_mapped, 127);
    s_header = pmtiles::deserialize_header(hstr);
    fprintf(stderr, "MapVector: open %s OK — tile_type=%u compression=%u zoom=%u-%u\n",
            pmtilesPath, s_header.tile_type, s_header.tile_compression,
            s_header.min_zoom, s_header.max_zoom);
    return true;
}

void close() {
    if (s_mapped) { munmap(s_mapped, s_mapSize); s_mapped = nullptr; }
    if (s_fd >= 0) { ::close(s_fd); s_fd = -1; }
}

// Conversion coord tile-locale (0..EXTENT) → coord canvas (0..canvasSize)
static inline int toCx(int v, int canvasSize) { return (int)((int64_t)v * canvasSize / EXTENT); }

// Handler VTzero pour collecter les points d'une ligne et la dessiner
struct LineDrawer {
    lv_obj_t *canvas;
    int canvasSize;
    lv_color_t color;
    int width;
    lv_layer_t *layer;
    std::vector<lv_point_precise_t> pts;

    void linestring_begin(uint32_t /*count*/) { pts.clear(); }
    void linestring_point(vtzero::point p) {
        lv_point_precise_t pt;
        pt.x = toCx(p.x, canvasSize);
        pt.y = toCx(p.y, canvasSize);
        pts.push_back(pt);
    }
    void linestring_end() {
        if (pts.size() < 2) return;
        lv_draw_line_dsc_t dsc;
        lv_draw_line_dsc_init(&dsc);
        dsc.color = color;
        dsc.width = width;
        dsc.opa   = LV_OPA_COVER;
        dsc.round_start = dsc.round_end = 1;
        for (size_t i = 1; i < pts.size(); i++) {
            dsc.p1 = pts[i-1];
            dsc.p2 = pts[i];
            lv_draw_line(layer, &dsc);
        }
    }
    // Stubs non utilisés
    void points_begin(uint32_t) {} void points_point(vtzero::point) {} void points_end() {}
    void ring_begin(uint32_t) {}   void ring_point(vtzero::point)   {} void ring_end(vtzero::ring_type) {}
};

// Handler pour les polygones — accumule les rings et dessine quand un outer ring se termine
struct PolyDrawer {
    lv_obj_t *canvas;
    int canvasSize;
    lv_color_t fill;
    lv_layer_t *layer;
    std::vector<lv_point_precise_t> ring;

    void ring_begin(uint32_t /*count*/) { ring.clear(); }
    void ring_point(vtzero::point p) {
        lv_point_precise_t pt;
        pt.x = toCx(p.x, canvasSize);
        pt.y = toCx(p.y, canvasSize);
        ring.push_back(pt);
    }
    void ring_end(vtzero::ring_type rt) {
        if (rt != vtzero::ring_type::outer || ring.size() < 3) return;
        // lv_draw_triangle ou triangulation manuelle ; pour démarrer on fait
        // un fan triangulation autour du premier point (suffit pour convexes
        // et acceptable visuel pour la plupart des features OSM)
        lv_draw_triangle_dsc_t dsc;
        lv_draw_triangle_dsc_init(&dsc);
        dsc.color = fill;
        dsc.opa   = LV_OPA_COVER;
        for (size_t i = 1; i + 1 < ring.size(); i++) {
            dsc.p[0] = ring[0];
            dsc.p[1] = ring[i];
            dsc.p[2] = ring[i+1];
            lv_draw_triangle(layer, &dsc);
        }
    }
    void points_begin(uint32_t) {} void points_point(vtzero::point) {} void points_end() {}
    void linestring_begin(uint32_t) {} void linestring_point(vtzero::point) {} void linestring_end() {}
};

bool renderTile(lv_obj_t *canvas, int z, int x, int y, int canvasSize) {
    if (!s_mapped || !canvas) return false;

    // Fond beige clair (style map de base)
    lv_canvas_fill_bg(canvas, lv_color_hex(0xf0ead6), LV_OPA_COVER);

    auto [off, len] = pmtiles::get_tile(gunzip, (const char*)s_mapped, z, x, y);
    if (len == 0) return false;

    std::string raw((const char*)s_mapped + off, len);
    std::string mvt = (s_header.tile_compression == pmtiles::COMPRESSION_GZIP)
                      ? gunzip(raw, 0) : raw;
    if (mvt.empty()) return false;

    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    vtzero::vector_tile tile{mvt};

    // PASSE 1 : polygones (fond) — landcover puis landuse puis water
    auto drawPolygons = [&](const char* layerName, lv_color_t color) {
        vtzero::vector_tile t2{mvt};
        while (auto lay = t2.next_layer()) {
            if (std::string(lay.name()) != layerName) continue;
            PolyDrawer pd;
            pd.canvas = canvas; pd.canvasSize = canvasSize;
            pd.fill = color; pd.layer = &layer;
            while (auto feat = lay.next_feature()) {
                if (feat.geometry_type() == vtzero::GeomType::POLYGON)
                    vtzero::decode_polygon_geometry(feat.geometry(), pd);
            }
            return;
        }
    };
    drawPolygons("landcover", lv_color_hex(0xcde0b3));
    drawPolygons("landuse",   lv_color_hex(0xe8d8c0));
    // water polygons désactivés : fan triangulation foire sur les rivières
    // larges/courbes (artefact triangle). À réactiver quand tessellation OK.
    // drawPolygons("water",     lv_color_hex(0x6ea8e0));

    // PASSE 2 : lignes — waterway puis transportation
    auto drawLines = [&](const char* layerName, lv_color_t color, int width) {
        vtzero::vector_tile t2{mvt};
        while (auto lay = t2.next_layer()) {
            if (std::string(lay.name()) != layerName) continue;
            LineDrawer ld;
            ld.canvas = canvas; ld.canvasSize = canvasSize;
            ld.color = color; ld.width = width; ld.layer = &layer;
            while (auto feat = lay.next_feature()) {
                if (feat.geometry_type() == vtzero::GeomType::LINESTRING)
                    vtzero::decode_linestring_geometry(feat.geometry(), ld);
            }
            return;
        }
    };
    drawLines("waterway",       lv_color_hex(0x4a7ab0), 2);
    drawLines("transportation", lv_color_hex(0x404040), 2);

    lv_canvas_finish_layer(canvas, &layer);
    return true;
}

}  // namespace MapVector
