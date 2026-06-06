#pragma once

#include "lvgl.h"

// Map orchestrator : owns the single composited canvas (tiles + trace +
// labels + markers), the reload/recompose pipeline and the zoom controls.
// Drives the overlays (MapTraces, MapLabels, MapMarkers) at composite time.

namespace MapEngine {

// Shared inertia/gesture flags. MapInput writes them on gesture events;
// timerTick() reads & decays them.
extern bool  panActive;
extern float velX, velY;

// Allocate the composited map canvas (+ snapshot/scratch buffers) under
// `parent`.
void init(lv_obj_t *parent);

// Free the map canvas and its buffers.
void destroy();

// Register the two header widgets so reloadTiles can update them.
void setLabels(lv_obj_t *titleLabel, lv_obj_t *infoLabel);

// Rebuild the static layer (tiles + labels) and recompose. Called on zoom /
// tile-cross / recenter.
void reloadTiles();

// Redraw the dynamic layer (trace + markers) over the cached static layer.
// Cheap — call on GPS / station updates without re-rendering tiles.
void recompose();

// Move the single map canvas in place after a pan (everything is baked in).
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
