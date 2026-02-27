#include "seedsigner.h"
#include "components.h"
#include "gui_constants.h"

#include "lvgl.h"


void demo_screen(void *ctx)
{
    top_nav_ctx_t top_nav_ctx = TOP_NAV_CTX_DEFAULTS;
    top_nav_ctx.title = "Settings";

    // Create a new screen
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(BACKGROUND_COLOR), LV_PART_MAIN); // Set background color to black

    // Set global style defaults
    lv_obj_set_style_radius(scr, 0, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(scr, BODY_LINE_SPACING, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(scr, 0, LV_PART_MAIN);

    // Load the new screen
    lv_scr_load(scr);

    lv_obj_t* lv_seedsigner_top_nav = top_nav(&top_nav_ctx); // Add top navigation bar

    lv_obj_t* body_content = lv_obj_create(scr);
    lv_obj_set_size(body_content, lv_obj_get_width(scr), lv_obj_get_height(scr) - TOP_NAV_HEIGHT - COMPONENT_PADDING);
    lv_obj_align_to(body_content, lv_seedsigner_top_nav, LV_ALIGN_OUT_BOTTOM_MID, 0, COMPONENT_PADDING);
    lv_obj_set_style_bg_color(body_content, lv_color_hex(BACKGROUND_COLOR), LV_PART_MAIN);
    lv_obj_set_style_pad_left(body_content, EDGE_PADDING, LV_PART_MAIN);
    lv_obj_set_style_pad_right(body_content, EDGE_PADDING, LV_PART_MAIN);
    lv_obj_set_style_pad_top(body_content, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(body_content, COMPONENT_PADDING, LV_PART_MAIN);

    // debugging
    // lv_obj_set_style_border_color(body_content, lv_color_hex(BUTTON_BACKGROUND_COLOR), LV_PART_MAIN);
    lv_obj_set_style_border_width(body_content, 0, LV_PART_MAIN);

    // Limit scrolling to the vertical direction only
    lv_obj_set_scroll_dir(body_content, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body_content, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_pad_right(body_content, 0, LV_PART_SCROLLBAR);

    static const button_list_item_t demo_buttons[] = {
        { .label = "Language", .value = NULL },
        { .label = "Persistent Settings", .value = NULL },
        { .label = "Another option", .value = NULL },
        { .label = "Wow so many options", .value = NULL },
        { .label = "Continue", .value = NULL },
    };
    lv_obj_t* lv_seedsigner_button = button_list(body_content, demo_buttons, sizeof(demo_buttons) / sizeof(demo_buttons[0]));
    button_set_active(lv_seedsigner_button, true);

    lv_obj_t* lv_body_text = lv_label_create(body_content);
    lv_obj_set_width(lv_body_text, lv_obj_get_width(body_content) - 2 * COMPONENT_PADDING);
    lv_obj_align_to(lv_body_text, lv_seedsigner_button, LV_ALIGN_OUT_BOTTOM_LEFT, 0, COMPONENT_PADDING);
    lv_obj_set_style_text_color(lv_body_text, lv_color_hex(BODY_FONT_COLOR), 0);
    lv_obj_set_style_text_font(lv_body_text, &BODY_FONT, LV_PART_MAIN);
    lv_label_set_text(lv_body_text, "Long the Paris streets, the death-carts rumble, hollow and harsh. Six tumbrils carry the day's wine to La Guillotine. All the devouring and insatiate Monsters imagined since imagination could record itself, are fused in the one realisation, Guillotine. And yet there is not in France, with its rich variety of soil and climate, a blade, a leaf, a root, a sprig, a peppercorn, which will grow to maturity under conditions more certain than those that have produced this horror. Crush humanity out of shape once more, under similar hammers, and it will twist itself into the same tortured forms. Sow the same seed of rapacious license and oppression over again, and it will surely yield the same fruit according to its kind.\n\nSix tumbrils roll along the streets. Change these back again to what they were, thou powerful enchanter, Time, and they shall be seen to be the carriages of absolute monarchs, the equipages of feudal nobles, the toilettes of flaring Jezebels, the churches that are not my Father's house but dens of thieves, the huts of millions of starving peasants! No; the great magician who majestically works out the appointed order of the Creator, never reverses his transformations. \"If thou be changed into this shape by the will of God,\" say the seers to the enchanted, in the wise Arabian stories, \"then remain so! But, if thou wear this form through mere passing conjuration, then resume thy former aspect!\" Changeless and hopeless, the tumbrils roll along.");
}



void button_list_screen(void *ctx)
{
    static const button_list_screen_ctx_t defaults = {
        .top_nav = {
            .title = "Settings",
            .show_back_button = false,
            .show_power_button = false,
        },
    };

    const button_list_screen_ctx_t *screen_ctx = (const button_list_screen_ctx_t *)ctx;
    if (!screen_ctx) {
        screen_ctx = &defaults;
    }

    // Create a new screen
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(BACKGROUND_COLOR), LV_PART_MAIN); // Set background color to black

    // Set global style defaults
    lv_obj_set_style_radius(scr, 0, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(scr, BODY_LINE_SPACING, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(scr, 0, LV_PART_MAIN);

    // Load the new screen
    lv_scr_load(scr);

    lv_obj_t* lv_seedsigner_top_nav = top_nav(&screen_ctx->top_nav); // Add top navigation bar

    lv_obj_t* body_content = lv_obj_create(scr);
    lv_obj_set_size(body_content, lv_obj_get_width(scr), lv_obj_get_height(scr) - TOP_NAV_HEIGHT - COMPONENT_PADDING);
    lv_obj_align_to(body_content, lv_seedsigner_top_nav, LV_ALIGN_OUT_BOTTOM_MID, 0, COMPONENT_PADDING);
    lv_obj_set_style_bg_color(body_content, lv_color_hex(BACKGROUND_COLOR), LV_PART_MAIN);
    lv_obj_set_style_pad_left(body_content, EDGE_PADDING, LV_PART_MAIN);
    lv_obj_set_style_pad_right(body_content, EDGE_PADDING, LV_PART_MAIN);
    lv_obj_set_style_pad_top(body_content, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(body_content, COMPONENT_PADDING, LV_PART_MAIN);

    // debugging
    // lv_obj_set_style_border_color(body_content, lv_color_hex(BUTTON_BACKGROUND_COLOR), LV_PART_MAIN);
    lv_obj_set_style_border_width(body_content, 0, LV_PART_MAIN);

    // Limit scrolling to the vertical direction only
    lv_obj_set_scroll_dir(body_content, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body_content, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_pad_right(body_content, 0, LV_PART_SCROLLBAR);

    lv_obj_t* lv_seedsigner_button = NULL;
    lv_seedsigner_button = button_list(body_content, screen_ctx->button_list, screen_ctx->button_list_len);
    button_set_active(lv_seedsigner_button, true);
}


void lv_seedsigner_screen_close(void)
{
    /*Delete all animation*/
    lv_anim_del(NULL, NULL);

    // lv_timer_del(meter2_timer);
    // meter2_timer = NULL;

    lv_obj_clean(lv_scr_act());

    // lv_style_reset(&style_text_muted);
    // lv_style_reset(&style_title);
    // lv_style_reset(&style_icon);
    // lv_style_reset(&style_bullet);
}


