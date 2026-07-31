#include "fonts.h"
#include "theme.h"

lv_obj_t * theme_create_button(lv_obj_t * parent, const char * text, lv_event_cb_t callback,
                               void * user_data)
{
    lv_obj_t * button = lv_button_create(parent);
    lv_obj_set_style_bg_color(button, lv_color_hex(THEME_COLOR_BUTTON_BG), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(THEME_COLOR_AMBER), 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_border_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(button, 4, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);

    lv_obj_t * label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(THEME_COLOR_AMBER), 0);
    // Retro default for every button's label -- previously fell through to
    // LV_FONT_DEFAULT (still Montserrat) since nothing here ever set a font
    // explicitly, which is why the first font-wide VGA-style swap missed
    // every button. spacemono_20 (see fonts.h), not spacemono_18 (the
    // grid/body size) -- buttons are touch targets, not dense data, and
    // get their own larger tier; also not LVGL's built-in unscii_16, which
    // measured out too wide for these buttons on hardware (text reaching
    // the sides). Space Mono, not the original custom VT323 conversion --
    // dropped after a direct side-by-side comparison found VT323's 0/2
    // digits read as too similar (see fonts.h). Callers whose text is
    // actually an LV_SYMBOL_* icon (the settings gear, the WiFi-setup
    // password eye-toggle) override this back to a Montserrat size
    // afterward -- Space Mono has no icon/symbol glyphs, so an icon button
    // left on this font renders as a blank/placeholder box.
    lv_obj_set_style_text_font(label, &lv_font_spacemono_20, 0);
    lv_obj_center(label);

    if (callback != nullptr) {
        lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);
    }
    return button;
}

lv_obj_t * theme_create_checkbox(lv_obj_t * parent, const char * text)
{
    lv_obj_t * checkbox = lv_checkbox_create(parent);
    lv_checkbox_set_text(checkbox, text);

    // Checkbox's own text (drawn as part of LV_PART_MAIN, not a child label).
    lv_obj_set_style_text_color(checkbox, lv_color_hex(THEME_COLOR_AMBER), 0);

    // Indicator box, styled explicitly for every state (default/checked/
    // pressed/focus) rather than relying on the default-state style to
    // fall through -- LVGL's default theme adds its own (blue-accented)
    // styling independently per state, so leaving any one of them
    // un-overridden lets that color show through only in that state.
    lv_obj_set_style_radius(checkbox, 3, LV_PART_INDICATOR);
    lv_obj_set_style_outline_width(checkbox, 0, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(checkbox, lv_color_hex(THEME_COLOR_BUTTON_BG), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(checkbox, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_border_color(checkbox, lv_color_hex(THEME_COLOR_AMBER), LV_PART_INDICATOR);
    lv_obj_set_style_border_width(checkbox, 1, LV_PART_INDICATOR);

    const lv_style_selector_t indicator_checked =
        static_cast<lv_style_selector_t>(LV_PART_INDICATOR) | static_cast<lv_style_selector_t>(LV_STATE_CHECKED);
    lv_obj_set_style_bg_image_src(checkbox, LV_SYMBOL_OK, indicator_checked);
    lv_obj_set_style_bg_color(checkbox, lv_color_hex(THEME_COLOR_AMBER), indicator_checked);
    lv_obj_set_style_border_color(checkbox, lv_color_hex(THEME_COLOR_AMBER), indicator_checked);
    lv_obj_set_style_text_color(checkbox, lv_color_hex(THEME_COLOR_BACKGROUND), indicator_checked);

    const lv_style_selector_t indicator_pressed =
        static_cast<lv_style_selector_t>(LV_PART_INDICATOR) | static_cast<lv_style_selector_t>(LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(checkbox, lv_color_hex(THEME_COLOR_AMBER_DIM), indicator_pressed);

    const lv_style_selector_t indicator_focus_key =
        static_cast<lv_style_selector_t>(LV_PART_INDICATOR) | static_cast<lv_style_selector_t>(LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_width(checkbox, 0, indicator_focus_key);
    lv_obj_set_style_border_color(checkbox, lv_color_hex(THEME_COLOR_AMBER), indicator_focus_key);

    return checkbox;
}

void theme_style_textarea(lv_obj_t * textarea)
{
    lv_obj_set_style_bg_color(textarea, lv_color_hex(THEME_COLOR_BUTTON_BG), 0);
    lv_obj_set_style_bg_opa(textarea, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(textarea, lv_color_hex(THEME_COLOR_AMBER_DIM), 0);
    lv_obj_set_style_border_width(textarea, 1, 0);
    lv_obj_set_style_text_color(textarea, lv_color_hex(THEME_COLOR_AMBER), 0);
    lv_obj_set_style_text_color(textarea, lv_color_hex(THEME_COLOR_AMBER_DIM), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_border_color(textarea, lv_color_hex(THEME_COLOR_AMBER), LV_PART_CURSOR);
}

void theme_style_dropdown(lv_obj_t * dropdown)
{
    lv_obj_set_style_bg_color(dropdown, lv_color_hex(THEME_COLOR_BUTTON_BG), 0);
    lv_obj_set_style_bg_opa(dropdown, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(dropdown, lv_color_hex(THEME_COLOR_AMBER_DIM), 0);
    lv_obj_set_style_border_width(dropdown, 1, 0);
    lv_obj_set_style_text_color(dropdown, lv_color_hex(THEME_COLOR_AMBER), 0);

    // The options list is created eagerly alongside the dropdown itself
    // (just hidden until opened), so it's safe to style right away here
    // rather than needing an on-open callback.
    lv_obj_t * list = lv_dropdown_get_list(dropdown);
    lv_obj_set_style_bg_color(list, lv_color_hex(THEME_COLOR_BUTTON_BG), 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(list, lv_color_hex(THEME_COLOR_AMBER), 0);
    lv_obj_set_style_border_width(list, 1, 0);
    lv_obj_set_style_text_color(list, lv_color_hex(THEME_COLOR_AMBER), 0);
    lv_obj_set_style_bg_color(list, lv_color_hex(THEME_COLOR_AMBER), LV_PART_SELECTED);
    lv_obj_set_style_text_color(list, lv_color_hex(THEME_COLOR_BACKGROUND), LV_PART_SELECTED);
}

void theme_style_list(lv_obj_t * list)
{
    lv_obj_set_style_bg_color(list, lv_color_hex(THEME_COLOR_BACKGROUND), 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    theme_style_scrollbar(list);
}

void theme_style_scrollbar(lv_obj_t * obj)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(THEME_COLOR_AMBER_DIM), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(obj, LV_OPA_70, LV_PART_SCROLLBAR);
}

void theme_style_checkbox_wrap(lv_obj_t * obj)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(THEME_COLOR_BACKGROUND), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    theme_style_scrollbar(obj);
}

void theme_style_list_button(lv_obj_t * button)
{
    lv_obj_set_style_bg_color(button, lv_color_hex(THEME_COLOR_BUTTON_BG), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(THEME_COLOR_AMBER_DIM), 0);
    lv_obj_set_style_border_width(button, 1, 0);
    // Inherited by the button's child label -- LVGL propagates text_color
    // down to children that don't set their own.
    lv_obj_set_style_text_color(button, lv_color_hex(THEME_COLOR_AMBER), 0);
}
