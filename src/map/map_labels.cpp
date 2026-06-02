#include "map/map_labels.h"
#include "map/map_state.h"
#include "map_vector.h"

#include <algorithm>
#include <cmath>
#include <cstring>
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

    struct SL { int x, y, prio; std::string text; uint8_t r, g, b; const lv_font_t *font;
                int angle; bool followLine; bool shield; };
    std::vector<SL> all;
    for (int dy = 0; dy < GRID; dy++)
        for (int dx = 0; dx < GRID; dx++) {
            int tx = centerTX + dx - GRID / 2, ty = centerTY + dy - GRID / 2;
            std::vector<MapVector::Label> tl;
            MapVector::getTileLabels(zoom, tx, ty, tl);
            int sx = dx * TILE_SIZE, sy = dy * TILE_SIZE;   // sprite coords, aligned with the tiles
            for (auto &l : tl) all.push_back({sx + l.px, sy + l.py, l.priority, l.text, l.r, l.g, l.b, l.font, l.angle, l.followLine, l.shield});
        }
    // A river/road spans many tiles, producing one label per tile. Merge labels with the
    // same name into a single stable one at the centroid of all its points; for waterways
    // the orientation is the points' principal direction (PCA) so it doesn't jump or swap
    // with a nearby river while panning.
    {
        std::vector<std::string> names;
        std::vector<std::vector<lv_point_t>> groups;
        std::vector<SL> metas;
        for (auto &l : all) {
            int idx = -1;
            for (size_t i = 0; i < names.size(); i++) if (names[i] == l.text) { idx = (int)i; break; }
            if (idx < 0) { names.push_back(l.text); groups.push_back({}); metas.push_back(l); idx = (int)names.size() - 1; }
            lv_point_t p; p.x = l.x; p.y = l.y;
            groups[idx].push_back(p);
            if (l.prio < metas[idx].prio) metas[idx] = l;
        }
        std::vector<SL> mg;
        for (size_t i = 0; i < names.size(); i++) {
            auto &pts = groups[i];
            double cx = 0, cy = 0;
            for (auto &p : pts) { cx += p.x; cy += p.y; }
            cx /= pts.size(); cy /= pts.size();
            SL s = metas[i];
            s.x = (int)cx; s.y = (int)cy;
            if (s.followLine && pts.size() >= 2) {
                double sxx = 0, syy = 0, sxy = 0;
                for (auto &p : pts) { double dx = p.x - cx, dy = p.y - cy; sxx += dx*dx; syy += dy*dy; sxy += dx*dy; }
                double ang = 0.5 * atan2(2.0 * sxy, sxx - syy) * 57.2957795;  // rad -> deg
                if (ang > 90) ang -= 180; else if (ang < -90) ang += 180;
                s.angle = (int)ang;
            }
            mg.push_back(s);
        }
        all.swap(mg);
    }
    std::sort(all.begin(), all.end(), [](const SL &a, const SL &b) { return a.prio < b.prio; });

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
        if (lx < 0 || lx + w > LABEL_CANVAS_W || ly < 0 || ly + h > LABEL_CANVAS_H) continue;
        bool ov = false;
        for (auto &p : placed)
            if (!(lx + w + 3 <= p.x || p.x + p.w + 3 <= lx || ly + h + 2 <= p.y || p.y + p.h + 2 <= ly)) { ov = true; break; }
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
        } else if (l.followLine && tmpLabelBuf) {
            // Waterway: render the word horizontally to the scratch canvas, then blit it
            // rotated so the whole word follows the river.
            int ww = (w < TMP_LABEL_W) ? w : TMP_LABEL_W;
            int hh = (h < TMP_LABEL_H) ? h : TMP_LABEL_H;
            memset(tmpLabelBuf, 0, TMP_LABEL_W * TMP_LABEL_H * 4);
            lv_layer_t tl; lv_canvas_init_layer(tmpLabelCanvas, &tl);
            drawHalo(&tl, l.text.c_str(), l.font, 0, 0, ww, hh, l.r, l.g, l.b);
            lv_canvas_finish_layer(tmpLabelCanvas, &tl);
            lv_draw_image_dsc_t id; lv_draw_image_dsc_init(&id);
            id.src = lv_canvas_get_image(tmpLabelCanvas);
            id.rotation = l.angle * 10;       // rotate the whole word (0.1° units)
            id.pivot.x = ww / 2; id.pivot.y = hh / 2;
            lv_area_t ia = { l.x - ww / 2, l.y - hh / 2,
                             l.x - ww / 2 + TMP_LABEL_W - 1, l.y - hh / 2 + TMP_LABEL_H - 1 };
            lv_draw_image(&layer, &id, &ia);
        } else {
            drawHalo(&layer, l.text.c_str(), l.font, lx, ly, w, h, l.r, l.g, l.b);
        }
        placed.push_back({lx, ly, w, h});
    }

    lv_canvas_finish_layer(labelCanvas, &layer);
    lv_obj_invalidate(labelCanvas);
}

} // namespace MapLabels
