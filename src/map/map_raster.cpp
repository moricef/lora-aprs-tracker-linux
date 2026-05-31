/* Raster tile map — ESP32 create_map_screen() + region discovery ported to
 * Linux APRS Stations: icons positioned on tiles, tap → info popup
 */
#include "map_raster.h"
#include "configuration.h"
#include "gps_math.h"
#include "map_coordinate_math.h"
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

#define TILE_SIZE 256
#define GRID 5 // 5x5: covers map area with margin for preloading
#define SPRITE_SIZE (GRID * TILE_SIZE) // 1280
#define CONT_W 1024
#define MAP_H (600 - 45 - 30) // 530  (below 45px titlebar, above 30px infobar)

// ---- Tile state ----
static lv_obj_t *tileImg[GRID][GRID];
static char tileDir[256] = "", mapRegion[64] = "";
static double centerLat = 42.96, centerLon = 1.37;
static double gpsLat = 0.0, gpsLon = 0.0; // 0,0 = pas de fix GPS
static int zoom = 7, centerTX = 0, centerTY = 0;
static int zoomMin = 6, zoomMax = 8;
static bool mapActive = false;

// Pan state + inertia
static lv_point_t dragLast;
static int dragAccumX = 0, dragAccumY = 0;
static bool panActive = false;
static bool mapFollowGps = true;  // firmware default: follow GPS on map open
static float velX = 0.0f, velY = 0.0f;
static uint32_t dragLastMs = 0;
static lv_timer_t *mapTimer = nullptr;

// Own trace ring buffer (same as firmware TracePoint)
#define OWN_TRACE_MAX 200
static TracePoint ownTrace[OWN_TRACE_MAX];
static int ownTraceCount = 0;
static int ownTraceHead = 0;

// LVGL objects
static lv_obj_t *titleLabel = nullptr;
static lv_obj_t *infoLabel = nullptr;
static lv_obj_t *mapCont = nullptr;
static lv_obj_t *btnRecenter = nullptr;

// ---- Station markers ----
#define MAX_MARKERS (MAP_STATIONS_MAX + 1) // 15 stations + own
struct MarkerInfo {
  lv_obj_t *obj;
  int stationIdx; // -1 = own station, >=0 = mapStations index
};
static MarkerInfo markers[MAX_MARKERS];
static int markerCount = 0;

// Station info popup
static lv_obj_t *stationPopup = nullptr;

// ============================================================
// Region / zoom discovery (same logic as ESP32 map_tiles)
// ============================================================
static const char *mapsRoot() {
  static char root[256];
  if (root[0])
    return root;
  const char *candidates[] = {
      "/home/fab2/Developpement/LoRa_APRS/SDCARD/TILES/LoRa_Tracker/Maps",
      "/media/fab2/TILES/LoRa_Tracker/Maps", "/data/LoRa_Tracker/Maps", NULL};
  for (int i = 0; candidates[i]; i++) {
    struct stat st;
    if (stat(candidates[i], &st) == 0) {
      strncpy(root, candidates[i], sizeof(root) - 1);
      return root;
    }
  }
  return NULL;
}

static void discoverRegion() {
  if (mapRegion[0])
    return;
  const char *root = mapsRoot();
  if (!root)
    return;
  DIR *d = opendir(root);
  if (!d)
    return;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (e->d_type == DT_DIR && e->d_name[0] != '.') {
      strncpy(mapRegion, e->d_name, sizeof(mapRegion) - 1);
      break;
    }
  }
  closedir(d);
}

static void discoverZooms() {
  if (!mapRegion[0]) {
    discoverRegion();
    if (!mapRegion[0])
      return;
  }
  char zpath[512];
  snprintf(zpath, sizeof(zpath), "%s/%s", mapsRoot(), mapRegion);
  DIR *d = opendir(zpath);
  if (!d)
    return;
  struct dirent *e;
  int zMin = INT_MAX, zMax = INT_MIN;
  while ((e = readdir(d))) {
    if (e->d_type == DT_DIR && e->d_name[0] != '.') {
      int z = atoi(e->d_name);
      if (z > 0 && z < 20) {
        if (z < zMin)
          zMin = z;
        if (z > zMax)
          zMax = z;
      }
    }
  }
  closedir(d);
  if (zMin <= zMax) {
    zoomMin = (zMin < 7) ? 7 : zMin;
    zoomMax = zMax;
    zoom = zMax;
  }
  // Extend max zoom to include vector tiles if available
  if (MapVector::isOpen()) {
    int vMax = MapVector::maxZoom();
    int vMin = MapVector::minZoom();
    if (vMax > zoomMax)
      zoomMax = vMax;
    if (vMin < zoomMin || zoomMin == INT_MAX)
      zoomMin = vMin;
  }
}

static void discoverDefaultPosition() {
  if (!mapRegion[0])
    return;
  char zpath[512];
  snprintf(zpath, sizeof(zpath), "%s/%s/%d", mapsRoot(), mapRegion, 6);
  DIR *zd = opendir(zpath);
  if (!zd)
    return;
  int xMin = INT_MAX, xMax = INT_MIN, yMin = INT_MAX, yMax = INT_MIN;
  struct dirent *xe;
  while ((xe = readdir(zd))) {
    if (xe->d_type != DT_DIR || xe->d_name[0] == '.')
      continue;
    int tx = atoi(xe->d_name);
    char xpath[600];
    snprintf(xpath, sizeof(xpath), "%s/%s", zpath, xe->d_name);
    DIR *xd = opendir(xpath);
    if (!xd)
      continue;
    struct dirent *ye;
    while ((ye = readdir(xd))) {
      if (ye->d_type != DT_REG)
        continue;
      char base[64];
      strncpy(base, ye->d_name, sizeof(base) - 1);
      base[sizeof(base) - 1] = 0;
      char *dot = strrchr(base, '.');
      if (dot)
        *dot = 0;
      int ty = atoi(base);
      if (tx < xMin)
        xMin = tx;
      if (tx > xMax)
        xMax = tx;
      if (ty < yMin)
        yMin = ty;
      if (ty > yMax)
        yMax = ty;
    }
    closedir(xd);
  }
  closedir(zd);
  if (xMin <= xMax && yMin <= yMax)
    MapMath::tileToLatLon((xMin + xMax) / 2, (yMin + yMax) / 2, 6,
                          (float *)&centerLat, (float *)&centerLon);
}

// ---- Tuiles ----
static uint32_t notFoundCache[128];
static int notFoundIdx = 0;
static bool tileExists(int tx, int ty, int z) {
  uint32_t key = ((uint32_t)z << 24) | ((uint32_t)(tx & 0xFFF) << 12) |
                 (uint32_t)(ty & 0xFFF);
  for (int i = 0; i < 128; i++)
    if (notFoundCache[i] == key)
      return false;
  char p[512];
  snprintf(p, sizeof(p), "%s/%s/%d/%d/%d.jpg", mapsRoot(), mapRegion, z, tx,
           ty);
  struct stat st;
  if (stat(p, &st) == 0)
    return true;
  notFoundCache[notFoundIdx++ % 128] = key;
  return false;
}

// ============================================================
// Sprite to container coordinate conversion (for marker placement)
// ============================================================
static inline int spriteToContX(int spriteX) {
  return spriteX + (CONT_W - SPRITE_SIZE) / 2 + dragAccumX;
}
static inline int spriteToContY(int spriteY) {
  return spriteY + (MAP_H - SPRITE_SIZE) / 2 + dragAccumY;
}

static bool latLonToContPos(float lat, float lon, int *cx, int *cy) {
  int px, py;
  MapMath::latLonToPixel(lat, lon, centerLat, centerLon, zoom, true, centerTX,
                         centerTY, &px, &py);
  *cx = spriteToContX(px);
  *cy = spriteToContY(py);
  // visible if within sprite (±1 tile margin)
  return (px >= -TILE_SIZE && px < SPRITE_SIZE + TILE_SIZE &&
          py >= -TILE_SIZE && py < SPRITE_SIZE + TILE_SIZE);
}

// ============================================================
// Popup d'info station
// ============================================================
static void closeStationPopup() {
  if (stationPopup && lv_obj_is_valid(stationPopup))
    lv_msgbox_close(stationPopup);
  stationPopup = nullptr;
}

static void station_popup_deleted_cb(lv_event_t *) { stationPopup = nullptr; }
static void station_popup_close_btn_cb(lv_event_t *) { closeStationPopup(); }

static void show_station_popup(int stationIdx) {
  closeStationPopup();

  char title[48];
  char body[400];

  if (stationIdx < 0) {
    // Own station
    const char *cs = (!Config.beacons.empty())
                         ? Config.beacons[myBeaconsIndex].callsign.c_str()
                         : "NOCALL";
    const char *ov = (!Config.beacons.empty())
                         ? Config.beacons[myBeaconsIndex].overlay.c_str()
                         : "/";
    const char *sy = (!Config.beacons.empty())
                         ? Config.beacons[myBeaconsIndex].symbol.c_str()
                         : "&";
    snprintf(title, sizeof(title), "%s  (own)", cs);
    snprintf(body, sizeof(body), "Symbole : %s%s\nPos : %.4f°  %.4f°", ov, sy,
             gpsLat, gpsLon);
  } else {
    MapStation *st = STATION_Utils::getMapStation(stationIdx);
    if (!st || !st->valid)
      return;

    snprintf(title, sizeof(title), "%s", st->callsign.c_str());

    uint32_t elapsed = millis() - st->lastTime;
    int secs = (int)(elapsed / 1000);
    int mins = secs / 60;
    int hours = mins / 60;

    char elapsed_str[32];
    if (hours > 0)
      snprintf(elapsed_str, sizeof(elapsed_str), "%dh%02d", hours, mins % 60);
    else if (mins > 0)
      snprintf(elapsed_str, sizeof(elapsed_str), "%d min", mins);
    else
      snprintf(elapsed_str, sizeof(elapsed_str), "%d s", secs);

    // Distance et cap si on a une position GPS
    char dist_str[64] = "";
    if ((gpsLat != 0.0 || gpsLon != 0.0) &&
        (st->latitude != 0.0f || st->longitude != 0.0f)) {
      float dist_m =
          calcDist((float)gpsLat, (float)gpsLon, st->latitude, st->longitude);
      float bearing =
          calcCourse((float)gpsLat, (float)gpsLon, st->latitude, st->longitude);
      if (dist_m >= 1000.0f)
        snprintf(dist_str, sizeof(dist_str), "\nDist : %.1f km  Cap : %.0f°",
                 dist_m / 1000.0f, bearing);
      else
        snprintf(dist_str, sizeof(dist_str), "\nDist : %.0f m  Cap : %.0f°",
                 dist_m, bearing);
    }

    snprintf(body, sizeof(body),
             "Symbole : %s%s\n"
             "Pos : %.4f°  %.4f°\n"
             "RSSI : %d dBm\n"
             "Entendu : %s%s",
             st->overlay.c_str(), st->symbol.c_str(), st->latitude,
             st->longitude, st->rssi, elapsed_str, dist_str);
  }

  // Backdrop + msgbox (NULL = backdrop 100%×100% bloquant les touch events)
  stationPopup = lv_msgbox_create(NULL);
  lv_obj_t *backdrop = lv_obj_get_parent(stationPopup);
  if (backdrop)
    lv_obj_set_style_bg_opa(backdrop, LV_OPA_30, 0);
  lv_msgbox_add_title(stationPopup, title);
  lv_msgbox_add_text(stationPopup, body);
  lv_obj_set_style_bg_color(stationPopup, lv_color_hex(0x16213e), 0);
  lv_obj_set_style_text_color(stationPopup, lv_color_hex(0xffffff),
                              LV_PART_MAIN);
  lv_obj_set_width(stationPopup, 380);
  lv_obj_center(stationPopup);
  lv_obj_add_event_cb(stationPopup, station_popup_deleted_cb, LV_EVENT_DELETE,
                      NULL);
  lv_obj_t *closeBtn = lv_msgbox_add_footer_button(stationPopup, "Close");
  lv_obj_add_event_cb(closeBtn, station_popup_close_btn_cb, LV_EVENT_CLICKED,
                      NULL);
}

static void deleteMarkers(); // forward decl

// ============================================================
// APRS icons — loaded from split PNG files (sd_card)
// ============================================================
static const char *symbolsRoot() {
  static char root[256];
  if (root[0])
    return root;
  const char *candidates[] = {"/home/fab2/Developpement/LoRa_APRS/aprs-symbols/"
                              "sd_card/LoRa_Tracker/Symbols",
                              "/media/fab2/TILES/LoRa_Tracker/Symbols",
                              "/data/LoRa_Tracker/Symbols", NULL};
  for (int i = 0; candidates[i]; i++) {
    struct stat st;
    if (stat(candidates[i], &st) == 0) {
      strncpy(root, candidates[i], sizeof(root) - 1);
      return root;
    }
  }
  return NULL;
}

// Remplit `path` (taille pathsz) avec le chemin LVGL "A:/..." vers le PNG du
// symbole table = '/' (primary) ou '\\' (alternate), symbol = char APRS (ex:
// '-', '>', '[')
static bool getSymbolPath(char table, char symbol, char *path, size_t pathsz) {
  const char *root = symbolsRoot();
  if (!root)
    return false;
  const char *tableName = (table == '/') ? "primary" : "alternate";
  snprintf(path, pathsz, "A:%s/%s/%02X.png", root, tableName, (uint8_t)symbol);
  return true;
}

// ============================================================
// Gestion des marqueurs stations (LVGL objects dans mapCont)
// ============================================================

// Marker container dimensions (24x24 icon + 14px callsign below)
#define MARKER_W 80
#define MARKER_H 40
#define ICON_SIZE 24

static void deleteMarkers() {
  for (int i = 0; i < markerCount; i++) {
    if (markers[i].obj && lv_obj_is_valid(markers[i].obj))
      lv_obj_del(markers[i].obj);
    markers[i].obj = nullptr;
  }
  markerCount = 0;
}

// Create a marker container centred on (cx, cy) with APRS icon and callsign.
// table/sym: APRS table ('/' or '\\') and symbol (char) — fallback coloured
// disc if PNG missing. overlayChar: alphanumeric to draw on icon, or 0 if none.
static lv_obj_t *createMarkerObj(lv_obj_t *parent, const char *callsign,
                                 char table, char sym, char overlayChar,
                                 lv_color_t fallbackColor, int cx, int cy,
                                 int stationIdx) {
  // Transparent clickable container
  lv_obj_t *m = lv_obj_create(parent);
  lv_obj_set_size(m, MARKER_W, MARKER_H);
  lv_obj_set_pos(m, cx - MARKER_W / 2, cy - ICON_SIZE / 2);
  lv_obj_set_style_bg_opa(m, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(m, 0, 0);
  lv_obj_set_style_pad_all(m, 0, 0);
  lv_obj_clear_flag(m, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(m, LV_OBJ_FLAG_CLICKABLE);

  // APRS icon or fallback disc
  char iconPath[320];
  if (sym != '\0' && getSymbolPath(table, sym, iconPath, sizeof(iconPath))) {
    lv_obj_t *img = lv_image_create(m);
    lv_image_set_src(img, iconPath);
    lv_obj_align(img, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_size(img, ICON_SIZE, ICON_SIZE);

    // Overlay character (digi 'D', iGate 'I', etc.) directly on icon
    if (overlayChar != 0 && overlayChar != '/' && overlayChar != '\\') {
      char ovTxt[2] = {overlayChar, 0};
      lv_obj_t *ovLbl = lv_label_create(m);
      lv_label_set_text(ovLbl, ovTxt);
      lv_obj_set_style_text_color(ovLbl, lv_color_hex(0xffffff), 0);
      lv_obj_set_style_text_font(ovLbl, &lv_font_montserrat_14, 0);
      lv_obj_set_style_bg_opa(ovLbl, LV_OPA_TRANSP, 0);
      lv_obj_set_style_pad_all(ovLbl, 0, 0);
      lv_obj_align(ovLbl, LV_ALIGN_TOP_MID, 0, 4);
    }
  } else {
    // Fallback: coloured disc 16x16
    lv_obj_t *dot = lv_obj_create(m);
    lv_obj_set_size(dot, 16, 16);
    lv_obj_set_style_bg_color(dot, fallbackColor, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_align(dot, LV_ALIGN_TOP_MID, 0, 4);
  }

  // Callsign with semi-transparent background for readability
  lv_obj_t *lbl = lv_label_create(m);
  lv_label_set_text(lbl, callsign);
  lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
  lv_obj_set_width(lbl, MARKER_W);
  lv_obj_set_style_text_color(lbl, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_bg_color(lbl, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(lbl, LV_OPA_30, 0);
  lv_obj_set_style_pad_hor(lbl, 2, 0);
  lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, 0);

  lv_obj_update_layout(m);
  lv_obj_move_foreground(m);
  return m;
}

static void createMarkers() {
  if (!mapCont || !mapActive)
    return;
  deleteMarkers();

  // Propre station
  if (gpsLat != 0.0 || gpsLon != 0.0) {
    int cx, cy;
    if (latLonToContPos((float)gpsLat, (float)gpsLon, &cx, &cy)) {
      const char *cs = (!Config.beacons.empty())
                           ? Config.beacons[myBeaconsIndex].callsign.c_str()
                           : "HOME";
      const char *ov = (!Config.beacons.empty())
                           ? Config.beacons[myBeaconsIndex].overlay.c_str()
                           : "/";
      const char *sy = (!Config.beacons.empty())
                           ? Config.beacons[myBeaconsIndex].symbol.c_str()
                           : "&";
      char table = (ov[0] == '/') ? '/' : '\\';
      char sym = sy[0];
      char ovChar = (ov[0] != '/' && ov[0] != '\\') ? ov[0] : 0;
      lv_obj_t *m = createMarkerObj(mapCont, cs, table, sym, ovChar,
                                    lv_color_hex(0x0055cc), cx, cy, -1);
      if (m && markerCount < MAX_MARKERS)
        markers[markerCount++] = {m, -1};
    }
  }

  // Received stations
  STATION_Utils::cleanOldMapStations();
  for (int i = 0; i < MAP_STATIONS_MAX && markerCount < MAX_MARKERS; i++) {
    MapStation *st = STATION_Utils::getMapStation(i);
    if (!st || !st->valid)
      continue;
    if (st->latitude == 0.0f && st->longitude == 0.0f)
      continue;

    int cx, cy;
    if (!latLonToContPos(st->latitude, st->longitude, &cx, &cy))
      continue;

    char ov0 = (st->overlay.length() > 0) ? st->overlay[0] : '/';
    char table = (ov0 == '/') ? '/' : '\\';
    char sym = (st->symbol.length() > 0) ? st->symbol[0] : '>';
    char ovChar = (ov0 != '/' && ov0 != '\\') ? ov0 : 0;

    // Fallback colour based on age
    uint32_t elapsed = millis() - st->lastTime;
    lv_color_t fc = (elapsed < 10 * 60 * 1000) ? lv_color_hex(0xff6600)
                                               : lv_color_hex(0x888888);

    lv_obj_t *m = createMarkerObj(mapCont, st->callsign.c_str(), table, sym,
                                  ovChar, fc, cx, cy, i);
    if (m)
      markers[markerCount++] = {m, i};
  }
}

// Update marker positions without recreating them (pan)
static void updateMarkerPositions() {
  for (int i = 0; i < markerCount; i++) {
    if (!markers[i].obj || !lv_obj_is_valid(markers[i].obj))
      continue;

    float lat, lon;
    if (markers[i].stationIdx < 0) {
      lat = (float)gpsLat;
      lon = (float)gpsLon;
    } else {
      MapStation *st = STATION_Utils::getMapStation(markers[i].stationIdx);
      if (!st || !st->valid)
        continue;
      lat = st->latitude;
      lon = st->longitude;
    }
    int cx, cy;
    latLonToContPos(lat, lon, &cx, &cy);
    lv_obj_set_pos(markers[i].obj, cx - MARKER_W / 2, cy - ICON_SIZE / 2);
    lv_obj_update_layout(markers[i].obj);
  }
}

// ============================================================
// Rechargement des tuiles + marqueurs
// ============================================================
// Canvas pour le rendu vectoriel (taille = TILE_SIZE)
static lv_obj_t *vecCanvas[GRID][GRID];
static int vecLastZ[GRID][GRID], vecLastTX[GRID][GRID], vecLastTY[GRID][GRID];
static uint8_t *vecBuf[GRID][GRID];

// Labels carto au niveau écran (widgets LVGL, collision globale inter-tuiles).
#include <vector>
#include <algorithm>
static bool fullscreenMap = false;
#define MAX_MAP_LABELS 100
static lv_obj_t *mapLabels[MAX_MAP_LABELS];
static int mapLabelSrx[MAX_MAP_LABELS], mapLabelSry[MAX_MAP_LABELS]; // pos relative au sprite
static int mapLabelCount = 0;

static void clearMapLabels() {
  for (int i = 0; i < mapLabelCount; i++)
    if (mapLabels[i] && lv_obj_is_valid(mapLabels[i])) lv_obj_del(mapLabels[i]);
  mapLabelCount = 0;
}

// Recale les labels selon l'origine sprite courante (ox,oy = coin haut-gauche du
// sprite à l'écran, dragAccum inclus). Appelé pendant le pan, comme les tuiles.
static void repositionMapLabels(int ox, int oy) {
  for (int i = 0; i < mapLabelCount; i++)
    if (mapLabels[i] && lv_obj_is_valid(mapLabels[i]))
      lv_obj_set_pos(mapLabels[i], ox + mapLabelSrx[i], oy + mapLabelSry[i]);
}

// Collecte les labels des tuiles visibles, placement global (collision), création widgets.
static void refreshMapLabels() {
  clearMapLabels();
  if (!(zoom >= 9 && MapVector::isOpen()) || !mapCont) return;
  struct SL { int x, y, prio; std::string text; uint8_t r, g, b; const lv_font_t *font;
              int angle; bool followLine; };
  std::vector<SL> all;
  int mapH = fullscreenMap ? 600 : MAP_H;
  int ox0 = (CONT_W - SPRITE_SIZE) / 2 + dragAccumX;
  int oy0 = (mapH - SPRITE_SIZE) / 2 + dragAccumY;
  for (int dy = 0; dy < GRID; dy++)
    for (int dx = 0; dx < GRID; dx++) {
      int tx = centerTX + dx - GRID / 2, ty = centerTY + dy - GRID / 2;
      std::vector<MapVector::Label> tl;
      MapVector::getTileLabels(zoom, tx, ty, tl);
      int sx = ox0 + dx * TILE_SIZE, sy = oy0 + dy * TILE_SIZE;
      for (auto &l : tl) all.push_back({sx + l.px, sy + l.py, l.priority, l.text, l.r, l.g, l.b, l.font, l.angle, l.followLine});
    }
  std::sort(all.begin(), all.end(), [](const SL &a, const SL &b) { return a.prio < b.prio; });
  struct Box { int x, y, w, h; };
  std::vector<Box> placed;
  std::vector<std::string> seenText; // 1 seule étiquette par nom/ref à l'écran (anti-répétition inter-tuiles)
  for (auto &l : all) {
    if (mapLabelCount >= MAX_MAP_LABELS) break;
    if (std::find(seenText.begin(), seenText.end(), l.text) != seenText.end()) continue;
    int h = l.font->line_height;
    int w = (int)(l.text.size() * h * 0.52f); // estimation largeur (évite l'API texte v8)
    int lx = l.x - w / 2, ly = l.y - h / 2;
    if (lx + w < 0 || lx > CONT_W || ly + h < 0 || ly > mapH) continue; // hors zone visible
    bool ov = false;
    for (auto &p : placed)
      if (!(lx + w + 3 <= p.x || p.x + p.w + 3 <= lx || ly + h + 2 <= p.y || p.y + p.h + 2 <= ly)) { ov = true; break; }
    if (ov) continue;
    lv_obj_t *lbl = lv_label_create(mapCont);
    lv_label_set_text(lbl, l.text.c_str());
    lv_obj_set_style_text_font(lbl, l.font, 0);
    lv_obj_set_style_text_color(lbl, lv_color_make(l.r, l.g, l.b), 0);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    if (l.followLine) {
      // Cours d'eau : texte le long de la ligne (firmware GEOM_TEXT_LINE).
      lv_obj_set_style_transform_pivot_x(lbl, w / 2, 0);
      lv_obj_set_style_transform_pivot_y(lbl, h / 2, 0);
      lv_obj_set_style_transform_rotation(lbl, l.angle * 10, 0); // LVGL : 0,1°
    }
    // Pas de fond : texte seul, comme sur OSM (halo fin = étape surface unique).
    lv_obj_set_pos(lbl, lx, ly);
    mapLabelSrx[mapLabelCount] = lx - ox0; // position relative au sprite (drag-suivi)
    mapLabelSry[mapLabelCount] = ly - oy0;
    mapLabels[mapLabelCount++] = lbl;
    placed.push_back({lx, ly, w, h});
    seenText.push_back(l.text);
  }
}

// Trace canvas overlay for station movement lines
static lv_obj_t *traceCanvas = nullptr;
static uint8_t *traceBuf = nullptr;
#define TRACE_CANVAS_W SPRITE_SIZE
#define TRACE_CANVAS_H 600

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

static void drawTraces() {
    if (!traceBuf) return;
    // Clear buffer (transparent)
    memset(traceBuf, 0, TRACE_CANVAS_W * TRACE_CANVAS_H * 4);

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
    if (traceCanvas && lv_obj_is_valid(traceCanvas))
        lv_obj_invalidate(traceCanvas);
}

// Own trace rendering — purple, same as firmware (0x9933FF)
static void drawOwnTrace() {
    if (!traceBuf || ownTraceCount < 2) return;
    uint32_t now = millis();
    int prevSX = INT_MIN, prevSY = INT_MIN;
    bool firstPt = true;
    // Pixel skip threshold (same as firmware)
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
    if (traceCanvas && lv_obj_is_valid(traceCanvas))
        lv_obj_invalidate(traceCanvas);
}

// Record own GPS position in ring buffer (threshold ~10m)
static void recordOwnTrace() {
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

static void reloadTiles();  // fwd
static void reposTraceCanvas(); // fwd
static void updateMarkerPositions(); // fwd
static void repositionAll();  // fwd, defined after fullscreenMap

// 50ms timer — inertia, GPS follow, periodic station refresh (firmware: map_refresh_timer_cb)
static void mapTimerCb(lv_timer_t *) {
    if (!mapActive || !mapCont) return;

    // Inertia — apply momentum when finger is off screen
    if (!panActive && (velX != 0.0f || velY != 0.0f)) {
        static uint32_t lastInertiaMs = 0;
        uint32_t now = millis();
        uint32_t dt = (lastInertiaMs > 0) ? (now - lastInertiaMs) : 0;
        lastInertiaMs = now;
        if (dt > 0 && dt < 100) {
            float friction = 0.85f;
            int dx = (int)(velX * (float)dt);
            int dy = (int)(velY * (float)dt);
            dragAccumX += dx;
            dragAccumY += dy;

            static int lastCTx = 0, lastCTy = 0;
            while (dragAccumX >= TILE_SIZE) { dragAccumX -= TILE_SIZE; centerTX--; }
            while (dragAccumX <= -TILE_SIZE) { dragAccumX += TILE_SIZE; centerTX++; }
            while (dragAccumY >= TILE_SIZE) { dragAccumY -= TILE_SIZE; centerTY--; }
            while (dragAccumY <= -TILE_SIZE) { dragAccumY += TILE_SIZE; centerTY++; }
            repositionAll();

            if (centerTX != lastCTx || centerTY != lastCTy) {
                lastCTx = centerTX; lastCTy = centerTY;
                MapMath::tileToLatLon(centerTX, centerTY, zoom, (float *)&centerLat, (float *)&centerLon);
                reloadTiles();
            }

            velX *= friction;
            velY *= friction;
            if (fabsf(velX) < 0.01f) velX = 0.0f;
            if (fabsf(velY) < 0.01f) velY = 0.0f;
        }
    }

    // GPS follow — recenter if following and GPS moved
    static int lastCenterTxGPS = 0, lastCenterTyGPS = 0;
    if (mapFollowGps && (gpsLat != 0.0 || gpsLon != 0.0)) {
        int gpsTX, gpsTY;
        MapMath::latLonToTile((float)gpsLat, (float)gpsLon, zoom, &gpsTX, &gpsTY);
        if (gpsTX != lastCenterTxGPS || gpsTY != lastCenterTyGPS) {
            centerTX = lastCenterTxGPS = gpsTX;
            centerTY = lastCenterTyGPS = gpsTY;
            centerLat = gpsLat; centerLon = gpsLon;
            int spriteX, spriteY;
            MapMath::latLonToPixel((float)gpsLat, (float)gpsLon,
                                   (float)gpsLat, (float)gpsLon,
                                   zoom, true, centerTX, centerTY, &spriteX, &spriteY);
            dragAccumX = SPRITE_SIZE / 2 - spriteX;
            dragAccumY = SPRITE_SIZE / 2 - spriteY;
            velX = velY = 0.0f;
            reloadTiles();
        }
    }

    // Periodic station refresh (~10s)
    static uint16_t tickCounter = 0;
    if (++tickCounter >= 200) {
        tickCounter = 0;
        STATION_Utils::cleanOldMapStations();
        if (!panActive) createMarkers();
    }
}

static void reposTraceCanvas(); // fwd

static void reloadTiles() {
  for (int dy = 0; dy < GRID; dy++) {
    for (int dx = 0; dx < GRID; dx++) {
      int tx = centerTX + dx - GRID / 2, ty = centerTY + dy - GRID / 2;
      // Vector tile if zoom >= 9, raster otherwise
      if (zoom >= 9 && MapVector::isOpen()) {
        // Hide raster, show vector canvas
        lv_obj_add_flag(tileImg[dy][dx], LV_OBJ_FLAG_HIDDEN);
        if (!vecCanvas[dy][dx]) {
          vecCanvas[dy][dx] = lv_canvas_create(mapCont);
          vecBuf[dy][dx] = (uint8_t *)lv_malloc(LV_CANVAS_BUF_SIZE(
              TILE_SIZE, TILE_SIZE, 32, LV_DRAW_BUF_STRIDE_ALIGN));
          lv_canvas_set_buffer(vecCanvas[dy][dx], vecBuf[dy][dx], TILE_SIZE,
                               TILE_SIZE, LV_COLOR_FORMAT_ARGB8888);
        }
        lv_obj_clear_flag(vecCanvas[dy][dx], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(vecCanvas[dy][dx],
                       (CONT_W - SPRITE_SIZE) / 2 + dx * TILE_SIZE + dragAccumX,
                       (MAP_H - SPRITE_SIZE) / 2 + dy * TILE_SIZE + dragAccumY);
        lv_obj_set_size(vecCanvas[dy][dx], TILE_SIZE, TILE_SIZE);
        // Cache: skip re-render if same tile as last time
        if (vecLastZ[dy][dx] != zoom || vecLastTX[dy][dx] != tx || vecLastTY[dy][dx] != ty) {
          if (!MapVector::renderTile(vecCanvas[dy][dx], zoom, tx, ty, TILE_SIZE)) {
            // Tile not in cache — queue async render for next time
            MapVector::requestTile(zoom, tx, ty);
          }
          vecLastZ[dy][dx]  = zoom;
          vecLastTX[dy][dx] = tx;
          vecLastTY[dy][dx] = ty;
        }
      } else {
        // Raster fallback
        if (vecCanvas[dy][dx])
          lv_obj_add_flag(vecCanvas[dy][dx], LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(tileImg[dy][dx], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(tileImg[dy][dx],
                       (CONT_W - SPRITE_SIZE) / 2 + dx * TILE_SIZE + dragAccumX,
                       (MAP_H - SPRITE_SIZE) / 2 + dy * TILE_SIZE + dragAccumY);
        char p[512];
        snprintf(p, sizeof(p), "A:%s/%s/%d/%d/%d.jpg", mapsRoot(), mapRegion,
                 zoom, tx, ty);
        if (tileExists(tx, ty, zoom))
          lv_image_set_src(tileImg[dy][dx], p);
        else
          lv_image_set_src(tileImg[dy][dx], LV_SYMBOL_IMAGE);
      }
    }
  }
  if (titleLabel) {
    char z[16];
    snprintf(z, sizeof(z), "MAP (Z%d)", zoom);
    lv_label_set_text(titleLabel, z);
  }
  if (infoLabel) {
    char ib[128];
    snprintf(ib, sizeof(ib), "Lat:%.4f  Lon:%.4f  Stn:%d", centerLat, centerLon,
             mapStationsCount);
    lv_label_set_text(infoLabel, ib);
  }
  drawTraces();
  drawOwnTrace();
  reposTraceCanvas();
  refreshMapLabels();
  createMarkers();
  setPosition(gpsLat, gpsLon);
}

// ============================================================
// API publique
// ============================================================
void setPosition(double lat, double lon) {
  gpsLat = lat;
  gpsLon = lon;
  recordOwnTrace();
  if (!mapActive || !mapCont)
    return;
  // Update own marker if it exists
  for (int i = 0; i < markerCount; i++) {
    if (markers[i].stationIdx == -1 && markers[i].obj &&
        lv_obj_is_valid(markers[i].obj)) {
      int cx, cy;
      latLonToContPos(lat, lon, &cx, &cy);
      lv_obj_set_pos(markers[i].obj, cx - MARKER_W / 2, cy - ICON_SIZE / 2);
      lv_obj_update_layout(markers[i].obj);
      return;
    }
  }
  // No own marker yet — recreate all markers
  createMarkers();
}

static void recenterForZoom(int newZoom) {
    int spriteCX = MAP_SPRITE_SIZE / 2 - dragAccumX;
    int spriteCY = MAP_SPRITE_SIZE / 2 - dragAccumY;
    float lat, lon;
    MapMath::pixelToLatLon(spriteCX, spriteCY, zoom, true,
                           centerTX, centerTY, 0, 0, &lat, &lon);

    MapMath::latLonToTile(lat, lon, newZoom, &centerTX, &centerTY);
    centerLat = lat;
    centerLon = lon;

    // Sub-tile correction: lat/lon may not land at sprite pixel 640 (screen
    // centre) after the tile index is truncated.  Absorb the offset into
    // dragAccumX/Y so markers and tiles stay aligned.
    int spriteX, spriteY;
    MapMath::latLonToPixel(lat, lon, lat, lon, newZoom, true,
                           centerTX, centerTY, &spriteX, &spriteY);
    dragAccumX = MAP_SPRITE_SIZE / 2 - spriteX;
    dragAccumY = MAP_SPRITE_SIZE / 2 - spriteY;
}

void zoomIn() {
  if (zoom < zoomMax) {
    mapFollowGps = false;
    if (btnRecenter) lv_obj_set_style_bg_color(btnRecenter, lv_color_hex(0xff6600), 0);
    recenterForZoom(zoom + 1);
    zoom++;
    if (mapActive) reloadTiles();
  }
}

void refreshStations() {
  if (mapActive && mapCont)
    createMarkers();
}

void zoomOut() {
  if (zoom > zoomMin) {
    mapFollowGps = false;
    if (btnRecenter) lv_obj_set_style_bg_color(btnRecenter, lv_color_hex(0xff6600), 0);
    recenterForZoom(zoom - 1);
    zoom--;
    if (mapActive) reloadTiles();
  }
}

// ============================================================
// Callbacks boutons & pan
// ============================================================
static void backCb(lv_event_t *) {
  closeStationPopup();
  mapActive = false;
  if (mapTimer) { lv_timer_del(mapTimer); mapTimer = nullptr; }
  deleteMarkers();
  clearMapLabels();
  // Free trace canvas
  if (traceCanvas && lv_obj_is_valid(traceCanvas)) lv_obj_del(traceCanvas);
  traceCanvas = nullptr;
  if (traceBuf) { lv_free(traceBuf); traceBuf = nullptr; }
  // Free vector canvases
  for (int dy = 0; dy < GRID; dy++)
    for (int dx = 0; dx < GRID; dx++) {
      if (vecCanvas[dy][dx]) {
        lv_obj_del(vecCanvas[dy][dx]);
        vecCanvas[dy][dx] = nullptr;
      }
      if (vecBuf[dy][dx]) {
        lv_free(vecBuf[dy][dx]);
        vecBuf[dy][dx] = nullptr;
      }
    }
  UIDashboard::returnToDashboard();
}

static void zoomCb(lv_event_t *e) {
  closeStationPopup();
  int d = (int)(intptr_t)lv_event_get_user_data(e);
  if (d > 0) zoomIn(); else zoomOut();
}

static void reposTraceCanvas() {
    if (!traceCanvas || !lv_obj_is_valid(traceCanvas)) return;
    int mapH = fullscreenMap ? 600 : MAP_H;
    lv_obj_set_pos(traceCanvas,
                   (CONT_W - SPRITE_SIZE) / 2 + dragAccumX,
                   (mapH - SPRITE_SIZE) / 2 + dragAccumY);
    lv_obj_set_size(traceCanvas, TRACE_CANVAS_W, mapH);
}

// Reposition tile grid + trace canvas + markers without reloading tiles
static void repositionAll() {
    int mapH = fullscreenMap ? 600 : MAP_H;
    for (int dy2 = 0; dy2 < GRID; dy2++)
        for (int dx2 = 0; dx2 < GRID; dx2++) {
            int tx = (CONT_W - SPRITE_SIZE) / 2 + dx2 * TILE_SIZE + dragAccumX;
            int ty = (mapH - SPRITE_SIZE) / 2 + dy2 * TILE_SIZE + dragAccumY;
            lv_obj_set_pos(tileImg[dy2][dx2], tx, ty);
            if (vecCanvas[dy2][dx2]) lv_obj_set_pos(vecCanvas[dy2][dx2], tx, ty);
        }
    reposTraceCanvas();
    updateMarkerPositions();
    repositionMapLabels((CONT_W - SPRITE_SIZE) / 2 + dragAccumX,
                        (mapH - SPRITE_SIZE) / 2 + dragAccumY);
}

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
  // Reposition tiles and markers
  for (int dy = 0; dy < GRID; dy++)
    for (int dx = 0; dx < GRID; dx++)
      lv_obj_set_pos(tileImg[dy][dx],
                     (CONT_W - SPRITE_SIZE) / 2 + dx * TILE_SIZE + dragAccumX,
                     (fullscreenMap ? (600 - SPRITE_SIZE) / 2
                                    : (MAP_H - SPRITE_SIZE) / 2) +
                         dy * TILE_SIZE + dragAccumY);
  reposTraceCanvas();
  updateMarkerPositions();
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
    panActive = true;
    velX = velY = 0.0f;
    closeStationPopup();
  } else if (code == LV_EVENT_PRESSING && panActive) {
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
      velX = velX * (1.0f - weight) + instVelX * weight;
      velY = velY * (1.0f - weight) + instVelY * weight;
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

    // Reposition tiles
    for (int dy2 = 0; dy2 < GRID; dy2++)
      for (int dx2 = 0; dx2 < GRID; dx2++) {
        lv_obj_set_pos(
            tileImg[dy2][dx2],
            (CONT_W - SPRITE_SIZE) / 2 + dx2 * TILE_SIZE + dragAccumX,
            (MAP_H - SPRITE_SIZE) / 2 + dy2 * TILE_SIZE + dragAccumY);
        // Vector canvases follow the same position
        if (vecCanvas[dy2][dx2])
          lv_obj_set_pos(
              vecCanvas[dy2][dx2],
              (CONT_W - SPRITE_SIZE) / 2 + dx2 * TILE_SIZE + dragAccumX,
              (MAP_H - SPRITE_SIZE) / 2 + dy2 * TILE_SIZE + dragAccumY);
      }

    updateMarkerPositions();
    reposTraceCanvas();
    // Les labels suivent le même offset que les tuiles, sinon ils traînent au pan.
    repositionMapLabels((CONT_W - SPRITE_SIZE) / 2 + dragAccumX,
                        (MAP_H - SPRITE_SIZE) / 2 + dragAccumY);

    if (centerTX != lctx || centerTY != lcty) {
      lctx = centerTX;
      lcty = centerTY;
      MapMath::tileToLatLon(centerTX, centerTY, zoom, (float *)&centerLat,
                            (float *)&centerLon);
      reloadTiles();
    }
  } else if (code == LV_EVENT_RELEASED) {
    int dx = p.x - pressPt.x, dy = p.y - pressPt.y;
    bool wasPan = panActive && (abs(dx) > 10 || abs(dy) > 10);
    panActive = false;

    if (wasPan) {
      if (infoLabel) {
        char buf[128];
        snprintf(buf, sizeof(buf), "Lat:%.4f  Lon:%.4f  Stn:%d", centerLat, centerLon, mapStationsCount);
        lv_label_set_text(infoLabel, buf);
      }
      return;
    }

    // No significant drag — check manual hit-test on markers
    uint32_t held = millis() - pressMs;

    for (int i = 0; i < markerCount; i++) {
      if (!markers[i].obj || !lv_obj_is_valid(markers[i].obj)) continue;
      lv_area_t a;
      lv_obj_get_coords(markers[i].obj, &a);
      if (pressPt.x < a.x1 || pressPt.x > a.x2 ||
          pressPt.y < a.y1 || pressPt.y > a.y2) continue;
      if (held > 400) {
        int idx = markers[i].stationIdx;
        if (idx >= 0) {
          MapStation *st = STATION_Utils::getMapStation(idx);
          if (st && st->valid) {
            closeStationPopup();
            UIMessaging::openComposeWithCallsign(st->callsign);
          }
        }
      } else {
        show_station_popup(markers[i].stationIdx);
      }
      return;
    }
  }
}

// ============================================================
// Create map screen
// ============================================================
lv_obj_t *create(lv_obj_t *) {
  discoverRegion();
  discoverZooms();
  discoverDefaultPosition();
  MapVector::initLabelFonts();   // police accentuée (OpenSans-Bold), comme le firmware

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
      velX = velY = 0.0f;
      reloadTiles();
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
  panActive = false;
  markerCount = 0;
  stationPopup = nullptr;

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
  lv_obj_add_event_cb(mapCont, mapTouchCB, LV_EVENT_DOUBLE_CLICKED, NULL);
  lv_obj_add_event_cb(mapCont, mapTouchCB, LV_EVENT_PRESSED, NULL);
  lv_obj_add_event_cb(mapCont, mapTouchCB, LV_EVENT_PRESSING, NULL);
  lv_obj_add_event_cb(mapCont, mapTouchCB, LV_EVENT_RELEASED, NULL);

  // Grille 3×3 de tuiles
  for (int dy = 0; dy < GRID; dy++)
    for (int dx = 0; dx < GRID; dx++) {
      tileImg[dy][dx] = lv_image_create(mapCont);
      lv_obj_set_size(tileImg[dy][dx], TILE_SIZE, TILE_SIZE);
      lv_obj_set_pos(tileImg[dy][dx],
                     (CONT_W - SPRITE_SIZE) / 2 + dx * TILE_SIZE,
                     (MAP_H - SPRITE_SIZE) / 2 + dy * TILE_SIZE);
    }

  // Trace canvas overlay for station movement lines
  traceBuf = (uint8_t *)lv_malloc(LV_CANVAS_BUF_SIZE(TRACE_CANVAS_W, TRACE_CANVAS_H, 32, LV_DRAW_BUF_STRIDE_ALIGN));
  traceCanvas = lv_canvas_create(mapCont);
  lv_canvas_set_buffer(traceCanvas, traceBuf, TRACE_CANVAS_W, TRACE_CANVAS_H, LV_COLOR_FORMAT_ARGB8888);
  lv_obj_set_pos(traceCanvas, (CONT_W - SPRITE_SIZE) / 2, (MAP_H - SPRITE_SIZE) / 2);
  lv_obj_set_size(traceCanvas, TRACE_CANVAS_W, MAP_H);
  lv_obj_clear_flag(traceCanvas, LV_OBJ_FLAG_CLICKABLE);

  mapActive = true;
  reloadTiles(); // load tiles + create station markers

  // Start 50ms periodic timer (inertia, GPS follow, station refresh)
  if (!mapTimer) mapTimer = lv_timer_create(mapTimerCb, 50, NULL);

  return scr;
}

} // namespace MapRaster
