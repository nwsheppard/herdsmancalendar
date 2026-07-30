#pragma once

#include <lvgl.h>

/**
 * Calendar server settings sub-screen: protocol dropdown + host/port
 * fields for the LXC/calendar_api.py container's address. Save verifies
 * the address is actually reachable (GET /health) before persisting it
 * and returning to `previous_screen`; Back discards.
 */
void settings_server_screen_open(lv_obj_t * previous_screen);
