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

// Single composited map canvas (ARGB8888, SPRITE_SIZE²). Raster and vector
// tiles are drawn into this one buffer instead of per-tile LVGL objects, so
// every map layer shares a single coordinate space.
static lv_obj_t *mapCanvas   = nullptr;
static uint8_t  *mapBuf      = nullptr;
static uint8_t  *tileScratch = nullptr;  // reused per vector tile render

// Owner widgets passed by map_view at init/setLabels.
static lv_obj_t *parentCont  = nullptr;
static lv_obj_t *titleLabel  = nullptr;
static lv_obj_t *infoLabel   = nullptr;

// Place the canvas so its centre tracks the map-area centre, plus the
// sub-tile pan offset. Single source of truth for the vertical origin.
static void positionCanvas() {
    if (!mapCanvas) return;
    int mapH = fullscreenMap ? 600 : MAP_H;
    lv_obj_set_pos(mapCanvas,
                   (CONT_W - SPRITE_SIZE) / 2 + dragAccumX,
                   (mapH - SPRITE_SIZE) / 2 + dragAccumY);
}

// Composite the 5×5 tile grid into mapBuf. Raster (zoom<9) uses the LVGL
// image pipeline; vector (zoom≥9) blits cached ARGB renders row by row.
static void compositeTiles() {
    if (!mapCanvas) return;
    lv_draw_buf_t *db = lv_canvas_get_draw_buf(mapCanvas);
    if (!db || !db->data) return;
    uint32_t dstStride = db->header.stride;

    if (zoom >= 9 && MapVector::isOpen()) {
        for (int dy = 0; dy < GRID; dy++)
            for (int dx = 0; dx < GRID; dx++) {
                int tx = centerTX + dx - GRID / 2, ty = centerTY + dy - GRID / 2;
                if (!MapVector::renderTileCached(tileScratch, TILE_SIZE, zoom, tx, ty))
                    continue;
                int ox = dx * TILE_SIZE, oy = dy * TILE_SIZE;
                for (int row = 0; row < TILE_SIZE; row++)
                    memcpy(db->data + (size_t)(oy + row) * dstStride + (size_t)ox * 4,
                           tileScratch + (size_t)row * TILE_SIZE * 4,
                           TILE_SIZE * 4);
            }
    } else {
        lv_canvas_fill_bg(mapCanvas, lv_color_hex(0x2F4F4F), LV_OPA_COVER);
        for (int dy = 0; dy < GRID; dy++)
            for (int dx = 0; dx < GRID; dx++) {
                int tx = centerTX + dx - GRID / 2, ty = centerTY + dy - GRID / 2;
                if (!MapIO::tileExists(tx, ty, zoom)) continue;
                char p[512];
                snprintf(p, sizeof(p), "A:%s/%s/%d/%d/%d.jpg", MapIO::mapsRoot(),
                         mapRegion, zoom, tx, ty);
                // Draw each tile in its own layer pass: lv_draw_image reads the
                // dsc (incl. src) at flush time, so the path must stay valid
                // until finish_layer commits the draw.
                lv_layer_t layer;
                lv_canvas_init_layer(mapCanvas, &layer);
                lv_draw_image_dsc_t idsc;
                lv_draw_image_dsc_init(&idsc);
                idsc.src = p;
                lv_area_t coords = { dx * TILE_SIZE, dy * TILE_SIZE,
                                     dx * TILE_SIZE + TILE_SIZE - 1,
                                     dy * TILE_SIZE + TILE_SIZE - 1 };
                lv_draw_image(&layer, &idsc, &coords);
                lv_canvas_finish_layer(mapCanvas, &layer);
            }
    }
    lv_obj_invalidate(mapCanvas);
}

void init(lv_obj_t *parent) {
    parentCont = parent;
    mapBuf = (uint8_t *)lv_malloc(
        LV_CANVAS_BUF_SIZE(SPRITE_SIZE, SPRITE_SIZE, 32, LV_DRAW_BUF_STRIDE_ALIGN));
    mapCanvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(mapCanvas, mapBuf, SPRITE_SIZE, SPRITE_SIZE,
                         LV_COLOR_FORMAT_ARGB8888);
    lv_obj_clear_flag(mapCanvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(mapCanvas, (CONT_W - SPRITE_SIZE) / 2, (MAP_H - SPRITE_SIZE) / 2);
    tileScratch = (uint8_t *)lv_malloc(TILE_SIZE * TILE_SIZE * 4);
}

void destroy() {
    if (mapCanvas && lv_obj_is_valid(mapCanvas)) lv_obj_del(mapCanvas);
    mapCanvas = nullptr;
    if (mapBuf)      { lv_free(mapBuf);      mapBuf = nullptr; }
    if (tileScratch) { lv_free(tileScratch); tileScratch = nullptr; }
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
    compositeTiles();
    positionCanvas();
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
    positionCanvas();
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
