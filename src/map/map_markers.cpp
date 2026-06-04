#include "map/map_markers.h"
#include "map/map_io.h"
#include "map/map_state.h"
#include "configuration.h"
#include "gps_math.h"
#include "station_utils.h"

#include "Arduino.h"  // millis()

#include <cstdio>
#include <cstring>

extern Configuration Config;
extern uint8_t myBeaconsIndex;

namespace MapMarkers {

using namespace MapState;

lv_obj_t *parent = nullptr;

#define MAX_MARKERS (MAP_STATIONS_MAX + 1) // 15 stations + own
struct MarkerInfo {
    lv_obj_t *obj;
    int stationIdx; // -1 = own station, >=0 = mapStations index
};
static MarkerInfo markers[MAX_MARKERS];
static int markerCount = 0;

static lv_obj_t *stationPopup = nullptr;

// Marker container dimensions (24x24 icon + 14px callsign below)
#define MARKER_W 80
#define MARKER_H 40
#define ICON_SIZE 24

void closeStationPopup() {
    if (stationPopup && lv_obj_is_valid(stationPopup))
        lv_msgbox_close(stationPopup);
    stationPopup = nullptr;
}

static void station_popup_deleted_cb(lv_event_t *) { stationPopup = nullptr; }
static void station_popup_close_btn_cb(lv_event_t *) { closeStationPopup(); }

void showStationPopup(int stationIdx) {
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

    // Backdrop + msgbox (NULL = backdrop 100%×100% blocking touch events)
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

void deleteMarkers() {
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
                                 int /*stationIdx*/) {
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
    if (sym != '\0' && MapIO::getSymbolPath(table, sym, iconPath, sizeof(iconPath))) {
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

void createMarkers() {
    if (!parent || !mapActive)
        return;
    deleteMarkers();

    // Own station
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
            lv_obj_t *m = createMarkerObj(parent, cs, table, sym, ovChar,
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

        lv_obj_t *m = createMarkerObj(parent, st->callsign.c_str(), table, sym,
                                      ovChar, fc, cx, cy, i);
        if (m)
            markers[markerCount++] = {m, i};
    }
}

void updateMarkerPositions() {
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

bool updateOwnMarker() {
    for (int i = 0; i < markerCount; i++) {
        if (markers[i].stationIdx != -1) continue;
        if (!markers[i].obj || !lv_obj_is_valid(markers[i].obj)) continue;
        int cx, cy;
        latLonToContPos((float)gpsLat, (float)gpsLon, &cx, &cy);
        lv_obj_set_pos(markers[i].obj, cx - MARKER_W / 2, cy - ICON_SIZE / 2);
        lv_obj_update_layout(markers[i].obj);
        return true;
    }
    return false;
}

bool hitTest(lv_point_t point, int *stationIdx) {
    for (int i = 0; i < markerCount; i++) {
        if (!markers[i].obj || !lv_obj_is_valid(markers[i].obj)) continue;
        lv_area_t a;
        lv_obj_get_coords(markers[i].obj, &a);
        if (point.x < a.x1 || point.x > a.x2 ||
            point.y < a.y1 || point.y > a.y2) continue;
        if (stationIdx) *stationIdx = markers[i].stationIdx;
        return true;
    }
    return false;
}

} // namespace MapMarkers
