#pragma once

// Global map state shared across map_view / map_engine / map_input /
// map_labels / map_traces / map_markers. UI objects and per-module buffers
// stay in their owning modules — this file only holds plain data.

namespace MapState {

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

// Tile region selected by map_io::discoverRegion (empty if no SD).
extern char mapRegion[64];

} // namespace MapState
