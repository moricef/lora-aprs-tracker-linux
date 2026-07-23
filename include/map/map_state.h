#pragma once

// Global map state shared across map_view / map_engine / map_input /
// map_labels / map_traces / map_markers. UI objects and per-module buffers
// stay in their owning modules — this file only holds plain data.

namespace MapState {

// Layout constants (1024×600 screen, 45px titlebar, 30px infobar).
constexpr int TILE_SIZE   = 256;
constexpr int GRID        = 5;             // 5×5 covers map area + 1-tile margin
constexpr int SPRITE_SIZE = GRID * TILE_SIZE; // 1280
constexpr int CONT_W      = 1024;
constexpr int MAP_H       = 600 - 45 - 30; // 525

// Viewport (center tile + sub-tile drag offset)
extern double centerLat, centerLon;
extern int zoom, centerTX, centerTY;
extern int zoomMin, zoomMax;
extern int dragAccumX, dragAccumY;

// GPS (0,0 = no fix)
extern double gpsLat, gpsLon;
extern bool mapFollowGps;

// Lifecycle / layout flags
extern bool mapActive;
extern bool fullscreenMap;

} // namespace MapState
