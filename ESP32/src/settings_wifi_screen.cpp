#include <WiFi.h>

#include "settings_wifi_screen.h"
#include "theme.h"
#include "wifi_credentials_store.h"
#include "wifi_setup_screen.h"

namespace {

lv_obj_t * screen = nullptr;
lv_obj_t * previous_screen_ref = nullptr;
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

void on_reconfigure_clicked(lv_event_t *)
{
    String ssid;
    String password;
    // previous_screen here is *this* screen -- once reconnected,
    // wifi_setup_screen_run() returns here rather than to the Settings menu.
    wifi_setup_screen_run(screen, ssid, password);
    wifi_credentials_save(ssid, password);
    lv_label_set_text_fmt(status_label, "WiFi: %s (%s)", ssid.c_str(),
                          WiFi.localIP().toString().c_str());
}

} // namespace

void settings_wifi_screen_open(lv_obj_t * previous_screen)
{
    previous_screen_ref = previous_screen;

    screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(screen, lv_color_hex(THEME_COLOR_BACKGROUND), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_screen_load(screen);

    lv_obj_t * title = lv_label_create(screen);
    lv_label_set_text(title, "WIFI");
    lv_obj_set_style_text_color(title, lv_color_hex(THEME_COLOR_AMBER), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 20, 10);

    status_label = lv_label_create(screen);
    lv_label_set_text_fmt(status_label, "WiFi: %s", WiFi.localIP().toString().c_str());
    lv_obj_set_style_text_color(status_label, lv_color_hex(THEME_COLOR_AMBER_DIM), 0);
    lv_obj_align(status_label, LV_ALIGN_TOP_LEFT, 20, 60);

    lv_obj_t * reconfigure_btn = theme_create_button(screen, "Reconfigure WiFi", on_reconfigure_clicked);
    lv_obj_align(reconfigure_btn, LV_ALIGN_TOP_LEFT, 20, 100);

    lv_obj_t * back_btn = theme_create_button(screen, "Back", on_back_clicked);
    lv_obj_align(back_btn, LV_ALIGN_BOTTOM_RIGHT, -20, -20);
}
