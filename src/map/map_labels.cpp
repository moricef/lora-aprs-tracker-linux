#include "map/map_labels.h"
#include "map/map_state.h"
#include "map_vector.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace MapLabels {

using namespace MapState;

// Main overlay : same size as the tile sprite, pans with it.
static lv_obj_t *labelCanvas = nullptr;
static uint8_t  *labelBuf    = nullptr;
constexpr int LABEL_CANVAS_W = SPRITE_SIZE;
constexpr int LABEL_CANVAS_H = SPRITE_SIZE;

// Scratch canvas : each waterway word is rendered here horizontally, then
// blitted rotated onto the overlay (label rotation only slants glyphs).
static lv_obj_t *tmpLabelCanvas = nullptr;
static uint8_t  *tmpLabelBuf    = nullptr;
constexpr int TMP_LABEL_W = 320;
constexpr int TMP_LABEL_H = 40;

void create(lv_obj_t *parent) {
    if (labelCanvas || !parent) return;

    labelBuf = (uint8_t *)lv_malloc(LV_CANVAS_BUF_SIZE(LABEL_CANVAS_W, LABEL_CANVAS_H, 32, LV_DRAW_BUF_STRIDE_ALIGN));
    labelCanvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(labelCanvas, labelBuf, LABEL_CANVAS_W, LABEL_CANVAS_H, LV_COLOR_FORMAT_ARGB8888);
    lv_obj_set_pos(labelCanvas, (CONT_W - SPRITE_SIZE) / 2, (MAP_H - SPRITE_SIZE) / 2);
    lv_obj_clear_flag(labelCanvas, LV_OBJ_FLAG_CLICKABLE);
    memset(labelBuf, 0, LABEL_CANVAS_W * LABEL_CANVAS_H * 4);

    // Scratch canvas (hidden, off-screen buffer only)
    tmpLabelBuf = (uint8_t *)lv_malloc(LV_CANVAS_BUF_SIZE(TMP_LABEL_W, TMP_LABEL_H, 32, LV_DRAW_BUF_STRIDE_ALIGN));
    tmpLabelCanvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(tmpLabelCanvas, tmpLabelBuf, TMP_LABEL_W, TMP_LABEL_H, LV_COLOR_FORMAT_ARGB8888);
    lv_obj_add_flag(tmpLabelCanvas, LV_OBJ_FLAG_HIDDEN);
}

void destroy() {
    if (labelCanvas && lv_obj_is_valid(labelCanvas)) lv_obj_del(labelCanvas);
    labelCanvas = nullptr;
    if (labelBuf) { lv_free(labelBuf); labelBuf = nullptr; }
    if (tmpLabelCanvas && lv_obj_is_valid(tmpLabelCanvas)) lv_obj_del(tmpLabelCanvas);
    tmpLabelCanvas = nullptr;
    if (tmpLabelBuf) { lv_free(tmpLabelBuf); tmpLabelBuf = nullptr; }
}

void clear() {
    if (!labelBuf) return;
    memset(labelBuf, 0, LABEL_CANVAS_W * LABEL_CANVAS_H * 4);
    if (labelCanvas && lv_obj_is_valid(labelCanvas)) lv_obj_invalidate(labelCanvas);
}

void reposition(int originX, int originY) {
    if (labelCanvas && lv_obj_is_valid(labelCanvas))
        lv_obj_set_pos(labelCanvas, originX, originY);
}

void moveToForeground() {
    if (labelCanvas && lv_obj_is_valid(labelCanvas))
        lv_obj_move_foreground(labelCanvas);
}

// Collect labels from the visible tiles, run global collision, draw them
// into the overlay canvas. Positions are in sprite coords (tile-aligned),
// like the tile grid.
void refresh() {
    if (!labelBuf || !labelCanvas) return;
    memset(labelBuf, 0, LABEL_CANVAS_W * LABEL_CANVAS_H * 4);
    if (!(zoom >= 9 && MapVector::isOpen())) { lv_obj_invalidate(labelCanvas); return; }

    // Each label keeps its tile-local position (centroid or polyline mid)
    // produced by MapVector::getTileLabels — that's already on the actual
    // geometry (the city node, the river midpoint, the road shield). We do
    // NOT recompute a cross-tile centroid : averaging positions across the
    // 25-tile window pulls river labels into the middle of the curve and
    // shifts city labels each time a tile enters or leaves the view.
    //
    // worldX/worldY = absolute world pixel for the anchor, independent of
    // the sprite origin. Used to key the hysteresis cache so labels don't
    // swap winners on every pan step.
    struct SL { int x, y, prio; std::string text; uint8_t r, g, b; const lv_font_t *font;
                int angle; bool followLine; bool shield;
                bool isPeak; int elevation;
                int worldX, worldY;
                int population;
                std::vector<lv_point_t> path;      // sprite-local polyline for waterway
                std::string displayText; };        // pre-formatted peak label (name + elevation), lives until finish_layer
    const int worldOriginX = (centerTX - GRID / 2) * TILE_SIZE;
    const int worldOriginY = (centerTY - GRID / 2) * TILE_SIZE;
    std::vector<SL> all;
    for (int dy = 0; dy < GRID; dy++)
        for (int dx = 0; dx < GRID; dx++) {
            int tx = centerTX + dx - GRID / 2, ty = centerTY + dy - GRID / 2;
            std::vector<MapVector::Label> tl;
            MapVector::getTileLabels(zoom, tx, ty, tl);
            int sx = dx * TILE_SIZE, sy = dy * TILE_SIZE;   // sprite coords
            for (auto &l : tl) {
                SL s;
                s.x = sx + l.px; s.y = sy + l.py;
                s.prio = l.priority; s.text = l.text;
                s.r = l.r; s.g = l.g; s.b = l.b;
                s.font = l.font; s.angle = l.angle;
                s.followLine = l.followLine; s.shield = l.shield;
                s.isPeak = l.isPeak; s.elevation = l.elevation;
                if (l.isPeak) {
                    if (l.elevation > 0)
                        s.displayText = l.text + " " + std::to_string(l.elevation) + "m";
                    else
                        s.displayText = l.text;
                }
                s.worldX = worldOriginX + sx + l.px;
                s.worldY = worldOriginY + sy + l.py;
                s.population = l.population;
                if (l.followLine && !l.path.empty()) {
                    s.path.reserve(l.path.size());
                    for (auto &p : l.path) {
                        lv_point_t q; q.x = p.x + sx; q.y = p.y + sy;
                        s.path.push_back(q);
                    }
                }
                all.push_back(std::move(s));
            }
        }

    // Hysteresis cache : world-pixel anchor of the last frame's winner per
    // name. Cleared on zoom change. Each candidate gets a gradient bonus
    // that scales from kHystMaxBonus at distance 0 down to 0 at the
    // radius — so the candidate closest to last frame's winner reliably
    // wins, even when waterway labels all share priority=60.
    static std::map<std::string, lv_point_t> s_anchorCache;
    static int s_anchorCacheZoom = -1;
    if (s_anchorCacheZoom != zoom) { s_anchorCache.clear(); s_anchorCacheZoom = zoom; }
    const int kHystRadiusPx = 400;
    const int kHystMaxBonus = 100;          // dominates raw priority spread
    auto scoreOf = [&](const SL& l) -> int {
        int s = l.prio * 100;               // scale priority so bonus is meaningful
        auto it = s_anchorCache.find(l.text);
        if (it != s_anchorCache.end()) {
            int dx = it->second.x - l.worldX, dy = it->second.y - l.worldY;
            int d2 = dx*dx + dy*dy;
            int r2 = kHystRadiusPx * kHystRadiusPx;
            if (d2 < r2) {
                // Linear gradient: closest match gets the full bonus.
                int d = (int)sqrt((double)d2);
                s -= kHystMaxBonus * 100 * (kHystRadiusPx - d) / kHystRadiusPx;
            }
        }
        return s;
    };
    // Stable sort with a deterministic tiebreaker (world anchor) so two
    // candidates with identical scores keep the same order frame to frame.
    std::stable_sort(all.begin(), all.end(),
              [&](const SL &a, const SL &b) {
                  int sa = scoreOf(a), sb = scoreOf(b);
                  if (sa != sb) return sa < sb;
                  // Same score: larger population wins (Cugnaux 17.5k beats
                  // Fonsorbes 12.8k, both town/prio=20/rank=9).
                  if (a.population != b.population) return a.population > b.population;
                  if (a.worldX != b.worldX) return a.worldX < b.worldX;
                  return a.worldY < b.worldY;
              });

    // Per-name dedup radius : drop a second "Garonne" label that lands too
    // close to one we already kept. Larger radius for followLine/shield
    // (rivers / road refs span many tiles) than for places (one node only).
    auto dedupRadiusPx = [](const SL& l) -> int {
        return (l.followLine || l.shield) ? 350 : 100;
    };
    std::vector<SL> kept;
    std::map<std::string, lv_point_t> newAnchorCache;

    struct Box { int x, y, w, h; };
    std::vector<Box> placed;

    lv_layer_t layer;
    lv_canvas_init_layer(labelCanvas, &layer);

    // Draw a word with a white halo (4 diagonal offsets) into the given layer/area.
    auto drawHalo = [](lv_layer_t *L, const char *txt, const lv_font_t *font,
                       int x0, int y0, int ww, int hh, uint8_t r, uint8_t g, uint8_t b) {
        lv_draw_label_dsc_t d; lv_draw_label_dsc_init(&d);
        d.text = txt; d.font = font; d.align = LV_TEXT_ALIGN_CENTER;
        d.color = lv_color_white();
        const int off[4][2] = {{-1,-1},{1,-1},{-1,1},{1,1}};
        for (auto &o : off) { lv_area_t ha = { x0 + o[0], y0 + o[1], x0 + o[0] + ww - 1, y0 + o[1] + hh - 1 }; lv_draw_label(L, &d, &ha); }
        d.color = lv_color_make(r, g, b);
        lv_area_t a = { x0, y0, x0 + ww - 1, y0 + hh - 1 };
        lv_draw_label(L, &d, &a);
    };

    for (auto &l : all) {
        int h = l.font->line_height;
        int w = (int)(l.text.size() * h * 0.55f) + 6;
        int lx = l.x - w / 2, ly = l.y - h / 2;
        if (lx < 0 || lx + w > LABEL_CANVAS_W || ly < 0 || ly + h > LABEL_CANVAS_H) {
            continue;
        }

        // Per-name dedup : if we already kept a label with the same text
        // within the dedup radius, this is a duplicate from another tile.
        bool sameNameNear = false;
        int dr = dedupRadiusPx(l);
        int dr2 = dr * dr;
        for (auto &k : kept) {
            if (k.text != l.text) continue;
            int ddx = k.x - l.x, ddy = k.y - l.y;
            if (ddx*ddx + ddy*ddy < dr2) { sameNameNear = true; break; }
        }
        if (sameNameNear) continue;

        // UTF-8 codepoint walker (used by waterway pre-pass and render).
        auto utf8Next = [](const char *s, int &cp) -> int {
            uint8_t c0 = (uint8_t)s[0];
            if      (c0 < 0x80) { cp = c0; return 1; }
            else if (c0 < 0xE0) { cp = ((c0 & 0x1F) << 6) | ((uint8_t)s[1] & 0x3F); return 2; }
            else if (c0 < 0xF0) { cp = ((c0 & 0x0F) << 12) | (((uint8_t)s[1] & 0x3F) << 6) | ((uint8_t)s[2] & 0x3F); return 3; }
            else                { cp = ((c0 & 0x07) << 18) | (((uint8_t)s[1] & 0x3F) << 12) | (((uint8_t)s[2] & 0x3F) << 6) | ((uint8_t)s[3] & 0x3F); return 4; }
        };

        // For followLine labels (rivers/canals), replace the upright
        // midpoint bbox with the swept bbox of the chosen straight
        // section. Otherwise the collision test sees a tiny box at the
        // polyline midpoint and lets the curved text sweep through
        // adjacent place labels.
        std::vector<float> wwArc;
        float wwStartDist = 0.0f;
        bool wwReady = false;
        int swX = lx, swY = ly, swW = w, swH = h;
        if (l.followLine && tmpLabelBuf && l.path.size() >= 2) {
            wwArc.assign(l.path.size(), 0.0f);
            for (size_t j = 1; j < l.path.size(); j++) {
                float ddx = (float)(l.path[j].x - l.path[j-1].x);
                float ddy = (float)(l.path[j].y - l.path[j-1].y);
                wwArc[j] = wwArc[j-1] + sqrtf(ddx*ddx + ddy*ddy);
            }
            float totalLen = wwArc.back();
            float textW = 0.0f;
            for (size_t i = 0; i < l.text.size(); ) {
                int cp; int n = utf8Next(l.text.c_str() + i, cp);
                textW += (float)lv_font_get_glyph_width(l.font, cp, 0);
                i += n;
            }
            if (textW <= 0.0f || textW > totalLen) continue;

            int nseg = (int)l.path.size() - 1;
            float bestSpread = 1e9f;
            float step = textW / 8.0f;
            if (step < 4.0f) step = 4.0f;
            for (float s = 0.0f; s + textW <= totalLen + 0.5f; s += step) {
                float e = s + textW;
                float aMin = 1e9f, aMax = -1e9f;
                for (int j = 0; j < nseg; j++) {
                    if (wwArc[j+1] < s) continue;
                    if (wwArc[j]   > e) break;
                    float ddx = (float)(l.path[j+1].x - l.path[j].x);
                    float ddy = (float)(l.path[j+1].y - l.path[j].y);
                    float a = atan2f(ddy, ddx);
                    if (a < aMin) aMin = a;
                    if (a > aMax) aMax = a;
                }
                float spread = aMax - aMin;
                if (spread > (float)M_PI) spread = 2.0f * (float)M_PI - spread;
                if (spread < bestSpread) { bestSpread = spread; wwStartDist = s; }
            }
            const float kMaxAngleSpread = 0.55f;
            if (bestSpread > kMaxAngleSpread) continue;

            // RTL decision (sample start/end of chosen window).
            auto pointAt = [&](float d, float &px, float &py) {
                int s = 0;
                while (s < (int)l.path.size() - 2 && wwArc[s + 1] < d) s++;
                float segLen = wwArc[s + 1] - wwArc[s];
                float t = segLen > 0.001f ? (d - wwArc[s]) / segLen : 0.0f;
                px = l.path[s].x + t * (l.path[s+1].x - l.path[s].x);
                py = l.path[s].y + t * (l.path[s+1].y - l.path[s].y);
            };
            float sx_, sy_, ex_, ey_;
            pointAt(wwStartDist,       sx_, sy_);
            pointAt(wwStartDist+textW, ex_, ey_);
            if (ex_ < sx_) {
                std::reverse(l.path.begin(), l.path.end());
                for (size_t j = 0; j < wwArc.size(); j++) wwArc[j] = 0.0f;
                for (size_t j = 1; j < l.path.size(); j++) {
                    float ddx = (float)(l.path[j].x - l.path[j-1].x);
                    float ddy = (float)(l.path[j].y - l.path[j-1].y);
                    wwArc[j] = wwArc[j-1] + sqrtf(ddx*ddx + ddy*ddy);
                }
                wwStartDist = totalLen - (wwStartDist + textW);
            }

            // Walk glyph centers once to compute the actual swept bbox.
            int seg = 0;
            float curDist = wwStartDist;
            int wMinX = INT_MAX, wMinY = INT_MAX, wMaxX = INT_MIN, wMaxY = INT_MIN;
            for (size_t i = 0; i < l.text.size(); ) {
                int cp; int n = utf8Next(l.text.c_str() + i, cp);
                int gW = lv_font_get_glyph_width(l.font, cp, 0);
                float mid = curDist + gW * 0.5f;
                while (seg < (int)l.path.size() - 2 && wwArc[seg + 1] < mid) seg++;
                float segLen = wwArc[seg + 1] - wwArc[seg];
                float t = segLen > 0.001f ? (mid - wwArc[seg]) / segLen : 0.0f;
                float gx = l.path[seg].x + t * (l.path[seg+1].x - l.path[seg].x);
                float gy = l.path[seg].y + t * (l.path[seg+1].y - l.path[seg].y);
                if ((int)gx - h/2 < wMinX) wMinX = (int)gx - h/2;
                if ((int)gy - h/2 < wMinY) wMinY = (int)gy - h/2;
                if ((int)gx + h/2 > wMaxX) wMaxX = (int)gx + h/2;
                if ((int)gy + h/2 > wMaxY) wMaxY = (int)gy + h/2;
                curDist += gW;
                i += n;
            }
            if (wMinX <= wMaxX) {
                swX = wMinX; swY = wMinY;
                swW = wMaxX - wMinX; swH = wMaxY - wMinY;
                wwReady = true;
            }
        }

        bool ov = false;
        for (auto &p : placed)
            if (!(swX + swW + 3 <= p.x || p.x + p.w + 3 <= swX || swY + swH + 2 <= p.y || p.y + p.h + 2 <= swY)) { ov = true; break; }
        if (ov) continue;

        lv_area_t a = { lx, ly, lx + w - 1, ly + h - 1 };
        if (l.shield) {
            // Road ref shield: lightened color as background, base color as border,
            // darkened color as text (r,g,b carry the road base color).
            lv_draw_rect_dsc_t rd; lv_draw_rect_dsc_init(&rd);
            rd.bg_color = lv_color_make((uint8_t)(l.r+(255-l.r)*0.78f), (uint8_t)(l.g+(255-l.g)*0.78f), (uint8_t)(l.b+(255-l.b)*0.78f));
            rd.bg_opa = LV_OPA_COVER;
            rd.border_color = lv_color_make(l.r, l.g, l.b);
            rd.border_width = 1; rd.border_opa = LV_OPA_COVER; rd.radius = 3;
            lv_draw_rect(&layer, &rd, &a);
            lv_draw_label_dsc_t ld; lv_draw_label_dsc_init(&ld);
            ld.text = l.text.c_str(); ld.font = l.font; ld.align = LV_TEXT_ALIGN_CENTER;
            ld.color = lv_color_make((uint8_t)(l.r*0.55f), (uint8_t)(l.g*0.55f), (uint8_t)(l.b*0.55f));
            lv_draw_label(&layer, &ld, &a);
            placed.push_back({lx, ly, w, h});
        } else if (wwReady) {
            // Waterway: walk the polyline glyph by glyph (firmware model :
            // each glyph is rendered upright on the scratch canvas then
            // blitted rotated by the local segment angle, so the text
            // follows the curve instead of being one rigid rotated word).
            // Sliding window + RTL decision were done in the pre-pass
            // above; we reuse wwArc and wwStartDist here.
            int seg = 0;
            float curDist = wwStartDist;
            int wMinX = INT_MAX, wMinY = INT_MAX, wMaxX = INT_MIN, wMaxY = INT_MIN;
            for (size_t i = 0; i < l.text.size(); ) {
                int cp; int n = utf8Next(l.text.c_str() + i, cp);
                int gW = lv_font_get_glyph_width(l.font, cp, 0);
                float mid = curDist + gW * 0.5f;
                while (seg < (int)l.path.size() - 2 && wwArc[seg + 1] < mid) seg++;
                float segLen = wwArc[seg + 1] - wwArc[seg];
                float t = segLen > 0.001f ? (mid - wwArc[seg]) / segLen : 0.0f;
                float gx = l.path[seg].x + t * (l.path[seg+1].x - l.path[seg].x);
                float gy = l.path[seg].y + t * (l.path[seg+1].y - l.path[seg].y);
                if ((int)gx - h/2 < wMinX) wMinX = (int)gx - h/2;
                if ((int)gy - h/2 < wMinY) wMinY = (int)gy - h/2;
                if ((int)gx + h/2 > wMaxX) wMaxX = (int)gx + h/2;
                if ((int)gy + h/2 > wMaxY) wMaxY = (int)gy + h/2;
                float dxs = l.path[seg+1].x - l.path[seg].x;
                float dys = l.path[seg+1].y - l.path[seg].y;
                float angRad = atan2f(dys, dxs);
                // No per-glyph flip : direction was decided once via the
                // RTL reversal above. Using the raw segment angle keeps
                // adjacent glyphs consistent across local angle wobble.
                int angDeg = (int)(angRad * 180.0f / M_PI);
                // Render the single glyph centered on tmpLabelCanvas, then
                // blit it rotated to the main label canvas at (gx, gy).
                int gH = l.font->line_height;
                memset(tmpLabelBuf, 0, TMP_LABEL_W * TMP_LABEL_H * 4);
                lv_layer_t tl; lv_canvas_init_layer(tmpLabelCanvas, &tl);
                char chBuf[8] = {0};
                int nb = 0;
                if      (cp < 0x80)   { chBuf[nb++] = (char)cp; }
                else if (cp < 0x800)  { chBuf[nb++] = 0xC0 | (cp >> 6); chBuf[nb++] = 0x80 | (cp & 0x3F); }
                else if (cp < 0x10000){ chBuf[nb++] = 0xE0 | (cp >> 12); chBuf[nb++] = 0x80 | ((cp >> 6) & 0x3F); chBuf[nb++] = 0x80 | (cp & 0x3F); }
                else                  { chBuf[nb++] = 0xF0 | (cp >> 18); chBuf[nb++] = 0x80 | ((cp >> 12) & 0x3F); chBuf[nb++] = 0x80 | ((cp >> 6) & 0x3F); chBuf[nb++] = 0x80 | (cp & 0x3F); }
                int gx0 = (TMP_LABEL_W - gW) / 2;
                int gy0 = (TMP_LABEL_H - gH) / 2;
                drawHalo(&tl, chBuf, l.font, gx0, gy0, gW, gH, l.r, l.g, l.b);
                lv_canvas_finish_layer(tmpLabelCanvas, &tl);
                lv_draw_image_dsc_t id; lv_draw_image_dsc_init(&id);
                id.src = lv_canvas_get_image(tmpLabelCanvas);
                id.rotation = angDeg * 10;             // 0.1° units
                id.pivot.x = TMP_LABEL_W / 2; id.pivot.y = TMP_LABEL_H / 2;
                lv_area_t ia = { (int)gx - TMP_LABEL_W / 2, (int)gy - TMP_LABEL_H / 2,
                                 (int)gx + TMP_LABEL_W / 2 - 1, (int)gy + TMP_LABEL_H / 2 - 1 };
                // lv_draw_image is deferred and reads its source buffer at
                // flush time. If multiple glyphs queue up they all sample
                // the same scratch buffer (last glyph drawn), so every
                // position renders the final letter. Close + reopen the
                // main layer to commit each blit with its own scratch.
                lv_draw_image(&layer, &id, &ia);
                lv_canvas_finish_layer(labelCanvas, &layer);
                lv_canvas_init_layer(labelCanvas, &layer);
                curDist += gW;
                i += n;
            }
            // Use the actual on-curve bbox so future labels (place, shield)
            // see the real footprint of the waterway text.
            if (wMinX <= wMaxX) {
                placed.push_back({wMinX, wMinY, wMaxX - wMinX, wMaxY - wMinY});
            } else {
                placed.push_back({lx, ly, w, h});
            }
        } else if (l.isPeak) {
            // Firmware spec (Tile-Generator-Pack/features.json): triangle
            // marker color #d08050, name + elevation under it.
            // Triangle apex at (l.x, l.y - triH/2), base width = triW.
            const int triH = 10, triW = 12;
            int tApexX = l.x,           tApexY = l.y - triH / 2;
            int tBLX   = l.x - triW/2,  tBLY   = l.y + triH / 2;
            int tBRX   = l.x + triW/2,  tBRY   = l.y + triH / 2;
            lv_draw_triangle_dsc_t td; lv_draw_triangle_dsc_init(&td);
            td.p[0].x = tApexX; td.p[0].y = tApexY;
            td.p[1].x = tBLX;   td.p[1].y = tBLY;
            td.p[2].x = tBRX;   td.p[2].y = tBRY;
            td.color = lv_color_make(l.r, l.g, l.b);
            td.opa = LV_OPA_COVER;
            lv_draw_triangle(&layer, &td);
            // Name (+ elevation if known) below the triangle.
            // displayText is pre-formatted during collection (owned string,
            // lives until finish_layer — LVGL v9 defers label drawing).
            int textW = (int)(l.displayText.size() * l.font->line_height * 0.55f) + 6;
            int textH = l.font->line_height;
            int tx = l.x - textW / 2;
            int ty = l.y + triH / 2 + 2;
            if (tx >= 0 && tx + textW <= LABEL_CANVAS_W &&
                ty >= 0 && ty + textH <= LABEL_CANVAS_H)
                drawHalo(&layer, l.displayText.c_str(), l.font, tx, ty, textW, textH, l.r, l.g, l.b);
            // Reserve both the triangle and the label box so future labels
            // don't overlap.
            int boxX = tx, boxY = tApexY;
            int boxW = textW, boxH = (ty + textH) - tApexY;
            placed.push_back({boxX, boxY, boxW, boxH});
        } else {
            drawHalo(&layer, l.text.c_str(), l.font, lx, ly, w, h, l.r, l.g, l.b);
            placed.push_back({lx, ly, w, h});
        }
        kept.push_back(l);
        // First kept label for a name wins the anchor slot ; later same-name
        // candidates have already been filtered by the dedup check above, so
        // this only ever fires once per name per frame.
        if (!newAnchorCache.count(l.text)) {
            lv_point_t p; p.x = l.worldX; p.y = l.worldY;
            newAnchorCache[l.text] = p;
        }
    }
    // Replace the previous cache with this frame's winners. Names that left
    // the viewport are evicted automatically — when they come back they'll
    // bootstrap from priority alone, which is fine.
    s_anchorCache.swap(newAnchorCache);

    lv_canvas_finish_layer(labelCanvas, &layer);
    lv_obj_invalidate(labelCanvas);
}

} // namespace MapLabels
