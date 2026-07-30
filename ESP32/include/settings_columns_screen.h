#pragma once

#include <lvgl.h>

/**
 * Settings > Calendar Filters > Data Columns: pick which per-event fields
 * (Impact/Actual/Forecast/Previous) calendar_api.py should return. Turning
 * one off just means that field comes back blank -- currency/time/name are
 * always present and not part of this list (calendar_api.py's
 * apply_column_filter() applies this after scraping, since Forex Factory
 * always renders all of these fields regardless of what's requested).
 */
void settings_columns_screen_open(lv_obj_t * previous_screen);
