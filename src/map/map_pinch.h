#ifndef MAP_PINCH_H
#define MAP_PINCH_H

// Parallel multi-touch reader for the map screen. LVGL's evdev input runs as a
// single pointer, so two-finger gestures are invisible to it; this opens the
// same device a second time (no EVIOCGRAB, both readers get the event stream)
// and decodes the protocol B slots to expose a pinch.
namespace MapPinch {

void start();
void stop();

// Drains pending events. Returns the number of fingers down (0, 1, or 2+).
// When >= 2, fills the pixel distance between the two fingers and their
// midpoint. Coordinates are 1:1 with the LVGL pointer (screen pixels).
int poll(float *dist, float *midX, float *midY);

}  // namespace MapPinch

#endif  // MAP_PINCH_H
