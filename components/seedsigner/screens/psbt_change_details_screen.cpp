#include "seedsigner.h"
#include "screen_scaffold.h"
#include "screen_helpers.h"
#include "components.h"
#include "gui_constants.h"
#include "navigation.h"

#include "lvgl.h"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <stdexcept>

using json = nlohmann::json;

// PSBTChangeDetailsScreen: the change (or self-receive) output — amount, an address-type
// label ("change address #N"), the single-line address, and an optional "Address
// verified!" confirmation centered in the space above the button. Top-anchored (Python
// pins the stack under the top nav rather than centering it).
//
// cfg:
//   top_nav.title              — screen title (default "Your Change").
//   btc_amount { ... }         — the change amount.
//   address (string, req.)     — the change/self-receive address.
//   address_type_label (str)   — host-formatted "change address #0" / "receive address #5".
//   is_verified (bool)         — show the "Address verified!" confirmation.
//   verified_text (str)        — host-localized confirmation (default "Address verified!").
//   button_list (array)        — action buttons.
void psbt_change_details_screen(void *ctx_json) {
    const char *json_str = (const char *)ctx_json;
    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    if (!cfg.contains("address") || !cfg["address"].is_string()) {
        throw std::runtime_error("psbt_change_details_screen requires an \"address\" string");
    }
    std::string address       = cfg["address"].get<std::string>();
    std::string type_label    = cfg.value("address_type_label", std::string("change address #0"));
    bool        is_verified   = cfg.value("is_verified", false);
    std::string verified_text = cfg.value("verified_text", std::string("Address verified!"));

    if (!cfg.contains("top_nav") || !cfg["top_nav"].is_object()) cfg["top_nav"] = json::object();
    if (!cfg["top_nav"].contains("title")) cfg["top_nav"]["title"] = "Your Change";
    cfg["is_bottom_list"] = true;
    if (!cfg.contains("button_list")) cfg["button_list"] = json::array({ "Done" });

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, false);

    const int32_t W = lv_display_get_horizontal_resolution(NULL);

    // Top-anchored stack (default upper_body flow, cross-centered). Python spacing:
    // amount COMPONENT_PADDING below the nav, the type label COMPONENT_PADDING below the
    // amount, and the address directly under the label. Zero the row gap and place the
    // label's own top margin so the two gaps differ.
    lv_obj_set_style_pad_top(screen.upper_body, COMPONENT_PADDING, LV_PART_MAIN);
    lv_obj_set_style_pad_row(screen.upper_body, 0, LV_PART_MAIN);

    if (cfg.contains("btc_amount") && cfg["btc_amount"].is_object()) {
        btc_amount_from_cfg(screen.upper_body, cfg["btc_amount"]);
    }

    // Small gray "change address #N" / "receive address #N" label. Body font (locale-
    // aware) in the label color, matching the seed_finalize label precedent.
    lv_obj_t *tlabel = lv_label_create(screen.upper_body);
    lv_label_set_text(tlabel, type_label.c_str());
    lv_obj_set_style_text_font(tlabel, &BODY_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(tlabel, lv_color_hex(LABEL_FONT_COLOR), LV_PART_MAIN);
    lv_obj_set_style_pad_all(tlabel, 0, LV_PART_MAIN);
    lv_obj_set_style_margin_top(tlabel, COMPONENT_PADDING, LV_PART_MAIN);

    // Single-line head…tail address (Python max_lines=1).
    formatted_address_opts_t fo = {};
    fo.address      = address.c_str();
    fo.width        = W - 2 * EDGE_PADDING;
    fo.max_lines    = 1;
    fo.accent_color = network_color(resolve_address_network(cfg, address));   // head/tail = network color
    fo.base_color   = SEEDSIGNER_ICON_COLOR_DEFAULT;
    lv_obj_t *addr = formatted_address(screen.upper_body, &fo);

    // Optional "Address verified!" line — a green success glyph + the confirmation text,
    // centered in the gap between the address and the button (Python auto-centers its
    // IconTextLine within that available height). Built on the non-flex screen root and
    // positioned by measurement so it floats mid-gap rather than hugging the address.
    lv_obj_t *vrow = nullptr;
    if (is_verified) {
        vrow = lv_obj_create(screen.screen);
        lv_obj_remove_style_all(vrow);
        lv_obj_set_size(vrow, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_layout(vrow, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(vrow, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(vrow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(vrow, COMPONENT_PADDING / 2, LV_PART_MAIN);
        lv_obj_remove_flag(vrow, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(vrow, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *vic = lv_label_create(vrow);
        lv_label_set_text(vic, SeedSignerIconConstants::SUCCESS);
        lv_obj_set_style_text_font(vic, &ICON_FONT__SEEDSIGNER, LV_PART_MAIN);
        lv_obj_set_style_text_color(vic, lv_color_hex(SUCCESS_COLOR), LV_PART_MAIN);
        lv_obj_set_style_pad_all(vic, 0, LV_PART_MAIN);

        lv_obj_t *vtx = lv_label_create(vrow);
        lv_label_set_text(vtx, verified_text.c_str());
        lv_obj_set_style_text_font(vtx, &BODY_FONT, LV_PART_MAIN);
        lv_obj_set_style_text_color(vtx, lv_color_hex(BODY_FONT_COLOR), LV_PART_MAIN);
        lv_obj_set_style_pad_all(vtx, 0, LV_PART_MAIN);
        // Force SINGLE-LINE. This is a one-line confirmation, but the default
        // LV_LABEL_LONG_WRAP makes the shaped-locale run layer wrap the run to the
        // label's content width (glyph_runs attach_runs) — and inside this tight
        // LV_SIZE_CONTENT row that width is narrow, so a long SHAPED translation
        // (e.g. Thai "ตรวจสอบที่อยู่แล้ว!") wraps and only line 0 survives in the
        // one-line-tall box. CLIP keeps wrap_width 0 so the whole run stays on one
        // line (same fix as the PSBTMath info word). The row stays content-sized,
        // so the icon+text unit still measures/centers as a tight block below.
        lv_label_set_long_mode(vtx, LV_LABEL_LONG_CLIP);
    }

    bind_screen_navigation(cfg, screen, 0);

    // With the scaffold + content laid out, center the verified line between the address
    // bottom and the first button top.
    if (vrow) {
        lv_obj_update_layout(screen.screen);
        lv_area_t aa; lv_obj_get_coords(addr, &aa);
        int32_t gap_top    = aa.y2;
        int32_t gap_bottom = bottom_button_top_y(screen);
        int32_t vh = lv_obj_get_height(vrow);
        int32_t vw = lv_obj_get_width(vrow);
        lv_obj_set_pos(vrow, (W - vw) / 2, (gap_top + gap_bottom) / 2 - vh / 2);
    }

    load_screen_and_cleanup_previous(screen.screen);
}
