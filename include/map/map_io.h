#pragma once

#include <cstddef>

// Filesystem discovery for APRS symbols plus PMTiles view metadata. No LVGL,
// no rendering — pure I/O.

namespace MapIO {

// Returns the first existing APRS symbols root, or NULL. Cached.
const char *symbolsRoot();

// Sets MapState::zoomMin/zoomMax from the open PMTiles source.
void discoverZooms();

// Picks the PMTiles header center as default lat/lon when available.
void discoverDefaultPosition();

// Fills `path` (size pathsz) with the LVGL "A:/..." path to the PNG of the
// given APRS symbol. table = '/' (primary) or '\\' (alternate).
bool getSymbolPath(char table, char symbol, char *path, size_t pathsz);

} // namespace MapIO
