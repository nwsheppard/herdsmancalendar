#include <algorithm>
#include <vector>

#include "calendar_client.h"
#include "settings_columns_screen.h"
#include "theme.h"

namespace {

lv_obj_t * screen = nullptr;
lv_obj_t * previous_screen_ref = nullptr;
lv_obj_t * column_list = nullptr;
lv_obj_t * status_label = nullptr;

// Cached after first successful fetch so reopening this screen later in the
// same session doesn't re-fetch. Checkbox user_data below is an index into
// this vector rather than a pointer into its String data, so it stays valid
// even though columns_cache itself outlives any single screen open/close.
std::vector<StringOption> columns_cache;

// Filled from GET /filters. This screen only edits columns, but /filters is
// one combined object -- Save must carry impact/countries/categories
// through unchanged rather than resetting them to CalendarFilters' defaults.
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

void on_save_clicked(lv_event_t *)
{
    lv_label_set_text(status_label, "Saving...");
    lv_timer_handler();

    CalendarFilters filters = loaded_filters;
    filters.column_codes.clear();

    const uint32_t child_count = lv_obj_get_child_count(column_list);
    for (uint32_t i = 0; i < child_count; ++i) {
        lv_obj_t * checkbox = lv_obj_get_child(column_list, i);
        if (lv_obj_has_state(checkbox, LV_STATE_CHECKED)) {
            const size_t idx = static_cast<size_t>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(checkbox)));
            filters.column_codes.push_back(columns_cache[idx].code);
        }
    }

    if (filters.column_codes.empty()) {
        lv_label_set_text(status_label, "Select at least one column.");
        return;
    }

    if (calendar_client_save_filters(filters)) {
        return_to_previous_screen();
    } else {
        lv_label_set_text(status_label, "Save failed -- check the connection and try again.");
    }
}

} // namespace

void settings_columns_screen_open(lv_obj_t * previous_screen)
{
    previous_screen_ref = previous_screen;

    screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(screen, lv_color_hex(THEME_COLOR_BACKGROUND), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_screen_load(screen);

    lv_obj_t * title = lv_label_create(screen);
    lv_label_set_text(title, "DATA COLUMNS");
    lv_obj_set_style_text_color(title, lv_color_hex(THEME_COLOR_AMBER), 0);
    lv_obj_set_style_text_font(title, &lv_font_unscii_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 20, 10);

    lv_obj_t * hint_label = lv_label_create(screen);
    lv_label_set_text(hint_label, "Show these fields for each event:");
    lv_obj_set_style_text_color(hint_label, lv_color_hex(THEME_COLOR_AMBER_DIM), 0);
    lv_obj_align(hint_label, LV_ALIGN_TOP_LEFT, 20, 56);

    column_list = lv_obj_create(screen);
    lv_obj_set_size(column_list, 760, 340);
    lv_obj_align(column_list, LV_ALIGN_TOP_LEFT, 20, 90);
    lv_obj_set_flex_flow(column_list, LV_FLEX_FLOW_ROW_WRAP);
    theme_style_checkbox_wrap(column_list);

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

    if (columns_cache.empty()) {
        calendar_client_get_columns(columns_cache);
    }

    if (columns_cache.empty()) {
        lv_label_set_text(status_label, "Could not load column list -- check the connection.");
    } else {
        for (size_t i = 0; i < columns_cache.size(); ++i) {
            const StringOption & option = columns_cache[i];
            lv_obj_t * checkbox = theme_create_checkbox(column_list, option.name.c_str());
            lv_obj_set_width(checkbox, 230);
            lv_obj_set_user_data(checkbox, reinterpret_cast<void *>(static_cast<intptr_t>(i)));
            const bool selected = std::find(loaded_filters.column_codes.begin(),
                                            loaded_filters.column_codes.end(),
                                            option.code) != loaded_filters.column_codes.end();
            lv_obj_add_state(checkbox, selected ? LV_STATE_CHECKED : 0);
        }
        lv_label_set_text(status_label, "");
    }
}
