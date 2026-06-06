#pragma once

#include "lvgl.h"

// Labels (place names, lake names, road shields, waterway names along the
// principal direction of the polyline). Collects per-tile labels from
// MapVector, runs global collision, draws them into the shared map canvas
// at sprite coordinates.

namespace MapLabels {

// Allocate the waterway scratch canvas (320×40, hidden, off-screen).
void create(lv_obj_t *parent);

// Free the scratch canvas + buffer.
void destroy();

// Collect labels from the 5×5 visible tiles, run global collision and draw
// them into `canvas`. No-op below z9 or when MapVector isn't open.
void drawInto(lv_obj_t *canvas);

} // namespace MapLabels
