#include "calendar_client.h"
#include "calendar_server_store.h"
#include "settings_server_screen.h"
#include "theme.h"

namespace {

lv_obj_t * screen = nullptr;
lv_obj_t * previous_screen_ref = nullptr;
lv_obj_t * protocol_dropdown = nullptr;
lv_obj_t * host_textarea = nullptr;
lv_obj_t * port_textarea = nullptr;
lv_obj_t * keyboard = nullptr;
lv_obj_t * status_label = nullptr;

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

/** Show the on-screen keyboard, attached to whichever textarea was tapped. */
void on_textarea_focused(lv_event_t * e)
{
    lv_obj_t * textarea = static_cast<lv_obj_t *>(lv_event_get_target(e));
    lv_keyboard_set_textarea(keyboard, textarea);
    lv_obj_remove_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(keyboard);
}

void on_textarea_defocused(lv_event_t *)
{
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
}

/** The keyboard's own Ready ("OK")/Cancel ("X") keys should also dismiss it. */
void on_keyboard_ready_or_cancel(lv_event_t *)
{
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
}

/**
 * Split a stored "http://host:port" URL into its three parts, so the
 * protocol dropdown/host field/port field can be pre-filled individually.
 * Defaults (http://, empty host, 8080) apply to whatever part is missing,
 * including when `url` itself is empty (nothing stored yet).
 */
void split_server_url(const String & url, String & protocol, String & host, String & port)
{
    protocol = "http://";
    host = "";
    port = "8080";
    if (url.length() == 0) {
        return;
    }

    String remainder = url;
    const int scheme_pos = remainder.indexOf("://");
    if (scheme_pos >= 0) {
        protocol = remainder.substring(0, scheme_pos + 3);
        remainder = remainder.substring(scheme_pos + 3);
    }
    const int port_pos = remainder.lastIndexOf(':');
    if (port_pos >= 0) {
        host = remainder.substring(0, port_pos);
        port = remainder.substring(port_pos + 1);
    } else {
        host = remainder;
    }
}

/** Read the protocol dropdown + host/port fields back into one "http://host:port" string. */
String read_server_url_fields()
{
    char protocol[16];
    lv_dropdown_get_selected_str(protocol_dropdown, protocol, sizeof(protocol));

    String host = lv_textarea_get_text(host_textarea);
    host.trim();

    String port = lv_textarea_get_text(port_textarea);
    port.trim();
    if (port.length() == 0) {
        port = "8080";
    }

    return String(protocol) + host + ":" + port;
}

void on_save_clicked(lv_event_t *)
{
    String host = lv_textarea_get_text(host_textarea);
    host.trim();
    if (host.length() == 0) {
        lv_label_set_text(status_label, "Enter the calendar server's IP/host.");
        return;
    }

    const String server_url = read_server_url_fields();
    lv_label_set_text(status_label, "Checking connection...");
    lv_timer_handler();

    // Verify the address actually resolves to a live calendar_api.py before
    // accepting it -- catches typos immediately instead of silently saving
    // a broken address.
    calendar_client_set_base_url(server_url);
    if (calendar_client_check_health().length() > 0) {
        calendar_server_url_save(server_url);
        return_to_previous_screen();
    } else {
        lv_label_set_text(status_label, "Couldn't reach that address -- check it and try again.");
    }
}

} // namespace

void settings_server_screen_open(lv_obj_t * previous_screen)
{
    previous_screen_ref = previous_screen;

    screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(screen, lv_color_hex(THEME_COLOR_BACKGROUND), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_screen_load(screen);

    lv_obj_t * title = lv_label_create(screen);
    lv_label_set_text(title, "CALENDAR SERVER");
    lv_obj_set_style_text_color(title, lv_color_hex(THEME_COLOR_AMBER), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 20, 10);

    lv_obj_t * hint_label = lv_label_create(screen);
    lv_label_set_text(hint_label, "Address of the LXC/calendar_api.py container:");
    lv_obj_set_style_text_color(hint_label, lv_color_hex(THEME_COLOR_AMBER_DIM), 0);
    lv_obj_align(hint_label, LV_ALIGN_TOP_LEFT, 20, 56);

    // Protocol/port default to http:// and 8080 so the common case is just
    // typing an IP on the on-screen keyboard -- both remain changeable via
    // the dropdown/port field for the less common https:// or custom-port case.
    protocol_dropdown = lv_dropdown_create(screen);
    lv_dropdown_set_options(protocol_dropdown, "http://\nhttps://");
    lv_obj_set_size(protocol_dropdown, 120, 44);
    lv_obj_align(protocol_dropdown, LV_ALIGN_TOP_LEFT, 20, 84);
    theme_style_dropdown(protocol_dropdown);

    host_textarea = lv_textarea_create(screen);
    lv_textarea_set_one_line(host_textarea, true);
    lv_textarea_set_placeholder_text(host_textarea, "192.168.1.50");
    lv_obj_set_size(host_textarea, 270, 44);
    theme_style_textarea(host_textarea);
    lv_obj_align(host_textarea, LV_ALIGN_TOP_LEFT, 150, 84);
    lv_obj_add_event_cb(host_textarea, on_textarea_focused, LV_EVENT_FOCUSED, nullptr);
    lv_obj_add_event_cb(host_textarea, on_textarea_defocused, LV_EVENT_DEFOCUSED, nullptr);

    lv_obj_t * colon_label = lv_label_create(screen);
    lv_label_set_text(colon_label, ":");
    lv_obj_set_style_text_color(colon_label, lv_color_hex(THEME_COLOR_AMBER_DIM), 0);
    lv_obj_align(colon_label, LV_ALIGN_TOP_LEFT, 428, 96);

    port_textarea = lv_textarea_create(screen);
    lv_textarea_set_one_line(port_textarea, true);
    lv_textarea_set_placeholder_text(port_textarea, "8080");
    lv_obj_set_size(port_textarea, 90, 44);
    theme_style_textarea(port_textarea);
    lv_obj_align(port_textarea, LV_ALIGN_TOP_LEFT, 440, 84);
    lv_obj_add_event_cb(port_textarea, on_textarea_focused, LV_EVENT_FOCUSED, nullptr);
    lv_obj_add_event_cb(port_textarea, on_textarea_defocused, LV_EVENT_DEFOCUSED, nullptr);

    String stored_server_url;
    calendar_server_url_load(stored_server_url); // ignore return -- defaults apply either way
    String initial_protocol, initial_host, initial_port;
    split_server_url(stored_server_url, initial_protocol, initial_host, initial_port);
    lv_dropdown_set_selected(protocol_dropdown, initial_protocol == "https://" ? 1 : 0);
    lv_textarea_set_text(host_textarea, initial_host.c_str());
    lv_textarea_set_text(port_textarea, initial_port.c_str());

    // On-screen keyboard, hidden until a textarea is focused -- overlays
    // the bottom of the screen rather than reserving permanent space.
    keyboard = lv_keyboard_create(screen);
    lv_obj_set_size(keyboard, 800, 240);
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(keyboard, on_keyboard_ready_or_cancel, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(keyboard, on_keyboard_ready_or_cancel, LV_EVENT_CANCEL, nullptr);

    status_label = lv_label_create(screen);
    lv_label_set_text(status_label, "");
    lv_obj_set_style_text_color(status_label, lv_color_hex(THEME_COLOR_IMPACT_HIGH), 0);
    lv_obj_align(status_label, LV_ALIGN_BOTTOM_LEFT, 20, -10);

    lv_obj_t * back_btn = theme_create_button(screen, "Back", on_back_clicked);
    lv_obj_align(back_btn, LV_ALIGN_BOTTOM_RIGHT, -140, -8);

    lv_obj_t * save_btn = theme_create_button(screen, "Save", on_save_clicked);
    lv_obj_align(save_btn, LV_ALIGN_BOTTOM_RIGHT, -20, -8);
}
