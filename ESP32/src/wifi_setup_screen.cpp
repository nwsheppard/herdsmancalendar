#include <WiFi.h>

#include "theme.h"
#include "wifi_manager.h"
#include "wifi_setup_screen.h"

namespace {

lv_obj_t * setup_screen = nullptr;
lv_obj_t * page_list = nullptr;
lv_obj_t * page_password = nullptr;
lv_obj_t * page_connecting = nullptr;

lv_obj_t * list_status_label = nullptr;
lv_obj_t * network_list = nullptr;
lv_obj_t * password_ssid_label = nullptr;
lv_obj_t * password_textarea = nullptr;
lv_obj_t * password_toggle_icon = nullptr;
lv_obj_t * password_error_label = nullptr;
lv_obj_t * connecting_label = nullptr;

String g_selected_ssid;
bool g_selected_open = false;
bool g_setup_done = false;
String g_result_ssid;
String g_result_password;

void attempt_connect(const String & ssid, const String & password);
void populate_network_list();

/** Show one page, hide the other two -- pages are pre-built, not recreated. */
void show_page(lv_obj_t * page)
{
    lv_obj_add_flag(page_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(page_password, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(page_connecting, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_HIDDEN);
}

void on_network_button_clicked(lv_event_t * e)
{
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    g_selected_ssid = WiFi.SSID(index);
    g_selected_open = (WiFi.encryptionType(index) == WIFI_AUTH_OPEN);
    if (g_selected_open) {
        attempt_connect(g_selected_ssid, "");
    } else {
        lv_label_set_text_fmt(password_ssid_label, "Password for: %s", g_selected_ssid.c_str());
        lv_textarea_set_text(password_textarea, "");
        lv_label_set_text(password_error_label, "");
        // Reset to hidden each time in case the user toggled visibility
        // on a previous attempt -- don't carry that choice to a new network.
        lv_textarea_set_password_mode(password_textarea, true);
        lv_label_set_text(password_toggle_icon, LV_SYMBOL_EYE_OPEN);
        show_page(page_password);
    }
}

void on_connect_button_clicked(lv_event_t *)
{
    attempt_connect(g_selected_ssid, lv_textarea_get_text(password_textarea));
}

void on_back_button_clicked(lv_event_t *)
{
    show_page(page_list);
}

/** Toggle whether the password textarea shows dots or the typed characters. */
void on_toggle_password_visibility(lv_event_t *)
{
    const bool was_hidden = lv_textarea_get_password_mode(password_textarea);
    lv_textarea_set_password_mode(password_textarea, !was_hidden);
    lv_label_set_text(password_toggle_icon, was_hidden ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_EYE_OPEN);
}

void on_rescan_button_clicked(lv_event_t *)
{
    populate_network_list();
}

/** Blocking scan; repaints "Scanning..." first since it takes a few seconds. */
void populate_network_list()
{
    lv_obj_clean(network_list);
    lv_label_set_text(list_status_label, "Scanning...");
    lv_timer_handler();

    const int count = WiFi.scanNetworks();
    if (count <= 0) {
        lv_label_set_text(list_status_label, "No networks found. Tap Rescan.");
        return;
    }

    lv_label_set_text_fmt(list_status_label, "Select a network (%d found):", count);
    for (int i = 0; i < count; ++i) {
        const bool open = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
        String text = WiFi.SSID(i) + "   " + (open ? "[open]" : "[secured]") + "   " +
                      String(WiFi.RSSI(i)) + "dBm";
        lv_obj_t * btn = lv_list_add_button(network_list, nullptr, text.c_str());
        theme_style_list_button(btn);
        lv_obj_add_event_cb(btn, on_network_button_clicked, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<intptr_t>(i)));
    }
}

/** Attempt a connection; on failure, returns to whichever page makes sense to retry from. */
void attempt_connect(const String & ssid, const String & password)
{
    lv_label_set_text_fmt(connecting_label, "Connecting to %s...", ssid.c_str());
    show_page(page_connecting);
    lv_timer_handler();

    if (wifi_connect(ssid, password)) {
        g_result_ssid = ssid;
        g_result_password = password;
        g_setup_done = true;
        return;
    }

    if (g_selected_open) {
        lv_label_set_text(list_status_label, "Couldn't connect to that network. Select another or Rescan.");
        show_page(page_list);
    } else {
        lv_label_set_text(password_error_label, "Couldn't connect -- check the password and try again.");
        show_page(page_password);
    }
}

lv_obj_t * build_setup_screen()
{
    lv_obj_t * screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(screen, lv_color_hex(THEME_COLOR_BACKGROUND), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    lv_obj_t * title = lv_label_create(screen);
    lv_label_set_text(title, "WIFI SETUP");
    lv_obj_set_style_text_color(title, lv_color_hex(THEME_COLOR_AMBER), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    // --- Network list page ---
    page_list = lv_obj_create(screen);
    lv_obj_remove_style_all(page_list);
    lv_obj_set_size(page_list, 760, 420);
    lv_obj_align(page_list, LV_ALIGN_BOTTOM_MID, 0, -10);

    list_status_label = lv_label_create(page_list);
    lv_label_set_text(list_status_label, "Scanning...");
    lv_obj_set_style_text_color(list_status_label, lv_color_hex(THEME_COLOR_AMBER_DIM), 0);
    lv_obj_align(list_status_label, LV_ALIGN_TOP_LEFT, 0, 0);

    network_list = lv_list_create(page_list);
    lv_obj_set_size(network_list, 760, 340);
    lv_obj_align(network_list, LV_ALIGN_TOP_MID, 0, 30);
    theme_style_list(network_list);

    lv_obj_t * rescan_btn = theme_create_button(page_list, "Rescan", on_rescan_button_clicked);
    lv_obj_align(rescan_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

    // --- Password entry page ---
    page_password = lv_obj_create(screen);
    lv_obj_remove_style_all(page_password);
    lv_obj_set_size(page_password, 760, 420);
    lv_obj_align(page_password, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_add_flag(page_password, LV_OBJ_FLAG_HIDDEN);

    password_ssid_label = lv_label_create(page_password);
    lv_label_set_text(password_ssid_label, "Password for:");
    lv_obj_set_style_text_color(password_ssid_label, lv_color_hex(THEME_COLOR_AMBER), 0);
    lv_obj_align(password_ssid_label, LV_ALIGN_TOP_LEFT, 0, 0);

    password_textarea = lv_textarea_create(page_password);
    lv_textarea_set_password_mode(password_textarea, true);
    lv_textarea_set_one_line(password_textarea, true);
    lv_obj_set_size(password_textarea, 420, 50);
    theme_style_textarea(password_textarea);
    lv_obj_align(password_textarea, LV_ALIGN_TOP_LEFT, 0, 30);

    // Buttons get an explicit size *before* being used as an lv_obj_align_to()
    // target -- content-fit sizing resolves on the next layout pass, not
    // synchronously, so aligning a sibling to a button immediately after
    // creation can use stale (pre-resize) dimensions and land in the wrong
    // place. Child-centering a button's own label (inside theme_create_button)
    // isn't affected by this -- that's a persistent alignment re-evaluated
    // whenever the parent's size changes, not a one-time calculation.
    lv_obj_t * toggle_btn = theme_create_button(page_password, LV_SYMBOL_EYE_OPEN, on_toggle_password_visibility);
    lv_obj_set_size(toggle_btn, 50, 50);
    password_toggle_icon = lv_obj_get_child(toggle_btn, 0);
    lv_obj_align_to(toggle_btn, password_textarea, LV_ALIGN_OUT_RIGHT_MID, 10, 0);

    lv_obj_t * connect_btn = theme_create_button(page_password, "Connect", on_connect_button_clicked);
    lv_obj_set_size(connect_btn, 110, 50);
    lv_obj_align_to(connect_btn, toggle_btn, LV_ALIGN_OUT_RIGHT_MID, 10, 0);

    lv_obj_t * back_btn = theme_create_button(page_password, "Back", on_back_button_clicked);
    lv_obj_set_size(back_btn, 90, 50);
    lv_obj_align_to(back_btn, connect_btn, LV_ALIGN_OUT_RIGHT_MID, 10, 0);

    password_error_label = lv_label_create(page_password);
    lv_label_set_text(password_error_label, "");
    lv_obj_set_style_text_color(password_error_label, lv_color_hex(THEME_COLOR_IMPACT_HIGH), 0);
    lv_obj_align(password_error_label, LV_ALIGN_TOP_LEFT, 0, 70);

    lv_obj_t * keyboard = lv_keyboard_create(page_password);
    lv_keyboard_set_textarea(keyboard, password_textarea);
    lv_obj_set_size(keyboard, 760, 260);
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);

    // --- Connecting status page ---
    page_connecting = lv_obj_create(screen);
    lv_obj_remove_style_all(page_connecting);
    lv_obj_set_size(page_connecting, 760, 420);
    lv_obj_align(page_connecting, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_add_flag(page_connecting, LV_OBJ_FLAG_HIDDEN);

    connecting_label = lv_label_create(page_connecting);
    lv_label_set_text(connecting_label, "Connecting...");
    lv_obj_set_style_text_color(connecting_label, lv_color_hex(THEME_COLOR_AMBER), 0);
    lv_obj_center(connecting_label);

    return screen;
}

} // namespace

void wifi_setup_screen_run(lv_obj_t * previous_screen, String & out_ssid, String & out_password)
{
    // A fresh device (no stored credentials) reaches this function without
    // wifi_connect() ever having run, so STA mode may not be enabled yet --
    // scanNetworks() needs it regardless of what the core version assumes.
    WiFi.mode(WIFI_STA);

    g_setup_done = false;
    setup_screen = build_setup_screen();
    lv_screen_load(setup_screen);
    show_page(page_list);
    populate_network_list();

    while (!g_setup_done) {
        lv_timer_handler();
        delay(5);
    }

    out_ssid = g_result_ssid;
    out_password = g_result_password;

    lv_screen_load(previous_screen);
    lv_obj_delete(setup_screen);
    setup_screen = nullptr;
}
