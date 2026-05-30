// Rend une tuile vectorielle via le VRAI renderer (MapVector::renderTileRaw)
// et l'écrit en PNG. Pixels identiques à ce que le canvas afficherait.
// Usage: render_tile <pmtiles> <z> <lat> <lon> <out.png> [size]
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <vector>
#include <zlib.h>
#include "lvgl/lvgl.h"
#include "map/map_vector.h"

// --- PNG minimal (RGB8) via zlib ---
static void put32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(x>>24); v.push_back(x>>16); v.push_back(x>>8); v.push_back(x);
}
static void chunk(std::vector<uint8_t>& out, const char* tag, const std::vector<uint8_t>& data) {
    put32(out, (uint32_t)data.size());
    size_t start = out.size();
    out.insert(out.end(), tag, tag+4);
    out.insert(out.end(), data.begin(), data.end());
    uint32_t crc = crc32(0, out.data()+start, (uInt)(out.size()-start));
    put32(out, crc);
}
static bool writePNG(const char* path, const uint8_t* rgb, int w, int h) {
    std::vector<uint8_t> raw;
    raw.reserve((size_t)(w*3+1)*h);
    for (int y = 0; y < h; y++) {
        raw.push_back(0); // filter none
        raw.insert(raw.end(), rgb + (size_t)y*w*3, rgb + (size_t)(y+1)*w*3);
    }
    uLongf clen = compressBound(raw.size());
    std::vector<uint8_t> comp(clen);
    if (compress2(comp.data(), &clen, raw.data(), raw.size(), 9) != Z_OK) return false;
    comp.resize(clen);

    std::vector<uint8_t> out;
    const uint8_t sig[8] = {137,80,78,71,13,10,26,10};
    out.insert(out.end(), sig, sig+8);
    std::vector<uint8_t> ihdr;
    put32(ihdr, w); put32(ihdr, h);
    ihdr.push_back(8); ihdr.push_back(2); // 8-bit, RGB
    ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    chunk(out, "IHDR", ihdr);
    chunk(out, "IDAT", comp);
    chunk(out, "IEND", {});

    FILE* f = fopen(path, "wb");
    if (!f) return false;
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    return true;
}

int main(int argc, char** argv) {
    if (argc < 6) {
        fprintf(stderr, "usage: %s <pmtiles> <z> <lat> <lon> <out.png> [size]\n", argv[0]);
        return 1;
    }
    const char* pm = argv[1];
    int z = atoi(argv[2]);
    double lat = atof(argv[3]), lon = atof(argv[4]);
    const char* out = argv[5];
    int sz = (argc > 6) ? atoi(argv[6]) : 256;

    lv_init();
    if (!MapVector::open(pm)) { fprintf(stderr, "open failed\n"); return 1; }

    int n = 1 << z;
    int x = (int)floor((lon + 180.0) / 360.0 * n);
    double latRad = lat * M_PI / 180.0;
    int y = (int)floor((1.0 - asinh(tan(latRad)) / M_PI) / 2.0 * n);
    fprintf(stderr, "tile z=%d x=%d y=%d sz=%d\n", z, x, y, sz);

    std::vector<uint8_t> buf((size_t)sz*sz*4, 0);
    if (!MapVector::renderTileRaw(buf.data(), sz, z, x, y)) {
        fprintf(stderr, "render failed\n"); return 1;
    }
    // ARGB8888 octets B,G,R,A -> RGB
    std::vector<uint8_t> rgb((size_t)sz*sz*3);
    for (int i = 0; i < sz*sz; i++) {
        rgb[i*3+0] = buf[i*4+2]; // R
        rgb[i*3+1] = buf[i*4+1]; // G
        rgb[i*3+2] = buf[i*4+0]; // B
    }
    if (!writePNG(out, rgb.data(), sz, sz)) { fprintf(stderr, "png write failed\n"); return 1; }
    fprintf(stderr, "wrote %s\n", out);
    return 0;
}
