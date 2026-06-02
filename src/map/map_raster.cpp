/* Raster tile map — ESP32 create_map_screen() + region discovery ported to
 * Linux APRS Stations: icons positioned on tiles, tap → info popup
 */
#include "map_raster.h"
#include "configuration.h"
#include "gps_math.h"
#include "map_coordinate_math.h"
#include "map/map_io.h"
#include "map/map_markers.h"
#include "map/map_state.h"
#include "map/map_engine.h"
#include "map/map_labels.h"
#include "map/map_traces.h"
#include "map_vector.h"
#include "gps_utils.h"
#include "station_utils.h"
#include "ui_dashboard.h"
#include "ui_messaging.h"
#include "gpx_writer.h"
#include <climits>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <sys/stat.h>

extern Configuration Config;
extern uint8_t myBeaconsIndex;

namespace MapRaster {

using namespace MapState;

// Tile grid / inertia state lives in map_engine.cpp.
static lv_point_t dragLast;
static uint32_t dragLastMs = 0;
static lv_timer_t *mapTimer = nullptr;

// LVGL objects
static lv_obj_t *titleLabel = nullptr;
static lv_obj_t *infoLabel = nullptr;
static lv_obj_t *mapCont = nullptr;
static lv_obj_t *btnRecenter = nullptr;

// Station markers and popup live in map_markers.cpp.
// Region / zoom discovery + tile existence lookup live in map_io.cpp.


// Tile reload + zoom + inertia/follow timer live in map_engine.cpp.
// Label overlay lives in map_labels.cpp.

// ============================================================
// API publique
// ============================================================
void setPosition(double lat, double lon) {
  gpsLat = lat;
  gpsLon = lon;
  MapTraces::recordOwnPosition();
  if (!mapActive || !mapCont)
    return;
  // Cheap path: move the existing own-marker in place. Fall back to a full
  // (re)create only if there is no own-marker yet — typically the first
  // GPS fix after the map screen opens.
  if (!MapMarkers::updateOwnMarker())
    MapMarkers::createMarkers();
}

void zoomIn() {
  if (btnRecenter) lv_obj_set_style_bg_color(btnRecenter, lv_color_hex(0xff6600), 0);
  MapEngine::zoomIn();
}

void zoomOut() {
  if (btnRecenter) lv_obj_set_style_bg_color(btnRecenter, lv_color_hex(0xff6600), 0);
  MapEngine::zoomOut();
}

void refreshStations() {
  if (mapActive && mapCont)
    MapMarkers::createMarkers();
}

// ============================================================
// Callbacks boutons & pan
// ============================================================
static void backCb(lv_event_t *) {
  MapMarkers::closeStationPopup();
  mapActive = false;
  if (mapTimer) { lv_timer_del(mapTimer); mapTimer = nullptr; }
  MapMarkers::deleteMarkers();
  MapLabels::destroy();
  MapTraces::destroy();
  MapEngine::destroy();
  UIDashboard::returnToDashboard();
}

static void zoomCb(lv_event_t *e) {
  MapMarkers::closeStationPopup();
  int d = (int)(intptr_t)lv_event_get_user_data(e);
  if (d > 0) zoomIn(); else zoomOut();
}

// Tile reposition lives in MapEngine::repositionAll().

static lv_obj_t *tbarMap = nullptr;
static lv_obj_t *ibarMap = nullptr;

static void toggleMapFullscreen() {
  fullscreenMap = !fullscreenMap;
  if (fullscreenMap) {
    lv_obj_add_flag(tbarMap, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ibarMap, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(mapCont, CONT_W, 600);
    lv_obj_set_pos(mapCont, 0, 0);
  } else {
    lv_obj_clear_flag(tbarMap, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ibarMap, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(mapCont, CONT_W, MAP_H);
    lv_obj_set_pos(mapCont, 0, 45);
  }
  MapEngine::repositionAll();
}

static void mapTouchCB(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  lv_indev_t *indev = lv_indev_get_act();
  if (!indev) return;
  lv_point_t p;
  lv_indev_get_point(indev, &p);

  // Track press start for manual hit-test on release
  static lv_point_t pressPt;
  static uint32_t  pressMs;

  if (code == LV_EVENT_DOUBLE_CLICKED) {
    toggleMapFullscreen();
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
      if (btnRecenter) lv_obj_set_style_bg_color(btnRecenter, lv_color_hex(0xff6600), 0);
    }

    // Track velocity for inertia — same sign convention as dragAccum (+dx = move right)
    if (dt > 0) {
      float weight = 0.7f;
      float instVelX = (float)dx / (float)dt;
      float instVelY = (float)dy / (float)dt;
      MapEngine::velX = MapEngine::velX * (1.0f - weight) + instVelX * weight;
      MapEngine::velY = MapEngine::velY * (1.0f - weight) + instVelY * weight;
    }

    // Central tile offset
    static int lctx = 0, lcty = 0;
    if (dragAccumX >= TILE_SIZE) {
      dragAccumX -= TILE_SIZE;
      centerTX--;
    }
    if (dragAccumX <= -TILE_SIZE) {
      dragAccumX += TILE_SIZE;
      centerTX++;
    }
    if (dragAccumY >= TILE_SIZE) {
      dragAccumY -= TILE_SIZE;
      centerTY--;
    }
    if (dragAccumY <= -TILE_SIZE) {
      dragAccumY += TILE_SIZE;
      centerTY++;
    }

    // Reposition tile grid + every overlay (engine knows the layout).
    MapEngine::repositionAll();

    if (centerTX != lctx || centerTY != lcty) {
      lctx = centerTX;
      lcty = centerTY;
      MapMath::tileToLatLon(centerTX, centerTY, zoom, (float *)&centerLat,
                            (float *)&centerLon);
      MapEngine::reloadTiles();
    }
  } else if (code == LV_EVENT_RELEASED) {
    int dx = p.x - pressPt.x, dy = p.y - pressPt.y;
    bool wasPan = MapEngine::panActive && (abs(dx) > 10 || abs(dy) > 10);
    MapEngine::panActive = false;

    if (wasPan) {
      if (infoLabel) {
        char buf[128];
        snprintf(buf, sizeof(buf), "Lat:%.4f  Lon:%.4f  Stn:%d", centerLat, centerLon, mapStationsCount);
        lv_label_set_text(infoLabel, buf);
      }
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

// ============================================================
// Create map screen
// ============================================================
lv_obj_t *create(lv_obj_t *) {
  MapIO::discoverRegion();
  MapIO::discoverZooms();
  MapIO::discoverDefaultPosition();
  MapVector::initLabelFonts();   // accented label font (OpenSans-Bold)

  lv_obj_t *scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x1a1a2e), 0);
  lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);

  // Title bar (green, same as firmware)
  lv_obj_t *tbar = lv_obj_create(scr);
  tbarMap = tbar;
  lv_obj_set_size(tbar, CONT_W, 45);
  lv_obj_set_pos(tbar, 0, 0);
  lv_obj_set_style_bg_color(tbar, lv_color_hex(0x009933), 0);
  lv_obj_set_style_border_width(tbar, 0, 0);
  lv_obj_set_style_radius(tbar, 0, 0);
  lv_obj_set_style_pad_all(tbar, 5, 0);

  // Back button (wider than icon buttons, same as firmware)
  lv_obj_t *btnBack = lv_btn_create(tbar);
  lv_obj_set_size(btnBack, 100, 32);
  lv_obj_set_style_bg_color(btnBack, lv_color_hex(0x16213e), 0);
  lv_obj_align(btnBack, LV_ALIGN_LEFT_MID, 5, 0);
  lv_obj_add_event_cb(btnBack, backCb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *bl = lv_label_create(btnBack);
  lv_label_set_text(bl, "< BACK");
  lv_obj_center(bl);

  // GPS recenter + follow toggle (firmware: blue when following, orange when not)
  btnRecenter = lv_btn_create(tbar);
  lv_obj_set_size(btnRecenter, 50, 32);
  lv_obj_align(btnRecenter, LV_ALIGN_RIGHT_MID, -195, 0);
  lv_obj_set_style_bg_color(btnRecenter, mapFollowGps ? lv_color_hex(0x16213e) : lv_color_hex(0xff6600), 0);
  lv_obj_set_style_bg_color(btnRecenter, lv_color_hex(0xff6600), LV_STATE_PRESSED);
  lv_obj_add_event_cb(btnRecenter, [](lv_event_t *) {
    mapFollowGps = true;
    lv_obj_set_style_bg_color(btnRecenter, lv_color_hex(0x16213e), 0);
    if (gpsLat != 0.0 || gpsLon != 0.0) {
      MapMath::latLonToTile((float)gpsLat, (float)gpsLon, zoom, &centerTX, &centerTY);
      centerLat = gpsLat; centerLon = gpsLon;
      int spriteX, spriteY;
      MapMath::latLonToPixel((float)gpsLat, (float)gpsLon,
                             (float)gpsLat, (float)gpsLon,
                             zoom, true, centerTX, centerTY, &spriteX, &spriteY);
      dragAccumX = SPRITE_SIZE / 2 - spriteX;
      dragAccumY = SPRITE_SIZE / 2 - spriteY;
      MapEngine::velX = MapEngine::velY = 0.0f;
      MapEngine::reloadTiles();
    }
  }, LV_EVENT_RELEASED, NULL);
  lv_obj_t *lblRec = lv_label_create(btnRecenter);
  lv_label_set_text(lblRec, LV_SYMBOL_GPS);
  lv_obj_center(lblRec);

  // Zoom +  (firmware: LV_EVENT_RELEASED, orange on press)
  lv_obj_t *zp = lv_btn_create(tbar);
  lv_obj_set_size(zp, 50, 32);
  lv_obj_align(zp, LV_ALIGN_RIGHT_MID, -140, 0);
  lv_obj_set_style_bg_color(zp, lv_color_hex(0x16213e), 0);
  lv_obj_set_style_bg_color(zp, lv_color_hex(0xff6600), LV_STATE_PRESSED);
  lv_obj_add_event_cb(zp, zoomCb, LV_EVENT_RELEASED, (void *)1);
  lv_obj_t *zlp = lv_label_create(zp);
  lv_label_set_text(zlp, "+");
  lv_obj_center(zlp);

  // Zoom -  (firmware: LV_EVENT_RELEASED, orange on press)
  lv_obj_t *zm = lv_btn_create(tbar);
  lv_obj_set_size(zm, 50, 32);
  lv_obj_align(zm, LV_ALIGN_RIGHT_MID, -85, 0);
  lv_obj_set_style_bg_color(zm, lv_color_hex(0x16213e), 0);
  lv_obj_set_style_bg_color(zm, lv_color_hex(0xff6600), LV_STATE_PRESSED);
  lv_obj_add_event_cb(zm, zoomCb, LV_EVENT_RELEASED, (void *)-1);
  lv_obj_t *zlm = lv_label_create(zm);
  lv_label_set_text(zlm, "-");
  lv_obj_center(zlm);

  // GPX record toggle (rightmost, orange when recording — same as firmware)
  static lv_obj_t *btnGPX = nullptr;
  btnGPX = lv_btn_create(tbar);
  lv_obj_set_size(btnGPX, 50, 32);
  lv_obj_align(btnGPX, LV_ALIGN_RIGHT_MID, -30, 0);
  lv_obj_set_style_bg_color(btnGPX, GPXWriter::isRecording() ? lv_color_hex(0xff6600) : lv_color_hex(0x16213e), 0);
  lv_obj_add_event_cb(btnGPX, [](lv_event_t *) {
    if (GPXWriter::isRecording()) {
      GPXWriter::stopRecording();
      lv_obj_set_style_bg_color(btnGPX, lv_color_hex(0x16213e), 0);
    } else {
      bool ok = GPXWriter::startRecording(gpsFix.year, gpsFix.month, gpsFix.date,
                                          gpsFix.hours, gpsFix.minutes);
      if (ok)
        lv_obj_set_style_bg_color(btnGPX, lv_color_hex(0xff6600), 0);
    }
  }, LV_EVENT_CLICKED, NULL);
  lv_obj_t *lblGPX = lv_label_create(btnGPX);
  lv_label_set_text(lblGPX, "GPX");
  lv_obj_center(lblGPX);

  // Title "MAP (Zxx)" — center offset -30, same style as firmware
  titleLabel = lv_label_create(tbar);
  lv_label_set_text(titleLabel, "MAP");
  lv_obj_set_style_text_color(titleLabel, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_text_font(titleLabel, &lv_font_montserrat_18, 0);
  lv_obj_align(titleLabel, LV_ALIGN_CENTER, -30, 0);

  // Info bar (always created)
  lv_obj_t *ibar = lv_obj_create(scr);
  ibarMap = ibar;
  lv_obj_set_size(ibar, CONT_W, 30);
  lv_obj_set_pos(ibar, 0, 570);
  lv_obj_set_style_bg_color(ibar, lv_color_hex(0x16213e), 0);
  lv_obj_set_style_border_width(ibar, 0, 0);
  lv_obj_set_style_radius(ibar, 0, 0);
  lv_obj_set_style_pad_all(ibar, 2, 0);
  infoLabel = lv_label_create(ibar);
  char ib[128];
  snprintf(ib, sizeof(ib), "Lat:%.4f  Lon:%.4f  Stn:%d", centerLat, centerLon,
           mapStationsCount);
  lv_label_set_text(infoLabel, ib);
  lv_obj_set_style_text_color(infoLabel, lv_color_hex(0xaaaaaa), 0);
  lv_obj_set_style_text_font(infoLabel, &lv_font_montserrat_16, 0);
  lv_obj_center(infoLabel);

  if (!mapRegion[0]) {
    lv_obj_t *l = lv_label_create(scr);
    lv_label_set_text(l, "Tuiles map introuvables\n\nCopier les tuiles "
                         "dans\n/data/LoRa_Tracker/Maps/<region>/");
    lv_obj_set_style_text_color(l, lv_color_hex(0xff6b6b), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(l);
    return scr;
  }

  // Reset pan state
  dragAccumX = dragAccumY = 0;
  MapEngine::panActive = false;
  // Marker / popup state is reset implicitly by createMarkers() called from
  // MapEngine::reloadTiles() at the end of this function.

  MapMath::latLonToTile(centerLat, centerLon, zoom, &centerTX, &centerTY);

  // Title bar: update zoom label
  char z[16];
  snprintf(z, sizeof(z), "MAP (Z%d)", zoom);
  lv_label_set_text(titleLabel, z);

  // ---- Conteneur map ----
  mapCont = lv_obj_create(scr);
  MapMarkers::parent = mapCont;
  lv_obj_set_size(mapCont, CONT_W, MAP_H);
  lv_obj_set_pos(mapCont, 0, 45);
  lv_obj_set_style_bg_color(mapCont, lv_color_hex(0x2F4F4F), 0);
  lv_obj_set_style_border_width(mapCont, 0, 0);
  lv_obj_set_scrollbar_mode(mapCont, LV_SCROLLBAR_MODE_OFF);
  lv_obj_clear_flag(mapCont, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(mapCont, LV_OBJ_FLAG_PRESS_LOCK);
  lv_obj_remove_flag(mapCont, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_flag(mapCont, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(mapCont, mapTouchCB, LV_EVENT_DOUBLE_CLICKED, NULL);
  lv_obj_add_event_cb(mapCont, mapTouchCB, LV_EVENT_PRESSED, NULL);
  lv_obj_add_event_cb(mapCont, mapTouchCB, LV_EVENT_PRESSING, NULL);
  lv_obj_add_event_cb(mapCont, mapTouchCB, LV_EVENT_RELEASED, NULL);

  // Tile grid (5×5 raster + lazy vector canvases) owned by map_engine.
  MapEngine::init(mapCont);
  MapEngine::setLabels(titleLabel, infoLabel);

  // Trace canvas overlay for station movement lines
  MapTraces::create(mapCont);

  // Label overlay (single sprite-sized canvas + scratch for rotated waterways)
  MapLabels::create(mapCont);

  mapActive = true;
  MapEngine::reloadTiles(); // load tiles + create station markers

  // Start 50ms periodic timer (inertia, GPS follow, station refresh)
  if (!mapTimer) mapTimer = lv_timer_create([](lv_timer_t *) { MapEngine::timerTick(); }, 50, NULL);

  return scr;
}

} // namespace MapRaster
