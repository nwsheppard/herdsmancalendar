#pragma once

#include <lvgl.h>

/**
 * Settings menu: lists WiFi / Calendar Server / Calendar Filters, each
 * opening its own dedicated screen (settings_wifi_screen.h,
 * settings_server_screen.h, settings_filters_screen.h) rather than one
 * cluttered page. None of these block -- loop() keeps servicing normally
 * the whole time, unlike wifi_setup_screen_run(). "Back to Calendar"
 * returns to `previous_screen`.
 */
void settings_screen_open(lv_obj_t * previous_screen);
