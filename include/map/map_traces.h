#pragma once

#include "lvgl.h"

// Overlay canvas for movement traces — station traces (blue) + own GPS
// trace (purple). Single ARGB8888 canvas sized to the sprite + map height,
// drawn with a Bresenham line per segment.

namespace MapTraces {

// Allocate the overlay canvas (child of `parent`) and its buffer.
// No-op if already created.
void create(lv_obj_t *parent);

// Free the canvas + buffer. Also resets the own-trace ring buffer.
void destroy();

// Redraw both station traces and the own trace (called from reloadTiles).
void redraw();

// Sample the current GPS position into the own-trace ring buffer
// (no-op below ~10m movement threshold).
void recordOwnPosition();

// Reposition the canvas under current pan / fullscreen state. Sized
// SPRITE_SIZE × current map height.
void reposition();

// True if the underlying canvas is allocated.
bool isReady();

// Lift the trace canvas above the tile/label canvases (called after
// reloadTiles which can create new vector canvases on top of overlays).
void moveToForeground();

} // namespace MapTraces
