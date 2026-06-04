#pragma once

#include "map_coordinate_math.h"

// Global map state shared across map_view / map_engine / map_input /
// map_labels / map_traces / map_markers. UI objects and per-module buffers
// stay in their owning modules — this file only holds plain data + the few
// inline projections that depend on it.

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

// Tile region selected by MapIO::discoverRegion (empty if no SD).
extern char mapRegion[64];

// Sprite-pixel → container-pixel projections. The sprite is centered in the
// map container; dragAccum carries the residual sub-tile pan offset.
inline int spriteToContX(int spriteX) {
    return spriteX + (CONT_W - SPRITE_SIZE) / 2 + dragAccumX;
}
inline int spriteToContY(int spriteY) {
    return spriteY + (MAP_H - SPRITE_SIZE) / 2 + dragAccumY;
}

// Lat/lon → container pixel. Returns true if the projected point falls
// inside the sprite (±1 tile margin).
inline bool latLonToContPos(float lat, float lon, int *cx, int *cy) {
    int px, py;
    MapMath::latLonToPixel(lat, lon, centerLat, centerLon, zoom, true,
                           centerTX, centerTY, &px, &py);
    *cx = spriteToContX(px);
    *cy = spriteToContY(py);
    return (px >= -TILE_SIZE && px < SPRITE_SIZE + TILE_SIZE &&
            py >= -TILE_SIZE && py < SPRITE_SIZE + TILE_SIZE);
}

} // namespace MapState
