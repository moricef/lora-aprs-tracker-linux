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

// True after MapLibre has produced its first complete map frame. Used to keep
// the first map opening covered while tiles/glyph atlases are initialized.
bool isReady();

// Capture the next composed frame to a PNG file (via SIGUSR2 from the loop).
void requestScreenshot(const char *path);

// Move the MapLibre camera (called on GPS updates).
void setCenter(double lat, double lon);
void setZoom(double zoom);
// Zoom to an absolute level while holding the given screen point fixed (pinch).
void zoomAround(double zoom, double anchorX, double anchorY);
double getZoom();
void moveBy(double dx, double dy);
bool getCenter(double *lat, double *lon);
bool project(double lat, double lon, int *x, int *y);

// Render one frame: MapLibre into framebuffer 0, LVGL overlay on top, page-flip.
// Pumps the MapLibre run loop so tiles keep loading.
void renderTick();

// Release DRM master and GL/MapLibre resources.
void shutdown();

} // namespace MaplibreDisplay

#endif // WITH_MAPLIBRE
#endif // MAPLIBRE_DISPLAY_H
