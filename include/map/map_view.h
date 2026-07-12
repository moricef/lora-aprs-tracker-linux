#pragma once
#include "lvgl/lvgl.h"

// Map screen — owns the LVGL widgets (title bar, info bar, buttons,
// container) and delegates rendering to MapEngine, overlays to
// MapTraces/Labels/Markers, gestures to MapInput.

namespace MapView {
    lv_obj_t *create(lv_obj_t *parent);
#ifdef WITH_MAPLIBRE
    // Transparent map screen for the GPU path: chrome only (buttons drive the
    // MapLibre camera), no software MapEngine — the map shows through.
    lv_obj_t *createGpuOverlay(lv_obj_t *parent);
#endif
    void setPosition(double lat, double lon);
    void zoomIn();
    void zoomOut();
    void refreshStations();   // recrée les marqueurs si la map est à l'écran

    // Hooks called by MapInput when gesture state requires a UI side-effect
    // (see map_input.h).
    void toggleFullscreen();
    void markFollowGpsDisabled();
    void refreshInfoBar();
}
