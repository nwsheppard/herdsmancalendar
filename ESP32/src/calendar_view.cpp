#include <Arduino.h>
#include <vector>

#include "alert_manager.h"
#include "calendar_client.h"
#include "calendar_model.h"
#include "calendar_server_store.h"
#include "calendar_view.h"
#include "fonts.h"
#include "settings_screen.h"
#include "theme.h"

namespace {

// 2026-08: this project went through a whole saga chasing Forex Factory's
// Cloudflare challenge -- calendar_api.py briefly needed up to 65s per
// fetch (FlareSolverr solving the challenge inline), which pushed a lot of
// complexity onto this file: a background FreeRTOS task just to keep the
// screen responsive during that wait, then a long-polling redesign
// (/calendar/wait) to make the ESP32 reactive instead of polling on a
// timer. calendar_api.py was restructured so a background thread on the
// LXC does that whole dance on its own schedule -- /calendar itself is
// just a fast local cache read now, regardless of how often it's asked.
// Reported directly, correctly: with that settled, the ESP32 doesn't need
// to carry any of the complexity that was only ever there to cope with a
// slow upstream it no longer has to wait on. Back to plain periodic
// polling -- simpler, and confirmed to also matter for a separate,
// intermittent connect issue this investigation eventually isolated to
// the background task itself: calls issued from it were failing to reach
// the server (confirmed via calendar_api.py's own access log showing zero
// record of them), while the exact same calls issued directly from this
// task always succeeded. refresh_events() is synchronous now (see its own
// comment) for that reason -- the background task is gone entirely, not
// just polled less often.
constexpr uint32_t poll_interval_ms = 10 * 60 * 1000;

// Used instead of poll_interval_ms right after a failed refresh -- retrying
// sooner than the full periodic interval means not leaving "Could not load
// events" on screen for up to 10 minutes after a transient failure, but
// still not hammering a persistently broken connection in a tight loop.
constexpr uint32_t refresh_retry_delay_ms = 30 * 1000;
bool retry_scheduled = false;
uint32_t retry_scheduled_at_ms = 0;

// Both updated by every refresh, periodic or otherwise (a tab switch, a
// Settings change, alert_manager's post-event refresh) -- so the periodic
// timer always measures from whatever the *last* refresh attempt actually
// was, not just from routine periodic ones, and calendar_view_poll() can
// tell which of poll_interval_ms/refresh_retry_delay_ms applied last time
// without needing a third variable to track that separately.
uint32_t last_poll_ms = 0;
bool last_refresh_ok = true;

lv_obj_t * calendar_screen_ref = nullptr;
lv_obj_t * wifi_icon_label = nullptr;
lv_obj_t * wifi_icon_strike = nullptr;
lv_obj_t * no_server_message = nullptr;
lv_obj_t * day_tab_button = nullptr;
lv_obj_t * week_tab_button = nullptr;
lv_obj_t * list = nullptr;
lv_obj_t * header = nullptr;
lv_obj_t * status_label = nullptr;
lv_obj_t * no_events_scene = nullptr;
lv_obj_t * fomc_badge = nullptr;

// Perspective floor grid line counts for no_events_scene -- see its own
// comment. Fixed at compile time so no_events_grid_points below can be a
// plain static array (LVGL only stores a pointer to a line's points, not a
// copy -- see lv_line.h -- so this storage has to outlive every lv_line_t
// built from it, which a namespace-scope array does for free).
constexpr int no_events_vertical_lines = 7;
constexpr int no_events_horizontal_lines = 6;
constexpr int no_events_total_lines = no_events_vertical_lines + no_events_horizontal_lines + 1; // +1 horizon bar
lv_point_precise_t no_events_grid_points[no_events_total_lines][2];

// Computed once in build_events_ui() from the WiFi icon's actual rendered
// position (see its own comment there) and reused by every later header
// rebuild in populate_events() -- the icon doesn't move at runtime, so
// there's no need to re-measure it on every refresh.
int16_t grid_width = 0;

// Default launch screen is Day Events, not calendar_api.py's own /calendar
// default ("week") -- a user preference, not a backend constraint.
String current_range = "day";

// Populated by refresh_events() alongside alert_manager_set_events() --
// update_next_event_row() needs each event's plain scheduled time text to
// restore a row that's no longer the countdown target (the row itself only
// carries its event id, via lv_obj_user_data, not its original time text).
std::vector<CalendarEvent> current_events;

// One entry per row currently highlighted as an active countdown --
// several at once whenever multiple events share (or nearly share) a
// scheduled time, which is routine for a trading calendar (several
// indicators dropping at the same 8:30am, for example), not an edge case
// worth collapsing down to a single "next" event.
struct HighlightedRow {
    long id;
    // Last "MM:SS" string actually written to this row's TIME label.
    // calendar_view_tick() (and so update_next_event_row()) runs every
    // loop() iteration, not just once a second -- alert_manager's own
    // countdown values only actually change once a second internally, so
    // without this guard lv_label_set_text() would still run on every
    // single iteration with an unchanged string. Confirmed directly on
    // hardware to matter: LVGL invalidates on every lv_label_set_text()
    // call regardless of whether the text changed, and this display's
    // LV_DISPLAY_RENDER_MODE_FULL means any invalidation forces a full
    // 800x480 frame redraw/present -- ~66ms at this panel's configured
    // pixel clock. Without this guard that ran on every loop() iteration
    // (~15/s) for as long as a countdown was showing, not once a second as
    // intended -- confirmed as the cause of the intermittent display
    // "blips" reported from hardware.
    String last_shown_countdown_text;
};
std::vector<HighlightedRow> highlighted_rows;

bool contains_id(const std::vector<long> & ids, long id)
{
    for (long existing : ids) {
        if (existing == id) {
            return true;
        }
    }
    return false;
}

// TIME/CCY stay fixed -- neither varies enough row to row to be worth
// measuring dynamically (TIME is always "H:MMam"-shaped or a short fixed
// string like "Tentative"; CCY is always exactly a 3-letter code, or
// hidden entirely -- see show_ccy in ColumnLayout below). ACT/FCST/PREV
// don't have their own constants anymore -- see compute_column_layout().
constexpr int16_t col_impact_w = 10;
constexpr int16_t col_time_w = 105;
constexpr int16_t col_ccy_w = 55;

// spacemono_18's advance width, in px -- confirmed from the generated
// font's adv_w=176 (1/16px fixed-point), and it lands on a clean integer
// (176/16 = 11 exactly), not a coincidence: lv_font_conv rounds advance
// widths to whole 1/16px units, and this particular size happened to land
// on a whole pixel.
constexpr int16_t spacemono_18_char_w = 11;

// Fallback widths for the header shown briefly before the first fetch
// resolves (build_events_ui() renders one immediately so the screen isn't
// blank while "Loading events..." shows) -- overwritten by
// compute_column_layout()'s actual measurement the moment real data (or a
// failed fetch) comes back. Sized generously since there's no real data
// yet to measure against.
constexpr int16_t col_act_w_fallback = 95;
constexpr int16_t col_fcst_w_fallback = 95;
constexpr int16_t col_prev_w_fallback = 100;

/**
 * ACT/FCST/PREV widths, and whether to show CCY at all, computed fresh
 * for whatever's actually on screen -- not a fixed guess wide enough for
 * a worst case that's rarely actually present. Maximizes room for EVENT,
 * the column that actually benefits from it (long economic-indicator
 * names), directly per the request that prompted this ("I want all the
 * space that I can get for EVENT").
 */
struct ColumnLayout {
    bool show_ccy = true;
    int16_t act_w = col_act_w_fallback;
    int16_t fcst_w = col_fcst_w_fallback;
    int16_t prev_w = col_prev_w_fallback;
};

/**
 * Measures the widest ACT/FCST/PREV value actually present in `events`
 * (never narrower than that column's own header label, so "ACT"/"FCST"/
 * "PREV" never themselves get clipped) and sizes each column to fit it
 * plus a little padding, instead of a fixed width wide enough for a
 * worst case ("Tentative" in TIME, an unusually wide number) that's
 * rarely actually on screen at once.
 */
ColumnLayout compute_column_layout(const std::vector<CalendarEvent> & events, bool show_ccy)
{
    ColumnLayout layout;
    layout.show_ccy = show_ccy;

    size_t max_act_chars = 3;  // "ACT"
    size_t max_fcst_chars = 4; // "FCST"
    size_t max_prev_chars = 4; // "PREV"

    for (const CalendarEvent & event : events) {
        if (static_cast<size_t>(event.actual.length()) > max_act_chars) {
            max_act_chars = static_cast<size_t>(event.actual.length());
        }
        if (static_cast<size_t>(event.forecast.length()) > max_fcst_chars) {
            max_fcst_chars = static_cast<size_t>(event.forecast.length());
        }
        // +1 for the revision "*" suffix (see make_event_row()) -- a
        // revised value one char longer than the widest plain one still
        // needs to fit without ellipsizing.
        size_t prev_chars = static_cast<size_t>(event.previous.length());
        if (event.previous_revised) {
            prev_chars += 1;
        }
        if (prev_chars > max_prev_chars) {
            max_prev_chars = prev_chars;
        }
    }

    // A little breathing room past the exact pixel measurement -- text
    // sized to the exact width of its own content reads as cramped
    // against its neighbors with zero margin.
    constexpr int16_t column_padding_px = 10;
    layout.act_w = static_cast<int16_t>(max_act_chars * spacemono_18_char_w + column_padding_px);
    layout.fcst_w = static_cast<int16_t>(max_fcst_chars * spacemono_18_char_w + column_padding_px);
    layout.prev_w = static_cast<int16_t>(max_prev_chars * spacemono_18_char_w + column_padding_px);
    return layout;
}

// Anything above roughly a couple of frames' worth of time (60fps = ~16.6ms
// each) is worth a log line -- the frame-shift hazard needs a stall long
// enough to starve the RGB panel's bounce-buffer refill, not routine
// per-frame variance.
constexpr uint32_t stall_log_threshold_ms = 20;

/**
 * lv_timer_handler(), but logs to Serial (with an absolute timestamp) if a
 * single call takes long enough to plausibly cause the frame-shift/pixelation
 * glitch -- see the write-up in the README. Every call site in this file
 * goes through this instead of the bare LVGL function, so a recurrence shows
 * up in the serial log with when and during what, instead of just being a
 * glitch nobody has a record of.
 */
void timed_timer_handler(const char * context)
{
    const uint32_t start_ms = millis();
    lv_timer_handler();
    const uint32_t elapsed_ms = millis() - start_ms;
    if (elapsed_ms > stall_log_threshold_ms) {
        Serial.printf("[stall] lv_timer_handler() took %lums during %s, ending t=%lums\n",
                      static_cast<unsigned long>(elapsed_ms), context, static_cast<unsigned long>(millis()));
    }
}

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

/** A thin day-separator label (e.g. "Mon Jul 27") between groups of events. */
lv_obj_t * make_day_separator_row(lv_obj_t * parent, const String & day_text)
{
    lv_obj_t * label = lv_label_create(parent);
    lv_label_set_text(label, day_text.c_str());
    lv_obj_set_style_text_color(label, lv_color_hex(THEME_COLOR_AMBER_DIM), 0);
    // Matches add_label()'s font below -- this label sits directly above
    // the grid and should read at the same density.
    lv_obj_set_style_text_font(label, &lv_font_spacemono_18, 0);
    lv_obj_set_style_pad_top(label, 8, 0);
    return label;
}

/**
 * One row; ACT/FCST/PREV widths and whether CCY shows at all come from
 * `layout` (see compute_column_layout()) so they match whatever
 * rebuild_header_row() built for this same refresh.
 *
 * Row height is LV_SIZE_CONTENT, not a fixed pixel value -- EVENT now word-
 * wraps (see add_label() below) instead of ellipsizing, so the row needs
 * to grow to fit however many lines that takes rather than clip/overflow
 * a fixed height. impact_bar's own fixed 40px height sets a sensible
 * minimum for a single-line row (LVGL sizes a content-fit flex container
 * to its tallest child); a row wrapping to 3+ lines simply grows taller
 * than that, with impact_bar staying centered and its original size.
 */
lv_obj_t * make_event_row(lv_obj_t * parent, const CalendarEvent & event, const ColumnLayout & layout)
{
    lv_obj_t * row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(THEME_COLOR_BACKGROUND), 0);
    // Lets update_next_event_row() find this row again by event id later --
    // day separator labels (make_day_separator_row()) are never tagged, so
    // their default user_data of nullptr/0 doubles as "not an event row"
    // (real Forex Factory ids and injected test event ids are never 0).
    lv_obj_set_user_data(row, reinterpret_cast<void *>(static_cast<intptr_t>(event.id)));
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_set_style_pad_left(row, 6, 0);
    lv_obj_set_style_pad_right(row, 6, 0);
    lv_obj_set_style_pad_top(row, 6, 0);
    lv_obj_set_style_pad_bottom(row, 6, 0);

    lv_obj_t * impact_bar = lv_obj_create(row);
    lv_obj_remove_style_all(impact_bar);
    lv_obj_set_size(impact_bar, col_impact_w, 40);
    lv_obj_set_style_bg_color(impact_bar, lv_color_hex(impact_color_for(event.impact_level)), 0);
    lv_obj_set_style_bg_opa(impact_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(impact_bar, 2, 0);

    // LVGL's built-in unscii_8/_16 (see fonts.h) turned out to be an
    // all-or-nothing choice for this dense a grid: _16's 16px/char advance
    // blew through these fixed columns and wrapped rows (reported directly
    // from hardware as "wraps the cells"), but _8's 8px line height was
    // reported right back as too small to read comfortably. spacemono_18
    // (see fonts.h) is a genuinely scalable font, so it splits the
    // difference properly instead of picking one of two fixed extremes.
    // The font itself went through two picks: an initial custom VT323
    // conversion (bumped once, 22px then 26px, after hardware feedback it
    // could read bigger) was dropped for Space Mono after a direct
    // side-by-side comparison (see fonts.h) -- VT323's 0/2 digits read as
    // too similar.
    auto add_label = [row](const String & text, int16_t width, bool grow, uint32_t color,
                            bool right_align = false) {
        lv_obj_t * label = lv_label_create(row);
        lv_label_set_text(label, text.c_str());
        lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
        lv_obj_set_style_text_font(label, &lv_font_spacemono_18, 0);
        if (grow) {
            lv_obj_set_flex_grow(label, 1);
            // Word-wrap, not ellipsis: reported directly as the preferred
            // default (Day tab already did this naturally; Week's longer
            // list was ellipsizing instead) -- LV_LABEL_LONG_DOT was
            // originally added as the fix for a real row-height-blowout
            // bug (a fixed-height row plus unbounded wrap), but the row is
            // no longer fixed-height (see this function's own doc comment
            // above) specifically so wrap is safe to turn back on.
            lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        } else {
            lv_obj_set_width(label, width);
            // Fixed-width columns (TIME/CCY/ACT/FCST/PREV) still ellipsize
            // rather than wrap -- these are short, single-value cells, not
            // free text, so multi-line wrapping would look wrong even
            // though the row can now grow to fit it. ACT/FCST/PREV are
            // sized from actual content each refresh (see
            // compute_column_layout()), so this is now genuinely a
            // fallback for a still-unusually-wide value, not routine.
            lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        }
        if (right_align) {
            lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, 0);
        }
    };

    // "*" flags a previous value Forex Factory revised from what was
    // originally reported last time -- independent of whether the revision
    // itself reads as better/worse (a plain "revised" wrapper span, with
    // neither class, is still a revision, just not one worth coloring).
    const String previous_text = event.previous_revised ? event.previous + "*" : event.previous;

    add_label(event.time, col_time_w, false, THEME_COLOR_AMBER);
    if (layout.show_ccy) {
        add_label(event.currency, col_ccy_w, false, THEME_COLOR_AMBER_DIM);
    }
    add_label(event.name, 0, true, THEME_COLOR_AMBER);
    // ACT/FCST right-justified too, matching PREV -- reported directly:
    // left/center-aligned values under a right-aligned PREV read as
    // inconsistent/misaligned against the header labels above them.
    add_label(event.actual, layout.act_w, false, value_color_for(event.actual_state), true);
    add_label(event.forecast, layout.fcst_w, false, THEME_COLOR_AMBER_DIM, true);
    // PREV right-justified: it's the last column, flush against the
    // grid's own right edge (see build_events_ui()'s grid_width comment) --
    // reported directly as wanted, and reads more like a clean table
    // column of numbers than left-aligned text would in that spot.
    add_label(previous_text, layout.prev_w, false, value_color_for(event.previous_state), true);

    return row;
}

/**
 * Column header labels, using the same widths/CCY visibility as
 * make_event_row() gets via the same `layout` (see compute_column_layout()).
 * Deletes and recreates `header` from scratch every call rather than
 * resizing labels in place -- this needs to change shape (CCY appearing/
 * disappearing, not just resizing) on filter changes, and header is cheap
 * (7 objects) next to the up-to-100+ row list that already gets fully
 * rebuilt every refresh the same way. Called once with fallback widths in
 * build_events_ui() (before the first fetch resolves), then again from
 * populate_events() every refresh with real measurements.
 */
void rebuild_header_row(lv_obj_t * parent, const ColumnLayout & layout)
{
    if (header != nullptr) {
        lv_obj_delete(header);
    }
    header = lv_obj_create(parent);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, grid_width, 24);
    lv_obj_align(header, LV_ALIGN_TOP_LEFT, 20, 108);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(header, 6, 0);
    lv_obj_set_style_pad_left(header, col_impact_w + 6, 0);

    auto add_header_label = [](const char * text, int16_t width, bool grow, bool right_align = false) {
        lv_obj_t * label = lv_label_create(header);
        lv_label_set_text(label, text);
        lv_obj_set_style_text_color(label, lv_color_hex(THEME_COLOR_AMBER_DIM), 0);
        // Matches add_label()'s font -- see its comment for why spacemono_18.
        lv_obj_set_style_text_font(label, &lv_font_spacemono_18, 0);
        if (grow) {
            lv_obj_set_flex_grow(label, 1);
        } else {
            lv_obj_set_width(label, width);
        }
        if (right_align) {
            lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, 0);
        }
    };

    add_header_label("TIME", col_time_w, false);
    if (layout.show_ccy) {
        add_header_label("CCY", col_ccy_w, false);
    }
    add_header_label("EVENT", 0, true);
    // ACT/FCST/PREV all right-aligned, matching their data columns below.
    add_header_label("ACT", layout.act_w, false, true);
    add_header_label("FCST", layout.fcst_w, false, true);
    add_header_label("PREV", layout.prev_w, false, true);
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
    // spacemono_18, not the button/title-sized spacemono_20 -- this label has no
    // explicit width set (auto-sized to content), and its longest line at
    // a wider advance width would run close to the screen's full 800px
    // width with no wrap safety net.
    lv_obj_set_style_text_font(no_server_message, &lv_font_spacemono_18, 0);
    lv_obj_set_style_text_align(no_server_message, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(no_server_message, LV_ALIGN_CENTER, 0, 20);
}

/**
 * Tron/Hackers-style neon grid scene, shown instead of the (empty) event
 * list on a range with genuinely no scheduled events -- previously just a
 * plain "No events match your current filters for this range." text line,
 * which read identically whether the filters were excluding everything or
 * the range was just quiet. Built once here, sized/positioned to exactly
 * match `list`'s own bounds (grid_width x 300 at (20, 136)) since
 * grid_width itself is only computed once, right after boot -- toggled
 * hidden/visible by populate_events() rather than rebuilt per refresh.
 *
 * A perspective floor grid (vertical lines converging on a vanishing point
 * at the horizon, horizontal rungs spaced with a quadratic ease so evenly-
 * drawn straight lines read as receding into the distance) plus a horizon
 * bar and a glowing headline, all in THEME_COLOR_SPLASH_TERMINAL_GREEN --
 * the same neon-on-black palette main.cpp's boot splash already
 * established for this device, rather than inventing a new one-off scheme.
 */
void build_no_events_scene(lv_obj_t * screen)
{
    no_events_scene = lv_obj_create(screen);
    lv_obj_remove_style_all(no_events_scene);
    lv_obj_set_size(no_events_scene, grid_width, 300);
    lv_obj_align(no_events_scene, LV_ALIGN_TOP_LEFT, 20, 136);
    lv_obj_add_flag(no_events_scene, LV_OBJ_FLAG_HIDDEN);

    constexpr int16_t horizon_y = 150;
    constexpr int16_t floor_bottom_y = 280;
    constexpr int16_t floor_margin_x = 40;
    const int16_t vanishing_x = grid_width / 2;
    const int16_t floor_left_x = floor_margin_x;
    const int16_t floor_right_x = grid_width - floor_margin_x;

    int line_index = 0;
    auto make_line = [&](lv_point_precise_t a, lv_point_precise_t b, int16_t width) {
        no_events_grid_points[line_index][0] = a;
        no_events_grid_points[line_index][1] = b;
        lv_obj_t * line = lv_line_create(no_events_scene);
        lv_line_set_points(line, no_events_grid_points[line_index], 2);
        lv_obj_set_style_line_color(line, lv_color_hex(THEME_COLOR_SPLASH_TERMINAL_GREEN), 0);
        lv_obj_set_style_line_width(line, width, 0);
        lv_obj_set_style_line_opa(line, LV_OPA_COVER, 0);
        ++line_index;
    };

    // Horizon bar first -- full width, so it's drawn under the converging
    // verticals below rather than over them.
    make_line({0, horizon_y}, {static_cast<lv_value_precise_t>(grid_width), horizon_y}, 2);

    // Converging verticals: vanishing point -> evenly-spaced points along
    // the floor's bottom edge.
    for (int i = 0; i < no_events_vertical_lines; ++i) {
        const float t = static_cast<float>(i) / (no_events_vertical_lines - 1);
        const int16_t bottom_x = static_cast<int16_t>(floor_left_x + t * (floor_right_x - floor_left_x));
        make_line({vanishing_x, horizon_y}, {bottom_x, floor_bottom_y}, 1);
    }

    // Horizontal rungs -- quadratic easing (t*t) packs more lines near the
    // horizon and fewer near the bottom, the standard trick for faking
    // perspective with straight, evenly-drawn lines. x bounds at each rung
    // are interpolated toward the vanishing point the same fraction t,
    // keeping every rung's endpoints exactly on the two outer converging
    // lines instead of just guessing a width.
    for (int i = 1; i <= no_events_horizontal_lines; ++i) {
        const float t = static_cast<float>(i) / no_events_horizontal_lines;
        const float eased = t * t;
        const int16_t y = static_cast<int16_t>(horizon_y + eased * (floor_bottom_y - horizon_y));
        const int16_t left_x = static_cast<int16_t>(vanishing_x + (floor_left_x - vanishing_x) * t);
        const int16_t right_x = static_cast<int16_t>(vanishing_x + (floor_right_x - vanishing_x) * t);
        make_line({left_x, y}, {right_x, y}, 1);
    }

    // Faux-bold headline -- same 1px-offset-duplicate technique main.cpp's
    // boot splash uses (LVGL's bitmap fonts have no bold variant).
    lv_obj_t * headline_shadow = lv_label_create(no_events_scene);
    lv_label_set_text(headline_shadow, "NO SCHEDULED EVENTS");
    lv_obj_set_style_text_color(headline_shadow, lv_color_hex(THEME_COLOR_SPLASH_TERMINAL_GREEN), 0);
    lv_obj_set_style_text_font(headline_shadow, &lv_font_spacemono_32, 0);
    lv_obj_align(headline_shadow, LV_ALIGN_TOP_MID, 1, 40);

    lv_obj_t * headline = lv_label_create(no_events_scene);
    lv_label_set_text(headline, "NO SCHEDULED EVENTS");
    lv_obj_set_style_text_color(headline, lv_color_hex(THEME_COLOR_SPLASH_TERMINAL_GREEN), 0);
    lv_obj_set_style_text_font(headline, &lv_font_spacemono_32, 0);
    lv_obj_align(headline, LV_ALIGN_TOP_MID, 0, 40);
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

    set_active_tab(day_tab_button, week_tab_button);

    // Grid width computed from the WiFi icon's actual rendered position,
    // not a guessed pixel constant -- two rounds of hand-picked widths (760,
    // then 780) both landed short of lining PREV up under the WiFi icon on
    // real hardware, since the icon's true on-screen edge depends on font
    // metrics that are easy to get wrong by hand. wifi_icon_label is
    // created and aligned earlier in calendar_view_create(), before this
    // function runs -- lv_obj_update_layout() forces its position to
    // resolve immediately (alignment is otherwise lazily applied on the
    // next layout pass, not synchronously) so lv_obj_get_coords() below
    // reflects where it will actually render, not stale coordinates.
    lv_obj_update_layout(wifi_icon_label);
    lv_area_t wifi_area;
    lv_obj_get_coords(wifi_icon_label, &wifi_area);
    constexpr int16_t grid_x = 20;
    grid_width = static_cast<int16_t>(wifi_area.x2 - grid_x + 1);

    // Fallback ColumnLayout (show_ccy=true, the *_fallback widths) -- real
    // values come from compute_column_layout() once populate_events() has
    // actual data (and the current currency filter) to measure against.
    rebuild_header_row(screen, ColumnLayout{});

    list = lv_obj_create(screen);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, grid_width, 300);
    lv_obj_align(list, LV_ALIGN_TOP_LEFT, grid_x, 136);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 4, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    theme_style_scrollbar(list);

    status_label = lv_label_create(screen);
    lv_label_set_text(status_label, "");
    lv_obj_set_style_text_color(status_label, lv_color_hex(THEME_COLOR_AMBER_DIM), 0);
    // spacemono_18, same reasoning as no_server_message above.
    lv_obj_set_style_text_font(status_label, &lv_font_spacemono_18, 0);
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 220);

    build_no_events_scene(screen);
}

/**
 * Deletes every child of `list` one at a time, yielding to
 * lv_timer_handler() after each one, instead of a single lv_obj_clean()
 * call. A week view can leave 50-100+ rows (8 LVGL objects each) behind
 * from the previous fetch -- lv_obj_clean() would destroy all of them in
 * one uninterrupted burst with no chance for the display pipeline to run
 * in between, same hazard as the creation loop below.
 */
void clear_list_yielding()
{
    while (lv_obj_get_child_count(list) > 0) {
        lv_obj_delete(lv_obj_get_child(list, lv_obj_get_child_count(list) - 1));
        timed_timer_handler("clear_list_yielding");
    }
}

void populate_events(const std::vector<CalendarEvent> & events, bool show_ccy)
{
    clear_list_yielding();

    // Rebuilt every refresh, not just resized -- widths need to match
    // whatever's actually in `events` this time (see
    // compute_column_layout()), and CCY can appear/disappear entirely
    // depending on the currency filter, not just change size.
    const ColumnLayout layout = compute_column_layout(events, show_ccy);
    rebuild_header_row(calendar_screen_ref, layout);
    // rebuild_header_row() deletes the old header and creates up to 7 new
    // objects (the header container + TIME/CCY/EVENT/ACT/FCST/PREV labels)
    // with no yield of its own -- without this call, that whole backlog
    // silently bundles into whichever lv_timer_handler() call happens
    // next (the first row below), inflating just that one call. Confirmed
    // directly on hardware: the first "populate_events row" [stall] after
    // this was added logged noticeably higher (114ms) than every
    // subsequent one (68ms) on the same refresh -- this is what actually
    // fixes that, not a fixed threshold or a bigger buffer.
    timed_timer_handler("populate_events header rebuild");

    if (events.empty()) {
        lv_obj_add_flag(list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(no_events_scene, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(status_label, "");
        return;
    }

    lv_obj_add_flag(no_events_scene, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(status_label, "");
    String last_day;
    for (const CalendarEvent & event : events) {
        if (event.day.length() > 0 && event.day != last_day) {
            make_day_separator_row(list, event.day);
            last_day = event.day;
        }
        make_event_row(list, event, layout);

        // A week view can be 50-100+ events -- each row is 8 LVGL objects
        // (the row + an impact bar + 6 labels), so building the whole list
        // in one uninterrupted burst is hundreds of object
        // creations/layouts with no call back to lv_timer_handler() in
        // between. Confirmed directly on hardware: that's long enough to
        // starve the RGB panel's bounce-buffer refill and cause a visible
        // "frame shift" (content wrapping toward the bottom of the
        // screen) -- notably reproducible with I2S audio removed entirely
        // from this project, ruling out I2S contention as the cause of
        // this specific symptom (a separate, now-abandoned line of
        // investigation lives in this file's git history and the
        // Milestone 8 section of this README).
        //
        // Yielding every 8 rows wasn't tight enough margin -- still
        // reproduced on the Week tab specifically (the tab with enough
        // events for that gap to matter; Day's much shorter list never
        // triggered it). Yielding after every single row costs more calls
        // to lv_timer_handler() but each one is cheap, and this is only
        // ever running during an explicit refresh, not every frame.
        timed_timer_handler("populate_events row");
    }
}

/**
 * Synchronous, deliberately -- 2026-08, after an extensive investigation
 * (see the README's own writeup) into intermittent fetch failures that
 * were eventually isolated to a specific cause: calls made from a
 * separately-created FreeRTOS task (this used to run on one, to keep the
 * screen responsive during up-to-65s Cloudflare-challenge-era fetches)
 * were failing to reach the server -- confirmed via calendar_api.py's own
 * access log showing zero record of them -- while the exact same calls,
 * issued directly from this task (the main/LVGL one, same as loop()'s own)
 * succeeded every single time, consistently, across many captures. Pinning
 * that background task to the same core this one runs on (ARDUINO_RUNNING_CORE)
 * didn't fix it either -- confirmed directly, ruling out core affinity as
 * the deciding factor. Whatever the actual mechanism, "issued from the
 * original loopTask" is the only configuration this project has ever
 * directly confirmed reliably works, so that's what this does now, rather
 * than continuing to debug a background-task design whose entire reason
 * for existing (surviving a 65s upstream fetch) stopped applying once
 * calendar_api.py itself was restructured to answer from a fast local
 * cache. This does mean a fetch briefly freezes the screen (no redraws,
 * no clock, no alerts) for however long it takes -- consistently under
 * 150ms in every successful capture so far, and this only runs once per
 * poll_interval_ms (10 minutes) or on an explicit user/event action, not
 * continuously, so that's a real but small and infrequent cost, not the
 * multi-second-to-65s stalls the original background-task design was
 * built to survive.
 */
void refresh_events()
{
    lv_label_set_text(status_label, "Loading events...");
    timed_timer_handler("refresh_events loading label");

    Serial.printf("[heap] free=%u largest_free_block=%u before refresh_events fetch, t=%lums\n",
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap(), static_cast<unsigned long>(millis()));

    CalendarFilters filters;
    const bool show_ccy = !(calendar_client_get_filters(filters) && filters.currency_codes.size() == 1);

    std::vector<CalendarEvent> events;
    bool refresh_ok = true;
    bool fomc_this_week = false;
    const bool success = calendar_client_get_calendar(current_range, events, refresh_ok, fomc_this_week);

    if (success) {
        populate_events(events, show_ccy);
        // Only touched on success -- a transient fetch failure shouldn't
        // flicker this off and back on by the next retry a few seconds
        // later. fomc_this_week defaults to false on a failed call (see
        // calendar_client_get_calendar()), so leaving it alone here means
        // the badge simply keeps showing whatever it last knew, same as
        // current_events would if this project chose to do the same for
        // it (it doesn't, deliberately -- see current_events' own comment
        // below; this is a much lower-stakes, weekly-timescale indicator,
        // not something alert_manager acts on).
        if (fomc_this_week) {
            lv_obj_clear_flag(fomc_badge, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(fomc_badge, LV_OBJ_FLAG_HIDDEN);
        }
        // Overwrites whatever populate_events() just set -- a request that
        // succeeded but reflects calendar_api.py's own failing background
        // refresh (FlareSolverr down, Forex Factory unreachable) is more
        // important to surface than that, since it means everything on
        // screen right now, empty list or not, may not actually be
        // current. See calendar_client_get_calendar()'s own doc comment on
        // refresh_ok.
        if (!refresh_ok) {
            lv_label_set_text(status_label, "Warning: calendar server can't refresh data -- showing last known values");
        }
    } else {
        // Also hides no_events_scene/shows list -- a fetch can fail right
        // after a previous refresh left the no-events grid scene visible
        // (an empty range, then a dropped connection before the next
        // refresh), and this error message needs the plain list area back,
        // not that scene still sitting on top of it.
        lv_obj_add_flag(no_events_scene, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clean(list);
        lv_label_set_text(status_label, "Could not load events -- check the connection and try again.");
    }

    // Unconditional, including on failure/empty (an empty vector clears any
    // previously tracked events) -- alert_manager shouldn't keep counting
    // down to or refreshing for an event that no longer matches current
    // filters or came from a now-stale fetch. current_events mirrors this
    // for the same reason: update_next_event_row() needs it to restore a
    // row's plain time text, and a stale entry there is just as wrong.
    current_events = events;
    alert_manager_set_events(events);

    // The list was just rebuilt from scratch, so even if the same event(s)
    // are still tracked as active after this refresh, their border
    // highlights wouldn't survive on the new row objects -- clear the
    // tracked set so the next tick re-applies highlights fresh instead of
    // assuming any from before the rebuild are still there.
    highlighted_rows.clear();

    last_refresh_ok = success;
    if (!success) {
        // Retry sooner than the full poll_interval_ms rather than leaving
        // "Could not load events" on screen for up to 10 minutes.
        // calendar_view_poll() (called every loop() iteration) is what
        // actually fires it.
        retry_scheduled = true;
        retry_scheduled_at_ms = millis();
    }
}

/** Finds the event row tagged with `id` (see make_event_row()), or nullptr. */
lv_obj_t * find_row_by_id(long id)
{
    const uint32_t child_count = lv_obj_get_child_count(list);
    for (uint32_t i = 0; i < child_count; ++i) {
        lv_obj_t * child = lv_obj_get_child(list, i);
        if (static_cast<long>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(child))) == id) {
            return child;
        }
    }
    return nullptr;
}

// child(row, 1) is always the TIME label, whether or not CCY is currently
// shown -- make_event_row() adds impact_bar(0), time(1), then CCY only if
// layout.show_ccy, so TIME's own index never shifts regardless of that.
// Relied on here and in update_next_event_row() below.

/** Restores `row`'s TIME label to its plain scheduled time and clears its highlight border. */
void unhighlight_row(lv_obj_t * row, long id)
{
    lv_obj_set_style_border_width(row, 0, 0);
    for (const CalendarEvent & event : current_events) {
        if (event.id == id) {
            lv_label_set_text(lv_obj_get_child(row, 1), event.time.c_str());
            break;
        }
    }
}

/**
 * Puts the countdown directly on the row(s) it refers to, instead of a
 * separate banner: every row alert_manager currently tracks as active
 * (alert_manager_get_active_event_ids() -- every event within the
 * 10-minute window, not just the single soonest, since simultaneous
 * releases are routine) gets its TIME column swapped for a live "MM:SS"
 * countdown plus a border highlight, and reverts to its plain scheduled
 * time once it stops being active (event fires, drops out of the window,
 * or gets filtered out of a later refresh).
 *
 * Only touches the row(s) that actually changed state -- the common case
 * (the same set of events still counting down) is just a label text
 * update on each already-highlighted row, not a rescan of the whole list
 * every second.
 */
void update_next_event_row()
{
    const std::vector<long> active_ids = alert_manager_get_active_event_ids();

    // Drop the highlight from any row that was active last tick but isn't anymore.
    for (size_t i = 0; i < highlighted_rows.size();) {
        if (contains_id(active_ids, highlighted_rows[i].id)) {
            ++i;
            continue;
        }
        lv_obj_t * row = find_row_by_id(highlighted_rows[i].id);
        if (row != nullptr) {
            unhighlight_row(row, highlighted_rows[i].id);
        }
        highlighted_rows.erase(highlighted_rows.begin() + static_cast<long>(i));
    }

    // Apply/refresh every currently-active row.
    for (long id : active_ids) {
        lv_obj_t * row = find_row_by_id(id);
        if (row == nullptr) {
            continue;
        }

        HighlightedRow * tracked = nullptr;
        for (HighlightedRow & candidate : highlighted_rows) {
            if (candidate.id == id) {
                tracked = &candidate;
                break;
            }
        }

        const String countdown = alert_manager_get_countdown_text_for(id);
        if (tracked == nullptr) {
            lv_obj_set_style_border_color(row, lv_color_hex(THEME_COLOR_IMPACT_MEDIUM), 0);
            lv_obj_set_style_border_width(row, 2, 0);
            lv_obj_set_style_border_opa(row, LV_OPA_COVER, 0);
            lv_label_set_text(lv_obj_get_child(row, 1), countdown.c_str());
            highlighted_rows.push_back({id, countdown});
        } else if (countdown != tracked->last_shown_countdown_text) {
            lv_label_set_text(lv_obj_get_child(row, 1), countdown.c_str());
            tracked->last_shown_countdown_text = countdown;
        }
    }
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
    // spacemono_32 (see fonts.h) -- originally unscii_16, then bumped to
    // the button-tier spacemono_20, both reported back as still not
    // reading like a size change next to the WiFi/gear icons sharing this
    // top strip. spacemono_32 is a dedicated size for just this title, not
    // reused anywhere else -- unlike the icons, which anchor to the
    // top-right corner, the title can't just move, so it had to actually
    // get bigger rather than share a tier with something else. Not an
    // app-wide title bump -- this is the only title sharing a row with
    // icons in the first place.
    lv_obj_set_style_text_font(title, &lv_font_spacemono_32, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 20, 10);

    // Hidden unless calendar_api.py's own "fomc_this_week" flag says
    // otherwise (see refresh_events()) -- reported directly (2026-08): the
    // week view goes mostly unread day to day, so an FOMC week was easy to
    // miss until it was already showing up on the day view. Anchored to
    // title itself via lv_obj_align_to() rather than a fixed x offset --
    // the gap this sits in is whatever's left between the title's own
    // (content-sized) width and the gear/WiFi icons further right, not a
    // fixed layout slot, so it needs to track the title's actual rendered
    // width rather than assume one.
    fomc_badge = lv_label_create(screen);
    lv_label_set_text(fomc_badge, "FOMC WEEK");
    lv_obj_set_style_text_color(fomc_badge, lv_color_hex(THEME_COLOR_IMPACT_HIGH), 0);
    lv_obj_set_style_text_font(fomc_badge, &lv_font_spacemono_18, 0);
    lv_obj_set_style_border_color(fomc_badge, lv_color_hex(THEME_COLOR_IMPACT_HIGH), 0);
    lv_obj_set_style_border_width(fomc_badge, 2, 0);
    lv_obj_set_style_border_opa(fomc_badge, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(fomc_badge, 3, 0);
    lv_obj_set_style_pad_left(fomc_badge, 10, 0);
    lv_obj_set_style_pad_right(fomc_badge, 10, 0);
    lv_obj_set_style_pad_top(fomc_badge, 4, 0);
    lv_obj_set_style_pad_bottom(fomc_badge, 4, 0);
    lv_obj_add_flag(fomc_badge, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(title); // same reasoning as wifi_icon_label's own update_layout() call below -- align_to needs title's real rendered width, not a stale/unresolved one.
    lv_obj_align_to(fomc_badge, title, LV_ALIGN_OUT_RIGHT_MID, 16, 0);

    // WiFi status icon takes the corner spot the gear used to occupy; the
    // gear moves left to make room. Same color/strike convention as
    // main.cpp's boot-splash icon (dim/green/red+strike), driven by the
    // same WiFi.onEvent() callback via calendar_view_set_wifi_icon() -- the
    // boot splash only exists briefly at startup, so without a second icon
    // here a WiFi drop later has nowhere on-screen to show up.
    wifi_icon_label = lv_label_create(screen);
    lv_label_set_text(wifi_icon_label, LV_SYMBOL_WIFI);
    // Montserrat, not the retro unscii font -- unscii has no icon/symbol
    // glyphs, and this label's text is LV_SYMBOL_WIFI, not a word.
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
    // theme_create_button() defaults its label to the retro spacemono_20 font,
    // which has no icon/symbol glyphs -- this label's text is actually
    // LV_SYMBOL_SETTINGS, so it needs a font that bundles LVGL's symbol
    // range back.
    lv_obj_set_style_text_font(lv_obj_get_child(settings_button, 0), &lv_font_montserrat_20, 0);

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
    if (list == nullptr) {
        return; // no server configured yet, or "no server" message still showing
    }
    update_next_event_row();
}

long calendar_view_get_event_id_at(size_t index)
{
    return index < current_events.size() ? current_events[index].id : 0;
}

/**
 * Rate-limits *checking* -- including the no-server-configured case where
 * refresh_events() never actually runs -- otherwise that case would
 * re-check calendar_server_url_load() on every single loop() iteration
 * once past the first interval, forever, instead of once per interval
 * like everything else. A retry_scheduled retry always takes priority
 * over (and effectively resets the clock on) the ordinary periodic
 * check -- see last_poll_ms's own comment for why both get updated by
 * every refresh, not just periodic ones.
 */
void calendar_view_poll()
{
    if (retry_scheduled) {
        if (millis() - retry_scheduled_at_ms >= refresh_retry_delay_ms) {
            retry_scheduled = false;
            last_poll_ms = millis();
            calendar_view_refresh();
        }
        return;
    }

    const uint32_t now = millis();
    if (now - last_poll_ms >= poll_interval_ms) {
        last_poll_ms = now;
        calendar_view_refresh();
    }
}
