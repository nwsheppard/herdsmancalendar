#include <Arduino.h>
#include <vector>

#include "alert_manager.h"
#include "calendar_client.h"
#include "calendar_model.h"
#include "calendar_server_store.h"
#include "calendar_view.h"
#include "settings_screen.h"
#include "theme.h"

namespace {

// Every few minutes is the brief's own suggested cadence for an economic
// calendar ("every 5-15 minutes is typical... don't hammer the source").
constexpr uint32_t poll_interval_ms = 10 * 60 * 1000;
uint32_t last_poll_ms = 0;

lv_obj_t * calendar_screen_ref = nullptr;
lv_obj_t * wifi_icon_label = nullptr;
lv_obj_t * wifi_icon_strike = nullptr;
lv_obj_t * no_server_message = nullptr;
lv_obj_t * day_tab_button = nullptr;
lv_obj_t * week_tab_button = nullptr;
lv_obj_t * list = nullptr;
lv_obj_t * status_label = nullptr;
lv_obj_t * countdown_label = nullptr;

// "week" matches calendar_api.py's own /calendar default.
String current_range = "week";

constexpr int16_t col_impact_w = 10;
constexpr int16_t col_time_w = 70;
constexpr int16_t col_ccy_w = 55;
constexpr int16_t col_act_w = 70;
constexpr int16_t col_fcst_w = 70;
constexpr int16_t col_prev_w = 70;

uint32_t impact_color_for(int level)
{
    switch (level) {
    case 3:
        return THEME_COLOR_IMPACT_HIGH;
    case 2:
        return THEME_COLOR_IMPACT_MEDIUM;
    default:
        // Covers both 1 (low) and 0 (holiday) -- neither warrants its own
        // accent color, both are meant to read as low visual priority.
        return THEME_COLOR_IMPACT_LOW;
    }
}

/** Colors a value (actual/previous) the same way forexfactory.com itself does. */
uint32_t value_color_for(const String & state)
{
    if (state == "better") {
        return THEME_COLOR_VALUE_BETTER;
    }
    if (state == "worse") {
        return THEME_COLOR_VALUE_WORSE;
    }
    return THEME_COLOR_AMBER_DIM;
}

void on_settings_clicked(lv_event_t *)
{
    settings_screen_open(calendar_screen_ref);
}

/** Recolors the two tab buttons so `active` reads as selected (filled amber) and `inactive` doesn't. */
void set_active_tab(lv_obj_t * active, lv_obj_t * inactive)
{
    lv_obj_set_style_bg_color(active, lv_color_hex(THEME_COLOR_AMBER), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(active, 0), lv_color_hex(THEME_COLOR_BACKGROUND), 0);

    lv_obj_set_style_bg_color(inactive, lv_color_hex(THEME_COLOR_BUTTON_BG), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(inactive, 0), lv_color_hex(THEME_COLOR_AMBER), 0);
}

void refresh_events();

void on_day_tab_clicked(lv_event_t *)
{
    current_range = "day";
    set_active_tab(day_tab_button, week_tab_button);
    refresh_events();
}

void on_week_tab_clicked(lv_event_t *)
{
    current_range = "week";
    set_active_tab(week_tab_button, day_tab_button);
    refresh_events();
}

/** A thin day-separator label (e.g. "MonJul 27") between groups of events. */
lv_obj_t * make_day_separator_row(lv_obj_t * parent, const String & day_text)
{
    lv_obj_t * label = lv_label_create(parent);
    lv_label_set_text(label, day_text.c_str());
    lv_obj_set_style_text_color(label, lv_color_hex(THEME_COLOR_AMBER_DIM), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_pad_top(label, 8, 0);
    return label;
}

/** One fixed-width-column row; widths match make_header_row() so text lines up. */
lv_obj_t * make_event_row(lv_obj_t * parent, const CalendarEvent & event)
{
    lv_obj_t * row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 56);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(THEME_COLOR_BACKGROUND), 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_set_style_pad_left(row, 6, 0);
    lv_obj_set_style_pad_right(row, 6, 0);

    lv_obj_t * impact_bar = lv_obj_create(row);
    lv_obj_remove_style_all(impact_bar);
    lv_obj_set_size(impact_bar, col_impact_w, 40);
    lv_obj_set_style_bg_color(impact_bar, lv_color_hex(impact_color_for(event.impact_level)), 0);
    lv_obj_set_style_bg_opa(impact_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(impact_bar, 2, 0);

    auto add_label = [row](const String & text, int16_t width, bool grow, uint32_t color) {
        lv_obj_t * label = lv_label_create(row);
        lv_label_set_text(label, text.c_str());
        lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
        if (grow) {
            lv_obj_set_flex_grow(label, 1);
            lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        } else {
            lv_obj_set_width(label, width);
        }
    };

    // "*" flags a previous value Forex Factory revised from what was
    // originally reported last time -- independent of whether the revision
    // itself reads as better/worse (a plain "revised" wrapper span, with
    // neither class, is still a revision, just not one worth coloring).
    const String previous_text = event.previous_revised ? event.previous + "*" : event.previous;

    add_label(event.time, col_time_w, false, THEME_COLOR_AMBER);
    add_label(event.currency, col_ccy_w, false, THEME_COLOR_AMBER_DIM);
    add_label(event.name, 0, true, THEME_COLOR_AMBER);
    add_label(event.actual, col_act_w, false, value_color_for(event.actual_state));
    add_label(event.forecast, col_fcst_w, false, THEME_COLOR_AMBER_DIM);
    add_label(previous_text, col_prev_w, false, value_color_for(event.previous_state));

    return row;
}

/** Column header labels, using the same fixed widths as make_event_row(). */
void make_header_row(lv_obj_t * parent)
{
    lv_obj_t * header = lv_obj_create(parent);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, 760, 24);
    lv_obj_align(header, LV_ALIGN_TOP_LEFT, 20, 108);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(header, 6, 0);
    lv_obj_set_style_pad_left(header, col_impact_w + 6, 0);

    auto add_header_label = [header](const char * text, int16_t width, bool grow) {
        lv_obj_t * label = lv_label_create(header);
        lv_label_set_text(label, text);
        lv_obj_set_style_text_color(label, lv_color_hex(THEME_COLOR_AMBER_DIM), 0);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
        if (grow) {
            lv_obj_set_flex_grow(label, 1);
        } else {
            lv_obj_set_width(label, width);
        }
    };

    add_header_label("TIME", col_time_w, false);
    add_header_label("CCY", col_ccy_w, false);
    add_header_label("EVENT", 0, true);
    add_header_label("ACT", col_act_w, false);
    add_header_label("FCST", col_fcst_w, false);
    add_header_label("PREV", col_prev_w, false);
}

/**
 * Shown instead of the event list when no calendar server has been set up
 * yet -- a brand new device, or one where Settings -> Calendar Server was
 * never filled in. Tracked in a namespace variable (unlike the other
 * static-shaped screen pieces) since calendar_view_refresh() needs to
 * delete it if a server gets configured later without a reboot.
 */
void make_no_server_message(lv_obj_t * parent)
{
    no_server_message = lv_label_create(parent);
    lv_label_set_text(no_server_message,
                       "No calendar server configured.\n\n"
                       "Tap the gear icon above to set one up in\n"
                       "Settings -> Calendar Server.");
    lv_obj_set_style_text_color(no_server_message, lv_color_hex(THEME_COLOR_AMBER_DIM), 0);
    lv_obj_set_style_text_font(no_server_message, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(no_server_message, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(no_server_message, LV_ALIGN_CENTER, 0, 20);
}

/** Builds the tabs/header/list/status pieces -- shared by first boot and a later calendar_view_refresh(). */
void build_events_ui(lv_obj_t * screen)
{
    day_tab_button = theme_create_button(screen, "Day Events", on_day_tab_clicked);
    lv_obj_set_size(day_tab_button, 180, 40);
    lv_obj_align(day_tab_button, LV_ALIGN_TOP_LEFT, 20, 58);

    week_tab_button = theme_create_button(screen, "Week Events", on_week_tab_clicked);
    lv_obj_set_size(week_tab_button, 180, 40);
    lv_obj_align(week_tab_button, LV_ALIGN_TOP_LEFT, 210, 58);

    set_active_tab(week_tab_button, day_tab_button);

    // Sits in the free space to the right of the tabs (they end at x=390;
    // the WiFi/settings icons start around x=728) rather than needing the
    // whole layout shifted down again for a dedicated row. Empty until
    // alert_manager reports a timed event within its 10-minute countdown
    // window -- see calendar_view_tick().
    countdown_label = lv_label_create(screen);
    lv_label_set_text(countdown_label, "");
    lv_obj_set_style_text_color(countdown_label, lv_color_hex(THEME_COLOR_IMPACT_MEDIUM), 0);
    lv_obj_set_style_text_font(countdown_label, &lv_font_montserrat_20, 0);
    lv_obj_set_width(countdown_label, 320);
    lv_label_set_long_mode(countdown_label, LV_LABEL_LONG_DOT);
    lv_obj_align(countdown_label, LV_ALIGN_TOP_LEFT, 410, 68);

    make_header_row(screen);

    list = lv_obj_create(screen);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, 760, 300);
    lv_obj_align(list, LV_ALIGN_TOP_LEFT, 20, 136);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 4, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    theme_style_scrollbar(list);

    status_label = lv_label_create(screen);
    lv_label_set_text(status_label, "");
    lv_obj_set_style_text_color(status_label, lv_color_hex(THEME_COLOR_AMBER_DIM), 0);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_14, 0);
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 220);
}

void populate_events(const std::vector<CalendarEvent> & events)
{
    lv_obj_clean(list);

    if (events.empty()) {
        lv_label_set_text(status_label, "No events match your current filters for this range.");
        return;
    }

    lv_label_set_text(status_label, "");
    String last_day;
    for (const CalendarEvent & event : events) {
        if (event.day.length() > 0 && event.day != last_day) {
            make_day_separator_row(list, event.day);
            last_day = event.day;
        }
        make_event_row(list, event);
    }
}

void refresh_events()
{
    lv_label_set_text(status_label, "Loading events...");
    lv_timer_handler();

    std::vector<CalendarEvent> events;
    if (calendar_client_get_calendar(current_range, events)) {
        populate_events(events);
    } else {
        lv_obj_clean(list);
        lv_label_set_text(status_label, "Could not load events -- check the connection and try again.");
    }

    // Unconditional, including on failure/empty (an empty vector clears any
    // previously tracked events) -- alert_manager shouldn't keep counting
    // down to or refreshing for an event that no longer matches current
    // filters or came from a now-stale fetch.
    alert_manager_set_events(events);
}

} // namespace

lv_obj_t * calendar_view_create()
{
    lv_obj_t * screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(screen, lv_color_hex(THEME_COLOR_BACKGROUND), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    calendar_screen_ref = screen;

    lv_obj_t * title = lv_label_create(screen);
    lv_label_set_text(title, "UPCOMING EVENTS");
    lv_obj_set_style_text_color(title, lv_color_hex(THEME_COLOR_AMBER), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 20, 10);

    // WiFi status icon takes the corner spot the gear used to occupy; the
    // gear moves left to make room. Same color/strike convention as
    // main.cpp's boot-splash icon (dim/green/red+strike), driven by the
    // same WiFi.onEvent() callback via calendar_view_set_wifi_icon() -- the
    // boot splash only exists briefly at startup, so without a second icon
    // here a WiFi drop later has nowhere on-screen to show up.
    wifi_icon_label = lv_label_create(screen);
    lv_label_set_text(wifi_icon_label, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(wifi_icon_label, &lv_font_montserrat_20, 0);
    lv_obj_align(wifi_icon_label, LV_ALIGN_TOP_RIGHT, -20, 16);

    // Strikethrough for the "disconnected" state -- LVGL's symbol set has
    // no "no-wifi" glyph, so this is a small red bar rotated across the
    // icon, hidden unless we're actually disconnected. Mirrors main.cpp's
    // boot-splash version exactly.
    wifi_icon_strike = lv_obj_create(screen);
    lv_obj_remove_style_all(wifi_icon_strike);
    lv_obj_set_size(wifi_icon_strike, 26, 3);
    lv_obj_set_style_bg_color(wifi_icon_strike, lv_color_hex(THEME_COLOR_IMPACT_HIGH), 0);
    lv_obj_set_style_bg_opa(wifi_icon_strike, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(wifi_icon_strike, 2, 0);
    lv_obj_set_style_transform_pivot_x(wifi_icon_strike, 13, 0);
    lv_obj_set_style_transform_pivot_y(wifi_icon_strike, 1, 0);
    lv_obj_set_style_transform_rotation(wifi_icon_strike, 450, 0);
    lv_obj_align_to(wifi_icon_strike, wifi_icon_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(wifi_icon_strike, LV_OBJ_FLAG_HIDDEN);

    // Explicit size rather than theme_create_button()'s content-fit default --
    // that default rendered taller on hardware than expected, overlapping the
    // header row below it (which assumed a much shorter button). Moved left
    // of its old spot (-20) to make room for the WiFi icon there instead.
    lv_obj_t * settings_button = theme_create_button(screen, LV_SYMBOL_SETTINGS, on_settings_clicked);
    lv_obj_set_size(settings_button, 44, 44);
    lv_obj_align(settings_button, LV_ALIGN_TOP_RIGHT, -72, 6);

    String server_url;
    if (!calendar_server_url_load(server_url)) {
        make_no_server_message(screen);
        return screen;
    }

    build_events_ui(screen);
    refresh_events();

    return screen;
}

void calendar_view_set_wifi_icon(uint32_t color_hex, bool show_strike)
{
    if (wifi_icon_label != nullptr) {
        lv_obj_set_style_text_color(wifi_icon_label, lv_color_hex(color_hex), 0);
    }
    if (wifi_icon_strike != nullptr) {
        if (show_strike) {
            lv_obj_remove_flag(wifi_icon_strike, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(wifi_icon_strike, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void calendar_view_refresh()
{
    if (calendar_screen_ref == nullptr) {
        return; // calendar_view_create() hasn't run yet
    }

    if (list == nullptr) {
        // Server wasn't configured when calendar_view_create() ran, so the
        // tabs/header/list were never built -- only worth building them now
        // if Settings has since gained a server address to actually fetch
        // from. Otherwise leave the "no server configured" message as-is.
        String server_url;
        if (!calendar_server_url_load(server_url)) {
            return;
        }
        if (no_server_message != nullptr) {
            lv_obj_delete(no_server_message);
            no_server_message = nullptr;
        }
        build_events_ui(calendar_screen_ref);
    }

    refresh_events();
}

void calendar_view_tick()
{
    if (countdown_label == nullptr) {
        return; // no server configured yet, or "no server" message still showing
    }
    lv_label_set_text(countdown_label, alert_manager_get_countdown_text().c_str());
}

void calendar_view_poll()
{
    // Timer is managed here, not inside refresh_events()/calendar_view_refresh():
    // this needs to rate-limit *checking*, including the no-server-configured
    // case where refresh_events() never actually runs -- otherwise that case
    // would re-check calendar_server_url_load() on every single loop()
    // iteration once past the first interval, forever, instead of once per
    // interval like everything else.
    const uint32_t now = millis();
    if (now - last_poll_ms >= poll_interval_ms) {
        last_poll_ms = now;
        calendar_view_refresh();
    }
}
