#pragma once

#include "lvgl.h"

// Tile-grid orchestrator : owns the 5×5 raster/vector canvas grid, the
// reload/render pipeline and the zoom controls. Drives the overlays
// (MapTraces, MapLabels, MapMarkers) after each reload.

namespace MapEngine {

// Shared inertia/gesture flags. MapInput writes them on gesture events;
// timerTick() reads & decays them.
extern bool  panActive;
extern float velX, velY;

// Allocate the 5×5 raster image widgets under `parent`. Vector canvases
// are created lazily inside reloadTiles when zoom switches to vector.
void init(lv_obj_t *parent);

// Free every vector canvas + tile widget. Raster widgets are deleted by
// LVGL when their parent (mapCont) is freed.
void destroy();

// Register the two header widgets so reloadTiles can update them.
void setLabels(lv_obj_t *titleLabel, lv_obj_t *infoLabel);

// (Re)render all 25 tiles + redraw overlays + (re)create markers.
void reloadTiles();

// Move the 25 tile widgets + overlays in place after a pan, without
// re-rendering tiles.
void repositionAll();

// Zoom in/out around the current viewport center. Disables follow-GPS.
void zoomIn();
void zoomOut();

// Recenter the viewport on a given lat/lon at a new zoom level. Absorbs
// the sub-tile residual into dragAccumX/Y.
void recenterForZoom(int newZoom);

// 50 ms periodic tick (called from a map-view timer). Handles inertia
// roll-out, follow-GPS recentering, and periodic station refresh.
void timerTick();

} // namespace MapEngine
