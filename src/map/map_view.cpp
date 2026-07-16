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
#include "map/map_pinch.h"
#include "map_vector.h"
#include "gps_utils.h"
#include "station_utils.h"
#include "ui_dashboard.h"
#include "ui_messaging.h"
#include "gpx_writer.h"
#ifdef WITH_MAPLIBRE
#include "maplibre_display.h"
#endif
#include <algorithm>
#include <climits>
#include <cmath>
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
static lv_obj_t *tbarMap = nullptr;
static lv_obj_t *ibarMap = nullptr;

#ifdef WITH_MAPLIBRE
static lv_obj_t *gpuScreen = nullptr;
static lv_obj_t *gpuMapLayer = nullptr;
static lv_obj_t *gpuTouch = nullptr;
static lv_obj_t *gpuLoadingCover = nullptr;
static lv_timer_t *gpuMarkerTimer = nullptr;
static lv_obj_t *gpuMarkers[MAP_STATIONS_MAX + 1] = {};
static lv_obj_t *gpuTraceLines[MAP_STATIONS_MAX + 1] = {};
static lv_point_precise_t gpuTracePoints[MAP_STATIONS_MAX + 1][201] = {};
static lv_obj_t *gpuSpiderLines[MAP_STATIONS_MAX + 1] = {};
static lv_point_precise_t gpuSpiderPoints[MAP_STATIONS_MAX + 1][2] = {};
static lv_obj_t *gpuSpiderCenter = nullptr;
static int gpuMarkerAnchorX[MAP_STATIONS_MAX + 1] = {};
static int gpuMarkerAnchorY[MAP_STATIONS_MAX + 1] = {};
static int gpuMarkerCluster[MAP_STATIONS_MAX + 1] = {};
static int gpuMarkerClusterSize[MAP_STATIONS_MAX + 1] = {};
static bool gpuMarkerVisible[MAP_STATIONS_MAX + 1] = {};
static int gpuExpandedClusterSlot = -1;
static bool gpuFollowGps = true;
static bool gpuPanActive = false;
static bool gpuStationsDirty = false;
static float gpuVelX = 0.0f, gpuVelY = 0.0f;
static uint32_t gpuLastInertiaMs = 0;
static int gpuLongPressedStation = -2;
static bool gpuPinchActive = false;
static float gpuPinchDist0 = 0.0f;
static double gpuPinchZoom0 = 0.0;
static uint32_t gpuPinchEndMs = 0;
static bool gpuPanReseed = false;
static void refreshGpuMarkers();
static void refreshGpuTraces();
static void collapseGpuSpiderfy();
static int gpuMapOriginY() { return fullscreenMap ? 0 : 45; }
static void refreshGpuInfoBar() {
  if (!infoLabel) return;
  double lat = centerLat, lon = centerLon;
  MaplibreDisplay::getCenter(&lat, &lon);
  char text[128];
  snprintf(text, sizeof(text), "Lat:%.4f  Lon:%.4f  Stn:%d",
           lat, lon, mapStationsCount);
  if (strcmp(lv_label_get_text(infoLabel), text) != 0)
    lv_label_set_text(infoLabel, text);
}
static void setGpuZoom(int delta) {
  collapseGpuSpiderfy();
  gpuFollowGps = false;
  mapFollowGps = false;
  markFollowGpsDisabled();
  double target = MaplibreDisplay::getZoom() + delta;
  if (target < zoomMin) target = zoomMin;
  if (target > zoomMax) target = zoomMax;
  MaplibreDisplay::setZoom(target);
  zoom = (int)(target + 0.5);
  if (titleLabel) {
    char text[16];
    snprintf(text, sizeof(text), "MAP (Z%d)", zoom);
    lv_label_set_text(titleLabel, text);
  }
  refreshGpuMarkers();
}
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
  MapTraces::recordOwnPosition();
#ifdef WITH_MAPLIBRE
  if (MaplibreDisplay::isActive()) {
    if (gpuFollowGps) MaplibreDisplay::setCenter(lat, lon);
    refreshGpuMarkers();
    return;
  }
#endif
  if (!mapActive || !mapCont)
    return;
  // Redraw the dynamic layer (own marker + trace) over the cached tiles.
  MapEngine::recompose();
}

void zoomIn() {
  if (btnRecenter) lv_obj_set_style_bg_color(btnRecenter, lv_color_hex(0xff6600), 0);
#ifdef WITH_MAPLIBRE
  if (MaplibreDisplay::isActive()) { setGpuZoom(1); return; }
#endif
  MapEngine::zoomIn();
}

void zoomOut() {
  if (btnRecenter) lv_obj_set_style_bg_color(btnRecenter, lv_color_hex(0xff6600), 0);
#ifdef WITH_MAPLIBRE
  if (MaplibreDisplay::isActive()) { setGpuZoom(-1); return; }
#endif
  MapEngine::zoomOut();
}

void refreshStations() {
#ifdef WITH_MAPLIBRE
  if (MaplibreDisplay::isActive()) {
    // Defer station widget updates to the UI timer. Rebuilding every marker
    // while an RX frame is being parsed produces a long frame at exactly the
    // moment the dashboard and counters are updated.
    gpuStationsDirty = true;
    return;
  }
#endif
  if (mapActive && mapCont)
    MapEngine::recompose();
}

#ifdef WITH_MAPLIBRE
static void gpuMarkerClicked(lv_event_t *e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  const int slot = idx + 1;
  if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) {
    gpuLongPressedStation = idx;
    if (idx >= 0) {
      MapStation *st = STATION_Utils::getMapStation(idx);
      if (st && st->valid) {
        MapMarkers::closeStationPopup();
        UIMessaging::openComposeWithCallsign(st->callsign);
      }
    }
    return;
  }
  if (gpuLongPressedStation == idx) {
    gpuLongPressedStation = -2;
    return;
  }
  if (slot >= 0 && slot <= MAP_STATIONS_MAX &&
      gpuMarkerClusterSize[slot] > 1) {
    const bool alreadyExpanded = gpuExpandedClusterSlot >= 0 &&
        gpuMarkerCluster[gpuExpandedClusterSlot] == gpuMarkerCluster[slot];
    if (!alreadyExpanded) {
      gpuExpandedClusterSlot = slot;
      MapMarkers::closeStationPopup();
      refreshGpuMarkers();
      return;
    }
  }
  MapMarkers::showStationPopup(idx);
}

static void collapseGpuSpiderfy() {
  gpuExpandedClusterSlot = -1;
  if (gpuSpiderCenter && lv_obj_is_valid(gpuSpiderCenter))
    lv_obj_add_flag(gpuSpiderCenter, LV_OBJ_FLAG_HIDDEN);
  for (int slot = 0; slot <= MAP_STATIONS_MAX; ++slot) {
    lv_obj_t *line = gpuSpiderLines[slot];
    if (line && lv_obj_is_valid(line)) lv_obj_add_flag(line, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *marker = gpuMarkers[slot];
    if (gpuMarkerVisible[slot] && marker && lv_obj_is_valid(marker))
      lv_obj_set_pos(marker, gpuMarkerAnchorX[slot] - 46,
                     gpuMarkerAnchorY[slot] - 12);
  }
}

static lv_obj_t *ensureGpuSpiderLine(int slot) {
  if (slot < 0 || slot > MAP_STATIONS_MAX || !gpuMapLayer) return nullptr;
  lv_obj_t *line = gpuSpiderLines[slot];
  if (!line || !lv_obj_is_valid(line)) {
    line = lv_line_create(gpuMapLayer);
    gpuSpiderLines[slot] = line;
    lv_obj_set_style_line_width(line, 2, 0);
    lv_obj_set_style_line_color(line, lv_color_hex(0x000000), 0);
    lv_obj_set_style_line_opa(line, LV_OPA_90, 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    // gpuTouch is child 0.  Keep connector lines above it, but below all
    // station markers so they remain readable and receive the click.
    lv_obj_move_to_index(line, 1);
  }
  return line;
}

static lv_obj_t *ensureGpuSpiderCenter() {
  if (!gpuMapLayer) return nullptr;
  lv_obj_t *dot = gpuSpiderCenter;
  if (!dot || !lv_obj_is_valid(dot)) {
    dot = lv_obj_create(gpuMapLayer);
    gpuSpiderCenter = dot;
    lv_obj_set_size(dot, 6, 6);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_90, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
  }
  return dot;
}

static lv_obj_t *ensureGpuMarker(int slot, int stationIdx) {
  if (slot < 0 || slot > MAP_STATIONS_MAX || !gpuMapLayer) return nullptr;
  lv_obj_t *marker = gpuMarkers[slot];
  if (!marker || !lv_obj_is_valid(marker)) {
    marker = lv_obj_create(gpuMapLayer);
    gpuMarkers[slot] = marker;
    lv_obj_set_size(marker, 92, 54);
    lv_obj_set_style_bg_opa(marker, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(marker, 0, 0);
    lv_obj_set_style_pad_all(marker, 0, 0);
    lv_obj_clear_flag(marker, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(marker, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(marker, gpuMarkerClicked, LV_EVENT_CLICKED,
                        (void *)(intptr_t)stationIdx);
    lv_obj_add_event_cb(marker, gpuMarkerClicked, LV_EVENT_LONG_PRESSED,
                        (void *)(intptr_t)stationIdx);

    lv_obj_t *icon = lv_image_create(marker);                 // child 0
    lv_obj_set_size(icon, 32, 32);
    lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *fallback = lv_obj_create(marker);               // child 1
    lv_obj_set_size(fallback, 18, 18);
    lv_obj_set_style_radius(fallback, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(fallback, 1, 0);
    lv_obj_set_style_border_color(fallback, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_pad_all(fallback, 0, 0);
    lv_obj_align(fallback, LV_ALIGN_TOP_MID, 0, 3);

    lv_obj_t *overlay = lv_label_create(marker);              // child 2
    lv_obj_set_style_text_color(overlay, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(overlay, &lv_font_montserrat_14, 0);
    lv_obj_align(overlay, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t *callsignLabel = lv_label_create(marker);         // child 3
    lv_obj_set_style_text_color(callsignLabel, lv_color_hex(0x000000), 0);
    const lv_font_t *cf = MapVector::stationLabelFont();
    lv_obj_set_style_text_font(callsignLabel,
                               cf ? cf : &lv_font_montserrat_12, 0);
    lv_obj_align(callsignLabel, LV_ALIGN_TOP_MID, 0, 34);
  }
  return marker;
}

static void positionGpuMarker(int slot, int stationIdx, const char *callsign,
                              double lat, double lon, char table, char symbol,
                              char overlayChar, lv_color_t fallbackColor) {
  int x = 0, y = 0;
  bool visible = MaplibreDisplay::project(lat, lon, &x, &y);
  lv_obj_t *marker = gpuMarkers[slot];
  if (!visible) {
    if (marker && lv_obj_is_valid(marker)) lv_obj_add_flag(marker, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  marker = ensureGpuMarker(slot, stationIdx);
  if (!marker) return;

  lv_obj_t *icon = lv_obj_get_child(marker, 0);
  lv_obj_t *fallback = lv_obj_get_child(marker, 1);
  lv_obj_t *overlay = lv_obj_get_child(marker, 2);
  lv_obj_t *callsignLabel = lv_obj_get_child(marker, 3);
  char iconPath[320];
  bool hasIcon = symbol != '\0' &&
                 MapIO::getSymbolPath(table, symbol, iconPath, sizeof(iconPath));
  if (hasIcon) {
    lv_image_set_src(icon, iconPath);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(fallback, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(icon, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(fallback, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(fallback, fallbackColor, 0);
  }
  char overlayText[2] = {overlayChar, '\0'};
  lv_label_set_text(overlay, overlayText);
  if (overlayChar) lv_obj_remove_flag(overlay, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text(callsignLabel, callsign);
  lv_obj_remove_flag(marker, LV_OBJ_FLAG_HIDDEN);
  gpuMarkerVisible[slot] = true;
  gpuMarkerAnchorX[slot] = x;
  gpuMarkerAnchorY[slot] = y - gpuMapOriginY();
  lv_obj_set_pos(marker, gpuMarkerAnchorX[slot] - 46,
                 gpuMarkerAnchorY[slot] - 12);
}

static int findGpuClusterRoot(int slot) {
  while (gpuMarkerCluster[slot] != slot) {
    gpuMarkerCluster[slot] = gpuMarkerCluster[gpuMarkerCluster[slot]];
    slot = gpuMarkerCluster[slot];
  }
  return slot;
}

static void unionGpuClusters(int a, int b) {
  a = findGpuClusterRoot(a);
  b = findGpuClusterRoot(b);
  if (a != b) gpuMarkerCluster[b] = a;
}

static void layoutGpuMarkerClusters() {
  constexpr int overlapDistance = 48;
  constexpr double pi = 3.14159265358979323846;

  for (int slot = 0; slot <= MAP_STATIONS_MAX; ++slot) {
    gpuMarkerCluster[slot] = slot;
    gpuMarkerClusterSize[slot] = gpuMarkerVisible[slot] ? 1 : 0;
    lv_obj_t *line = gpuSpiderLines[slot];
    if (line && lv_obj_is_valid(line)) lv_obj_add_flag(line, LV_OBJ_FLAG_HIDDEN);
  }

  // Connected components are intentional: A may overlap B and B overlap C
  // even when A and C are just outside the threshold.  They must expand as
  // one group or two markers would remain superposed.
  for (int a = 0; a <= MAP_STATIONS_MAX; ++a) {
    if (!gpuMarkerVisible[a]) continue;
    for (int b = a + 1; b <= MAP_STATIONS_MAX; ++b) {
      if (!gpuMarkerVisible[b]) continue;
      const int dx = gpuMarkerAnchorX[a] - gpuMarkerAnchorX[b];
      const int dy = gpuMarkerAnchorY[a] - gpuMarkerAnchorY[b];
      if (dx * dx + dy * dy <= overlapDistance * overlapDistance)
        unionGpuClusters(a, b);
    }
  }

  int sizes[MAP_STATIONS_MAX + 1] = {};
  for (int slot = 0; slot <= MAP_STATIONS_MAX; ++slot) {
    if (!gpuMarkerVisible[slot]) continue;
    gpuMarkerCluster[slot] = findGpuClusterRoot(slot);
    ++sizes[gpuMarkerCluster[slot]];
  }
  for (int slot = 0; slot <= MAP_STATIONS_MAX; ++slot)
    gpuMarkerClusterSize[slot] = gpuMarkerVisible[slot]
                                     ? sizes[gpuMarkerCluster[slot]] : 0;

  if (gpuExpandedClusterSlot < 0 ||
      gpuExpandedClusterSlot > MAP_STATIONS_MAX ||
      !gpuMarkerVisible[gpuExpandedClusterSlot] ||
      gpuMarkerClusterSize[gpuExpandedClusterSlot] < 2) {
    gpuExpandedClusterSlot = -1;
    return;
  }

  const int root = gpuMarkerCluster[gpuExpandedClusterSlot];
  const int count = sizes[root];
  int centerX = 0, centerY = 0;
  for (int slot = 0; slot <= MAP_STATIONS_MAX; ++slot) {
    if (gpuMarkerVisible[slot] && gpuMarkerCluster[slot] == root) {
      centerX += gpuMarkerAnchorX[slot];
      centerY += gpuMarkerAnchorY[slot];
    }
  }
  centerX /= count;
  centerY /= count;

  lv_obj_t *center = ensureGpuSpiderCenter();
  if (center) {
    lv_obj_set_pos(center, centerX - 3, centerY - 3);
    lv_obj_remove_flag(center, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(center);
  }

  const int layerW = lv_obj_get_width(gpuMapLayer);
  const int layerH = lv_obj_get_height(gpuMapLayer);
  int member = 0;
  for (int slot = 0; slot <= MAP_STATIONS_MAX; ++slot) {
    if (!gpuMarkerVisible[slot] || gpuMarkerCluster[slot] != root) continue;

    // Eight stations per ring keeps adjacent symbols separated. Very dense
    // groups grow outwards ring by ring instead of piling the overflow onto
    // the same angles.
    const int ring = member / 8;
    const int ringStart = ring * 8;
    const int ringCount = std::min(8, count - ringStart);
    const int ringIndex = member - ringStart;
    const double angle = -pi / 2.0 + (2.0 * pi * ringIndex) / ringCount;
    const int radius = 32 + ring * 28;
    int displayX = centerX + (int)std::lround(std::cos(angle) * radius);
    int displayY = centerY + (int)std::lround(std::sin(angle) * radius);
    displayX = std::max(46, std::min(layerW - 46, displayX));
    displayY = std::max(12, std::min(layerH - 32, displayY));

    lv_obj_set_pos(gpuMarkers[slot], displayX - 46, displayY - 12);
    gpuSpiderPoints[slot][0] = {
        (lv_value_precise_t)gpuMarkerAnchorX[slot],
        (lv_value_precise_t)gpuMarkerAnchorY[slot]};
    gpuSpiderPoints[slot][1] = {
        (lv_value_precise_t)displayX, (lv_value_precise_t)displayY};
    lv_obj_t *line = ensureGpuSpiderLine(slot);
    if (line) {
      lv_line_set_points(line, gpuSpiderPoints[slot], 2);
      lv_obj_remove_flag(line, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_move_foreground(gpuMarkers[slot]);
    ++member;
  }
}

static void refreshGpuMarkers() {
  if (!gpuScreen || !lv_obj_is_valid(gpuScreen) || !MaplibreDisplay::isActive()) return;
  refreshGpuTraces();
  memset(gpuMarkerVisible, 0, sizeof(gpuMarkerVisible));
  bool used[MAP_STATIONS_MAX + 1] = {};
  if (gpsLat != 0.0 || gpsLon != 0.0) {
    const char *own = (!Config.beacons.empty())
                          ? Config.beacons[myBeaconsIndex].callsign.c_str()
                          : "OWN";
    const char *ov = (!Config.beacons.empty())
                         ? Config.beacons[myBeaconsIndex].overlay.c_str() : "/";
    const char *sy = (!Config.beacons.empty())
                         ? Config.beacons[myBeaconsIndex].symbol.c_str() : ">";
    char ov0 = (ov && ov[0]) ? ov[0] : '/';
    char table = (ov0 == '/') ? '/' : '\\';
    char overlayChar = (ov0 != '/' && ov0 != '\\') ? ov0 : 0;
    positionGpuMarker(0, -1, own, gpsLat, gpsLon, table,
                      (sy && sy[0]) ? sy[0] : '>', overlayChar,
                      lv_color_hex(0x0055cc));
    used[0] = true;
  }
  for (int i = 0; i < MAP_STATIONS_MAX; ++i) {
    MapStation *st = STATION_Utils::getMapStation(i);
    if (!st || !st->valid || (st->latitude == 0.0f && st->longitude == 0.0f)) continue;
    uint32_t age = millis() - st->lastTime;
    lv_color_t color = age < 10 * 60 * 1000 ? lv_color_hex(0xff6600)
                                             : lv_color_hex(0x777777);
    char ov0 = st->overlay.length() ? st->overlay[0] : '/';
    char table = (ov0 == '/') ? '/' : '\\';
    char overlayChar = (ov0 != '/' && ov0 != '\\') ? ov0 : 0;
    char symbol = st->symbol.length() ? st->symbol[0] : '>';
    positionGpuMarker(i + 1, i, st->callsign.c_str(), st->latitude,
                      st->longitude, table, symbol, overlayChar, color);
    used[i + 1] = true;
  }
  for (int i = 0; i <= MAP_STATIONS_MAX; ++i) {
    if (!used[i] && gpuMarkers[i] && lv_obj_is_valid(gpuMarkers[i]))
      lv_obj_add_flag(gpuMarkers[i], LV_OBJ_FLAG_HIDDEN);
  }
  layoutGpuMarkerClusters();
  // Camera coordinates and station count can both change while this screen
  // stays open; keep the information bar synchronized with the GPU map.
  refreshGpuInfoBar();

  // gpuMapLayer is clipped to the map viewport, so its dynamic children can
  // never overlap the fixed bars. Reordering the bars on every refresh caused
  // avoidable full-object invalidation and visible jitter at the bottom edge.
}

static lv_obj_t *ensureGpuTrace(int slot, lv_color_t color) {
  if (slot < 0 || slot > MAP_STATIONS_MAX || !gpuMapLayer) return nullptr;
  lv_obj_t *line = gpuTraceLines[slot];
  if (!line || !lv_obj_is_valid(line)) {
    line = lv_line_create(gpuMapLayer);
    gpuTraceLines[slot] = line;
    lv_obj_set_style_line_width(line, 3, 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    lv_obj_set_style_line_opa(line, LV_OPA_80, 0);
  }
  lv_obj_set_style_line_color(line, color, 0);
  return line;
}

static void setGpuTracePoints(int slot, int count, lv_color_t color) {
  lv_obj_t *line = gpuTraceLines[slot];
  if (count < 2) {
    if (line && lv_obj_is_valid(line)) lv_obj_add_flag(line, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  line = ensureGpuTrace(slot, color);
  lv_line_set_points(line, gpuTracePoints[slot], count);
  lv_obj_remove_flag(line, LV_OBJ_FLAG_HIDDEN);
}

static void refreshGpuTraces() {
  if (!gpuMapLayer || !MaplibreDisplay::isActive()) return;
  uint32_t now = millis();
  const int originY = gpuMapOriginY();

  int ownCount = 0;
  for (int i = 0; i < MapTraces::ownTraceSize() && ownCount < 200; ++i) {
    double lat = 0.0, lon = 0.0;
    int x = 0, y = 0;
    if (MapTraces::ownTracePoint(i, &lat, &lon) &&
        MaplibreDisplay::project(lat, lon, &x, &y))
      gpuTracePoints[0][ownCount++] = {(lv_value_precise_t)x, (lv_value_precise_t)(y - originY)};
  }
  if ((gpsLat != 0.0 || gpsLon != 0.0) && ownCount < 201) {
    int x = 0, y = 0;
    if (MaplibreDisplay::project(gpsLat, gpsLon, &x, &y))
      gpuTracePoints[0][ownCount++] = {(lv_value_precise_t)x, (lv_value_precise_t)(y - originY)};
  }
  setGpuTracePoints(0, ownCount, lv_color_hex(0x9933ff));

  for (int s = 0; s < MAP_STATIONS_MAX; ++s) {
    MapStation *st = STATION_Utils::getMapStation(s);
    int count = 0;
    if (st && st->valid) {
      for (int i = 0; i < st->traceCount && count < TRACE_MAX_POINTS; ++i) {
        int idx = (st->traceHead - st->traceCount + i + TRACE_MAX_POINTS) % TRACE_MAX_POINTS;
        if (now - st->trace[idx].time > 3600000) continue;
        int x = 0, y = 0;
        if (MaplibreDisplay::project(st->trace[idx].lat, st->trace[idx].lon, &x, &y))
          gpuTracePoints[s + 1][count++] = {(lv_value_precise_t)x, (lv_value_precise_t)(y - originY)};
      }
      int x = 0, y = 0;
      if (count < 201 && MaplibreDisplay::project(st->latitude, st->longitude, &x, &y))
        gpuTracePoints[s + 1][count++] = {(lv_value_precise_t)x, (lv_value_precise_t)(y - originY)};
    }
    setGpuTracePoints(s + 1, count, lv_color_hex(0x0055ff));
  }
}

static void gpuTouchCb(lv_event_t *e) {
  static lv_point_t last{};
  static uint32_t lastMs = 0;
  lv_indev_t *indev = lv_indev_get_act();
  if (!indev) return;
  lv_point_t p{};
  lv_indev_get_point(indev, &p);
  lv_event_code_t code = lv_event_get_code(e);
  // While two fingers are down the pinch owns the gesture; LVGL still emits a
  // single-pointer stream for finger 1 that must not pan the map.
  if (gpuPinchActive) { gpuPanReseed = true; return; }
  if (code == LV_EVENT_DOUBLE_CLICKED) {
    if (millis() - gpuPinchEndMs < 400) return;
    toggleFullscreen();
  } else if (code == LV_EVENT_PRESSED) {
    collapseGpuSpiderfy();
    last = p;
    lastMs = millis();
    gpuPanActive = true;
    gpuVelX = gpuVelY = 0.0f;
    MapMarkers::closeStationPopup();
  } else if (code == LV_EVENT_PRESSING) {
    // The finger left over after a pinch release keeps its old anchor; re-seed
    // it once instead of applying the stale delta, which would jump the map.
    if (gpuPanReseed) {
      gpuPanReseed = false;
      last = p;
      lastMs = millis();
      return;
    }
    int dx = p.x - last.x, dy = p.y - last.y;
    uint32_t now = millis();
    uint32_t dt = now - lastMs;
    last = p;
    lastMs = now;
    if (dx || dy) {
      gpuFollowGps = false;
      mapFollowGps = false;
      markFollowGpsDisabled();
      if (dt > 0) {
        constexpr float weight = 0.7f;
        gpuVelX = gpuVelX * (1.0f - weight) + ((float)dx / dt) * weight;
        gpuVelY = gpuVelY * (1.0f - weight) + ((float)dy / dt) * weight;
      }
      MaplibreDisplay::moveBy(dx, dy);
      refreshGpuMarkers();
    }
  } else if (code == LV_EVENT_RELEASED) {
    gpuPanActive = false;
    if (millis() - gpuPinchEndMs < 400) {
      gpuVelX = gpuVelY = 0.0f;
      return;
    }
    gpuLastInertiaMs = millis();
    refreshGpuInfoBar();
  }
}

static void gpuTimerTick(lv_timer_t *) {
  static uint8_t refreshDivider = 0;
  if (gpuLoadingCover && MaplibreDisplay::isReady()) {
    lv_obj_del(gpuLoadingCover);
    gpuLoadingCover = nullptr;
  }

  // Two-finger pinch zoom, anchored on the finger midpoint. The evdev reader
  // runs in parallel because the LVGL pointer only reports one finger. Zoom is
  // recomputed from the start distance each frame, so it does not drift.
  float pdist = 0.0f, pmx = 0.0f, pmy = 0.0f;
  int fingers = MapPinch::poll(&pdist, &pmx, &pmy);
  if (fingers >= 2) {
    if (!gpuPinchActive) {
      gpuPinchActive = true;
      gpuPinchDist0 = pdist;
      gpuPinchZoom0 = MaplibreDisplay::getZoom();
      gpuPanActive = false;
      gpuVelX = gpuVelY = 0.0f;
      collapseGpuSpiderfy();
      gpuFollowGps = false;
      mapFollowGps = false;
      markFollowGpsDisabled();
      MapMarkers::closeStationPopup();
    } else if (pdist > 1.0f && gpuPinchDist0 > 1.0f) {
      double target = gpuPinchZoom0 + std::log2(pdist / gpuPinchDist0);
      if (target < zoomMin) target = zoomMin;
      if (target > zoomMax) target = zoomMax;
      MaplibreDisplay::zoomAround(target, pmx, pmy);
      zoom = (int)(target + 0.5);
      if (titleLabel) {
        char text[16];
        snprintf(text, sizeof(text), "MAP (Z%d)", zoom);
        lv_label_set_text(titleLabel, text);
      }
      refreshGpuMarkers();
    }
    return;
  }
  if (gpuPinchActive) {
    gpuPinchActive = false;
    gpuPinchEndMs = millis();
    gpuPanReseed = true;
    gpuLastInertiaMs = millis();
    refreshGpuInfoBar();
  }

  if (!gpuPanActive && (gpuVelX != 0.0f || gpuVelY != 0.0f)) {
    uint32_t now = millis();
    uint32_t dt = now - gpuLastInertiaMs;
    gpuLastInertiaMs = now;
    if (dt > 0 && dt < 100) {
      int dx = (int)(gpuVelX * dt);
      int dy = (int)(gpuVelY * dt);
      if (dx || dy) MaplibreDisplay::moveBy(dx, dy);
      gpuVelX *= 0.85f;
      gpuVelY *= 0.85f;
      if (gpuVelX > -0.01f && gpuVelX < 0.01f) gpuVelX = 0.0f;
      if (gpuVelY > -0.01f && gpuVelY < 0.01f) gpuVelY = 0.0f;
      refreshGpuMarkers();
      return;
    }
  }
  if (gpuStationsDirty || ++refreshDivider >= 5) {
    gpuStationsDirty = false;
    refreshDivider = 0;
    refreshGpuMarkers();
  }
}

static void gpuScreenDeleted(lv_event_t *) {
  if (gpuMarkerTimer) { lv_timer_del(gpuMarkerTimer); gpuMarkerTimer = nullptr; }
  MapPinch::stop();
  gpuPinchActive = false;
  gpuPanReseed = false;
  MapMarkers::closeStationPopup();
  MaplibreDisplay::getCenter(&centerLat, &centerLon);
  zoom = (int)(MaplibreDisplay::getZoom() + 0.5);
  gpuScreen = nullptr;
  gpuMapLayer = nullptr;
  gpuTouch = nullptr;
  gpuLoadingCover = nullptr;
  gpuPanActive = false;
  gpuStationsDirty = false;
  gpuVelX = gpuVelY = 0.0f;
  memset(gpuMarkers, 0, sizeof(gpuMarkers));
  memset(gpuTraceLines, 0, sizeof(gpuTraceLines));
  memset(gpuSpiderLines, 0, sizeof(gpuSpiderLines));
  gpuSpiderCenter = nullptr;
  memset(gpuMarkerVisible, 0, sizeof(gpuMarkerVisible));
  gpuExpandedClusterSlot = -1;
  MapTraces::destroy();
}

#endif

// ============================================================
// Callbacks boutons & pan
// ============================================================
static void backCb(lv_event_t *) {
  MapMarkers::closeStationPopup();
  mapActive = false;
#ifdef WITH_MAPLIBRE
  if (MaplibreDisplay::isActive()) {
    if (gpuMarkerTimer) { lv_timer_del(gpuMarkerTimer); gpuMarkerTimer = nullptr; }
    UIDashboard::returnToDashboard();
    return;
  }
#endif
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

void toggleFullscreen() {
  fullscreenMap = !fullscreenMap;
#ifdef WITH_MAPLIBRE
  if (MaplibreDisplay::isActive()) {
    if (fullscreenMap) {
      lv_obj_add_flag(tbarMap, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(ibarMap, LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_size(gpuMapLayer, CONT_W, 600);
      lv_obj_set_pos(gpuMapLayer, 0, 0);
    } else {
      lv_obj_clear_flag(tbarMap, LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(ibarMap, LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_size(gpuMapLayer, CONT_W, MAP_H);
      lv_obj_set_pos(gpuMapLayer, 0, 45);
    }
    refreshGpuMarkers();
    lv_refr_now(NULL);
    return;
  }
#endif
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
#ifdef WITH_MAPLIBRE
    if (MaplibreDisplay::isActive()) {
      gpuFollowGps = true;
      if (gpsLat != 0.0 || gpsLon != 0.0) MaplibreDisplay::setCenter(gpsLat, gpsLon);
      refreshGpuMarkers();
      lv_obj_set_style_bg_color(btnRecenter, lv_color_hex(0x16213e), 0);
      lv_obj_invalidate(btnRecenter);
      return;
    }
#endif
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
      bool ok = gpsFix.valid_location && gpsFix.valid_date && gpsFix.valid_time &&
                GPXWriter::startRecording(gpsFix.year, gpsFix.month, gpsFix.date,
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

#ifdef WITH_MAPLIBRE
  if (MaplibreDisplay::isActive()) {
    // GPU map: the real chrome (built above) sits over MapLibre, which
    // renderTick draws underneath. Transparent screen + touch/marker layer;
    // no software MapEngine/MapLabels/MapInput.
    lv_obj_set_style_bg_opa(scr, LV_OPA_TRANSP, 0);
    gpuScreen = scr;
    fullscreenMap = false;
    gpuFollowGps = mapFollowGps;
    lv_obj_add_event_cb(scr, gpuScreenDeleted, LV_EVENT_DELETE, nullptr);

    // All moving overlays live in this clipped map viewport. They can no
    // longer draw over or receive touches through the fixed chrome.
    gpuMapLayer = lv_obj_create(scr);
    lv_obj_set_size(gpuMapLayer, CONT_W, MAP_H);
    lv_obj_set_pos(gpuMapLayer, 0, 45);
    lv_obj_set_style_bg_opa(gpuMapLayer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(gpuMapLayer, 0, 0);
    lv_obj_set_style_pad_all(gpuMapLayer, 0, 0);
    lv_obj_clear_flag(gpuMapLayer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(gpuMapLayer, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    gpuTouch = lv_obj_create(gpuMapLayer);
    lv_obj_set_size(gpuTouch, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(gpuTouch, 0, 0);
    lv_obj_set_style_bg_opa(gpuTouch, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(gpuTouch, 0, 0);
    lv_obj_set_style_pad_all(gpuTouch, 0, 0);
    lv_obj_clear_flag(gpuTouch, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(gpuTouch, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(gpuTouch, gpuTouchCb, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(gpuTouch, gpuTouchCb, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(gpuTouch, gpuTouchCb, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(gpuTouch, gpuTouchCb, LV_EVENT_DOUBLE_CLICKED, nullptr);

    // The first MapLibre frame after a reboot builds tile buffers, glyph
    // atlases and both KMS scanout buffers. Keep those intermediate frames
    // hidden; otherwise they appear as a short flicker on first map entry.
    if (!MaplibreDisplay::isReady()) {
      gpuLoadingCover = lv_obj_create(gpuMapLayer);
      lv_obj_set_size(gpuLoadingCover, LV_PCT(100), LV_PCT(100));
      lv_obj_set_pos(gpuLoadingCover, 0, 0);
      lv_obj_set_style_bg_color(gpuLoadingCover, lv_color_hex(0x1a1a2e), 0);
      lv_obj_set_style_bg_opa(gpuLoadingCover, LV_OPA_COVER, 0);
      lv_obj_set_style_border_width(gpuLoadingCover, 0, 0);
      lv_obj_set_style_radius(gpuLoadingCover, 0, 0);
      lv_obj_clear_flag(gpuLoadingCover, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_add_flag(gpuLoadingCover, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_t *loading = lv_label_create(gpuLoadingCover);
      lv_label_set_text(loading, "Chargement de la carte...");
      lv_obj_set_style_text_color(loading, lv_color_hex(0xaaaaaa), 0);
      lv_obj_set_style_text_font(loading, &lv_font_montserrat_16, 0);
      lv_obj_center(loading);
    }

    char zg[16];
    snprintf(zg, sizeof(zg), "MAP (Z%d)", (int)(MaplibreDisplay::getZoom() + 0.5));
    lv_label_set_text(titleLabel, zg);

    refreshGpuMarkers();
    if (gpuLoadingCover) lv_obj_move_foreground(gpuLoadingCover);
    if (!gpuMarkerTimer)
      gpuMarkerTimer = lv_timer_create(gpuTimerTick, 50, nullptr);
    MapPinch::start();
    mapActive = true;
    return scr;
  }
#endif

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
