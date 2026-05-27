/* Raster tile map — ESP32 create_map_screen() + region discovery ported to
 * Linux Stations APRS : icônes positionnées sur les tuiles, clic → popup info
 */
#include "map_raster.h"
#include "configuration.h"
#include "gps_math.h"
#include "map_coordinate_math.h"
#include "map_vector.h"
#include "station_utils.h"
#include "ui_dashboard.h"
#include "ui_messaging.h"
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
#define GRID 5 // 5×5 : couvre 1024×520 avec marge pour préchargement
#define SPRITE_SIZE (GRID * TILE_SIZE) // 1280
#define CONT_W 1024
#define MAP_H (600 - 50 - 30) // 520  (sous titlebar, au-dessus infobar)

// ---- Tile state ----
static lv_obj_t *tileImg[GRID][GRID];
static char tileDir[256] = "", mapRegion[64] = "";
static double centerLat = 42.96, centerLon = 1.37;
static double gpsLat = 0.0, gpsLon = 0.0; // 0,0 = pas de fix GPS
static int zoom = 7, centerTX = 0, centerTY = 0;
static int zoomMin = 6, zoomMax = 8;
static bool mapActive = false;

// Pan state
static lv_point_t dragLast;
static int dragAccumX = 0, dragAccumY = 0;
static bool panActive = false;

// LVGL objects
static lv_obj_t *titleLabel = nullptr;
static lv_obj_t *infoLabel = nullptr;
static lv_obj_t *mapCont = nullptr;

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
// Region / zoom discovery (même logique que l'ESP32 map_tiles)
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
    zoomMin = zMin;
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
// Conversion sprite → coordonnées container (pour placement markers)
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
  // visible si dans le sprite (±1 tuile de marge)
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
    // Propre station
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

static void station_click_cb(lv_event_t *e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  show_station_popup(idx);
}

// Long press : ouvre l'écran compose pré-rempli avec le callsign de la station
static void station_longpress_cb(lv_event_t *e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if (idx < 0)
    return; // own station : pas de message à soi-même
  MapStation *st = STATION_Utils::getMapStation(idx);
  if (!st || !st->valid)
    return;
  closeStationPopup();
  mapActive = false;
  deleteMarkers();
  UIMessaging::openComposeWithCallsign(st->callsign);
}

// ============================================================
// Icônes APRS — chargement depuis les PNG découpés (sd_card)
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

// Dimensions du conteneur marqueur  (icône 24×24 + callsign 14px en-dessous)
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

// Crée un conteneur marqueur centré sur (cx, cy) avec l'icône APRS et le
// callsign table/sym : table APRS ('/' ou '\\') et symbole (char) — fallback
// disque coloré si PNG absent overlayChar : caractère alphanumérique à dessiner
// sur l'icône (alternate table avec overlay),
//               ou 0 si pas d'overlay
static lv_obj_t *createMarkerObj(lv_obj_t *parent, const char *callsign,
                                 char table, char sym, char overlayChar,
                                 lv_color_t fallbackColor, int cx, int cy,
                                 int stationIdx) {
  // Conteneur transparent et cliquable
  lv_obj_t *m = lv_obj_create(parent);
  lv_obj_set_size(m, MARKER_W, MARKER_H);
  lv_obj_set_pos(m, cx - MARKER_W / 2, cy - ICON_SIZE / 2);
  lv_obj_set_style_bg_opa(m, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(m, 0, 0);
  lv_obj_set_style_pad_all(m, 0, 0);
  lv_obj_clear_flag(m, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(m, LV_OBJ_FLAG_CLICKABLE);

  // Icône APRS ou disque fallback
  char iconPath[320];
  if (sym != '\0' && getSymbolPath(table, sym, iconPath, sizeof(iconPath))) {
    lv_obj_t *img = lv_image_create(m);
    lv_image_set_src(img, iconPath);
    lv_obj_align(img, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_size(img, ICON_SIZE, ICON_SIZE);

    // Overlay character (digi 'D', iGate 'I', etc.) directement sur l'icône,
    // sans fond
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
    // Fallback : disque coloré 16×16
    lv_obj_t *dot = lv_obj_create(m);
    lv_obj_set_size(dot, 16, 16);
    lv_obj_set_style_bg_color(dot, fallbackColor, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_align(dot, LV_ALIGN_TOP_MID, 0, 4);
  }

  // Callsign avec fond semi-transparent pour lisibilité
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

  lv_obj_add_event_cb(m, station_click_cb, LV_EVENT_CLICKED,
                      (void *)(intptr_t)stationIdx);
  lv_obj_add_event_cb(m, station_longpress_cb, LV_EVENT_LONG_PRESSED,
                      (void *)(intptr_t)stationIdx);
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

  // Stations reçues
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

    // Couleur fallback selon ancienneté
    uint32_t elapsed = millis() - st->lastTime;
    lv_color_t fc = (elapsed < 10 * 60 * 1000) ? lv_color_hex(0xff6600)
                                               : lv_color_hex(0x888888);

    lv_obj_t *m = createMarkerObj(mapCont, st->callsign.c_str(), table, sym,
                                  ovChar, fc, cx, cy, i);
    if (m)
      markers[markerCount++] = {m, i};
  }
}

// Met à jour les positions des marqueurs sans les recréer (pan)
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
  }
}

// ============================================================
// Rechargement des tuiles + marqueurs
// ============================================================
// Canvas pour le rendu vectoriel (taille = TILE_SIZE)
static lv_obj_t *vecCanvas[GRID][GRID];
static int vecLastZ[GRID][GRID], vecLastTX[GRID][GRID], vecLastTY[GRID][GRID];
static uint8_t *vecBuf[GRID][GRID];

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
  createMarkers();
  setPosition(gpsLat, gpsLon);
}

// ============================================================
// API publique
// ============================================================
void setPosition(double lat, double lon) {
  gpsLat = lat;
  gpsLon = lon;
  if (!mapActive || !mapCont)
    return;
  // Mise à jour du marqueur own si existant
  for (int i = 0; i < markerCount; i++) {
    if (markers[i].stationIdx == -1 && markers[i].obj &&
        lv_obj_is_valid(markers[i].obj)) {
      int cx, cy;
      latLonToContPos(lat, lon, &cx, &cy);
      lv_obj_set_pos(markers[i].obj, cx - MARKER_W / 2, cy - ICON_SIZE / 2);
      return;
    }
  }
  // Pas encore de marqueur own → on recrée les marqueurs
  createMarkers();
}

void zoomIn() {
  if (zoom < zoomMax) {
    zoom++;
    if (mapActive) {
      MapMath::latLonToTile(centerLat, centerLon, zoom, &centerTX, &centerTY);
      reloadTiles();
    }
  }
}

void refreshStations() {
  if (mapActive && mapCont)
    createMarkers();
}

void zoomOut() {
  if (zoom > zoomMin) {
    zoom--;
    if (mapActive) {
      MapMath::latLonToTile(centerLat, centerLon, zoom, &centerTX, &centerTY);
      reloadTiles();
    }
  }
}

// ============================================================
// Callbacks boutons & pan
// ============================================================
static void backCb(lv_event_t *) {
  closeStationPopup();
  mapActive = false;
  deleteMarkers();
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
  double clat = centerLat, clon = centerLon;
  MapMath::tileToLatLon(centerTX, centerTY, zoom, (float *)&clat,
                        (float *)&clon);
  if (d > 0)
    zoomIn();
  else
    zoomOut();
  MapMath::latLonToTile(clat, clon, zoom, &centerTX, &centerTY);
  reloadTiles();
}

static bool fullscreenMap = false;
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
    lv_obj_set_pos(mapCont, 0, 50);
  }
  // Reposition tiles and markers
  for (int dy = 0; dy < GRID; dy++)
    for (int dx = 0; dx < GRID; dx++)
      lv_obj_set_pos(tileImg[dy][dx],
                     (CONT_W - SPRITE_SIZE) / 2 + dx * TILE_SIZE + dragAccumX,
                     (fullscreenMap ? (600 - SPRITE_SIZE) / 2
                                    : (MAP_H - SPRITE_SIZE) / 2) +
                         dy * TILE_SIZE + dragAccumY);
  updateMarkerPositions();
}

static void mapTouchCB(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  lv_indev_t *indev = lv_indev_get_act();
  if (!indev)
    return;
  lv_point_t p;
  lv_indev_get_point(indev, &p);

  if (code == LV_EVENT_DOUBLE_CLICKED) {
    toggleMapFullscreen();
    return;
  }

  if (code == LV_EVENT_PRESSED) {
    // Ne pas reset dragAccumX/Y : on conserve le résidu sub-tuile du pan
    // précédent sinon les tuiles sautent visuellement au premier PRESSING.
    dragLast = p;
    panActive = true;
    closeStationPopup();
  } else if (code == LV_EVENT_PRESSING && panActive) {
    int dx = p.x - dragLast.x, dy = p.y - dragLast.y;
    dragLast = p;
    dragAccumX += dx;
    dragAccumY += dy;

    // Décalage de tuile central
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

    // Repositionner les tuiles
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

    if (centerTX != lctx || centerTY != lcty) {
      lctx = centerTX;
      lcty = centerTY;
      MapMath::tileToLatLon(centerTX, centerTY, zoom, (float *)&centerLat,
                            (float *)&centerLon);
      reloadTiles();
    }
  } else if (code == LV_EVENT_RELEASED && panActive) {
    panActive = false;
    if (infoLabel) {
      char buf[128];
      snprintf(buf, sizeof(buf), "Lat:%.4f  Lon:%.4f  Stn:%d", centerLat,
               centerLon, mapStationsCount);
      lv_label_set_text(infoLabel, buf);
    }
  }
}

// ============================================================
// Création de l'écran map
// ============================================================
lv_obj_t *create(lv_obj_t *) {
  discoverRegion();
  discoverZooms();
  discoverDefaultPosition();

  lv_obj_t *scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x1a1a2e), 0);
  lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);

  // Title bar (always created)
  lv_obj_t *tbar = lv_obj_create(scr);
  tbarMap = tbar;
  lv_obj_set_size(tbar, CONT_W, 50);
  lv_obj_set_pos(tbar, 0, 0);
  lv_obj_set_style_bg_color(tbar, lv_color_hex(0x009933), 0);
  lv_obj_set_style_border_width(tbar, 0, 0);
  lv_obj_set_style_radius(tbar, 0, 0);
  lv_obj_t *btnBack = lv_btn_create(tbar);
  lv_obj_set_size(btnBack, 80, 35);
  lv_obj_set_style_bg_color(btnBack, lv_color_hex(0x16213e), 0);
  lv_obj_align(btnBack, LV_ALIGN_LEFT_MID, 10, 0);
  lv_obj_add_event_cb(btnBack, backCb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *bl = lv_label_create(btnBack);
  lv_label_set_text(bl, "< BACK");
  lv_obj_center(bl);
  titleLabel = lv_label_create(tbar);
  lv_label_set_text(titleLabel, "MAP");
  lv_obj_set_style_text_color(titleLabel, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_text_font(titleLabel, &lv_font_montserrat_20, 0);
  lv_obj_center(titleLabel);

  // Info bar (always created)
  lv_obj_t *ibar = lv_obj_create(scr);
  ibarMap = ibar;
  lv_obj_set_size(ibar, CONT_W, 30);
  lv_obj_set_pos(ibar, 0, 570);
  lv_obj_set_style_bg_color(ibar, lv_color_hex(0x16213e), 0);
  lv_obj_set_style_border_width(ibar, 0, 0);
  lv_obj_set_style_radius(ibar, 0, 0);
  lv_obj_set_style_pad_all(ibar, 4, 0);
  infoLabel = lv_label_create(ibar);
  char ib[128];
  snprintf(ib, sizeof(ib), "Lat:%.4f  Lon:%.4f  Stn:%d", centerLat, centerLon,
           mapStationsCount);
  lv_label_set_text(infoLabel, ib);
  lv_obj_set_style_text_color(infoLabel, lv_color_hex(0xaaaaaa), 0);
  lv_obj_set_style_text_font(infoLabel, &lv_font_montserrat_14, 0);
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
  // Add zoom buttons
  lv_obj_t *zp = lv_btn_create(tbar);
  lv_obj_set_size(zp, 36, 36);
  lv_obj_align(zp, LV_ALIGN_RIGHT_MID, -50, 0);
  lv_obj_set_style_bg_color(zp, lv_color_hex(0x16213e), 0);
  lv_obj_add_event_cb(zp, zoomCb, LV_EVENT_CLICKED, (void *)1);
  lv_obj_t *zlp = lv_label_create(zp);
  lv_label_set_text(zlp, "+");
  lv_obj_center(zlp);
  lv_obj_t *zm = lv_btn_create(tbar);
  lv_obj_set_size(zm, 36, 36);
  lv_obj_align(zm, LV_ALIGN_RIGHT_MID, -5, 0);
  lv_obj_set_style_bg_color(zm, lv_color_hex(0x16213e), 0);
  lv_obj_add_event_cb(zm, zoomCb, LV_EVENT_CLICKED, (void *)-1);
  lv_obj_t *zlm = lv_label_create(zm);
  lv_label_set_text(zlm, "-");
  lv_obj_center(zlm);

  // ---- Conteneur map ----
  mapCont = lv_obj_create(scr);
  lv_obj_set_size(mapCont, CONT_W, MAP_H);
  lv_obj_set_pos(mapCont, 0, 50);
  lv_obj_set_style_bg_color(mapCont, lv_color_hex(0x2F4F4F), 0);
  lv_obj_set_style_border_width(mapCont, 0, 0);
  lv_obj_set_scrollbar_mode(mapCont, LV_SCROLLBAR_MODE_OFF);
  lv_obj_clear_flag(mapCont, LV_OBJ_FLAG_SCROLLABLE);
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

  mapActive = true;
  reloadTiles(); // charge tuiles + crée les marqueurs stations

  return scr;
}

} // namespace MapRaster
