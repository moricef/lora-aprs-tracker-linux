// Inspecteur de tuile PMTiles : dump des layers/classes/types par tuile z/x/y.
// Usage: inspect_tile <pmtiles> <z> <lat> <lon>
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <zlib.h>
#include <string>
#include <map>
#include <set>

#include "pmtiles.hpp"
#include "vtzero/vector_tile.hpp"

static std::string gunzip(const std::string& in, uint8_t) {
    z_stream zs{};
    zs.next_in = (Bytef*)in.data(); zs.avail_in = (uInt)in.size();
    if (inflateInit2(&zs, 15 + 32) != Z_OK) return {};
    std::string out; out.reserve(in.size() * 4);
    char buf[16384]; int ret;
    do {
        zs.next_out = (Bytef*)buf; zs.avail_out = sizeof(buf);
        ret = inflate(&zs, Z_NO_FLUSH);
        if (ret < 0) { inflateEnd(&zs); return {}; }
        out.append(buf, sizeof(buf) - zs.avail_out);
    } while (ret != Z_STREAM_END);
    inflateEnd(&zs);
    return out;
}

// Calcule l'aire (shoelace) de chaque polygone d'une feature, en unités MVT² (extent 4096).
struct AreaHandler {
    std::vector<std::pair<int,int>> ring;
    double featArea = 0;
    int nOuter = 0, nInner = 0, nInvalid = 0;
    double outerArea = 0, innerArea = 0;
    void ring_begin(uint32_t) { ring.clear(); }
    void ring_point(vtzero::point p) { ring.push_back({p.x, p.y}); }
    void ring_end(vtzero::ring_type rt) {
        double a = 0; int n = ring.size();
        for (int i = 0; i < n; i++) { auto&p=ring[i]; auto&q=ring[(i+1)%n]; a += (double)p.first*q.second - (double)q.first*p.second; }
        a = a/2.0; double aa = a<0?-a:a;
        if (rt == vtzero::ring_type::outer)      { nOuter++; outerArea += aa; featArea += aa; }
        else if (rt == vtzero::ring_type::inner) { nInner++; innerArea += aa; featArea -= aa; }
        else nInvalid++;
    }
    void points_begin(uint32_t){} void points_point(vtzero::point){} void points_end(){}
    void linestring_begin(uint32_t){} void linestring_point(vtzero::point){} void linestring_end(){}
};

static const char *geomName(vtzero::GeomType g) {
    switch (g) {
        case vtzero::GeomType::POINT:      return "POINT";
        case vtzero::GeomType::LINESTRING: return "LINE";
        case vtzero::GeomType::POLYGON:    return "POLY";
        default:                           return "?";
    }
}

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: %s <pmtiles> <z> <lat> <lon>\n", argv[0]); return 1; }
    const char *path = argv[1];
    int z = atoi(argv[2]);
    double lat = atof(argv[3]), lon = atof(argv[4]);

    int n = 1 << z;
    int x = (int)floor((lon + 180.0) / 360.0 * n);
    double latRad = lat * M_PI / 180.0;
    int y = (int)floor((1.0 - asinh(tan(latRad)) / M_PI) / 2.0 * n);
    printf("Tile z=%d x=%d y=%d (lat=%.4f lon=%.4f)\n", z, x, y, lat, lon);

    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("open"); return 1; }
    struct stat st; fstat(fd, &st);
    void *m = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (m == MAP_FAILED) { perror("mmap"); return 1; }

    std::string hstr((const char*)m, 127);
    auto hdr = pmtiles::deserialize_header(hstr);
    printf("pmtiles z%d-%d\n", hdr.min_zoom, hdr.max_zoom);

    auto [off, len] = pmtiles::get_tile(gunzip, (const char*)m, z, x, y);
    if (len == 0) { printf("TUILE VIDE / absente\n"); return 0; }
    std::string raw((const char*)m + off, len);
    std::string mvt = (raw.size() > 2 && (uint8_t)raw[0] == 0x1f && (uint8_t)raw[1] == 0x8b)
                      ? gunzip(raw, 0) : raw;
    printf("tuile %zu octets (decode %zu)\n\n", raw.size(), mvt.size());

    // layer -> (geomType -> count), layer -> classes
    std::map<std::string, std::map<std::string,int>> geomCounts;
    std::map<std::string, std::set<std::string>> classes;
    std::map<std::string, int> total;
    // pour le layer ciblé via env DUMPLAYER : toutes les paires class/subclass
    const char *dumpLayer = getenv("DUMPLAYER");
    std::map<std::string,int> subclassCount;

    vtzero::vector_tile t{mvt};
    while (auto lay = t.next_layer()) {
        std::string ln(lay.name());
        while (auto f = lay.next_feature()) {
            total[ln]++;
            geomCounts[ln][geomName(f.geometry_type())]++;
            std::string cls, sub;
            while (auto p = f.next_property()) {
                if (p.value().type() != vtzero::property_value_type::string_value) continue;
                auto v = p.value().string_value();
                std::string val(v.data(), v.size());
                if (p.key() == "class")    { classes[ln].insert(val); cls = val; }
                if (p.key() == "subclass") { sub = val; }
            }
            if (dumpLayer && ln == dumpLayer)
                subclassCount[cls + " / " + sub]++;
        }
    }
    // DUMPAREA=layer:class -> distribution d'aires des polygones (% de la tuile)
    const char *dumpArea = getenv("DUMPAREA");
    if (dumpArea) {
        std::string spec(dumpArea); auto cpos = spec.find(':');
        std::string aLayer = spec.substr(0, cpos);
        std::string aClass = cpos==std::string::npos ? "" : spec.substr(cpos+1);
        double tileArea = 4096.0*4096.0;
        std::vector<double> areas;
        int gOuter=0, gInner=0, gInvalid=0; double gOuterA=0, gInnerA=0;
        vtzero::vector_tile t2{mvt};
        while (auto lay = t2.next_layer()) {
            if (std::string(lay.name()) != aLayer) continue;
            while (auto f = lay.next_feature()) {
                if (f.geometry_type() != vtzero::GeomType::POLYGON) continue;
                std::string cls;
                while (auto p = f.next_property())
                    if (p.key()=="class" && p.value().type()==vtzero::property_value_type::string_value)
                        { auto v=p.value().string_value(); cls.assign(v.data(),v.size()); }
                if (!aClass.empty() && cls != aClass) continue;
                AreaHandler ah; vtzero::decode_polygon_geometry(f.geometry(), ah);
                areas.push_back(ah.featArea / tileArea * 100.0); // % de la tuile
                gOuter += ah.nOuter; gInner += ah.nInner; gInvalid += ah.nInvalid;
                gOuterA += ah.outerArea; gInnerA += ah.innerArea;
            }
        }
        std::sort(areas.rbegin(), areas.rend());
        double tot=0; for (double a:areas) tot+=a;
        printf("\n[AREA] %s class=%s : %zu polys, total(net)=%.2f%% de la tuile\n",
               aLayer.c_str(), aClass.c_str(), areas.size(), tot);
        printf("  rings: outer=%d (%.1f%%)  inner=%d (%.1f%%)  invalid=%d\n",
               gOuter, gOuterA/tileArea*100.0, gInner, gInnerA/tileArea*100.0, gInvalid);
    }

    if (dumpLayer) {
        printf("\n[%s] class / subclass :\n", dumpLayer);
        for (auto &kv : subclassCount) printf("    %-30s %d\n", kv.first.c_str(), kv.second);
        printf("\n");
    }

    for (auto &kv : total) {
        const std::string &ln = kv.first;
        printf("%-16s %5d feat  ", ln.c_str(), kv.second);
        for (auto &g : geomCounts[ln]) printf("%s:%d ", g.first.c_str(), g.second);
        printf("\n");
        if (!classes[ln].empty()) {
            std::string vals;
            for (auto &c : classes[ln]) { if (!vals.empty()) vals += ","; vals += c; }
            printf("    class: [%s]\n", vals.c_str());
        }
    }
    return 0;
}
