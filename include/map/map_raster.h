#pragma once
#include "lvgl/lvgl.h"

namespace MapRaster {
    lv_obj_t* create(lv_obj_t *parent);
    void setPosition(double lat, double lon);
    void zoomIn();
    void zoomOut();
    void refreshStations();   // recrée les marqueurs si la map est à l'écran
}
