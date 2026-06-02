#pragma once

#include <cstddef>

// Filesystem discovery for map tiles + APRS symbols. Reads from SD card
// candidates (project paths, /media, /data) and fills MapState::mapRegion +
// zoom range. No LVGL, no rendering — pure I/O.

namespace MapIO {

// Returns the first existing tiles root from the candidate list, or NULL.
// Cached on first call.
const char *mapsRoot();

// Returns the first existing APRS symbols root, or NULL. Cached.
const char *symbolsRoot();

// Picks the first subdirectory under mapsRoot() as the active region and
// stores its name in MapState::mapRegion (no-op if already set).
void discoverRegion();

// Scans MapState::mapRegion/<zoom>/ directories to set MapState::zoomMin,
// zoomMax, zoom. Extended by MapVector's PMTiles range if open.
void discoverZooms();

// Picks the center of the z=6 tiles found on disk as the default lat/lon
// (MapState::centerLat/centerLon).
void discoverDefaultPosition();

// True if mapRegion/<z>/<tx>/<ty>.jpg exists. Backed by a small negative
// cache (last 128 lookups) to avoid hammering the FS on missing tiles.
bool tileExists(int tx, int ty, int z);

// Fills `path` (size pathsz) with the LVGL "A:/..." path to the PNG of the
// given APRS symbol. table = '/' (primary) or '\\' (alternate).
bool getSymbolPath(char table, char symbol, char *path, size_t pathsz);

} // namespace MapIO
