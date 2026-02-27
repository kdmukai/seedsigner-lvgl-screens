#include "components.h"
#include "gui_constants.h"
#include "lvgl.h"


extern "C" const top_nav_ctx_t TOP_NAV_CTX_DEFAULTS = {
    .title = "My Title",
    .show_back_button = true,
    .show_power_button = false,
};

lv_obj_t* top_nav(const top_nav_ctx_t *ctx) {
    if (!ctx) {
        ctx = &TOP_NAV_CTX_DEFAULTS;
    }

    lv_obj_t* lv_parent = lv_scr_act();
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


void button_toggle_callback(lv_event_t* e) {
    lv_obj_t* btn = lv_event_get_target(e);
    lv_obj_t* parent = lv_obj_get_parent(btn);

    // Enforce single-select behavior: deactivate all sibling buttons first.
    if (parent) {
        uint32_t child_count = lv_obj_get_child_cnt(parent);
        for (uint32_t i = 0; i < child_count; ++i) {
            lv_obj_t* child = lv_obj_get_child(parent, i);
            if (!child || child == btn) {
                continue;
            }
            // Only apply button active-state styling to actual buttons.
            if (lv_obj_check_type(child, &lv_btn_class)) {
                button_set_active(child, false);
            }
        }
    }

    // Keep clicked button active (no toggle-off on repeat click).
    button_set_active(btn, true);
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

    // Wire up the toggle callback
    lv_obj_add_event_cb(lv_button, button_toggle_callback, LV_EVENT_CLICKED, NULL);

    // Default to inactive state
    button_set_active(lv_button, false);

    return lv_button;
}

lv_obj_t* button_list(lv_obj_t* lv_parent, const button_list_item_t *items, size_t item_count) {
    lv_obj_t* last_button = NULL;
    if (!lv_parent || !items || item_count == 0) {
        return last_button;
    }

    for (size_t i = 0; i < item_count; ++i) {
        const char *label = items[i].label ? items[i].label : "";
        last_button = button(lv_parent, label, last_button);
    }

    return last_button;
}

