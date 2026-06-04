#include "ui_popups.h"
#include "lvgl/lvgl.h"
#include "ui_common.h"
#include "map_state.h"

namespace UIPopups {

static lv_obj_t *makePopup(lv_obj_t *parent,
                            int w, int h,
                            uint32_t bg, uint32_t border, uint32_t fg,
                            const char *title, const char *body)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, w, h);
    lv_obj_set_style_bg_color(box, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(border), 0);
    lv_obj_set_style_border_width(box, 3, 0);
    lv_obj_set_style_border_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(box, 6, 0);
    lv_obj_set_style_pad_all(box, 12, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(box);

    lv_obj_t *t = lv_label_create(box);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_color(t, lv_color_hex(fg), 0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_22, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *b = lv_label_create(box);
    lv_label_set_text(b, body);
    lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(b, w - 24);
    lv_obj_set_style_text_color(b, lv_color_hex(fg), 0);
    lv_obj_set_style_text_font(b, &lv_font_montserrat_18, 0);
    lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 34);

    return box;
}

// forward decl
static void hide_bcn(lv_timer_t *);

// ── TX ───────────────────────────────────────────────────────────────────────
static lv_obj_t   *tx_box   = nullptr;
static lv_timer_t *tx_timer = nullptr;

static void hide_tx(lv_timer_t *) {
    if (tx_box && lv_obj_is_valid(tx_box)) { lv_obj_del(tx_box); tx_box = nullptr; }
    tx_timer = nullptr;
}

void showTxPacket(const std::string &packet) {
    // Pas de popup TX sur l'écran carte
    if (MapState::screen_map && lv_screen_active() == MapState::screen_map) return;
    hide_bcn(nullptr);
    if (tx_box && lv_obj_is_valid(tx_box)) { lv_obj_del(tx_box); tx_box = nullptr; }
    if (tx_timer) { lv_timer_del(tx_timer); tx_timer = nullptr; }
    tx_box = makePopup(lv_layer_top(), 600, 150,
                       0x004400, 0x008800, 0x00cc00,
                       "<<< TX >>>", packet.c_str());
    lv_refr_now(NULL);
    tx_timer = lv_timer_create(hide_tx, 3000, nullptr);
    lv_timer_set_repeat_count(tx_timer, 1);
}

// ── RX ───────────────────────────────────────────────────────────────────────
static lv_obj_t   *rx_box   = nullptr;
static lv_timer_t *rx_timer = nullptr;

static void hide_rx(lv_timer_t *) {
    if (rx_box && lv_obj_is_valid(rx_box)) { lv_obj_del(rx_box); rx_box = nullptr; }
    rx_timer = nullptr;
}

void showRxPacket(const std::string &packet) {
    fprintf(stderr, "[POPUP] showRxPacket: %s\n", packet.c_str());
    if (rx_box && lv_obj_is_valid(rx_box)) { lv_obj_del(rx_box); rx_box = nullptr; }
    if (rx_timer) { lv_timer_del(rx_timer); rx_timer = nullptr; }
    rx_box = makePopup(lv_layer_top(), 600, 150,
                       0x000033, 0x4488ff, 0x88bbff,
                       ">>> RX <<<", packet.c_str());
    lv_refr_now(NULL);
    rx_timer = lv_timer_create(hide_rx, 3000, nullptr);
    lv_timer_set_repeat_count(rx_timer, 1);
}

// ── Beacon pending ────────────────────────────────────────────────────────────
static lv_obj_t   *bcn_box   = nullptr;
static lv_timer_t *bcn_timer = nullptr;

static void hide_bcn(lv_timer_t *) {
    if (bcn_box && lv_obj_is_valid(bcn_box)) { lv_obj_del(bcn_box); bcn_box = nullptr; }
    bcn_timer = nullptr;
}

void showBeaconPending() {
    if (bcn_box && lv_obj_is_valid(bcn_box)) { lv_obj_del(bcn_box); bcn_box = nullptr; }
    if (bcn_timer) { lv_timer_del(bcn_timer); bcn_timer = nullptr; }
    bcn_box = makePopup(lv_layer_top(), 300, 100,
                        0x332200, 0xaa6600, 0xffaa00,
                        "BEACON", "Waiting for GPS...");
    lv_refr_now(NULL);
    bcn_timer = lv_timer_create(hide_bcn, 5000, nullptr);
    lv_timer_set_repeat_count(bcn_timer, 1);
}

void closeAll() {
    hide_tx(nullptr);
    hide_rx(nullptr);
    hide_bcn(nullptr);
}

void showMapLoading()        {}
void hideMapLoading()        {}
void showCapsLockPopup(bool) {}

} // namespace UIPopups
