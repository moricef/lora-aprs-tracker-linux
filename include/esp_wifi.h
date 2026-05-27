#pragma once
#define WIFI_MODE_NULL 0
typedef int wifi_mode_t;
#define WIFI_MODE_STA 1
#define WIFI_MODE_AP 2
inline int esp_wifi_disconnect(){ return 0; }
inline int esp_wifi_stop(){ return 0; }
inline int esp_wifi_get_mode(wifi_mode_t* = nullptr){ return WIFI_MODE_NULL; }
inline int esp_wifi_set_mode(int){ return 0; }
