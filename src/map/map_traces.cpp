#include "map/map_traces.h"
#include "map/map_state.h"
#include "map_coordinate_math.h"
#include "station_utils.h"

#include "Arduino.h"  // millis()

#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace MapTraces {

using namespace MapState;

// Own trace ring buffer (mirrors firmware TracePoint).
#define OWN_TRACE_MAX 200
static TracePoint ownTrace[OWN_TRACE_MAX];
static int ownTraceCount = 0;
static int ownTraceHead = 0;

// Overlay canvas (ARGB8888, sized SPRITE_SIZE × map-height).
static lv_obj_t *traceCanvas = nullptr;
static uint8_t  *traceBuf    = nullptr;
constexpr int TRACE_CANVAS_W = SPRITE_SIZE;
constexpr int TRACE_CANVAS_H = 600;

static inline void traceSetPx(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    if (x < 0 || x >= TRACE_CANVAS_W || y < 0 || y >= TRACE_CANVAS_H) return;
    uint8_t *p = traceBuf + (y * TRACE_CANVAS_W + x) * 4;
    p[0] = b; p[1] = g; p[2] = r; p[3] = 0xFF;
}

static void traceDrawLine(int x0, int y0, int x1, int y1, uint8_t r, uint8_t g, uint8_t b) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (true) {
        traceSetPx(x0, y0, r, g, b);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void drawStationsTrace() {
    uint32_t now = millis();
    for (int s = 0; s < MAP_STATIONS_MAX; s++) {
        MapStation *st = STATION_Utils::getMapStation(s);
        if (!st || !st->valid || st->traceCount < 2) continue;

        int prevSX = INT_MIN, prevSY = INT_MIN;
        bool firstPt = true;
        for (int i = 0; i < st->traceCount; i++) {
            int idx = (st->traceHead - st->traceCount + i + TRACE_MAX_POINTS) % TRACE_MAX_POINTS;
            uint32_t age = now - st->trace[idx].time;
            if (age > 3600000) continue; // TTL 1h
            int sx, sy;
            MapMath::latLonToPixel(st->trace[idx].lat, st->trace[idx].lon,
                                   centerLat, centerLon, zoom, true,
                                   centerTX, centerTY, &sx, &sy);
            if (!firstPt)
                traceDrawLine(prevSX, prevSY, sx, sy, 0x00, 0x55, 0xFF);
            prevSX = sx; prevSY = sy;
            firstPt = false;
        }
        // Line to current position
        int cx, cy;
        MapMath::latLonToPixel(st->latitude, st->longitude,
                               centerLat, centerLon, zoom, true,
                               centerTX, centerTY, &cx, &cy);
        if (!firstPt)
            traceDrawLine(prevSX, prevSY, cx, cy, 0x00, 0x55, 0xFF);
    }
}

// Own trace rendering — purple, same as firmware (0x9933FF).
static void drawOwnTrace() {
    if (ownTraceCount < 2) return;
    uint32_t now = millis();
    int prevSX = INT_MIN, prevSY = INT_MIN;
    bool firstPt = true;
    // Pixel skip threshold (same as firmware) — drops jitter at low zoom.
    int minDist2 = 0;
    if (zoom <= 10)      minDist2 = 144;
    else if (zoom <= 12) minDist2 = 36;
    else if (zoom <= 14) minDist2 = 9;

    for (int i = 0; i < ownTraceCount; i++) {
        int idx = (ownTraceHead - ownTraceCount + i + OWN_TRACE_MAX) % OWN_TRACE_MAX;
        if (now - ownTrace[idx].time > 3600000) continue; // 1h TTL
        int sx, sy;
        MapMath::latLonToPixel(ownTrace[idx].lat, ownTrace[idx].lon,
                               centerLat, centerLon, zoom, true,
                               centerTX, centerTY, &sx, &sy);
        if (!firstPt) {
            int dx = sx - prevSX, dy = sy - prevSY;
            if (minDist2 == 0 || dx*dx + dy*dy >= minDist2)
                traceDrawLine(prevSX, prevSY, sx, sy, 0x99, 0x33, 0xFF);
        }
        prevSX = sx; prevSY = sy;
        firstPt = false;
    }
    // Line to current GPS position
    if (gpsLat != 0.0 || gpsLon != 0.0) {
        int cx, cy;
        MapMath::latLonToPixel((float)gpsLat, (float)gpsLon,
                               centerLat, centerLon, zoom, true,
                               centerTX, centerTY, &cx, &cy);
        if (!firstPt)
            traceDrawLine(prevSX, prevSY, cx, cy, 0x99, 0x33, 0xFF);
    }
}

void create(lv_obj_t *parent) {
    if (traceCanvas || !parent) return;
    traceBuf = (uint8_t *)lv_malloc(LV_CANVAS_BUF_SIZE(TRACE_CANVAS_W, TRACE_CANVAS_H, 32, LV_DRAW_BUF_STRIDE_ALIGN));
    traceCanvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(traceCanvas, traceBuf, TRACE_CANVAS_W, TRACE_CANVAS_H, LV_COLOR_FORMAT_ARGB8888);
    lv_obj_set_pos(traceCanvas, (CONT_W - SPRITE_SIZE) / 2, (MAP_H - SPRITE_SIZE) / 2);
    lv_obj_set_size(traceCanvas, TRACE_CANVAS_W, MAP_H);
    lv_obj_clear_flag(traceCanvas, LV_OBJ_FLAG_CLICKABLE);
}

void destroy() {
    if (traceCanvas && lv_obj_is_valid(traceCanvas)) lv_obj_del(traceCanvas);
    traceCanvas = nullptr;
    if (traceBuf) { lv_free(traceBuf); traceBuf = nullptr; }
    ownTraceCount = 0;
    ownTraceHead = 0;
}

void redraw() {
    if (!traceBuf) return;
    memset(traceBuf, 0, TRACE_CANVAS_W * TRACE_CANVAS_H * 4);
    drawStationsTrace();
    drawOwnTrace();
    if (traceCanvas && lv_obj_is_valid(traceCanvas))
        lv_obj_invalidate(traceCanvas);
}

void recordOwnPosition() {
    if (gpsLat == 0.0 && gpsLon == 0.0) return;
    if (ownTraceCount > 0) {
        int lastIdx = (ownTraceHead - 1 + OWN_TRACE_MAX) % OWN_TRACE_MAX;
        float dlat = (float)(gpsLat - ownTrace[lastIdx].lat);
        float dlon = (float)(gpsLon - ownTrace[lastIdx].lon);
        if (dlat*dlat + dlon*dlon < 0.000001f) return; // ~10m threshold
    }
    ownTrace[ownTraceHead] = {(float)gpsLat, (float)gpsLon, millis()};
    ownTraceHead = (ownTraceHead + 1) % OWN_TRACE_MAX;
    if (ownTraceCount < OWN_TRACE_MAX) ownTraceCount++;
}

void reposition() {
    if (!traceCanvas || !lv_obj_is_valid(traceCanvas)) return;
    int mapH = fullscreenMap ? 600 : MAP_H;
    lv_obj_set_pos(traceCanvas,
                   (CONT_W - SPRITE_SIZE) / 2 + dragAccumX,
                   (mapH - SPRITE_SIZE) / 2 + dragAccumY);
    lv_obj_set_size(traceCanvas, TRACE_CANVAS_W, mapH);
}

bool isReady() { return traceCanvas != nullptr; }

void moveToForeground() {
    if (traceCanvas && lv_obj_is_valid(traceCanvas))
        lv_obj_move_foreground(traceCanvas);
}

} // namespace MapTraces
