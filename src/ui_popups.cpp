#include "ui_popups.h"
#include "lvgl/lvgl.h"
#include "ui_common.h"

namespace UIPopups {

// TX popup (vert)
static lv_obj_t   *tx_msgbox = nullptr;
static lv_timer_t *tx_timer  = nullptr;

static void hide_tx(lv_timer_t *) {
    if (tx_msgbox && lv_obj_is_valid(tx_msgbox)) { lv_obj_del(tx_msgbox); tx_msgbox = nullptr; }
    tx_timer = nullptr;
}

void showTxPacket(const std::string &packet) {
    if (tx_msgbox && lv_obj_is_valid(tx_msgbox)) { lv_obj_del(tx_msgbox); tx_msgbox = nullptr; }
    if (tx_timer) { lv_timer_del(tx_timer); tx_timer = nullptr; }
    tx_msgbox = lv_msgbox_create(lv_layer_top());
    lv_msgbox_add_title(tx_msgbox, "<<< TX >>>");
    lv_msgbox_add_text(tx_msgbox, packet.c_str());
    lv_obj_set_size(tx_msgbox, 560, 140);
    lv_obj_set_style_bg_color(tx_msgbox, lv_color_hex(0x002200), 0);
    lv_obj_set_style_bg_opa(tx_msgbox, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(tx_msgbox, lv_color_hex(0x006600), 0);
    lv_obj_set_style_border_width(tx_msgbox, 3, 0);
    lv_obj_set_style_text_color(tx_msgbox, lv_color_hex(0x00cc00), 0);
    lv_obj_center(tx_msgbox);
    tx_timer = lv_timer_create(hide_tx, 3000, nullptr);
    lv_timer_set_repeat_count(tx_timer, 1);
}

// RX popup (bleu)
static lv_obj_t   *rx_msgbox = nullptr;
static lv_timer_t *rx_timer  = nullptr;

static void hide_rx(lv_timer_t *) {
    if (rx_msgbox && lv_obj_is_valid(rx_msgbox)) { lv_obj_del(rx_msgbox); rx_msgbox = nullptr; }
    rx_timer = nullptr;
}

void showRxPacket(const std::string &packet) {
    if (rx_msgbox && lv_obj_is_valid(rx_msgbox)) { lv_obj_del(rx_msgbox); rx_msgbox = nullptr; }
    if (rx_timer) { lv_timer_del(rx_timer); rx_timer = nullptr; }
    rx_msgbox = lv_msgbox_create(lv_layer_top());
    lv_msgbox_add_title(rx_msgbox, ">>> RX <<<");
    lv_msgbox_add_text(rx_msgbox, packet.c_str());
    lv_obj_set_size(rx_msgbox, 560, 140);
    lv_obj_set_style_bg_color(rx_msgbox, lv_color_hex(0x000033), 0);
    lv_obj_set_style_bg_opa(rx_msgbox, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(rx_msgbox, lv_color_hex(0x4488ff), 0);
    lv_obj_set_style_border_width(rx_msgbox, 3, 0);
    lv_obj_set_style_text_color(rx_msgbox, lv_color_hex(0x88bbff), 0);
    lv_obj_center(rx_msgbox);
    rx_timer = lv_timer_create(hide_rx, 3000, nullptr);
    lv_timer_set_repeat_count(rx_timer, 1);
}

void closeAll()              { hide_tx(nullptr); hide_rx(nullptr); }
void showBeaconPending()     {}
void showMapLoading()        {}
void hideMapLoading()        {}
void showCapsLockPopup(bool) {}

} // namespace UIPopups
