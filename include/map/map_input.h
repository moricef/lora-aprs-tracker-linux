#pragma once

#include "lvgl.h"

// Touch + gesture handler for the map container : pan, inertia velocity,
// double-tap fullscreen, marker hit-test (long-press vs short tap).
// Calls back into MapEngine / MapMarkers / a small set of view hooks.

namespace MapInput {

// Attach LV_EVENT_DOUBLE_CLICKED / PRESSED / PRESSING / RELEASED handlers
// to `target` (the map container). Idempotent.
void install(lv_obj_t *target);

} // namespace MapInput

// View hooks implemented by map_view (currently map_raster.cpp). MapInput
// calls them when the gesture state transitions require UI side-effects.
namespace MapView {

// Double-tap on the map → expand/restore the map container.
void toggleFullscreen();

// Drag was significant enough that follow-GPS should be disabled. View
// updates the recenter button colour.
void markFollowGpsDisabled();

// Pan ended after a real drag — refresh the info bar text.
void refreshInfoBar();

} // namespace MapView
