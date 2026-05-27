#pragma once
#include <cmath>

#ifndef MAP_TILE_SIZE
#define MAP_TILE_SIZE 256
#endif
#ifndef MAP_TILES_GRID
#define MAP_TILES_GRID 5
#endif
#ifndef MAP_SPRITE_SIZE
#define MAP_SPRITE_SIZE (MAP_TILES_GRID * MAP_TILE_SIZE)
#endif

namespace MapMath {
    void latLonToTile(float lat, float lon, int zoom, int* tileX, int* tileY);
    void tileToLatLon(int tileX, int tileY, int zoom, float* lat, float* lon);
    void latLonToPixel(float lat, float lon, float centerLat, float centerLon,
                       int zoom, bool navModeActive, int centerTileX, int centerTileY,
                       int* pixelX, int* pixelY);
    void shiftMapCenter(float centerLat, float centerLon, int zoom,
                        int deltaTileX, int deltaTileY,
                        float* newLat, float* newLon);
    void pixelToLatLon(int pixelX, int pixelY, int zoom, bool navModeActive,
                       int centerTileX, int centerTileY,
                       float centerLat, float centerLon,
                       float* lat, float* lon);
}
