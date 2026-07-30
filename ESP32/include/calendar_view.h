#pragma once

#include <lvgl.h>

/**
 * Build the main calendar screen: dark background, amber text, a scrollable
 * list of events color-coded by impact level, day-separated, fetched live
 * from calendar_api.py's /calendar endpoint. If no calendar server has been
 * configured yet, shows a message pointing at Settings instead (see
 * make_no_server_message() in calendar_view.cpp) rather than attempting a
 * request with nowhere to send it.
 *
 * Does not load the screen; caller decides when (lv_screen_load()).
 */
lv_obj_t * calendar_view_create();

/**
 * Update this screen's own WiFi status icon (top right, left of the
 * settings gear) to reflect a connection state change -- same
 * color_hex/show_strike shape as main.cpp's boot-splash icon, since both
 * are driven by the same WiFi.onEvent() callback and should always show
 * the same state. Safe to call before calendar_view_create() has run yet
 * (a no-op until the icon exists).
 */
void calendar_view_set_wifi_icon(uint32_t color_hex, bool show_strike);

/**
 * Re-fetch and redisplay events for whichever range (day/week) is
 * currently selected. Called periodically from main.cpp's loop() (see
 * calendar_view_poll()) and right after returning from Settings, since a
 * filter/server change there won't otherwise show up until the next poll.
 *
 * If the calendar server was never configured at boot, this also handles
 * building the tabs/list UI for the first time (rather than requiring a
 * reboot) if Settings -> Calendar Server has since been filled in.
 */
void calendar_view_refresh();

/**
 * Call every loop() iteration. Internally rate-limits itself to once every
 * few minutes -- polling more often than that would hammer calendar_api.py
 * (and, transitively, Forex Factory) for no benefit, per the brief's
 * "don't hammer the source" guidance.
 */
void calendar_view_poll();

/**
 * Call every loop() iteration, after alert_manager_tick(). Refreshes the
 * on-screen countdown label from alert_manager_get_countdown_text() -- a
 * no-op if the events UI hasn't been built yet (no server configured).
 */
void calendar_view_tick();
