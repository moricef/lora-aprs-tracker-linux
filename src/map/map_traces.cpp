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

// Own GPS trace ring buffer.
#define OWN_TRACE_MAX 200
static TracePoint ownTrace[OWN_TRACE_MAX];
static int ownTraceCount = 0;
static int ownTraceHead = 0;

// Overlay canvas (ARGB8888, sized SPRITE_SIZE × map-height).
static lv_obj_t *traceCanvas = nullptr;
static uint8_t  *traceBuf    = nullptr;
constexpr int TRACE_CANVAS_W = SPRITE_SIZE;
constexpr int TRACE_CANVAS_H = 600;

// Antialiased line on the trace canvas. lv_draw_line is deferred: the
// dsc is read at flush time, after the local has gone out of scope.
// Queuing several lines on a single layer leaves only the last one
// valid. We commit each line by closing and reopening the layer.
static void drawAALine(lv_obj_t *canvas, int x0, int y0, int x1, int y1,
                       uint32_t color, int width) {
    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);
    lv_draw_line_dsc_t d; lv_draw_line_dsc_init(&d);
    d.color = lv_color_hex(color);
    d.width = width;
    d.opa = LV_OPA_COVER;
    d.p1.x = x0; d.p1.y = y0;
    d.p2.x = x1; d.p2.y = y1;
    lv_draw_line(&layer, &d);
    lv_canvas_finish_layer(canvas, &layer);
}

// Skip points that fall way outside the sprite — saves the draw call
// when a polyline tail leaves the visible area at high zoom.
static inline bool spriteOutOfBounds(int px, int py) {
    return px < -SPRITE_SIZE || px > 2 * SPRITE_SIZE ||
           py < -SPRITE_SIZE || py > 2 * SPRITE_SIZE;
}

static void drawStationsTrace(lv_obj_t *canvas) {
    uint32_t now = millis();
    for (int s = 0; s < MAP_STATIONS_MAX; s++) {
        MapStation *st = STATION_Utils::getMapStation(s);
        if (!st || !st->valid || st->traceCount < 1) continue;

        int prevSX = INT_MIN, prevSY = INT_MIN;
        bool firstPt = true;
        for (int i = 0; i < st->traceCount; i++) {
            int idx = (st->traceHead - st->traceCount + i + TRACE_MAX_POINTS) % TRACE_MAX_POINTS;
            uint32_t age = now - st->trace[idx].time;
            if (age > 3600000) continue; // 1h TTL
            int sx, sy;
            MapMath::latLonToPixel(st->trace[idx].lat, st->trace[idx].lon,
                                   centerLat, centerLon, zoom, true,
                                   centerTX, centerTY, &sx, &sy);
            if (spriteOutOfBounds(sx, sy)) continue;
            if (!firstPt)
                drawAALine(canvas, prevSX, prevSY, sx, sy, 0x0055FF, 2);
            prevSX = sx; prevSY = sy;
            firstPt = false;
        }
        int cx, cy;
        MapMath::latLonToPixel(st->latitude, st->longitude,
                               centerLat, centerLon, zoom, true,
                               centerTX, centerTY, &cx, &cy);
        if (!firstPt && !spriteOutOfBounds(cx, cy))
            drawAALine(canvas, prevSX, prevSY, cx, cy, 0x0055FF, 2);
    }
}

// Own trace: purple 0x9933FF, width=2 AA, no TTL.
static void drawOwnTrace(lv_obj_t *canvas) {
    if (ownTraceCount < 2) return;

    int minPixDist2 = 0;
    if (zoom <= 10)      minPixDist2 = 144;
    else if (zoom <= 12) minPixDist2 = 36;
    else if (zoom <= 14) minPixDist2 = 9;

    int prevSX = INT_MIN, prevSY = INT_MIN;
    bool firstPt = true;
    for (int i = 0; i < ownTraceCount; i++) {
        int idx = (ownTraceHead - ownTraceCount + i + OWN_TRACE_MAX) % OWN_TRACE_MAX;
        int sx, sy;
        MapMath::latLonToPixel(ownTrace[idx].lat, ownTrace[idx].lon,
                               centerLat, centerLon, zoom, true,
                               centerTX, centerTY, &sx, &sy);
        // Jitter filter exempts the last stored point — always draw the final segment.
        if (!firstPt && i < ownTraceCount - 1 && minPixDist2 > 0) {
            int dx = sx - prevSX, dy = sy - prevSY;
            if (dx*dx + dy*dy < minPixDist2) continue;
        }
        if (!firstPt)
            drawAALine(canvas, prevSX, prevSY, sx, sy, 0x9933FF, 2);
        prevSX = sx; prevSY = sy;
        firstPt = false;
    }
    if (gpsLat != 0.0 || gpsLon != 0.0) {
        int cx, cy;
        MapMath::latLonToPixel((float)gpsLat, (float)gpsLon,
                               centerLat, centerLon, zoom, true,
                               centerTX, centerTY, &cx, &cy);
        if (!firstPt)
            drawAALine(canvas, prevSX, prevSY, cx, cy, 0x9933FF, 2);
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
    if (!traceCanvas || !lv_obj_is_valid(traceCanvas)) return;
    drawStationsTrace(traceCanvas);
    drawOwnTrace(traceCanvas);
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
