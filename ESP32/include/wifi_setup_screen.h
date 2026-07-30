#pragma once

#include <Arduino.h>
#include <lvgl.h>

/**
 * Show a full-screen WiFi setup UI: scan for networks, let the user tap one
 * and enter a password via on-screen keyboard, then connect. Blocks (running
 * its own lv_timer_handler() loop) until a connection succeeds -- there's no
 * cancel, since the device has nothing useful to do without WiFi anyway.
 *
 * On return, `previous_screen` is reloaded and the connection is already
 * live; the caller still owns persisting the credentials (see
 * wifi_credentials_store.h) since this module only handles the UI/connect.
 *
 * @param previous_screen Screen to switch back to once connected.
 * @param out_ssid Set to the SSID that connected successfully.
 * @param out_password Set to the password that connected successfully.
 */
void wifi_setup_screen_run(lv_obj_t * previous_screen, String & out_ssid, String & out_password);
