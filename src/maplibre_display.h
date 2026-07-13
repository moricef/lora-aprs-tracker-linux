// Alternate display path: MapLibre GPU base map + LVGL overlay on KMS/DRM.
// Opt-in (WITH_MAPLIBRE build + runtime selection); the default lv_linux_drm
// software path is untouched. The caller builds its UI on the returned overlay
// display, drives the camera from GPS, and calls renderTick() each loop.
#ifndef MAPLIBRE_DISPLAY_H
#define MAPLIBRE_DISPLAY_H

#ifdef WITH_MAPLIBRE

#include "lvgl.h"

namespace MaplibreDisplay {

constexpr double DEFAULT_ZOOM = 7.0;

// Takes DRM master on card0, sets up EGL/GBM + MapLibre + an LVGL overlay
// texture display. Returns the overlay display (build the UI on it), or NULL
// on failure. stylePath is a file:// URL to the MapLibre style JSON.
lv_display_t *init(const char *stylePath, double lat, double lon, double zoom);

// True once init() has succeeded and the MapLibre path is driving the display.
bool isActive();

// Move the MapLibre camera (called on GPS updates).
void setCenter(double lat, double lon);
void setZoom(double zoom);
double getZoom();
void moveBy(double dx, double dy);
bool project(double lat, double lon, int *x, int *y);

// Render one frame: MapLibre into framebuffer 0, LVGL overlay on top, page-flip.
// Pumps the MapLibre run loop so tiles keep loading.
void renderTick();

// Release DRM master and GL/MapLibre resources.
void shutdown();

} // namespace MaplibreDisplay

#endif // WITH_MAPLIBRE
#endif // MAPLIBRE_DISPLAY_H
