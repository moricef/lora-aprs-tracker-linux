#pragma once
#include "lvgl/lvgl.h"
#include "ui_messaging.h"
#include "ui_settings.h"
#include "ui_dashboard.h"

static void _settings_gesture_cb(lv_event_t *e) {
    if (lv_indev_get_gesture_dir(lv_indev_get_act()) == LV_DIR_RIGHT)
        UIDashboard::returnToDashboard();
}
static void _settings_back_clicked(lv_event_t *e) {
    UIDashboard::returnToDashboard();
}

namespace LVGL_UI {
    inline void openMessagesScreen() { UIMessaging::openMessagesScreen(); }
    inline void openSetupScreen() {
        lv_obj_t *scr = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(scr, lv_color_hex(0x1a1a2e), 0);
        lv_obj_t *lbl = lv_label_create(scr);
        lv_label_set_text(lbl, "Settings - Not implemented yet");
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xffffff), 0);
        lv_obj_center(lbl);
        // Back button
        lv_obj_t *btn = lv_btn_create(scr);
        lv_obj_set_pos(btn, 10, 10);
        lv_obj_set_size(btn, 80, 40);
        lv_obj_add_event_cb(btn, _settings_back_clicked, LV_EVENT_CLICKED, NULL);
        lv_obj_t *bl = lv_label_create(btn);
        lv_label_set_text(bl, LV_SYMBOL_LEFT " Back");
        lv_obj_center(bl);
        // Swipe back
        lv_obj_add_event_cb(scr, _settings_gesture_cb, LV_EVENT_GESTURE, NULL);
        lv_screen_load(scr);
    }
}
