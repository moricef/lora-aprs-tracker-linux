/* Map screen — owns the LVGL screen widgets (title bar, info bar, buttons,
 * container) + lifecycle (back, zoom, GPS recenter, fullscreen, GPX toggle).
 * Tile rendering, overlays and gestures live in their dedicated modules
 * (map_engine, map_traces, map_labels, map_markers, map_input).
 */
#include "map/map_view.h"
#include "configuration.h"
#include "gps_math.h"
#include "map_coordinate_math.h"
#include "map/map_io.h"
#include "map/map_markers.h"
#include "map/map_state.h"
#include "map/map_engine.h"
#include "map/map_input.h"
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

namespace MapView {

using namespace MapState;

// Tile grid + inertia in map_engine.cpp ; touch handler in map_input.cpp.
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
  // Paint the pressed button orange, flush a frame so the user sees it,
  // then run the (synchronous, slow) zoom which blocks the UI thread
  // while tiles re-render. When zoom returns, restore the dark bg.
  lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0xff6600), 0);
  lv_obj_invalidate(btn);
  lv_refr_now(NULL);
  int d = (int)(intptr_t)lv_event_get_user_data(e);
  if (d > 0) zoomIn(); else zoomOut();
  lv_obj_set_style_bg_color(btn, lv_color_hex(0x16213e), 0);
  lv_obj_invalidate(btn);
}

// Tile reposition lives in MapEngine::repositionAll().

static lv_obj_t *tbarMap = nullptr;
static lv_obj_t *ibarMap = nullptr;

void toggleFullscreen() {
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
  // Same reason as the Z+/Z- buttons: under tile-render load the next
  // refresh may not happen for a while, making the toggle feel laggy.
  // Flush a frame synchronously so the new layout is on screen before
  // we return from the double-click handler.
  lv_refr_now(NULL);
}

void markFollowGpsDisabled() {
  if (btnRecenter)
    lv_obj_set_style_bg_color(btnRecenter, lv_color_hex(0xff6600), 0);
}

void refreshInfoBar() {
  if (!infoLabel) return;
  char buf[128];
  snprintf(buf, sizeof(buf), "Lat:%.4f  Lon:%.4f  Stn:%d",
           centerLat, centerLon, mapStationsCount);
  lv_label_set_text(infoLabel, buf);
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

  // Orange feedback during zoom: zoomCb paints the button orange,
  // flushes a frame, runs the (blocking) zoom, then restores the dark
  // bg. The orange stays on screen for the whole reload duration —
  // exactly what the user sees as "the button works".

  // Zoom +
  lv_obj_t *zp = lv_btn_create(tbar);
  lv_obj_set_size(zp, 50, 32);
  lv_obj_align(zp, LV_ALIGN_RIGHT_MID, -140, 0);
  lv_obj_set_style_bg_color(zp, lv_color_hex(0x16213e), 0);
  lv_obj_add_event_cb(zp, zoomCb, LV_EVENT_RELEASED, (void *)1);
  lv_obj_t *zlp = lv_label_create(zp);
  lv_label_set_text(zlp, "+");
  lv_obj_center(zlp);

  // Zoom -
  lv_obj_t *zm = lv_btn_create(tbar);
  lv_obj_set_size(zm, 50, 32);
  lv_obj_align(zm, LV_ALIGN_RIGHT_MID, -85, 0);
  lv_obj_set_style_bg_color(zm, lv_color_hex(0x16213e), 0);
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
  MapInput::install(mapCont);

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

} // namespace MapView
