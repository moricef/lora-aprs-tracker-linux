#include "map/map_engine.h"
#include "map/map_io.h"
#include "map/map_labels.h"
#include "map/map_markers.h"
#include "map/map_state.h"
#include "map/map_traces.h"
#include "map_coordinate_math.h"
#include "map_vector.h"
#include "station_utils.h"

#include "Arduino.h"  // millis()

#include <cmath>
#include <cstdio>
#include <cstring>

namespace MapEngine {

using namespace MapState;

// Inertia state — written by MapInput on gesture end, decayed here in
// timerTick(). panActive gates inertia roll-out + station refresh.
bool  panActive = false;
float velX = 0.0f, velY = 0.0f;

// 5×5 raster image grid (one per visible tile when zoom < 9).
static lv_obj_t *tileImg[GRID][GRID] = {};

// Vector canvas grid (lazily created when zoom ≥ 9 && PMTiles open).
// vecLast* caches the (z,x,y) tuple rendered into each slot so we don't
// re-render an identical tile while panning sub-tile distances.
static lv_obj_t *vecCanvas[GRID][GRID] = {};
static int       vecLastZ[GRID][GRID]  = {};
static int       vecLastTX[GRID][GRID] = {};
static int       vecLastTY[GRID][GRID] = {};
static uint8_t  *vecBuf[GRID][GRID]    = {};

// Owner widgets passed by map_view at init/setLabels.
static lv_obj_t *parentCont  = nullptr;
static lv_obj_t *titleLabel  = nullptr;
static lv_obj_t *infoLabel   = nullptr;

void init(lv_obj_t *parent) {
    parentCont = parent;
    for (int dy = 0; dy < GRID; dy++)
        for (int dx = 0; dx < GRID; dx++) {
            tileImg[dy][dx] = lv_image_create(parent);
            lv_obj_set_size(tileImg[dy][dx], TILE_SIZE, TILE_SIZE);
            lv_obj_set_pos(tileImg[dy][dx],
                           (CONT_W - SPRITE_SIZE) / 2 + dx * TILE_SIZE,
                           (MAP_H - SPRITE_SIZE) / 2 + dy * TILE_SIZE);
        }
}

void destroy() {
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
            tileImg[dy][dx] = nullptr;  // owned by parent, deleted with it
            vecLastZ[dy][dx] = vecLastTX[dy][dx] = vecLastTY[dy][dx] = 0;
        }
    parentCont = nullptr;
    titleLabel = nullptr;
    infoLabel  = nullptr;
    velX = velY = 0.0f;
    panActive = false;
}

void setLabels(lv_obj_t *title, lv_obj_t *info) {
    titleLabel = title;
    infoLabel  = info;
}

void reloadTiles() {
    if (!parentCont) return;
    for (int dy = 0; dy < GRID; dy++) {
        for (int dx = 0; dx < GRID; dx++) {
            int tx = centerTX + dx - GRID / 2, ty = centerTY + dy - GRID / 2;
            // Vector tile if zoom >= 9, raster otherwise
            if (zoom >= 9 && MapVector::isOpen()) {
                // Hide raster, show vector canvas
                lv_obj_add_flag(tileImg[dy][dx], LV_OBJ_FLAG_HIDDEN);
                if (!vecCanvas[dy][dx]) {
                    vecCanvas[dy][dx] = lv_canvas_create(parentCont);
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
                snprintf(p, sizeof(p), "A:%s/%s/%d/%d/%d.jpg", MapIO::mapsRoot(),
                         mapRegion, zoom, tx, ty);
                if (MapIO::tileExists(tx, ty, zoom))
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
        int spriteCX = SPRITE_SIZE / 2 - dragAccumX;
        int spriteCY = SPRITE_SIZE / 2 - dragAccumY;
        float lat = centerLat, lon = centerLon;
        MapMath::pixelToLatLon(spriteCX, spriteCY, zoom, true,
                               centerTX, centerTY, 0, 0, &lat, &lon);
        char ib[128];
        snprintf(ib, sizeof(ib), "Lat:%.4f  Lon:%.4f  Stn:%d", lat, lon,
                 mapStationsCount);
        lv_label_set_text(infoLabel, ib);
    }
    MapTraces::redraw();
    MapTraces::reposition();
    MapLabels::refresh();
    // reloadTiles places each tile canvas using the current dragAccum
    // (sub-tile correction after a zoom). The label overlay carries the
    // labels in sprite-local coords, so it has to follow the same origin
    // — without this call, labels stay glued to the no-drag origin and
    // appear shifted by dragAccum at first paint after a zoom switch.
    int mapH = fullscreenMap ? 600 : MAP_H;
    MapLabels::reposition((CONT_W - SPRITE_SIZE) / 2 + dragAccumX,
                          (mapH - SPRITE_SIZE) / 2 + dragAccumY);
    // Keep overlays above the (re)created vector tile canvases; markers go on top next.
    MapTraces::moveToForeground();
    MapLabels::moveToForeground();
    MapMarkers::createMarkers();
}

void repositionAll() {
    int mapH = fullscreenMap ? 600 : MAP_H;
    for (int dy2 = 0; dy2 < GRID; dy2++)
        for (int dx2 = 0; dx2 < GRID; dx2++) {
            int tx = (CONT_W - SPRITE_SIZE) / 2 + dx2 * TILE_SIZE + dragAccumX;
            int ty = (mapH - SPRITE_SIZE) / 2 + dy2 * TILE_SIZE + dragAccumY;
            lv_obj_set_pos(tileImg[dy2][dx2], tx, ty);
            if (vecCanvas[dy2][dx2]) lv_obj_set_pos(vecCanvas[dy2][dx2], tx, ty);
        }
    MapTraces::reposition();
    MapMarkers::updateMarkerPositions();
    MapLabels::reposition((CONT_W - SPRITE_SIZE) / 2 + dragAccumX,
                          (mapH - SPRITE_SIZE) / 2 + dragAccumY);
}

void recenterForZoom(int newZoom) {
    int spriteCX = SPRITE_SIZE / 2 - dragAccumX;
    int spriteCY = SPRITE_SIZE / 2 - dragAccumY;
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
    dragAccumX = SPRITE_SIZE / 2 - spriteX;
    dragAccumY = SPRITE_SIZE / 2 - spriteY;
}

void zoomIn() {
    if (zoom < zoomMax) {
        mapFollowGps = false;
        recenterForZoom(zoom + 1);
        zoom++;
        if (mapActive) reloadTiles();
    }
}

void zoomOut() {
    if (zoom > zoomMin) {
        mapFollowGps = false;
        recenterForZoom(zoom - 1);
        zoom--;
        if (mapActive) reloadTiles();
    }
}

void timerTick() {
    if (!mapActive || !parentCont) return;

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
        if (!panActive) MapMarkers::createMarkers();
        if (infoLabel) {
            int spriteCX = SPRITE_SIZE / 2 - dragAccumX;
            int spriteCY = SPRITE_SIZE / 2 - dragAccumY;
            float lat = centerLat, lon = centerLon;
            MapMath::pixelToLatLon(spriteCX, spriteCY, zoom, true,
                                   centerTX, centerTY, 0, 0, &lat, &lon);
            char ib[128];
            snprintf(ib, sizeof(ib), "Lat:%.4f  Lon:%.4f  Stations:%d",
                     lat, lon, mapStationsCount);
            lv_label_set_text(infoLabel, ib);
        }
    }
}

} // namespace MapEngine
