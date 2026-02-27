#include "components.h"
#include "gui_constants.h"
#include "lvgl.h"

#include <stdint.h>

extern "C" const top_nav_ctx_t TOP_NAV_CTX_DEFAULTS = {
    .title = "My Title",
    .show_back_button = true,
    .show_power_button = false,
};

lv_obj_t* top_nav(lv_obj_t* parent, const top_nav_ctx_t *ctx) {
    if (!ctx) {
        ctx = &TOP_NAV_CTX_DEFAULTS;
    }

    lv_obj_t* lv_parent = parent ? parent : lv_scr_act();
    lv_obj_t* lv_top_nav = lv_obj_create(lv_parent);

    // TopNav should be the full horizontal width
    lv_obj_set_size(lv_top_nav, lv_obj_get_width(lv_parent), TOP_NAV_HEIGHT);
    lv_obj_set_scrollbar_mode(lv_top_nav, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(lv_top_nav, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_set_style_bg_color(lv_top_nav, lv_color_hex(BACKGROUND_COLOR), LV_PART_MAIN);
    lv_obj_set_style_radius(lv_top_nav, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(lv_top_nav, 0, LV_PART_MAIN);

    // debugging
    lv_obj_set_style_border_color(lv_top_nav, lv_color_hex(BUTTON_BACKGROUND_COLOR), LV_PART_MAIN);
    lv_obj_set_style_border_width(lv_top_nav, 1, LV_PART_MAIN);
    // lv_obj_set_style_border_width(lv_top_nav, 0, LV_PART_MAIN);

    lv_obj_set_style_outline_width(lv_top_nav, 0, LV_PART_MAIN);


    // lv_style_t style;
    // lv_style_init(&style);
    // lv_style_set_pad_all(&style, 0);
    // lv_style_set_border_width(&style, 0);
    // lv_style_set_radius(&style, 0);
    // lv_style_set_bg_color(&style, lv_color_hex(BACKGROUND_COLOR));
    // lv_obj_add_style(lv_top_nav, &style, 0);

    // if (this->show_back_button) {
    //     this->back_button = new Button();
    //     this->back_button->text = "";
    //     this->back_button->icon_name = SeedSignerCustomIconConstants::LARGE_CHEVRON_LEFT;
    //     this->back_button->width = GUIConstants::TOP_NAV_BUTTON_SIZE;
    //     this->back_button->height = GUIConstants::TOP_NAV_BUTTON_SIZE;
    //     this->back_button->align = {LV_ALIGN_LEFT_MID, GUIConstants::EDGE_PADDING, -4};
    //     this->back_button->add_to_lv_obj(lv_top_nav);
    // }

    // if (this->show_power_button) {
    //     this->power_button = new Button();
    //     this->power_button->text = "";
    //     this->power_button->icon_name = FontAwesomeIconConstants::POWER_OFF;
    //     this->power_button->width = GUIConstants::TOP_NAV_BUTTON_SIZE;
    //     this->power_button->height = GUIConstants::TOP_NAV_BUTTON_SIZE;
    //     this->power_button->align = {LV_ALIGN_RIGHT_MID, -1 * GUIConstants::EDGE_PADDING, -4};
    //     this->power_button->add_to_lv_obj(lv_top_nav);
    // }

    const char *label_text = ctx->title;
    lv_obj_t* label = lv_label_create(lv_top_nav);
    lv_label_set_text(label, label_text);
    // lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR);

    lv_obj_set_style_text_color(label, lv_color_hex(BODY_FONT_COLOR), LV_PART_MAIN); // Set text color to white
    lv_obj_set_style_text_font(label, &TOP_NAV_TITLE_FONT, LV_PART_MAIN);
    lv_obj_align_to(label, lv_top_nav, LV_ALIGN_CENTER, 0, 0);  // Adjust when back button is added


    // if (this->back_button) {
    //     lv_obj_set_width(label, 320 - GUIConstants::EDGE_PADDING - GUIConstants::TOP_NAV_BUTTON_SIZE - GUIConstants::COMPONENT_PADDING);
    //     lv_obj_align_to(label, this->back_button->lv_btn, LV_ALIGN_OUT_RIGHT_MID, GUIConstants::COMPONENT_PADDING, -4);
    // } else {
    //     lv_obj_center(label);
    // }
    // lv_style_t label_style;
    // lv_style_init(&label_style);
    // // if (this->font_size == TOP_NAV_TITLE_FONT_SIZE) {
    // //     lv_style_set_text_font(&label_style, Fonts::FONT__OPEN_SANS__SEMIBOLD__20);
    // // } else if (this->font_size == 26) {
    // //     lv_style_set_text_font(&label_style, Fonts::FONT__OPEN_SANS__SEMIBOLD__26);
    // // }
    // lv_style_set_text_font(&label_style, &lv_font_montserrat_20);
    // lv_style_set_text_color(&label_style, lv_color_hex(BODY_FONT_COLOR));
    // lv_style_set_pad_right(&label_style, EDGE_PADDING);
    // lv_obj_add_style(label, &label_style, 0);

    return lv_top_nav;
}



void button_set_active(lv_obj_t* lv_button, bool active) {
    if (!lv_button) {
        return;
    }
    lv_obj_t* label = lv_obj_get_child(lv_button, 0);

    if (active) {
        lv_obj_set_style_bg_color(lv_button, lv_color_hex(ACCENT_COLOR), 0);
        if (label) {
            lv_obj_set_style_text_color(label, lv_color_hex(BUTTON_SELECTED_FONT_COLOR), 0);
        }
    } else {
        lv_obj_set_style_bg_color(lv_button, lv_color_hex(BUTTON_BACKGROUND_COLOR), 0);
        if (label) {
            lv_obj_set_style_text_color(label, lv_color_hex(BUTTON_FONT_COLOR), 0);
        }
    }
}


extern "C" __attribute__((weak)) void seedsigner_lvgl_on_button_selected(uint32_t index, const char *label) {
    (void)index;
    (void)label;
}

static lv_obj_t *s_press_btn = NULL;
static lv_point_t s_press_point = {0, 0};
static bool s_press_dragged = false;

void button_toggle_callback(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t* btn = lv_event_get_target(e);

    lv_indev_t *indev = lv_indev_get_act();

    if (code == LV_EVENT_PRESSED) {
        s_press_btn = btn;
        s_press_dragged = false;
        if (indev) {
            lv_indev_get_point(indev, &s_press_point);
        }
        return;
    }

    if (code == LV_EVENT_PRESSING) {
        if (btn == s_press_btn && indev) {
            lv_point_t p;
            lv_indev_get_point(indev, &p);
            int32_t dx = p.x - s_press_point.x;
            int32_t dy = p.y - s_press_point.y;
            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;
            const int32_t drag_threshold = BUTTON_HEIGHT / 2;
            if (dx > drag_threshold || dy > drag_threshold) {
                s_press_dragged = true;
            }
        }
        return;
    }

    if (code == LV_EVENT_RELEASED) {
        // Keep press state until CLICKED is evaluated.
        return;
    }

    if (code != LV_EVENT_CLICKED) {
        return;
    }

    lv_obj_t* parent = lv_obj_get_parent(btn);

    // Click must correspond to the same pressed button.
    if (s_press_btn != btn) {
        s_press_btn = NULL;
        s_press_dragged = false;
        return;
    }

    // Distinguish tap vs drag gestures.
    // NOTE: Do not gate clicks on lv_indev_get_scroll_obj(). On some flows
    // (notably a single-button list after a swipe starts on that button),
    // LVGL can retain a non-NULL scroll_obj and incorrectly suppress future
    // taps. The movement threshold tracked via s_press_dragged is the stable
    // source of truth for rejecting swipe/scroll gestures here.
    if (s_press_dragged) {
        s_press_btn = NULL;
        s_press_dragged = false;
        return;
    }

    // Enforce single-select behavior: deactivate all sibling buttons first.
    if (parent) {
        uint32_t child_count = lv_obj_get_child_cnt(parent);
        for (uint32_t i = 0; i < child_count; ++i) {
            lv_obj_t* child = lv_obj_get_child(parent, i);
            if (!child || child == btn) {
                continue;
            }
            if (lv_obj_check_type(child, &lv_btn_class)) {
                button_set_active(child, false);
            }
        }
    }

    button_set_active(btn, true);

    uint32_t selected_index = 0;
    if (parent) {
        uint32_t child_count = lv_obj_get_child_cnt(parent);
        uint32_t btn_pos = 0;
        for (uint32_t i = 0; i < child_count; ++i) {
            lv_obj_t* child = lv_obj_get_child(parent, i);
            if (!child || !lv_obj_check_type(child, &lv_btn_class)) {
                continue;
            }
            if (child == btn) {
                selected_index = btn_pos;
                break;
            }
            btn_pos++;
        }
    }

    const char *label_text = "";
    lv_obj_t* label = lv_obj_get_child(btn, 0);
    if (label) {
        label_text = lv_label_get_text(label);
    }
    seedsigner_lvgl_on_button_selected(selected_index, label_text);

    s_press_btn = NULL;
    s_press_dragged = false;
}


lv_obj_t* button(lv_obj_t* lv_parent, const char* text, lv_obj_t* align_to) {
    lv_obj_t* lv_button = lv_btn_create(lv_parent);
    lv_obj_set_size(lv_button, lv_obj_get_content_width(lv_parent), BUTTON_HEIGHT);

    if (align_to != NULL) {
        // Align to the outside bottom of the provided object
        lv_obj_align_to(lv_button, align_to, LV_ALIGN_OUT_BOTTOM_MID, 0, LIST_ITEM_PADDING);
    } else {
        // Align to the inside top of the parent container
        lv_obj_align_to(lv_button, lv_parent, LV_ALIGN_TOP_MID, 0, 0);
    }

    lv_obj_set_style_shadow_width(lv_button, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(lv_button, BUTTON_RADIUS, LV_PART_MAIN);

    lv_obj_t* label = lv_label_create(lv_button);
    lv_obj_set_style_text_font(label, &BUTTON_FONT, LV_PART_MAIN);
    lv_label_set_text(label, text);

    // Wire up gesture-aware input callback
    lv_obj_add_event_cb(lv_button, button_toggle_callback, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(lv_button, button_toggle_callback, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(lv_button, button_toggle_callback, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(lv_button, button_toggle_callback, LV_EVENT_RELEASED, NULL);

    // Default to inactive state
    button_set_active(lv_button, false);

    return lv_button;
}

lv_obj_t* button_list(lv_obj_t* lv_parent, const button_list_item_t *items, size_t item_count) {
    lv_obj_t* last_button = NULL;
    lv_obj_t* first_button = NULL;
    if (!lv_parent || !items || item_count == 0) {
        return last_button;
    }

    for (size_t i = 0; i < item_count; ++i) {
        const char *label = items[i].label ? items[i].label : "";
        last_button = button(lv_parent, label, last_button);
        if (i == 0) {
            first_button = last_button;
        }
    }

    // Default behavior:
    // - no button highlighted when there are multiple buttons
    // - highlight the only button when there is exactly one
    if (item_count == 1 && first_button) {
        button_set_active(first_button, true);
    } else {
        uint32_t child_count = lv_obj_get_child_cnt(lv_parent);
        for (uint32_t i = 0; i < child_count; ++i) {
            lv_obj_t* child = lv_obj_get_child(lv_parent, i);
            if (child && lv_obj_check_type(child, &lv_btn_class)) {
                button_set_active(child, false);
            }
        }
    }

    return last_button;
}

