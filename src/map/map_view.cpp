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
#ifdef WITH_MAPLIBRE
#include "maplibre_display.h"
#endif
#include <climits>
#include <cstdint>
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

#ifdef WITH_MAPLIBRE
static lv_obj_t *gpuScreen = nullptr;
static lv_obj_t *gpuTouch = nullptr;
static lv_timer_t *gpuMarkerTimer = nullptr;
static lv_obj_t *gpuMarkers[MAP_STATIONS_MAX + 1] = {};
static bool gpuFollowGps = true;
static void refreshGpuMarkers();
#endif

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
#ifdef WITH_MAPLIBRE
  if (MaplibreDisplay::isActive()) {
    if (gpuFollowGps) MaplibreDisplay::setCenter(lat, lon);
    refreshGpuMarkers();
    return;
  }
#endif
  MapTraces::recordOwnPosition();
  if (!mapActive || !mapCont)
    return;
  // Redraw the dynamic layer (own marker + trace) over the cached tiles.
  MapEngine::recompose();
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
#ifdef WITH_MAPLIBRE
  if (MaplibreDisplay::isActive()) {
    refreshGpuMarkers();
    return;
  }
#endif
  if (mapActive && mapCont)
    MapEngine::recompose();
}

#ifdef WITH_MAPLIBRE
static void gpuMarkerClicked(lv_event_t *e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  MapMarkers::showStationPopup(idx);
}

static lv_obj_t *ensureGpuMarker(int slot, int stationIdx, const char *callsign,
                                lv_color_t color) {
  if (slot < 0 || slot > MAP_STATIONS_MAX || !gpuScreen) return nullptr;
  lv_obj_t *marker = gpuMarkers[slot];
  if (!marker || !lv_obj_is_valid(marker)) {
    marker = lv_btn_create(gpuScreen);
    gpuMarkers[slot] = marker;
    lv_obj_set_size(marker, 92, 38);
    lv_obj_set_style_radius(marker, 19, 0);
    lv_obj_set_style_border_width(marker, 1, 0);
    lv_obj_set_style_border_color(marker, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_opa(marker, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(marker, 3, 0);
    lv_obj_add_event_cb(marker, gpuMarkerClicked, LV_EVENT_CLICKED,
                        (void *)(intptr_t)stationIdx);
    lv_obj_t *label = lv_label_create(marker);
    lv_obj_set_style_text_color(label, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
    lv_obj_center(label);
  }
  lv_obj_set_style_bg_color(marker, color, 0);
  lv_obj_t *label = lv_obj_get_child(marker, 0);
  if (label) lv_label_set_text(label, callsign);
  return marker;
}

static void positionGpuMarker(int slot, int stationIdx, const char *callsign,
                              double lat, double lon, lv_color_t color) {
  int x = 0, y = 0;
  bool visible = MaplibreDisplay::project(lat, lon, &x, &y);
  lv_obj_t *marker = gpuMarkers[slot];
  if (!visible) {
    if (marker && lv_obj_is_valid(marker)) lv_obj_add_flag(marker, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  marker = ensureGpuMarker(slot, stationIdx, callsign, color);
  if (!marker) return;
  lv_obj_remove_flag(marker, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_pos(marker, x - 46, y - 19);
}

static void refreshGpuMarkers() {
  if (!gpuScreen || !lv_obj_is_valid(gpuScreen) || !MaplibreDisplay::isActive()) return;
  bool used[MAP_STATIONS_MAX + 1] = {};
  if (gpsLat != 0.0 || gpsLon != 0.0) {
    const char *own = (!Config.beacons.empty())
                          ? Config.beacons[myBeaconsIndex].callsign.c_str()
                          : "OWN";
    positionGpuMarker(0, -1, own, gpsLat, gpsLon, lv_color_hex(0x0055cc));
    used[0] = true;
  }
  for (int i = 0; i < MAP_STATIONS_MAX; ++i) {
    MapStation *st = STATION_Utils::getMapStation(i);
    if (!st || !st->valid || (st->latitude == 0.0f && st->longitude == 0.0f)) continue;
    uint32_t age = millis() - st->lastTime;
    lv_color_t color = age < 10 * 60 * 1000 ? lv_color_hex(0xff6600)
                                             : lv_color_hex(0x777777);
    positionGpuMarker(i + 1, i, st->callsign.c_str(), st->latitude, st->longitude, color);
    used[i + 1] = true;
  }
  for (int i = 0; i <= MAP_STATIONS_MAX; ++i) {
    if (!used[i] && gpuMarkers[i] && lv_obj_is_valid(gpuMarkers[i]))
      lv_obj_add_flag(gpuMarkers[i], LV_OBJ_FLAG_HIDDEN);
  }
}

static void gpuTouchCb(lv_event_t *e) {
  static lv_point_t last{};
  lv_indev_t *indev = lv_indev_get_act();
  if (!indev) return;
  lv_point_t p{};
  lv_indev_get_point(indev, &p);
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_PRESSED) {
    last = p;
    MapMarkers::closeStationPopup();
  } else if (code == LV_EVENT_PRESSING) {
    int dx = p.x - last.x, dy = p.y - last.y;
    last = p;
    if (dx || dy) {
      gpuFollowGps = false;
      MaplibreDisplay::moveBy(dx, dy);
      refreshGpuMarkers();
    }
  }
}

static void gpuScreenDeleted(lv_event_t *) {
  if (gpuMarkerTimer) { lv_timer_del(gpuMarkerTimer); gpuMarkerTimer = nullptr; }
  MapMarkers::closeStationPopup();
  gpuScreen = nullptr;
  gpuTouch = nullptr;
  memset(gpuMarkers, 0, sizeof(gpuMarkers));
}

// Transparent map screen for the GPU path. MapLibre renders underneath every
// frame (renderTick); here we only add the chrome, whose buttons drive the
// MapLibre camera. No MapEngine/MapLabels/MapInput.
lv_obj_t *createGpuOverlay(lv_obj_t *parent) {
  (void)parent;
  lv_obj_t *scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_opa(scr, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(scr, 0, 0);
  lv_obj_set_style_pad_all(scr, 0, 0);
  gpuScreen = scr;
  gpuFollowGps = true;
  lv_obj_add_event_cb(scr, gpuScreenDeleted, LV_EVENT_DELETE, nullptr);

  // Dedicated drag surface below the toolbar.  It never overlaps the command
  // buttons, unlike the former full-screen touch layer.
  gpuTouch = lv_obj_create(scr);
  lv_obj_set_size(gpuTouch, lv_pct(100), 530);
  lv_obj_set_pos(gpuTouch, 0, 70);
  lv_obj_set_style_bg_opa(gpuTouch, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(gpuTouch, 0, 0);
  lv_obj_set_style_pad_all(gpuTouch, 0, 0);
  lv_obj_clear_flag(gpuTouch, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(gpuTouch, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(gpuTouch, gpuTouchCb, LV_EVENT_PRESSED, nullptr);
  lv_obj_add_event_cb(gpuTouch, gpuTouchCb, LV_EVENT_PRESSING, nullptr);

  lv_obj_t *tbar = lv_obj_create(scr);
  lv_obj_set_size(tbar, lv_pct(100), 70);
  lv_obj_align(tbar, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_bg_color(tbar, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(tbar, LV_OPA_40, 0);
  lv_obj_set_style_border_width(tbar, 0, 0);
  lv_obj_set_style_radius(tbar, 0, 0);
  lv_obj_set_style_pad_all(tbar, 8, 0);
  lv_obj_set_flex_flow(tbar, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(tbar, LV_FLEX_ALIGN_SPACE_EVENLY,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  auto addBtn = [&](const char *sym, lv_event_cb_t cb) {
    lv_obj_t *b = lv_btn_create(tbar);
    lv_obj_set_size(b, 140, 52);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x16213e), 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, sym);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_PRESSED, nullptr);
  };
  addBtn(LV_SYMBOL_LEFT,  [](lv_event_t *) { UIDashboard::returnToDashboard(); });
  addBtn(LV_SYMBOL_PLUS,  [](lv_event_t *) { MaplibreDisplay::setZoom(MaplibreDisplay::getZoom() + 1); });
  addBtn(LV_SYMBOL_MINUS, [](lv_event_t *) { MaplibreDisplay::setZoom(MaplibreDisplay::getZoom() - 1); });
  addBtn(LV_SYMBOL_GPS,   [](lv_event_t *) {
    gpuFollowGps = true;
    MaplibreDisplay::setCenter(gpsLat, gpsLon);
    refreshGpuMarkers();
  });
  refreshGpuMarkers();
  gpuMarkerTimer = lv_timer_create([](lv_timer_t *) { refreshGpuMarkers(); }, 250, nullptr);
  lv_obj_move_foreground(tbar);
  return scr;
}
#endif

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
  // centerLat/centerLon only snap to tile origin (updated when a tile
  // boundary is crossed during pan). Compute the actual lat/lon under
  // the screen center from the current dragAccum offset — same pattern
  // as MapEngine::recenterForZoom().
  int spriteCX = SPRITE_SIZE / 2 - dragAccumX;
  int spriteCY = SPRITE_SIZE / 2 - dragAccumY;
  float lat = centerLat, lon = centerLon;
  MapMath::pixelToLatLon(spriteCX, spriteCY, zoom, true,
                         centerTX, centerTY, 0, 0, &lat, &lon);
  char buf[128];
  snprintf(buf, sizeof(buf), "Lat:%.4f  Lon:%.4f  Stn:%d",
           lat, lon, mapStationsCount);
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

  // Title bar
  lv_obj_t *tbar = lv_obj_create(scr);
  tbarMap = tbar;
  lv_obj_set_size(tbar, CONT_W, 45);
  lv_obj_set_pos(tbar, 0, 0);
  lv_obj_set_style_bg_color(tbar, lv_color_hex(0x009933), 0);
  lv_obj_set_style_border_width(tbar, 0, 0);
  lv_obj_set_style_radius(tbar, 0, 0);
  lv_obj_set_style_pad_all(tbar, 5, 0);

  // Back button (wider than icon buttons)
  lv_obj_t *btnBack = lv_btn_create(tbar);
  lv_obj_set_size(btnBack, 100, 32);
  lv_obj_set_style_bg_color(btnBack, lv_color_hex(0x16213e), 0);
  lv_obj_align(btnBack, LV_ALIGN_LEFT_MID, 5, 0);
  lv_obj_add_event_cb(btnBack, backCb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *bl = lv_label_create(btnBack);
  lv_label_set_text(bl, "< BACK");
  lv_obj_center(bl);

  // GPS recenter + follow toggle (blue when following, orange when not)
  btnRecenter = lv_btn_create(tbar);
  lv_obj_set_size(btnRecenter, 50, 32);
  lv_obj_align(btnRecenter, LV_ALIGN_RIGHT_MID, -195, 0);
  lv_obj_set_style_bg_color(btnRecenter, mapFollowGps ? lv_color_hex(0x16213e) : lv_color_hex(0xff6600), 0);
  lv_obj_set_style_bg_color(btnRecenter, lv_color_hex(0xff6600), LV_STATE_PRESSED);
  lv_obj_add_event_cb(btnRecenter, [](lv_event_t *) {
    // Visual feedback while reloadTiles is blocking (same pattern as Z+/Z-).
    lv_obj_set_style_bg_color(btnRecenter, lv_color_hex(0xff6600), 0);
    lv_obj_invalidate(btnRecenter);
    lv_refr_now(NULL);
    mapFollowGps = true;
    if (gpsLat != 0.0 || gpsLon != 0.0) {
      printf("[map] recenter -> GPS %.5f,%.5f\n", gpsLat, gpsLon);
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
    } else {
      printf("[map] recenter : no GPS fix (gpsLat=gpsLon=0), follow=ON but no jump\n");
    }
    // Back to "following GPS" colour (blue).
    lv_obj_set_style_bg_color(btnRecenter, lv_color_hex(0x16213e), 0);
    lv_obj_invalidate(btnRecenter);
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

  // GPX record toggle (rightmost, orange when recording)
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

  // Title "MAP (Zxx)"
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

  // Single composited map canvas owned by map_engine (tiles + overlays).
  MapEngine::init(mapCont);
  MapEngine::setLabels(titleLabel, infoLabel);

  // Waterway label scratch canvas (hidden, off-screen).
  MapLabels::create(mapCont);

  mapActive = true;
  MapEngine::reloadTiles(); // composite tiles + labels + traces + markers

  // Start 50ms periodic timer (inertia, GPS follow, station refresh)
  if (!mapTimer) mapTimer = lv_timer_create([](lv_timer_t *) { MapEngine::timerTick(); }, 50, NULL);

  return scr;
}

} // namespace MapView
