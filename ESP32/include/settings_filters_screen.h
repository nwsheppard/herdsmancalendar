#pragma once

#include <lvgl.h>

/**
 * Calendar filters settings sub-screen: impact level checkboxes (including
 * Holiday, for Forex Factory's grey/non-economic tier) + a currency
 * checkbox list, fetched from calendar_api.py on open. Save POSTs the
 * selection and returns to `previous_screen`; Back discards.
 */
void settings_filters_screen_open(lv_obj_t * previous_screen);
