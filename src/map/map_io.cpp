#include "map/map_io.h"
#include "map/map_state.h"
#include "map_coordinate_math.h"
#include "map_vector.h"

#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace MapIO {

const char *mapsRoot() {
  static char root[256];
  if (root[0])
    return root;
  const char *candidates[] = {
      "/home/fab2/Developpement/LoRa_APRS/SDCARD/TILES/LoRa_Tracker/Maps",
      "/media/fab2/TILES/LoRa_Tracker/Maps", "/data/LoRa_Tracker/Maps", NULL};
  for (int i = 0; candidates[i]; i++) {
    struct stat st;
    if (stat(candidates[i], &st) == 0) {
      strncpy(root, candidates[i], sizeof(root) - 1);
      return root;
    }
  }
  return NULL;
}

const char *symbolsRoot() {
  static char root[256];
  if (root[0])
    return root;
  const char *candidates[] = {"/home/fab2/Developpement/LoRa_APRS/aprs-symbols/"
                              "sd_card/LoRa_Tracker/Symbols",
                              "/media/fab2/TILES/LoRa_Tracker/Symbols",
                              "/data/LoRa_Tracker/Symbols", NULL};
  for (int i = 0; candidates[i]; i++) {
    struct stat st;
    if (stat(candidates[i], &st) == 0) {
      strncpy(root, candidates[i], sizeof(root) - 1);
      return root;
    }
  }
  return NULL;
}

void discoverRegion() {
  if (MapState::mapRegion[0])
    return;
  const char *root = mapsRoot();
  if (!root)
    return;
  DIR *d = opendir(root);
  if (!d)
    return;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (e->d_type == DT_DIR && e->d_name[0] != '.') {
      strncpy(MapState::mapRegion, e->d_name, sizeof(MapState::mapRegion) - 1);
      break;
    }
  }
  closedir(d);
}

void discoverZooms() {
  if (!MapState::mapRegion[0]) {
    discoverRegion();
    if (!MapState::mapRegion[0])
      return;
  }
  char zpath[512];
  snprintf(zpath, sizeof(zpath), "%s/%s", mapsRoot(), MapState::mapRegion);
  DIR *d = opendir(zpath);
  if (!d)
    return;
  struct dirent *e;
  int zMin = INT_MAX, zMax = INT_MIN;
  while ((e = readdir(d))) {
    if (e->d_type == DT_DIR && e->d_name[0] != '.') {
      int z = atoi(e->d_name);
      if (z > 0 && z < 20) {
        if (z < zMin)
          zMin = z;
        if (z > zMax)
          zMax = z;
      }
    }
  }
  closedir(d);
  if (zMin <= zMax) {
    MapState::zoomMin = (zMin < 7) ? 7 : zMin;
    MapState::zoomMax = zMax;
    MapState::zoom = zMax;
  }
  // Extend max zoom to include vector tiles + 3 overzoom levels. The
  // pmtiles file stops at maxZoom, but MapVector::ozSetup fetches the
  // z=maxZoom tile and re-rasterizes its sub-region at the screen zoom —
  // sharp because geometry is redrawn, not bitmap-stretched.
  if (MapVector::isOpen()) {
    constexpr int OVERZOOM_LEVELS = 3;
    int vMax = MapVector::maxZoom() + OVERZOOM_LEVELS;
    int vMin = MapVector::minZoom();
    if (vMax > MapState::zoomMax)
      MapState::zoomMax = vMax;
    if (vMin < MapState::zoomMin || MapState::zoomMin == INT_MAX)
      MapState::zoomMin = vMin;
  }
}

void discoverDefaultPosition() {
  if (!MapState::mapRegion[0])
    return;
  char zpath[512];
  snprintf(zpath, sizeof(zpath), "%s/%s/%d", mapsRoot(), MapState::mapRegion, 6);
  DIR *zd = opendir(zpath);
  if (!zd)
    return;
  int xMin = INT_MAX, xMax = INT_MIN, yMin = INT_MAX, yMax = INT_MIN;
  struct dirent *xe;
  while ((xe = readdir(zd))) {
    if (xe->d_type != DT_DIR || xe->d_name[0] == '.')
      continue;
    int tx = atoi(xe->d_name);
    char xpath[600];
    snprintf(xpath, sizeof(xpath), "%s/%s", zpath, xe->d_name);
    DIR *xd = opendir(xpath);
    if (!xd)
      continue;
    struct dirent *ye;
    while ((ye = readdir(xd))) {
      if (ye->d_type != DT_REG)
        continue;
      char base[64];
      strncpy(base, ye->d_name, sizeof(base) - 1);
      base[sizeof(base) - 1] = 0;
      char *dot = strrchr(base, '.');
      if (dot)
        *dot = 0;
      int ty = atoi(base);
      if (tx < xMin)
        xMin = tx;
      if (tx > xMax)
        xMax = tx;
      if (ty < yMin)
        yMin = ty;
      if (ty > yMax)
        yMax = ty;
    }
    closedir(xd);
  }
  closedir(zd);
  if (xMin <= xMax && yMin <= yMax)
    MapMath::tileToLatLon((xMin + xMax) / 2, (yMin + yMax) / 2, 6,
                          (float *)&MapState::centerLat,
                          (float *)&MapState::centerLon);
}

bool tileExists(int tx, int ty, int z) {
  static uint32_t notFoundCache[128];
  static int notFoundIdx = 0;
  uint32_t key = ((uint32_t)z << 24) | ((uint32_t)(tx & 0xFFF) << 12) |
                 (uint32_t)(ty & 0xFFF);
  for (int i = 0; i < 128; i++)
    if (notFoundCache[i] == key)
      return false;
  char p[512];
  snprintf(p, sizeof(p), "%s/%s/%d/%d/%d.jpg", mapsRoot(), MapState::mapRegion,
           z, tx, ty);
  struct stat st;
  if (stat(p, &st) == 0)
    return true;
  notFoundCache[notFoundIdx++ % 128] = key;
  return false;
}

bool getSymbolPath(char table, char symbol, char *path, size_t pathsz) {
  const char *root = symbolsRoot();
  if (!root)
    return false;
  const char *tableName = (table == '/') ? "primary" : "alternate";
  snprintf(path, pathsz, "A:%s/%s/%02X.png", root, tableName, (uint8_t)symbol);
  return true;
}

} // namespace MapIO
