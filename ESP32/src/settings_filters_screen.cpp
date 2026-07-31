#include <algorithm>
#include <vector>

#include "calendar_client.h"
#include "settings_filters_screen.h"
#include "theme.h"

namespace {

lv_obj_t * screen = nullptr;
lv_obj_t * previous_screen_ref = nullptr;
lv_obj_t * currency_list = nullptr;
lv_obj_t * status_label = nullptr;
lv_obj_t * checkbox_holiday = nullptr;
lv_obj_t * checkbox_low = nullptr;
lv_obj_t * checkbox_medium = nullptr;
lv_obj_t * checkbox_high = nullptr;

// Cached after first successful fetch so reopening this screen later in the
// same session doesn't re-fetch. Checkbox user_data below is an index into
// this vector rather than a pointer into its String data, so it stays valid
// even though currencies_cache itself outlives any single screen open/close.
std::vector<StringOption> currencies_cache;

// Filled in settings_filters_screen_open() from GET /filters. This screen
// only edits impact/currencies, but /filters is one combined object -- Save
// must carry column_codes through unchanged rather than silently resetting
// it to CalendarFilters' defaults.
CalendarFilters loaded_filters;

void return_to_previous_screen()
{
    lv_screen_load(previous_screen_ref);
    lv_obj_delete(screen);
    screen = nullptr;
}

void on_back_clicked(lv_event_t *)
{
    return_to_previous_screen();
}

/** Read every checkbox back into a CalendarFilters and POST it. */
void on_save_clicked(lv_event_t *)
{
    lv_label_set_text(status_label, "Saving...");
    lv_timer_handler();

    CalendarFilters filters = loaded_filters;
    filters.impact_holiday = lv_obj_has_state(checkbox_holiday, LV_STATE_CHECKED);
    filters.impact_low = lv_obj_has_state(checkbox_low, LV_STATE_CHECKED);
    filters.impact_medium = lv_obj_has_state(checkbox_medium, LV_STATE_CHECKED);
    filters.impact_high = lv_obj_has_state(checkbox_high, LV_STATE_CHECKED);

    filters.currency_codes.clear();
    const uint32_t child_count = lv_obj_get_child_count(currency_list);
    for (uint32_t i = 0; i < child_count; ++i) {
        lv_obj_t * checkbox = lv_obj_get_child(currency_list, i);
        if (lv_obj_has_state(checkbox, LV_STATE_CHECKED)) {
            const size_t idx = static_cast<size_t>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(checkbox)));
            filters.currency_codes.push_back(currencies_cache[idx].code);
        }
    }

    if (filters.currency_codes.empty() ||
        (!filters.impact_holiday && !filters.impact_low && !filters.impact_medium && !filters.impact_high)) {
        lv_label_set_text(status_label, "Select at least one impact level and one currency.");
        return;
    }

    if (calendar_client_save_filters(filters)) {
        return_to_previous_screen();
    } else {
        lv_label_set_text(status_label, "Save failed -- check the connection and try again.");
    }
}

} // namespace

void settings_filters_screen_open(lv_obj_t * previous_screen)
{
    previous_screen_ref = previous_screen;

    screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(screen, lv_color_hex(THEME_COLOR_BACKGROUND), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_screen_load(screen);

    lv_obj_t * title = lv_label_create(screen);
    lv_label_set_text(title, "IMPACT & CURRENCIES");
    lv_obj_set_style_text_color(title, lv_color_hex(THEME_COLOR_AMBER), 0);
    lv_obj_set_style_text_font(title, &lv_font_unscii_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 20, 10);

    lv_obj_t * impact_label = lv_label_create(screen);
    lv_label_set_text(impact_label, "Impact level:");
    lv_obj_set_style_text_color(impact_label, lv_color_hex(THEME_COLOR_AMBER_DIM), 0);
    lv_obj_align(impact_label, LV_ALIGN_TOP_LEFT, 20, 56);

    checkbox_holiday = theme_create_checkbox(screen, "Holiday");
    lv_obj_align(checkbox_holiday, LV_ALIGN_TOP_LEFT, 150, 54);

    checkbox_low = theme_create_checkbox(screen, "Low");
    lv_obj_align(checkbox_low, LV_ALIGN_TOP_LEFT, 280, 54);

    checkbox_medium = theme_create_checkbox(screen, "Medium");
    lv_obj_align(checkbox_medium, LV_ALIGN_TOP_LEFT, 400, 54);

    checkbox_high = theme_create_checkbox(screen, "High");
    lv_obj_align(checkbox_high, LV_ALIGN_TOP_LEFT, 540, 54);

    lv_obj_t * currencies_label = lv_label_create(screen);
    lv_label_set_text(currencies_label, "Currencies:");
    lv_obj_set_style_text_color(currencies_label, lv_color_hex(THEME_COLOR_AMBER_DIM), 0);
    lv_obj_align(currencies_label, LV_ALIGN_TOP_LEFT, 20, 90);

    // Tall enough to show all ~10 currencies (vs. investing.com's 107
    // countries) without needing to scroll to see the last one.
    currency_list = lv_obj_create(screen);
    lv_obj_set_size(currency_list, 760, 190);
    lv_obj_align(currency_list, LV_ALIGN_TOP_LEFT, 20, 114);
    lv_obj_set_flex_flow(currency_list, LV_FLEX_FLOW_ROW_WRAP);
    theme_style_checkbox_wrap(currency_list);

    status_label = lv_label_create(screen);
    lv_label_set_text(status_label, "");
    lv_obj_set_style_text_color(status_label, lv_color_hex(THEME_COLOR_IMPACT_HIGH), 0);
    lv_obj_align(status_label, LV_ALIGN_BOTTOM_LEFT, 20, -10);

    lv_obj_t * back_btn = theme_create_button(screen, "Back", on_back_clicked);
    lv_obj_align(back_btn, LV_ALIGN_BOTTOM_RIGHT, -140, -8);

    lv_obj_t * save_btn = theme_create_button(screen, "Save", on_save_clicked);
    lv_obj_align(save_btn, LV_ALIGN_BOTTOM_RIGHT, -20, -8);

    // Fetch current state (blocking, same pattern used elsewhere in this
    // codebase) and populate checkboxes to match before the user sees them.
    lv_label_set_text(status_label, "Loading current settings...");
    lv_timer_handler();

    calendar_client_get_filters(loaded_filters); // leaves the struct's defaults in place on failure
    lv_obj_add_state(checkbox_holiday, loaded_filters.impact_holiday ? LV_STATE_CHECKED : 0);
    lv_obj_add_state(checkbox_low, loaded_filters.impact_low ? LV_STATE_CHECKED : 0);
    lv_obj_add_state(checkbox_medium, loaded_filters.impact_medium ? LV_STATE_CHECKED : 0);
    lv_obj_add_state(checkbox_high, loaded_filters.impact_high ? LV_STATE_CHECKED : 0);

    if (currencies_cache.empty()) {
        calendar_client_get_currencies(currencies_cache);
    }

    if (currencies_cache.empty()) {
        lv_label_set_text(status_label, "Could not load currency list -- check the connection.");
    } else {
        for (size_t i = 0; i < currencies_cache.size(); ++i) {
            const StringOption & option = currencies_cache[i];
            lv_obj_t * checkbox = theme_create_checkbox(currency_list, option.name.c_str());
            lv_obj_set_width(checkbox, 230);
            lv_obj_set_user_data(checkbox, reinterpret_cast<void *>(static_cast<intptr_t>(i)));
            const bool selected = std::find(loaded_filters.currency_codes.begin(),
                                            loaded_filters.currency_codes.end(),
                                            option.code) != loaded_filters.currency_codes.end();
            lv_obj_add_state(checkbox, selected ? LV_STATE_CHECKED : 0);
        }
        lv_label_set_text(status_label, "");
    }
}
