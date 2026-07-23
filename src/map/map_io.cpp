#include "map/map_io.h"
#include "map/map_state.h"
#include "map_vector.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

namespace MapIO {

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

void discoverZooms() {
  if (!MapVector::isOpen())
    return;

  // Extend max zoom to include vector tiles + 3 overzoom levels. The
  // pmtiles file stops at maxZoom, but MapVector::ozSetup fetches the
  // z=maxZoom tile and redraws its sub-region at the screen zoom — sharp
  // because geometry is redrawn, not bitmap-stretched.
  constexpr int OVERZOOM_LEVELS = 3;
  MapState::zoomMin = MapVector::minZoom();
  MapState::zoomMax = MapVector::maxZoom() + OVERZOOM_LEVELS;

  // Preserve the zoom across map close/open — clamp only if the prior
  // value falls outside the currently available range.
  if (MapState::zoom < MapState::zoomMin) MapState::zoom = MapState::zoomMin;
  if (MapState::zoom > MapState::zoomMax) MapState::zoom = MapState::zoomMax;
}

void discoverDefaultPosition() {
  if (!MapVector::isOpen())
    return;
  MapState::centerLat = MapVector::centerLat();
  MapState::centerLon = MapVector::centerLon();
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
