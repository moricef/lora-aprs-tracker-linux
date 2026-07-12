#pragma once

#include "lvgl.h"

// Movement traces — station traces (blue) + own GPS trace (purple), drawn
// directly into the shared map canvas at sprite coordinates.

namespace MapTraces {

// Reset the own-trace ring buffer (on map close).
void destroy();

// Draw both station traces and the own trace into `canvas` at sprite coords.
void drawInto(lv_obj_t *canvas);

// Sample the current GPS position into the own-trace ring buffer
// (no-op below ~10m movement threshold).
void recordOwnPosition();

// Read the own trace in chronological order for alternate renderers.
int ownTraceSize();
bool ownTracePoint(int chronologicalIndex, double *lat, double *lon);

} // namespace MapTraces
