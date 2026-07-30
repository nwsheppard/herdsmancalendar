#include "calendar_view.h"
#include "settings_columns_screen.h"
#include "settings_filters_screen.h"
#include "settings_screen.h"
#include "settings_server_screen.h"
#include "settings_wifi_screen.h"
#include "theme.h"

namespace {

lv_obj_t * screen = nullptr;
lv_obj_t * previous_screen_ref = nullptr;

void on_back_clicked(lv_event_t *)
{
    lv_screen_load(previous_screen_ref);
    lv_obj_delete(screen);
    screen = nullptr;

    // A filter or server-address change made in one of the sub-screens
    // otherwise wouldn't show up on the calendar view until the next
    // periodic poll (up to several minutes away) -- refresh immediately
    // instead. A no-op if nothing actually changed.
    calendar_view_refresh();
}

void on_wifi_clicked(lv_event_t *)
{
    settings_wifi_screen_open(screen);
}

void on_server_clicked(lv_event_t *)
{
    settings_server_screen_open(screen);
}

void on_filters_clicked(lv_event_t *)
{
    settings_filters_screen_open(screen);
}

void on_columns_clicked(lv_event_t *)
{
    settings_columns_screen_open(screen);
}

/** Menu buttons are bigger than theme_create_button()'s default -- bump the label font to match. */
lv_obj_t * make_menu_button(lv_obj_t * parent, const char * text, lv_event_cb_t callback)
{
    lv_obj_t * button = theme_create_button(parent, text, callback);
    lv_obj_set_size(button, 360, 56);
    lv_obj_t * label = lv_obj_get_child(button, 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
    return button;
}

} // namespace

void settings_screen_open(lv_obj_t * previous_screen)
{
    previous_screen_ref = previous_screen;

    screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(screen, lv_color_hex(THEME_COLOR_BACKGROUND), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_screen_load(screen);

    lv_obj_t * title = lv_label_create(screen);
    lv_label_set_text(title, "SETTINGS");
    lv_obj_set_style_text_color(title, lv_color_hex(THEME_COLOR_AMBER), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_40, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

    lv_obj_t * wifi_btn = make_menu_button(screen, "WiFi", on_wifi_clicked);
    lv_obj_align(wifi_btn, LV_ALIGN_CENTER, 0, -99);

    lv_obj_t * server_btn = make_menu_button(screen, "Calendar Server", on_server_clicked);
    lv_obj_align(server_btn, LV_ALIGN_CENTER, 0, -33);

    lv_obj_t * filters_btn = make_menu_button(screen, "Impact & Currencies", on_filters_clicked);
    lv_obj_align(filters_btn, LV_ALIGN_CENTER, 0, 33);

    lv_obj_t * columns_btn = make_menu_button(screen, "Data Columns", on_columns_clicked);
    lv_obj_align(columns_btn, LV_ALIGN_CENTER, 0, 99);

    lv_obj_t * back_btn = theme_create_button(screen, "Back to Calendar", on_back_clicked);
    lv_obj_align(back_btn, LV_ALIGN_BOTTOM_MID, 0, -20);
}
