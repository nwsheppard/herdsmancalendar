#pragma once

#include <lvgl.h>

/** WiFi settings sub-screen: status + "Reconfigure WiFi". Back returns to `previous_screen`. */
void settings_wifi_screen_open(lv_obj_t * previous_screen);
