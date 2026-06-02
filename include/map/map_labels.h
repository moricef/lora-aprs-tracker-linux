#pragma once

#include "lvgl.h"

// Label overlay : single sprite-sized ARGB canvas above the tile grid.
// Collects per-tile labels from MapVector, runs global collision, draws
// them (place names, lake names, road shields, waterway names along the
// principal direction of the polyline).

namespace MapLabels {

// Allocate the main overlay + waterway scratch canvas. Sized SPRITE_SIZE
// for the main canvas, 320×40 for the scratch.
void create(lv_obj_t *parent);

// Free both canvases and their buffers.
void destroy();

// Clear the overlay (transparent).
void clear();

// Move the overlay to the given container coordinates (sprite origin).
void reposition(int originX, int originY);

// Collect labels from the 5×5 visible tiles, run global collision and
// (re)draw the overlay. No-op below z9 or when MapVector isn't open.
void refresh();

// Move the label overlay above the tile / trace canvases.
void moveToForeground();

} // namespace MapLabels
