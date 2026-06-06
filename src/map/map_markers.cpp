#include "map/map_markers.h"
#include "map/map_io.h"
#include "map/map_state.h"
#include "map_coordinate_math.h"
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

#define MAX_MARKERS (MAP_STATIONS_MAX + 1) // stations + own
struct MarkerInfo {
    int sx, sy;     // sprite coords of the geo anchor (icon centre)
    int stationIdx; // -1 = own station, >=0 = mapStations index
};
static MarkerInfo markers[MAX_MARKERS];
static int markerCount = 0;

// The map canvas markers are drawn into — kept so hitTest can map a screen
// point back to sprite coords via the canvas's absolute position.
static lv_obj_t *markerCanvas = nullptr;

static lv_obj_t *stationPopup = nullptr;

// Marker dimensions (24x24 icon + callsign below)
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

void deleteMarkers() { markerCount = 0; markerCanvas = nullptr; }

// Draw one marker centred on sprite (sx, sy): APRS icon (or fallback disc),
// optional overlay char, and the callsign with a semi-transparent backdrop.
// Each marker uses its own layer pass — lv_draw_image reads the icon path at
// flush time, so it must stay valid until finish_layer commits the draw.
static void drawMarker(lv_obj_t *canvas, int sx, int sy, const char *callsign,
                       char table, char sym, char overlayChar,
                       lv_color_t fallbackColor) {
    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    char iconPath[320];
    lv_area_t ia = { sx - ICON_SIZE / 2, sy - ICON_SIZE / 2,
                     sx + ICON_SIZE / 2 - 1, sy + ICON_SIZE / 2 - 1 };
    if (sym != '\0' && MapIO::getSymbolPath(table, sym, iconPath, sizeof(iconPath))) {
        lv_draw_image_dsc_t id; lv_draw_image_dsc_init(&id);
        id.src = iconPath;
        lv_draw_image(&layer, &id, &ia);

        // Overlay character (digi 'D', iGate 'I', etc.) drawn on the icon.
        if (overlayChar != 0 && overlayChar != '/' && overlayChar != '\\') {
            char ovTxt[2] = { overlayChar, 0 };
            lv_draw_label_dsc_t od; lv_draw_label_dsc_init(&od);
            od.text = ovTxt; od.font = &lv_font_montserrat_14;
            od.color = lv_color_hex(0xffffff); od.align = LV_TEXT_ALIGN_CENTER;
            lv_area_t oa = { sx - ICON_SIZE / 2, sy - ICON_SIZE / 2 + 4,
                             sx + ICON_SIZE / 2 - 1, sy + ICON_SIZE / 2 - 1 };
            lv_draw_label(&layer, &od, &oa);
        }
    } else {
        // Fallback: coloured disc 16x16.
        lv_draw_rect_dsc_t dd; lv_draw_rect_dsc_init(&dd);
        dd.bg_color = fallbackColor; dd.bg_opa = LV_OPA_COVER;
        dd.radius = LV_RADIUS_CIRCLE;
        lv_area_t da = { sx - 8, sy - 8, sx + 7, sy + 7 };
        lv_draw_rect(&layer, &dd, &da);
    }

    // Callsign with semi-transparent background, centred under the icon.
    int lh = lv_font_montserrat_12.line_height;
    int tw = (int)(strlen(callsign) * 8) + 6;
    int tx = sx - tw / 2, ty = sy + ICON_SIZE / 2;
    lv_area_t ta = { tx, ty, tx + tw - 1, ty + lh - 1 };
    lv_draw_rect_dsc_t bd; lv_draw_rect_dsc_init(&bd);
    bd.bg_color = lv_color_hex(0x000000); bd.bg_opa = LV_OPA_30;
    lv_draw_rect(&layer, &bd, &ta);
    lv_draw_label_dsc_t ld; lv_draw_label_dsc_init(&ld);
    ld.text = callsign; ld.font = &lv_font_montserrat_12;
    ld.color = lv_color_hex(0xffffff); ld.align = LV_TEXT_ALIGN_CENTER;
    lv_draw_label(&layer, &ld, &ta);

    lv_canvas_finish_layer(canvas, &layer);
}

// lat/lon → sprite pixel; returns true if within the sprite (±1 tile margin).
static bool latLonToSprite(float lat, float lon, int *sx, int *sy) {
    int px, py;
    MapMath::latLonToPixel(lat, lon, centerLat, centerLon, zoom, true,
                           centerTX, centerTY, &px, &py);
    *sx = px; *sy = py;
    return (px >= -TILE_SIZE && px < SPRITE_SIZE + TILE_SIZE &&
            py >= -TILE_SIZE && py < SPRITE_SIZE + TILE_SIZE);
}

// Draw own station + every valid received station into `canvas`, recording
// each marker's sprite position for hit-testing.
void drawInto(lv_obj_t *canvas) {
    if (!canvas || !mapActive) return;
    markerCanvas = canvas;
    markerCount = 0;
    STATION_Utils::cleanOldMapStations();

    // Own station
    if (gpsLat != 0.0 || gpsLon != 0.0) {
        int sx, sy;
        if (latLonToSprite((float)gpsLat, (float)gpsLon, &sx, &sy)) {
            const char *cs = (!Config.beacons.empty())
                                 ? Config.beacons[myBeaconsIndex].callsign.c_str()
                                 : "HOME";
            const char *ov = (!Config.beacons.empty())
                                 ? Config.beacons[myBeaconsIndex].overlay.c_str()
                                 : "/";
            const char *sy_ = (!Config.beacons.empty())
                                 ? Config.beacons[myBeaconsIndex].symbol.c_str()
                                 : "&";
            char table = (ov[0] == '/') ? '/' : '\\';
            char sym = sy_[0];
            char ovChar = (ov[0] != '/' && ov[0] != '\\') ? ov[0] : 0;
            drawMarker(canvas, sx, sy, cs, table, sym, ovChar,
                       lv_color_hex(0x0055cc));
            if (markerCount < MAX_MARKERS)
                markers[markerCount++] = {sx, sy, -1};
        }
    }

    // Received stations
    for (int i = 0; i < MAP_STATIONS_MAX && markerCount < MAX_MARKERS; i++) {
        MapStation *st = STATION_Utils::getMapStation(i);
        if (!st || !st->valid)
            continue;
        if (st->latitude == 0.0f && st->longitude == 0.0f)
            continue;

        int sx, sy;
        if (!latLonToSprite(st->latitude, st->longitude, &sx, &sy))
            continue;

        char ov0 = (st->overlay.length() > 0) ? st->overlay[0] : '/';
        char table = (ov0 == '/') ? '/' : '\\';
        char sym = (st->symbol.length() > 0) ? st->symbol[0] : '>';
        char ovChar = (ov0 != '/' && ov0 != '\\') ? ov0 : 0;

        // Fallback colour based on age
        uint32_t elapsed = millis() - st->lastTime;
        lv_color_t fc = (elapsed < 10 * 60 * 1000) ? lv_color_hex(0xff6600)
                                                   : lv_color_hex(0x888888);

        drawMarker(canvas, sx, sy, st->callsign.c_str(), table, sym, ovChar, fc);
        markers[markerCount++] = {sx, sy, i};
    }
}

bool hitTest(lv_point_t point, int *stationIdx) {
    // Screen point → sprite coords via the canvas's absolute on-screen origin
    // (robust to the container position, fullscreen and the pan offset).
    if (!markerCanvas || !lv_obj_is_valid(markerCanvas)) return false;
    lv_area_t ca;
    lv_obj_get_coords(markerCanvas, &ca);
    int sx = point.x - ca.x1;
    int sy = point.y - ca.y1;
    for (int i = 0; i < markerCount; i++) {
        int bx = markers[i].sx - MARKER_W / 2;
        int by = markers[i].sy - ICON_SIZE / 2;
        if (sx < bx || sx > bx + MARKER_W || sy < by || sy > by + MARKER_H)
            continue;
        if (stationIdx) *stationIdx = markers[i].stationIdx;
        return true;
    }
    return false;
}

} // namespace MapMarkers
