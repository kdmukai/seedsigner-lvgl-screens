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

// PSBTMathScreen: the fee "math" — a right-aligned, fixed-width equation of the input
// total minus recipients minus fee, ruled off, equalling the change. In btc mode the
// trailing satoshi digits dim through three grays (Python's supersampled digit zones);
// the change line's unit is called out in orange. The host passes each amount as an
// already-formatted (unpadded) number string plus the denomination flag; this screen
// pads them to a common width so the monospace columns line up, applies the +/- signs,
// and colors the zones.
//
// cfg:
//   amounts { input, spend, fee, change }  — host-formatted number strings (unpadded).
//   denomination ("btc" | "sats")          — selects the 3-zone digit dimming.
//   num_recipients (int)                    — >0 renders the "- spend" recipients row.
//   labels { inputs, recipients, fee, change } — host-localized (already pluralized) info
//                                            words drawn after each amount.
//   button_list (array)                     — default ["Review recipients"].
void psbt_math_screen(void *ctx_json) {
    const char *json_str = (const char *)ctx_json;
    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    const json amounts = (cfg.contains("amounts") && cfg["amounts"].is_object()) ? cfg["amounts"] : json::object();
    std::string s_input  = amounts.value("input",  std::string("0"));
    std::string s_spend  = amounts.value("spend",  std::string("0"));
    std::string s_fee    = amounts.value("fee",    std::string("0"));
    std::string s_change = amounts.value("change", std::string("0"));

    bool is_btc = cfg.value("denomination", std::string("sats")) == "btc";
    int  num_recipients = cfg.value("num_recipients", 1);

    const json labels = (cfg.contains("labels") && cfg["labels"].is_object()) ? cfg["labels"] : json::object();
    std::string l_inputs     = labels.value("inputs",     std::string("inputs"));
    std::string l_recipients = labels.value("recipients", std::string("recipients"));
    std::string l_fee        = labels.value("fee",        std::string("fee"));
    std::string l_change     = labels.value("change",     std::string("change"));

    if (!cfg.contains("top_nav") || !cfg["top_nav"].is_object()) cfg["top_nav"] = json::object();
    if (!cfg["top_nav"].contains("title")) cfg["top_nav"]["title"] = "Transaction Math";
    cfg["is_bottom_list"] = true;
    if (!cfg.contains("button_list")) cfg["button_list"] = json::array({ "Review recipients" });

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, false);

    const int32_t W = lv_display_get_horizontal_resolution(NULL);
    const lv_font_t *digit_font = &KEYBOARD_FONT;   // profile fixed-width (Inconsolata)

    // Fixed-width metrics: one monospace advance + the digit line height.
    lv_point_t sz10;
    lv_text_get_size(&sz10, "0000000000", digit_font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int32_t char_width = sz10.x / 10; if (char_width < 1) char_width = 1;
    int32_t digit_h    = lv_font_get_line_height(digit_font);

    // Left-pad all four amounts to a common width so the monospace digits right-align
    // (Python pads to the longest before prefixing the +/- sign).
    size_t longest = std::max(std::max(s_input.size(), s_spend.size()),
                              std::max(s_fee.size(), s_change.size()));
    auto pad = [&](const std::string &s) {
        return s.size() >= longest ? s : std::string(longest - s.size(), ' ') + s;
    };
    s_input = pad(s_input); s_spend = pad(s_spend); s_fee = pad(s_fee); s_change = pad(s_change);

    int32_t dgs          = LIST_ITEM_PADDING / 2; if (dgs < 1) dgs = 1;   // digit-group gap
    int32_t digits_width = (int32_t)(longest + 1) * char_width;           // sign + digits
    int32_t info_x       = digits_width + 3 * dgs;                        // info text column
    int32_t row_advance  = digit_h + BODY_LINE_SPACING;

    // Measure the equation's actual content width — the digits column (info_x) plus the
    // widest info word — so a block narrower than the body is CENTERED horizontally on
    // wide screens instead of hugging the left edge. Only the info words actually rendered
    // are measured (the recipients row is dropped on self-transfers).
    auto body_text_w = [&](const std::string &s) -> int32_t {
        lv_point_t sz;
        lv_text_get_size(&sz, s.c_str(), &BODY_FONT, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        return sz.x;
    };
    int32_t max_info_w = std::max(body_text_w(l_inputs),
                                  std::max(body_text_w(l_fee), body_text_w(l_change)));
    if (num_recipients > 0) max_info_w = std::max(max_info_w, body_text_w(l_recipients));

    const int32_t content_w = info_x + max_info_w;   // digits column + info column
    const int32_t body_w    = W - 2 * EDGE_PADDING;

    // Horizontal centering: mc spans the FULL body width (so the info labels always have
    // room and a shaped script is never width-capped into a wrap), and the block is
    // centered by shifting every element right by center_off when it fits.
    int32_t center_off = (body_w - content_w) / 2;
    if (center_off < 0) center_off = 0;

    // Equation body container. Its y is a placeholder here — the final y is set below once
    // the block height is known (vertical centering). It floats over the scaffold's
    // (empty) upper_body; the button stays pinned at the bottom.
    lv_obj_t *mc = lv_obj_create(screen.screen);
    lv_obj_remove_style_all(mc);
    lv_obj_set_pos(mc, EDGE_PADDING, TOP_NAV_HEIGHT + COMPONENT_PADDING);
    lv_obj_set_width(mc, body_w);
    lv_obj_set_height(mc, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(mc, 0, LV_PART_MAIN);
    lv_obj_remove_flag(mc, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(mc, LV_OBJ_FLAG_CLICKABLE);

    // Fixed-width digit label (Latin monospace, exact metrics — never wraps).
    auto add_digits = [&](uint32_t color, const std::string &text, int32_t x, int32_t y) {
        lv_obj_t *lbl = lv_label_create(mc);
        lv_obj_set_style_pad_all(lbl, 0, LV_PART_MAIN);
        lv_obj_set_style_text_font(lbl, digit_font, LV_PART_MAIN);
        lv_obj_set_style_text_color(lbl, lv_color_hex(color), LV_PART_MAIN);
        lv_label_set_text(lbl, text.c_str());
        lv_obj_set_pos(lbl, center_off + x, y);
    };

    // Info word (locale-aware BODY_FONT). Forced SINGLE-LINE (LV_LABEL_LONG_CLIP) with an
    // explicit width = the room to its right, so a long SHAPED translation (e.g. the Thai
    // word for "fee") stays on one line instead of wrapping onto the rule-off line below
    // it. It clips only in the extreme case where even the full remaining width can't hold
    // it — a graceful failure vs. the overlapping wrap.
    auto add_info = [&](uint32_t color, const std::string &text, int32_t y) {
        lv_obj_t *lbl = lv_label_create(mc);
        lv_obj_set_style_pad_all(lbl, 0, LV_PART_MAIN);
        lv_obj_set_style_text_font(lbl, &BODY_FONT, LV_PART_MAIN);
        lv_obj_set_style_text_color(lbl, lv_color_hex(color), LV_PART_MAIN);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(lbl, body_w - center_off - info_x);
        lv_label_set_text(lbl, text.c_str());
        lv_obj_set_pos(lbl, center_off + info_x, y);
    };

    // Render one equation row: the signed amount (dimmed satoshi zones in btc mode) then
    // the info word one column over.
    auto render_amount = [&](int32_t y, const std::string &amount, const std::string &info, uint32_t info_color) {
        if (is_btc && amount.size() > 6) {
            // Split the last 6 characters into two 3-digit zones, dimming each further
            // (Python's secondary #888 / tertiary #666 supersampled digit groups).
            std::string main_zone = amount.substr(0, amount.size() - 6);
            std::string mid_zone  = amount.substr(amount.size() - 6, 3);
            std::string end_zone  = amount.substr(amount.size() - 3);
            int32_t main_w = (int32_t)main_zone.size() * char_width;
            add_digits((uint32_t)BODY_FONT_COLOR, main_zone, 0, y);
            add_digits(0x888888, mid_zone, main_w + dgs, y);
            add_digits(0x666666, end_zone, main_w + dgs + 3 * char_width + dgs, y);
        } else {
            add_digits((uint32_t)BODY_FONT_COLOR, amount, 0, y);
        }
        add_info(info_color, info, y);
    };

    int32_t cur_y = 0;
    render_amount(cur_y, std::string(" ") + s_input, l_inputs, (uint32_t)BODY_FONT_COLOR);

    // The spend line is omitted on self-transfers (no external recipient).
    if (num_recipients > 0) {
        cur_y += row_advance;
        render_amount(cur_y, std::string("-") + s_spend, l_recipients, (uint32_t)BODY_FONT_COLOR);
    }

    cur_y += row_advance;
    render_amount(cur_y, std::string("-") + s_fee, l_fee, (uint32_t)BODY_FONT_COLOR);

    // Rule-off line (aligned + sized to the centered block), then the change total below.
    cur_y += row_advance;
    int32_t divider_h = LIST_ITEM_PADDING / 4; if (divider_h < 1) divider_h = 1;
    lv_obj_t *divider = lv_obj_create(mc);
    lv_obj_remove_style_all(divider);
    lv_obj_set_size(divider, content_w, divider_h);
    lv_obj_set_style_bg_color(divider, lv_color_hex(BODY_FONT_COLOR), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_pos(divider, center_off, cur_y);

    cur_y += BODY_LINE_SPACING;
    render_amount(cur_y, std::string(" ") + s_change, l_change, 0xff8c00 /* darkorange */);

    bind_screen_navigation(cfg, screen, 0);

    // Vertical centering: now that the equation's height is known, center it in the gap
    // between the top nav and the pinned button (Python top-anchors it; here the wider
    // screens have slack to spare).
    lv_obj_update_layout(screen.screen);
    int32_t mc_h    = lv_obj_get_height(mc);
    int32_t gap_top = TOP_NAV_HEIGHT;
    int32_t gap_bot = lv_display_get_vertical_resolution(NULL) - BUTTON_HEIGHT;
    if (screen.button_list_count > 0 && lv_obj_is_valid(screen.button_list[0])) {
        lv_area_t ba; lv_obj_get_coords(screen.button_list[0], &ba);
        gap_bot = ba.y1;
    }
    int32_t mc_y = gap_top + (gap_bot - gap_top - mc_h) / 2;
    if (mc_y < gap_top + COMPONENT_PADDING) mc_y = gap_top + COMPONENT_PADDING;   // clear the nav
    lv_obj_set_y(mc, mc_y);

    load_screen_and_cleanup_previous(screen.screen);
}
