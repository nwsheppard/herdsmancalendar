#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include <lvgl.h>

#include "alert_manager.h"
#include "boot_splash_image.h"
#include "calendar_client.h"
#include "calendar_server_store.h"
#include "calendar_view.h"
#include "fonts.h"
#include "theme.h"
#include "touch.h"
#include "wifi_credentials_store.h"
#include "wifi_manager.h"
#include "wifi_setup_screen.h"

/*---------------------------------------------------------------
 * Milestone 1: LVGL "hello world" on real hardware (display + touch).
 * Milestone 2: WiFi connect + HTTP GET against the LXC calendar API.
 *
 * Display bring-up (RGB timings, pin mapping) is copied from Elecrow's own
 * verified working example for this exact board
 * (Elecrow-RD/CrowPanel-5.0-HMI-ESP32-Display-800x480,
 * example/V3.0/PlatformIO50) rather than derived from datasheets, since
 * this is the highest-risk, hardest-to-debug-remotely part.
 *--------------------------------------------------------------*/
namespace {
constexpr uint16_t screen_width = 800;
constexpr uint16_t screen_height = 480;
constexpr uint8_t backlight_pin = 2;

class Display : public lgfx::LGFX_Device {
public:
    lgfx::Bus_RGB bus;
    lgfx::Panel_RGB panel;

    /** Configure RGB timing, data pins, and the 800x480 panel. */
    Display()
    {
        auto bus_config = bus.config();
        bus_config.panel = &panel;
        const int8_t data_pins[16] = {8, 3, 46, 9, 1, 5, 6, 7, 15, 16, 4, 45, 48, 47, 21, 14};
        memcpy(bus_config.pin_data, data_pins, sizeof(data_pins));
        bus_config.pin_henable = 40;
        bus_config.pin_vsync = 41;
        bus_config.pin_hsync = 39;
        bus_config.pin_pclk = 0;
        bus_config.freq_write = 12000000;
        bus_config.hsync_polarity = 0;
        bus_config.hsync_front_porch = 8;
        bus_config.hsync_pulse_width = 4;
        bus_config.hsync_back_porch = 43;
        bus_config.vsync_polarity = 0;
        bus_config.vsync_front_porch = 8;
        bus_config.vsync_pulse_width = 4;
        bus_config.vsync_back_porch = 12;
        bus_config.pclk_active_neg = 1;
        bus_config.de_idle_high = 0;
        bus_config.pclk_idle_high = 0;
        bus.config(bus_config);

        auto panel_config = panel.config();
        panel_config.memory_width = screen_width;
        panel_config.memory_height = screen_height;
        panel_config.panel_width = screen_width;
        panel_config.panel_height = screen_height;
        panel.config(panel_config);
        panel.setBus(&bus);
        setPanel(&panel);
    }
};

Display lcd;
lv_obj_t * main_screen = nullptr;
lv_obj_t * boot_log_label = nullptr;
lv_obj_t * boot_log_label_shadow = nullptr;
lv_obj_t * wifi_icon_label = nullptr;
lv_obj_t * wifi_icon_strike = nullptr;

// WiFi state changes (including post-boot drops/reconnects) arrive via
// WiFi.onEvent(), which runs on the WiFi/event task -- not safe to touch
// LVGL objects from there directly. The callback only records which state
// happened; loop() (same task LVGL was initialized on) applies it.
enum class WifiIconState { CONNECTING, CONNECTED, DISCONNECTED };
volatile WifiIconState pending_wifi_icon_state = WifiIconState::CONNECTING;
volatile bool wifi_icon_state_dirty = false;

// WiFi.setAutoReconnect(true) (see wifi_manager.cpp) is not reliable by
// itself on every arduino-esp32 core version/disconnect reason -- it's a
// known quirk that the AP coming back doesn't always trigger a retry on
// its own. loop() backstops it with an explicit periodic WiFi.reconnect()
// while disconnected, rather than trusting the built-in mechanism alone --
// see wifi_reconnect_if_down() (wifi_manager.cpp) for why that backstop's
// cooldown timer now lives there instead of a local variable here.
constexpr uint32_t wifi_reconnect_interval_ms = 10000;

// Stall/glitch diagnostics -- see loop()'s own comment. Same threshold as
// calendar_view.cpp's timed_timer_handler(): a couple of frames' worth
// (60fps = ~16.6ms each) is well past routine per-frame variance.
constexpr uint32_t stall_log_threshold_ms = 20;
uint32_t last_loop_end_ms = 0;

// Dev/test aid: buffers characters typed into the serial monitor until a
// newline, then checks for known commands. See handle_serial_command().
String serial_command_buffer;

// Terminal box interior in terminal_800x480.png, measured from the image
// (cyan border top/bottom rows), inset a few px from the border stroke.
constexpr int16_t box_text_x = 180;
constexpr int16_t box_text_y = 300;
constexpr int16_t box_text_width = 440;

// Fixed boot-sequence lines, updated in place as each subsystem resolves
// (mirrors the real setup() order) rather than an ever-growing scroll --
// the box only has room for a handful of lines.
String make_boot_line(const char * label, const char * status)
{
    String line = ">> ";
    line += label;
    while (line.length() < 28) {
        line += '.';
    }
    line += "[";
    line += status;
    line += "]";
    return line;
}

String boot_line_display = make_boot_line("DISPLAY", " OK ");
String boot_line_touch = make_boot_line("TOUCH (GT911)", "....");
String boot_line_wifi = make_boot_line("WIFI", "....");
String boot_line_api = make_boot_line("CALENDAR API", "....");

uint32_t lvgl_tick()
{
    return millis();
}

/** Present the full RGB frame buffer selected by LovyanGFX. */
void display_flush(lv_display_t * display, const lv_area_t *, uint8_t * pixel_map)
{
    if (!lcd.bus.presentFrameBuffer(pixel_map)) {
        Serial.println("Display frame switch timeout");
    }
    lv_display_flush_ready(display);
}

/** Supply the latest GT911 point and log each new press. */
void touch_read(lv_indev_t *, lv_indev_data_t * data)
{
    static int16_t last_x = 0;
    static int16_t last_y = 0;
    static bool was_pressed = false;
    const bool pressed = touch_read_point(last_x, last_y);
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->point.x = last_x;
    data->point.y = last_y;
    if (pressed && !was_pressed) {
        Serial.printf("Touch %d,%d\n", last_x, last_y);
    }
    was_pressed = pressed;
}

/** Join the fixed boot-sequence lines into the on-screen log label. */
void refresh_boot_log()
{
    if (boot_log_label == nullptr) {
        return;
    }
    const String text = boot_line_display + "\n" + boot_line_touch + "\n" +
                         boot_line_wifi + "\n" + boot_line_api;
    lv_label_set_text(boot_log_label, text.c_str());
    if (boot_log_label_shadow != nullptr) {
        lv_label_set_text(boot_log_label_shadow, text.c_str());
    }
}

/** Set the WiFi icon's color and strikethrough to reflect connection state. */
void set_wifi_icon_state(uint32_t color_hex, bool show_strike)
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

/**
 * Recorded by the WiFi event callback (see wifi_on_event below); applies
 * the icon/boot-log update for a state change that happened after boot,
 * e.g. the AP dropping or WiFi.setAutoReconnect() reconnecting. Updates
 * both the boot-splash icon (only actually visible during boot, but kept
 * in sync regardless) and the calendar screen's own icon -- the latter is
 * a no-op via calendar_view_set_wifi_icon()'s null-check until
 * calendar_view_create() has run.
 *
 * The two icons intentionally use different "connected"/"connecting"
 * colors, not the same pending state applied identically everywhere: the
 * boot splash's neon green/dim-grey match the terminal artwork's own
 * palette (THEME_COLOR_SPLASH_*), but that same green read as too bright
 * against the calendar screen's amber theme (reported directly from
 * hardware) -- so the calendar icon uses THEME_COLOR_AMBER/_AMBER_DIM
 * instead, matching the rest of that screen's text. Both keep the same
 * red "disconnected" color and strike -- that's a status signal, not
 * theme-driven, and should read the same everywhere.
 */
void apply_pending_wifi_icon_state()
{
    switch (pending_wifi_icon_state) {
    case WifiIconState::CONNECTED:
        set_wifi_icon_state(THEME_COLOR_SPLASH_TERMINAL_GREEN, false);
        calendar_view_set_wifi_icon(THEME_COLOR_AMBER, false);
        boot_line_wifi = make_boot_line("WIFI", WiFi.localIP().toString().c_str());
        break;
    case WifiIconState::DISCONNECTED:
        set_wifi_icon_state(THEME_COLOR_IMPACT_HIGH, true);
        calendar_view_set_wifi_icon(THEME_COLOR_IMPACT_HIGH, true);
        boot_line_wifi = make_boot_line("WIFI", "DOWN");
        break;
    case WifiIconState::CONNECTING:
        set_wifi_icon_state(THEME_COLOR_SPLASH_DIM, false);
        calendar_view_set_wifi_icon(THEME_COLOR_AMBER_DIM, false);
        break;
    }
    refresh_boot_log();
}

/**
 * WiFi.onEvent() callback -- runs on the WiFi/event task, so it must not
 * touch LVGL objects directly (see comment on pending_wifi_icon_state).
 *
 * Takes the `arduino_event_info_t` overload (previously just the bare
 * event id) specifically to log the disconnect reason/RSSI -- added after
 * a real drop-and-never-reconnects report with nothing in the log to
 * explain why. `WiFi.reconnect()` in loop()'s backstop retries blind
 * either way, but this at least tells us (auth failure vs. AP unreachable
 * vs. beacon timeout vs. something else) the next time it happens, rather
 * than just "still down" with no further information.
 */
void wifi_on_event(arduino_event_id_t event, arduino_event_info_t info)
{
    switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
        pending_wifi_icon_state = WifiIconState::CONNECTED;
        wifi_icon_state_dirty = true;
        break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
        pending_wifi_icon_state = WifiIconState::DISCONNECTED;
        wifi_icon_state_dirty = true;
        Serial.printf("WiFi disconnected: reason=%d rssi=%d\n", info.wifi_sta_disconnected.reason,
                      info.wifi_sta_disconnected.rssi);
        break;
    default:
        break;
    }
}

/**
 * Build the boot splash: the supplied retro artwork as a full-screen
 * background image, with live status text overlaid on top of it -- a boot
 * log inside the image's terminal box, "(c) 2026 Herdsman Corp."
 * bottom-left, and a WiFi icon bottom-right whose color reflects
 * connection state (dim -> green connected -> red failed).
 */
void build_splash_ui()
{
    lv_obj_t * screen = lv_screen_active();
    main_screen = screen;

    lv_obj_t * background = lv_image_create(screen);
    lv_image_set_src(background, &terminal_800x480);
    lv_obj_set_pos(background, 0, 0);

    // Faux-bold: LVGL's built-in bitmap fonts have no bold variant, so the
    // boot log is drawn twice with a 1px horizontal offset to thicken the
    // strokes and read denser inside the terminal box.
    // spacemono_18 (see fonts.h), not the bigger button/title spacemono_20 --
    // boot_line_* strings are hand-padded to 28 chars (make_boot_line())
    // to fit box_text_width=440px, and 28 * spacemono_20's wider advance width
    // wouldn't fit that box.
    boot_log_label_shadow = lv_label_create(screen);
    lv_obj_set_pos(boot_log_label_shadow, box_text_x + 1, box_text_y);
    lv_obj_set_width(boot_log_label_shadow, box_text_width);
    lv_obj_set_style_text_color(boot_log_label_shadow, lv_color_hex(THEME_COLOR_SPLASH_TERMINAL_GREEN), 0);
    lv_obj_set_style_text_font(boot_log_label_shadow, &lv_font_spacemono_18, 0);

    boot_log_label = lv_label_create(screen);
    lv_obj_set_pos(boot_log_label, box_text_x, box_text_y);
    lv_obj_set_width(boot_log_label, box_text_width);
    lv_obj_set_style_text_color(boot_log_label, lv_color_hex(THEME_COLOR_SPLASH_TERMINAL_GREEN), 0);
    lv_obj_set_style_text_font(boot_log_label, &lv_font_spacemono_18, 0);
    refresh_boot_log();

    lv_obj_t * copyright_label = lv_label_create(screen);
    lv_label_set_text(copyright_label, "(c) 2026 Herdsman Corp.");
    lv_obj_set_style_text_color(copyright_label, lv_color_hex(THEME_COLOR_SPLASH_TERMINAL_GREEN), 0);
    lv_obj_set_style_text_font(copyright_label, &lv_font_spacemono_18, 0);
    lv_obj_align(copyright_label, LV_ALIGN_BOTTOM_LEFT, 20, -8);

    wifi_icon_label = lv_label_create(screen);
    lv_label_set_text(wifi_icon_label, LV_SYMBOL_WIFI);
    // Montserrat, not the retro Space Mono fonts -- see calendar_view.cpp's
    // identical comment on its own wifi_icon_label; Space Mono has no
    // LV_SYMBOL_* glyphs, same gap as unscii before it.
    lv_obj_set_style_text_font(wifi_icon_label, &lv_font_montserrat_20, 0);
    lv_obj_align(wifi_icon_label, LV_ALIGN_BOTTOM_RIGHT, -20, -10);

    // Strikethrough for the "disconnected" state: LVGL's built-in symbol
    // set has no "no-wifi" glyph, so this is a small red bar rotated across
    // the icon, hidden unless we're actually disconnected.
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

    set_wifi_icon_state(THEME_COLOR_SPLASH_DIM, false);
}

/** Force LVGL to paint pending label changes before a blocking network call. */
void repaint_now()
{
    lv_timer_handler();
}

/**
 * Dev/test aid: lets the countdown/post-event-refresh pipeline be
 * exercised on real hardware without waiting for an actual calendar event
 * to approach -- type into the serial monitor:
 *   testalert                  -> test event 400s (6:40) out, on row 0
 *   testalert <seconds>        -> ...that many seconds out instead
 *   testalert <seconds> <row>  -> ...targeting the Nth displayed row (0 =
 *                                  first) instead of always the first --
 *                                  send two overlapping calls with
 *                                  different row indices to verify
 *                                  multiple simultaneous countdowns
 *                                  highlight independently
 * (No sound alert or impact level anymore -- see alert_manager.h for why
 * the sound alert was dropped.)
 */
void handle_serial_command(const String & command)
{
    if (!command.startsWith("testalert")) {
        return;
    }

    long seconds_from_now = 400;
    size_t row_index = 0;

    String rest = command.substring(String("testalert").length());
    rest.trim();
    if (rest.length() > 0) {
        const int space_index = rest.indexOf(' ');
        if (space_index < 0) {
            seconds_from_now = rest.toInt();
        } else {
            seconds_from_now = rest.substring(0, space_index).toInt();
            row_index = static_cast<size_t>(rest.substring(space_index + 1).toInt());
        }
    }

    // Reuse a real, currently-displayed row's id (rather than a synthetic
    // one) so the countdown actually has a row to show up on -- see
    // alert_manager_inject_test_event()'s doc comment for why a synthetic
    // id alone doesn't render anything anymore.
    alert_manager_inject_test_event(seconds_from_now, calendar_view_get_event_id_at(row_index));
}

void poll_serial_commands()
{
    while (Serial.available() > 0) {
        const char c = static_cast<char>(Serial.read());
        if (c == '\n' || c == '\r') {
            if (serial_command_buffer.length() > 0) {
                handle_serial_command(serial_command_buffer);
                serial_command_buffer = "";
            }
        } else {
            serial_command_buffer += c;
        }
    }
}

} // namespace

/**
 * @brief Initialize hardware, LVGL callbacks, and the milestone-1 splash.
 *
 * Called once after reset; the serial log identifies each major subsystem
 * so a failure at any stage (panel, touch expander, GT911 probe) is visible
 * over USB serial even if the display itself never lights up.
 */
void setup()
{
    Serial.begin(115200);
    Wire.begin(19, 20);

    if (!lcd.begin()) {
        Serial.println("LovyanGFX lcd.begin() failed");
        return;
    }
    Serial.println("LovyanGFX lcd.begin() OK");
    delay(200);
    pinMode(backlight_pin, OUTPUT);
    digitalWrite(backlight_pin, HIGH);

    lv_init();
    lv_tick_set_cb(lvgl_tick);
    delay(100);

    touch_init();
    boot_line_touch = make_boot_line("TOUCH (GT911)", touch_is_ready() ? " OK " : "FAIL");

    lv_color_t * frame_buffer_0 = reinterpret_cast<lv_color_t *>(lcd.bus.getFrameBuffer(0));
    lv_color_t * frame_buffer_1 = reinterpret_cast<lv_color_t *>(lcd.bus.getFrameBuffer(1));
    lv_display_t * display = lv_display_create(screen_width, screen_height);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(display, display_flush);
    lv_display_set_buffers(display, frame_buffer_0, frame_buffer_1,
                           screen_width * screen_height * sizeof(lv_color_t),
                           LV_DISPLAY_RENDER_MODE_FULL);

    lv_indev_t * touch = lv_indev_create();
    lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touch, touch_read);

    build_splash_ui();
    lv_timer_handler();
    Serial.printf("Ready: Arduino %s, LVGL %d.%d.%d\n", ESP_ARDUINO_VERSION_STR,
                  LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);

    // Registered before the initial connect attempt so later drops/
    // reconnects (WiFi.setAutoReconnect(true), see wifi_manager.cpp) keep
    // the icon accurate for the life of the program, not just at boot.
    WiFi.onEvent(wifi_on_event);

    boot_line_wifi = make_boot_line("WIFI", "CONN");
    refresh_boot_log();
    repaint_now();

    // Try whatever's stored in NVS first (works silently on every boot
    // after the first). If there's nothing stored, or it no longer
    // connects (new router, changed password), fall through to the
    // on-device setup screen -- there's no way to proceed without WiFi
    // anyway, so it blocks until a connection actually succeeds.
    String ssid;
    String password;
    const bool have_stored = wifi_credentials_load(ssid, password);
    if (!have_stored || !wifi_connect(ssid, password)) {
        wifi_setup_screen_run(main_screen, ssid, password);
        wifi_credentials_save(ssid, password);
    }

    pending_wifi_icon_state = WifiIconState::CONNECTED;
    apply_pending_wifi_icon_state();

    // NTP + America/New_York (with DST rules), for alert_manager's event-time
    // comparisons. Forex Factory serves event times in this zone for an
    // anonymous request -- confirmed directly, not assumed: the page's own
    // displayed clock read exactly 4 hours behind real UTC when checked in
    // July (matching EDT), and its JS explicitly declares
    // timezone_name: 'America/New_York'. The POSIX TZ string's DST rule
    // (M3.2.0 = 2nd Sunday in March, M11.1.0 = 1st Sunday in November) lets
    // getLocalTime()/mktime() handle EST/EDT transitions correctly
    // year-round via the standard C library instead of hand-rolled DST math.
    // Non-blocking -- getLocalTime() simply fails until sync completes in
    // the background, which alert_manager already treats as "nothing to do
    // yet" rather than assuming success.
    configTzTime("EST5EDT,M3.2.0,M11.1.0", "pool.ntp.org", "time.nist.gov");

    // No compiled-in default server address (a sold product can't assume
    // everyone's calendar_api.py container lives at the same address) --
    // if nothing's been configured via Settings yet, skip straight past
    // the health check rather than requesting against an empty URL. The
    // calendar view still comes up fine (mock data for now); Settings is
    // where this gets set.
    String server_url;
    if (calendar_server_url_load(server_url)) {
        calendar_client_set_base_url(server_url);
        boot_line_api = make_boot_line("CALENDAR API", "CONN");
        refresh_boot_log();
        repaint_now();

        const String health = calendar_client_check_health();
        boot_line_api = make_boot_line("CALENDAR API", health.length() > 0 ? " OK " : "FAIL");
    } else {
        boot_line_api = make_boot_line("CALENDAR API", "NOT SET");
    }
    refresh_boot_log();
    repaint_now();

    // Milestone 5: hold the completed boot log on screen briefly, then move
    // to the calendar view (Milestone 7 wires in the real fetch behind
    // calendar_view_create() itself, so nothing here needed to change).
    //
    // (2026-08 footnote: this delay, and configTzTime()'s own position
    // below, both went through a round of "does boot timing matter"
    // experimentation while chasing an intermittent calendar-fetch
    // failure -- a longer delay here, and moving configTzTime() to fire
    // after this point instead of right after WiFi connects, on the
    // theory that NTP's own background DNS/network activity was
    // contending with the calendar fetch. Both were cleanly ruled out
    // (see the README's own writeup) once the real cause was found: a
    // background FreeRTOS task's requests weren't reaching the server at
    // all, regardless of timing. calendar_view_create() below now fetches
    // synchronously, on this same task, so there's no longer a separate
    // background fetch for boot timing to matter to either way -- both
    // reverted to their original, simpler form.)
    delay(1500);
    lv_obj_t * calendar_screen = calendar_view_create();
    lv_screen_load(calendar_screen);

    // The calendar screen's WiFi icon didn't exist yet the last time
    // apply_pending_wifi_icon_state() ran (right after the initial
    // connect, well before this point) -- re-applying the same
    // (still-current) pending state now syncs its icon for the first time
    // without needing a real WiFi event to happen first.
    apply_pending_wifi_icon_state();
}

void loop()
{
    // Diagnostics for the still-recurring frame-shift/pixelation glitch:
    // any gap this large between consecutive loop() iterations means
    // *something* blocked for a while without giving lv_timer_handler() a
    // chance to run and keep the RGB panel's bounce buffer fed -- this
    // catches any such stall generically (WiFi reconnects, NVS/Preferences
    // access, anything else that isn't already individually timed), not
    // just the specific spots already instrumented (calendar_client.cpp's
    // GET/parse timing, calendar_view.cpp's timed_timer_handler()).
    const uint32_t loop_start_ms = millis();
    if (last_loop_end_ms != 0) {
        const uint32_t gap_ms = loop_start_ms - last_loop_end_ms;
        if (gap_ms > stall_log_threshold_ms) {
            Serial.printf("[stall] %lums gap between loop() iterations, ending t=%lums\n",
                          static_cast<unsigned long>(gap_ms), static_cast<unsigned long>(loop_start_ms));
        }
    }

    if (wifi_icon_state_dirty) {
        wifi_icon_state_dirty = false;
        apply_pending_wifi_icon_state();
    }

    wifi_reconnect_if_down(wifi_reconnect_interval_ms);

    poll_serial_commands();
    calendar_view_poll();
    alert_manager_tick();
    calendar_view_tick();

    const uint32_t timer_start_ms = millis();
    lv_timer_handler();
    const uint32_t timer_elapsed_ms = millis() - timer_start_ms;
    if (timer_elapsed_ms > stall_log_threshold_ms) {
        Serial.printf("[stall] lv_timer_handler() (main loop) took %lums, ending t=%lums\n",
                      static_cast<unsigned long>(timer_elapsed_ms), static_cast<unsigned long>(millis()));
    }

    delay(5);
    last_loop_end_ms = millis();
}
