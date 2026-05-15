#include "Arduino.h"
#include <time.h>
#include <stdint.h>

static uint64_t _startMs = 0;

static void ensureStart() {
    if (_startMs == 0) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        _startMs = (uint64_t)ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL;
    }
}

uint32_t millis() {
    ensureStart();
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL;
    return (uint32_t)(now - _startMs);
}

// stub globals for hardware-specific state
bool winlinkCommentState = false;
uint8_t winlinkStatus    = 0;
int  wxModuleType        = 0;
bool wxModuleFound       = false;
bool disableGPS          = false;
bool gpsShouldSleep      = false;
bool SleepModeActive     = false;
