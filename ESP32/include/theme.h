#pragma once

#include <lvgl.h>

/*---------------------------------------------------------------
 * Shared dark/amber color palette for the retro terminal look.
 * Pass these to lv_color_hex() at the call site.
 *--------------------------------------------------------------*/

// Background lifted slightly from pure near-black (was 0x0D0D0D) -- gives
// the amber a touch more to sit against without breaking the dark-CRT feel.
// AMBER_DIM brightened substantially (was 0x8A5E00): it drives most of a
// row's content (currency, actual/forecast/previous, headers), and while
// its contrast ratio against the old background was technically fine on
// paper, warm dark hues read as muddy/brown in practice rather than
// legible -- especially on a cheaper TFT panel with reduced black levels.
#define THEME_COLOR_BACKGROUND 0x141414
#define THEME_COLOR_AMBER 0xFFB000
#define THEME_COLOR_AMBER_DIM 0xC98A00

// Event impact-level accents, layered on top of the amber/dark base.
#define THEME_COLOR_IMPACT_HIGH 0xFF3B30
#define THEME_COLOR_IMPACT_MEDIUM 0xFF9500
#define THEME_COLOR_IMPACT_LOW 0x8A8A5C

// Actual/previous value coloring (CalendarEvent.actual_state/previous_state)
// -- matches forexfactory.com's own stylesheet exactly (`.better{color:#090}`,
// `.worse{color:#c00}`), not a retro-palette color, so a value reads the same
// "good"/"bad" as it would on the website itself.
#define THEME_COLOR_VALUE_BETTER 0x009900
#define THEME_COLOR_VALUE_WORSE 0xCC0000

// Boot splash only: matches the neon-green terminal text baked into
// assets/terminal-800x480.png, sampled directly from the image so
// overlaid live text (boot log, WiFi status) blends in rather than
// clashing with the amber palette used elsewhere.
#define THEME_COLOR_SPLASH_TERMINAL_GREEN 0x04FFAE
// WiFi icon: dim while connecting/idle, green once connected, red on failure.
#define THEME_COLOR_SPLASH_DIM 0x555566

// Slightly lighter than THEME_COLOR_BACKGROUND so bordered buttons read as
// raised/tappable against the screen behind them.
#define THEME_COLOR_BUTTON_BG 0x1E1A10

/**
 * A button styled to fit the dark/amber theme -- dark fill, amber border
 * and text, no drop shadow -- instead of LVGL's default theme, which
 * renders plain lv_button_create() as a glossy blue button that clashes
 * hard with a retro terminal look. Every button in this project should be
 * created through this rather than lv_button_create() directly.
 *
 * @param user_data Passed through to the click event's user data (nullptr if unused).
 */
lv_obj_t * theme_create_button(lv_obj_t * parent, const char * text, lv_event_cb_t callback,
                               void * user_data = nullptr);

/**
 * A checkbox styled to fit the dark/amber theme. LVGL's default theme draws
 * checkbox text and the indicator box's border/fill in colors meant for a
 * light background -- against THEME_COLOR_BACKGROUND that text is
 * essentially invisible. Every checkbox in this project should be created
 * through this rather than lv_checkbox_create() directly.
 */
lv_obj_t * theme_create_checkbox(lv_obj_t * parent, const char * text);

/**
 * Apply dark/amber styling (fill, border, text, placeholder, cursor) to an
 * already-created textarea. Call right after lv_textarea_create().
 */
void theme_style_textarea(lv_obj_t * textarea);

/**
 * Apply dark/amber styling to an already-created dropdown, including its
 * (eagerly-created, just hidden) options list -- both otherwise render with
 * LVGL's default light-theme colors. Call right after lv_dropdown_create().
 */
void theme_style_dropdown(lv_obj_t * dropdown);

/**
 * Apply dark/amber styling to an already-created lv_list and its buttons.
 * LVGL's default theme renders both as solid white/light cards. Call once
 * on the list right after lv_list_create(), and once per button right after
 * each lv_list_add_button() call.
 */
void theme_style_list(lv_obj_t * list);
void theme_style_list_button(lv_obj_t * button);

/**
 * Recolor an already-scrollable object's scrollbar to fit the dark/amber
 * theme -- LVGL's default is a plain grey pill. Call right after creating
 * any container that might need to scroll (checkbox wrap lists, the
 * calendar event list, etc).
 */
void theme_style_scrollbar(lv_obj_t * obj);

/**
 * Style a plain lv_obj_create() used as a checkbox wrap-list (currencies,
 * columns, etc.): dark fill, no border, themed scrollbar. LVGL's default
 * theme applies its "card" style to any unstyled generic object -- bg_opa
 * COVER + a border color/width neither of these screens had explicitly
 * overridden before, so the default theme's border color showed through.
 * Call right after lv_obj_create().
 */
void theme_style_checkbox_wrap(lv_obj_t * obj);
