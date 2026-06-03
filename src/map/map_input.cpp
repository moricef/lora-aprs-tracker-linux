#include "map/map_input.h"
#include "map/map_engine.h"
#include "map/map_markers.h"
#include "map/map_state.h"
#include "map_coordinate_math.h"
#include "station_utils.h"
#include "ui_messaging.h"

#include "Arduino.h"  // millis()

#include <cmath>
#include <cstdlib>

namespace MapInput {

using namespace MapState;

// Drag carrier (last known position + timestamp). Velocity itself lives
// in MapEngine (read by timerTick to roll out inertia).
static lv_point_t dragLast;
static uint32_t   dragLastMs = 0;

static void touchCb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    // Track press start for manual hit-test on release
    static lv_point_t pressPt;
    static uint32_t   pressMs;

    if (code == LV_EVENT_DOUBLE_CLICKED) {
        MapView::toggleFullscreen();
        return;
    }

    if (code == LV_EVENT_PRESSED) {
        pressPt = p;
        pressMs = millis();
        dragLast = p;
        dragLastMs = millis();
        MapEngine::panActive = true;
        MapEngine::velX = MapEngine::velY = 0.0f;
        MapMarkers::closeStationPopup();
    } else if (code == LV_EVENT_PRESSING && MapEngine::panActive) {
        int dx = p.x - dragLast.x, dy = p.y - dragLast.y;
        uint32_t now = millis();
        uint32_t dt = now - dragLastMs;
        dragLast = p;
        dragLastMs = now;
        dragAccumX += dx;
        dragAccumY += dy;

        // Disable GPS follow on drag (firmware behaviour)
        if ((dx != 0 || dy != 0) && mapFollowGps) {
            mapFollowGps = false;
            MapView::markFollowGpsDisabled();
        }

        // Track velocity for inertia — same sign convention as dragAccum
        // (+dx = move right). Weighted average smooths jitter at end of drag.
        if (dt > 0) {
            float weight = 0.7f;
            float instVelX = (float)dx / (float)dt;
            float instVelY = (float)dy / (float)dt;
            MapEngine::velX = MapEngine::velX * (1.0f - weight) + instVelX * weight;
            MapEngine::velY = MapEngine::velY * (1.0f - weight) + instVelY * weight;
        }

        // Central tile offset
        static int lctx = 0, lcty = 0;
        if (dragAccumX >= TILE_SIZE) { dragAccumX -= TILE_SIZE; centerTX--; }
        if (dragAccumX <= -TILE_SIZE) { dragAccumX += TILE_SIZE; centerTX++; }
        if (dragAccumY >= TILE_SIZE) { dragAccumY -= TILE_SIZE; centerTY--; }
        if (dragAccumY <= -TILE_SIZE) { dragAccumY += TILE_SIZE; centerTY++; }

        // Reposition tile grid + every overlay (engine knows the layout).
        MapEngine::repositionAll();

        if (centerTX != lctx || centerTY != lcty) {
            lctx = centerTX;
            lcty = centerTY;
            MapMath::tileToLatLon(centerTX, centerTY, zoom,
                                  (float *)&centerLat, (float *)&centerLon);
            MapEngine::reloadTiles();
        }
    } else if (code == LV_EVENT_RELEASED) {
        int dx = p.x - pressPt.x, dy = p.y - pressPt.y;
        bool wasPan = MapEngine::panActive && (abs(dx) > 10 || abs(dy) > 10);
        MapEngine::panActive = false;

        if (wasPan) {
            MapView::refreshInfoBar();
            return;
        }

        // No significant drag — check manual hit-test on markers.
        // Long-press on a known station → open compose; short tap → info popup.
        uint32_t held = millis() - pressMs;
        int hitIdx = -2;
        if (MapMarkers::hitTest(pressPt, &hitIdx)) {
            if (held > 400) {
                if (hitIdx >= 0) {
                    MapStation *st = STATION_Utils::getMapStation(hitIdx);
                    if (st && st->valid) {
                        MapMarkers::closeStationPopup();
                        UIMessaging::openComposeWithCallsign(st->callsign);
                    }
                }
            } else {
                MapMarkers::showStationPopup(hitIdx);
            }
        }
    }
}

void install(lv_obj_t *target) {
    if (!target) return;
    // Attach unconditionally : every map open recreates the container, so
    // the previous registration died with it. The caller is expected to
    // invoke install() exactly once per create() — no idempotency guard
    // here because that guard outlived the destroyed mapCont and silently
    // dropped pan after the second map open.
    lv_obj_add_event_cb(target, touchCb, LV_EVENT_DOUBLE_CLICKED, NULL);
    lv_obj_add_event_cb(target, touchCb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(target, touchCb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(target, touchCb, LV_EVENT_RELEASED, NULL);
}

} // namespace MapInput
