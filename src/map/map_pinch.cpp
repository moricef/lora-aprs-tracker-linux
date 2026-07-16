#include "map/map_pinch.h"

#include <fcntl.h>
#include <linux/input.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace MapPinch {
namespace {

int fd = -1;
int curSlot = 0;
struct Slot {
  bool down = false;
  bool gotX = false;  // both axes must arrive before the contact is usable
  bool gotY = false;
  int x = 0;
  int y = 0;
};
Slot slots[2];

void resetSlots() {
  curSlot = 0;
  slots[0] = Slot{};
  slots[1] = Slot{};
}

}  // namespace

void start() {
  if (fd >= 0) return;
  const char *path = "/dev/input/by-id/usb-WaveShare_WS170120_220211-event-if00";
  struct stat st;
  if (stat(path, &st) != 0) path = "/dev/input/event5";
  fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0)
    fprintf(stderr, "MapPinch: open(%s) failed: %s — pinch disabled\n", path,
            strerror(errno));
  resetSlots();
}

void stop() {
  if (fd >= 0) {
    close(fd);
    fd = -1;
  }
  resetSlots();
}

int poll(float *dist, float *midX, float *midY) {
  if (fd < 0) return 0;
  input_event ev;
  while (read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
    if (ev.type == EV_SYN) {
      // Kernel dropped events: our slot state may be stale. Force a clean
      // re-acquisition on the next complete contact rather than acting on
      // a partial view.
      if (ev.code == SYN_DROPPED) resetSlots();
      continue;
    }
    if (ev.type != EV_ABS) continue;
    switch (ev.code) {
      case ABS_MT_SLOT:
        curSlot = ev.value;
        break;
      case ABS_MT_TRACKING_ID:
        if (curSlot >= 0 && curSlot < 2) {
          slots[curSlot].down = (ev.value >= 0);
          slots[curSlot].gotX = false;
          slots[curSlot].gotY = false;
        }
        break;
      case ABS_MT_POSITION_X:
        if (curSlot >= 0 && curSlot < 2) {
          slots[curSlot].x = ev.value;
          slots[curSlot].gotX = true;
        }
        break;
      case ABS_MT_POSITION_Y:
        if (curSlot >= 0 && curSlot < 2) {
          slots[curSlot].y = ev.value;
          slots[curSlot].gotY = true;
        }
        break;
      default:
        break;
    }
  }
  // A contact only counts once it has reported both axes, so the first pinch
  // frame can never be seeded from an uninitialised slot.
  bool a = slots[0].down && slots[0].gotX && slots[0].gotY;
  bool b = slots[1].down && slots[1].gotX && slots[1].gotY;
  int fingers = (a ? 1 : 0) + (b ? 1 : 0);
  if (a && b) {
    float dx = (float)(slots[0].x - slots[1].x);
    float dy = (float)(slots[0].y - slots[1].y);
    if (dist) *dist = std::sqrt(dx * dx + dy * dy);
    if (midX) *midX = (slots[0].x + slots[1].x) * 0.5f;
    if (midY) *midY = (slots[0].y + slots[1].y) * 0.5f;
  }
  return fingers;
}

}  // namespace MapPinch
