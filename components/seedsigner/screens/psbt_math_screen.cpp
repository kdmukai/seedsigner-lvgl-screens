// psbt_math_screen
//
// Python provenance: PSBTMathScreen (psbt_screens.py)
//
// PSBT signing-flow fee-"math" screen: a right-aligned, fixed-width equation of
// the input total minus the recipient spend minus the fee, ruled off, equalling
// the change — above a bottom-pinned button list with a back button. In btc
// denomination the trailing satoshi digits dim through three grays (Python's
// supersampled digit zones); the change line's info word is called out in
// darkorange. The pressed button index returns through the standard navigation
// callback.
//
// Platform contract: the host passes each amount as an already-formatted
// (unpadded) number string plus the denomination flag; this screen pads the
// amounts to a common width so the monospace columns line up, applies the +/-
// signs, and colors the digit zones. (Python's __post_init__ both formats the
// amounts AND draws the equation onto a supersampled temp image; here the
// formatting half stays host-side, where the locale lives.)
//
// Layout notes: the equation is a bespoke absolute-positioned block — one
// style-less container floated over the scaffold's (empty) upper_body, every
// child placed at hand-derived (x, y) coordinates from the monospace font
// metrics; the scaffold is used only for its chrome (top nav + pinned button).
// Documented deviations from Python:
//   - the block is vertically CENTERED in the free band between the top nav and
//     the button (Python top-anchors it at top_nav.height + COMPONENT_PADDING);
//     the wider profiles have slack to spare.
//   - the block is horizontally centered when narrower than the body width
//     (Python's pasted equation image is left-anchored at EDGE_PADDING).
//   - info words render single-line (LV_LABEL_LONG_CLIP) so a long shaped
//     translation clips gracefully instead of wrapping onto the row below.
//
// Lifecycle: Tier 1 (stateless) — no statics, timers, or heap ctx; the equation
// block is widget-tree-owned.
//
// cfg:
//   top_nav.title              (string, required)     localized screen title
//            (Python: _("Transaction Math")); read by the scaffold.
//   top_nav.show_back_button   (bool, default true)   Python ButtonListScreen
//            default; read by the scaffold.
//   top_nav.show_power_button  (bool, default false)  Python ButtonListScreen
//            default; read by the scaffold.
//   amounts                    (object, required)     host-formatted (unpadded)
//            number strings; all four sub-keys required strings: amounts.input,
//            amounts.spend, amounts.fee, amounts.change. spend is required even
//            on self-transfers — it participates in the common-width padding.
//   denomination               (string, default "sats")  "btc" selects the
//            3-zone satoshi-digit dimming; any other value renders the digits
//            plain (Python derives the denomination from the input amount's
//            magnitude rather than passing it).
//   num_recipients             (int, default 1)       >0 renders the "- spend"
//            recipients row; 0 = self-transfer, row omitted. (Python's dataclass
//            default is 0; the host passes the real recipient count.)
//   labels                     (object, required)     host-localized (already
//            pluralized) info words drawn after each amount: labels.inputs,
//            labels.fee, labels.change required strings; labels.recipients
//            required when num_recipients > 0 (unread otherwise).
//   button_list                (array, required, non-empty)  the localized
//            action buttons (Python: "Review recipients"); rendered by the
//            scaffold as the bottom-pinned list.
//   is_bottom_list             forced true (Python: is_bottom_list = True); a
//            host-supplied value is ignored.
//   initial_selected_index     (int, optional)        overrides the default initial
//            focus of 0 (navigation layer; Python selected_button).
//   input.mode                 (string, optional)     "touch" | "hardware" input-mode
//            override (navigation layer).
//   input.keys.key1/key2/key3  (string, optional)     per-aux-key policy "enter" |
//            "noop" | "emit" (navigation layer).
//   allow_screensaver          (bool, default true)   per-screen screensaver policy
//            (normalized by parse_screen_json_ctx, stamped by the scaffold).

#include "screen_scaffold.h"  // parse_screen_json_ctx / create_top_nav_screen_scaffold / bottom_button_top_y / bind_screen_navigation / load_screen_and_cleanup_previous
#include "seedsigner.h"       // psbt_math_screen decl, screen_scaffold_t fields
#include "components.h"       // monospace_char_width
#include "gui_constants.h"    // KEYBOARD_FONT, BODY_FONT, BODY_FONT_COLOR, LIST_ITEM_PADDING, BODY_LINE_SPACING, EDGE_PADDING, TOP_NAV_HEIGHT, COMPONENT_PADDING
#include "screen_helpers.h"   // ensure_top_nav_structure, require_top_nav_title

#include "lvgl.h"             // lv_obj/lv_label creation, per-object style setters, lv_text_get_size

#include <nlohmann/json.hpp>  // json (cfg reads + structural-default writes)

#include <algorithm>          // std::max (longest-amount + widest-info-word folds)
#include <cstddef>            // size_t (amount-string lengths)
#include <stdexcept>          // std::runtime_error (required-field validation)
#include <string>             // std::string

using json = nlohmann::json;


void psbt_math_screen(void *ctx_json) {
    // --- Config ---

    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    // Required fields: the four amounts are the PSBT data this screen exists to
    // show; the info labels and button_list are user-visible CONTENT, which
    // always arrives localized from the host view layer (a string literal baked
    // here would be English-only by construction). One throw per field, before
    // the scaffold exists, so no throw path can leak LVGL objects.
    if (!cfg.contains("amounts") || !cfg["amounts"].is_object()) {
        throw std::runtime_error("psbt_math_screen: amounts is required and must be an object");
    }
    const json &amounts = cfg["amounts"];
    if (!amounts.contains("input") || !amounts["input"].is_string()) {
        throw std::runtime_error("psbt_math_screen: amounts.input is required and must be a string");
    }
    // spend participates in the common-width padding below even when the
    // recipients row itself is omitted, so it is required unconditionally.
    if (!amounts.contains("spend") || !amounts["spend"].is_string()) {
        throw std::runtime_error("psbt_math_screen: amounts.spend is required and must be a string");
    }
    if (!amounts.contains("fee") || !amounts["fee"].is_string()) {
        throw std::runtime_error("psbt_math_screen: amounts.fee is required and must be a string");
    }
    if (!amounts.contains("change") || !amounts["change"].is_string()) {
        throw std::runtime_error("psbt_math_screen: amounts.change is required and must be a string");
    }

    std::string input_amount  = amounts["input"].get<std::string>();
    std::string spend_amount  = amounts["spend"].get<std::string>();
    std::string fee_amount    = amounts["fee"].get<std::string>();
    std::string change_amount = amounts["change"].get<std::string>();

    // Structural flags (never rendered as text): "btc" turns on the 3-zone digit
    // dimming (Python derives the denomination from the amount magnitude);
    // num_recipients > 0 renders the recipients row (Python's dataclass default
    // is 0 — the host passes the real count).
    bool is_btc         = cfg.value("denomination", std::string("sats")) == "btc";
    int  num_recipients = cfg.value("num_recipients", 1);

    if (!cfg.contains("labels") || !cfg["labels"].is_object()) {
        throw std::runtime_error("psbt_math_screen: labels is required and must be an object");
    }
    const json &labels = cfg["labels"];
    if (!labels.contains("inputs") || !labels["inputs"].is_string()) {
        throw std::runtime_error("psbt_math_screen: labels.inputs is required and must be a string");
    }
    // labels.recipients is content too, but it is only rendered (and measured)
    // when num_recipients > 0 — so it is required exactly then (a self-transfer
    // cfg may legitimately omit it).
    if (num_recipients > 0 && (!labels.contains("recipients") || !labels["recipients"].is_string())) {
        throw std::runtime_error("psbt_math_screen: labels.recipients is required when num_recipients > 0 and must be a string");
    }
    if (!labels.contains("fee") || !labels["fee"].is_string()) {
        throw std::runtime_error("psbt_math_screen: labels.fee is required and must be a string");
    }
    if (!labels.contains("change") || !labels["change"].is_string()) {
        throw std::runtime_error("psbt_math_screen: labels.change is required and must be a string");
    }

    if (!cfg.contains("button_list") || !cfg["button_list"].is_array() || cfg["button_list"].empty()) {
        throw std::runtime_error("psbt_math_screen: button_list is required and must be a non-empty array");
    }

    std::string inputs_label     = labels["inputs"].get<std::string>();
    std::string recipients_label = num_recipients > 0 ? labels["recipients"].get<std::string>() : std::string();
    std::string fee_label        = labels["fee"].get<std::string>();
    std::string change_label     = labels["change"].get<std::string>();

    // Structural defaults (write-if-absent, never user-visible text). Python
    // ButtonListScreen defaults: show_back_button=True, show_power_button=False.
    // The localized title itself is content and must come from the host.
    ensure_top_nav_structure(cfg, /*default_show_back_button=*/true,
                                  /*default_show_power_button=*/false);
    require_top_nav_title(cfg, "psbt_math_screen");

    cfg["is_bottom_list"] = true;    // forced, not defaulted — Python: is_bottom_list = True

    // --- Scaffold ---

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, /*scrollable=*/false);

    // --- Body ---

    const int32_t display_width = lv_display_get_horizontal_resolution(NULL);
    const lv_font_t *digit_font = &KEYBOARD_FONT;   // profile fixed-width (Inconsolata)

    // Fixed-width metrics: one monospace advance + the digit line height.
    int32_t char_width        = monospace_char_width(digit_font);
    int32_t digit_line_height = lv_font_get_line_height(digit_font);

    // Left-pad all four amounts to a common width so the monospace digits
    // right-align (Python pads to the longest before prefixing the +/- sign).
    size_t longest = std::max(std::max(input_amount.size(), spend_amount.size()),
                              std::max(fee_amount.size(), change_amount.size()));
    auto pad = [&](const std::string &amount) {
        return amount.size() >= longest ? amount : std::string(longest - amount.size(), ' ') + amount;
    };
    input_amount  = pad(input_amount);
    spend_amount  = pad(spend_amount);
    fee_amount    = pad(fee_amount);
    change_amount = pad(change_amount);

    // Column geometry, all derived from the monospace advance: the digit columns
    // (one leading sign cell + the padded digits), then the info-word column a
    // few digit-group gaps to the right (Python: digits_width + 3 *
    // digit_group_spacing).
    int32_t digit_group_gap = LIST_ITEM_PADDING / 2;
    if (digit_group_gap < 1) digit_group_gap = 1;
    int32_t digits_width = (int32_t)(longest + 1) * char_width;          // sign + digits
    int32_t info_x       = digits_width + 3 * digit_group_gap;           // info text column
    int32_t row_advance  = digit_line_height + BODY_LINE_SPACING;

    // Measure the equation's actual content width — the digits column (info_x)
    // plus the widest info word — so a block narrower than the body is CENTERED
    // horizontally on wide screens instead of hugging the left edge. Only the
    // info words actually rendered are measured (the recipients row is dropped
    // on self-transfers).
    auto body_text_width = [&](const std::string &text) -> int32_t {
        lv_point_t text_size;
        lv_text_get_size(&text_size, text.c_str(), &BODY_FONT, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        return text_size.x;
    };
    int32_t max_info_width = std::max(body_text_width(inputs_label),
                                      std::max(body_text_width(fee_label), body_text_width(change_label)));
    if (num_recipients > 0) max_info_width = std::max(max_info_width, body_text_width(recipients_label));

    const int32_t content_width = info_x + max_info_width;   // digits column + info column
    const int32_t body_width    = display_width - 2 * EDGE_PADDING;

    // Horizontal centering: the container spans the FULL body width (so the info
    // labels always have room and a shaped script is never width-capped into a
    // wrap), and the block is centered by shifting every element right by
    // center_offset when it fits.
    int32_t center_offset = (body_width - content_width) / 2;
    if (center_offset < 0) center_offset = 0;

    // 1. Equation body container. Its y is a placeholder here — the final y is
    //    set in the Geometry pass below, once the block height is known
    //    (vertical centering). Built on the SCREEN ROOT rather than inside the
    //    flex upper_body because every child is placed at absolute coordinates;
    //    it floats over the scaffold's (empty) upper_body while the button stays
    //    pinned at the bottom.
    lv_obj_t *math_container = lv_obj_create(screen.screen);
    lv_obj_remove_style_all(math_container);
    lv_obj_set_pos(math_container, EDGE_PADDING, TOP_NAV_HEIGHT + COMPONENT_PADDING);
    lv_obj_set_width(math_container, body_width);
    lv_obj_set_height(math_container, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(math_container, 0, LV_PART_MAIN);
    lv_obj_remove_flag(math_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(math_container, LV_OBJ_FLAG_CLICKABLE);

    // Fixed-width digit label (Latin monospace, exact metrics — never wraps).
    auto add_digits = [&](uint32_t color, const std::string &text, int32_t x, int32_t y) {
        lv_obj_t *label = lv_label_create(math_container);
        lv_obj_set_style_pad_all(label, 0, LV_PART_MAIN);
        lv_obj_set_style_text_font(label, digit_font, LV_PART_MAIN);
        lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
        lv_label_set_text(label, text.c_str());
        lv_obj_set_pos(label, center_offset + x, y);
    };

    // Info word (locale-aware BODY_FONT). Forced SINGLE-LINE (LV_LABEL_LONG_CLIP)
    // with an explicit width = the room to its right, so a long SHAPED
    // translation (e.g. the Thai word for "fee") stays on one line instead of
    // wrapping onto the rule-off line below it. It clips only in the extreme
    // case where even the full remaining width can't hold it — a graceful
    // failure vs. the overlapping wrap.
    auto add_info = [&](uint32_t color, const std::string &text, int32_t y) {
        lv_obj_t *label = lv_label_create(math_container);
        lv_obj_set_style_pad_all(label, 0, LV_PART_MAIN);
        lv_obj_set_style_text_font(label, &BODY_FONT, LV_PART_MAIN);
        lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
        lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(label, body_width - center_offset - info_x);
        lv_label_set_text(label, text.c_str());
        lv_obj_set_pos(label, center_offset + info_x, y);
    };

    // Render one equation row: the signed amount (dimmed satoshi zones in btc
    // mode) then the info word one column over.
    auto render_amount = [&](int32_t y, const std::string &amount, const std::string &info, uint32_t info_color) {
        if (is_btc && amount.size() > 6) {
            // Split the last 6 characters into two 3-digit zones, dimming each
            // further (Python's secondary #888 / tertiary #666 supersampled
            // digit groups).
            std::string main_zone = amount.substr(0, amount.size() - 6);
            std::string mid_zone  = amount.substr(amount.size() - 6, 3);
            std::string end_zone  = amount.substr(amount.size() - 3);
            int32_t main_zone_width = (int32_t)main_zone.size() * char_width;
            add_digits((uint32_t)BODY_FONT_COLOR, main_zone, 0, y);
            add_digits(0x888888, mid_zone, main_zone_width + digit_group_gap, y);
            add_digits(0x666666, end_zone, main_zone_width + digit_group_gap + 3 * char_width + digit_group_gap, y);
        } else {
            add_digits((uint32_t)BODY_FONT_COLOR, amount, 0, y);
        }
        add_info(info_color, info, y);
    };

    // 2. Input-total row (Python: f" {input_amount}" + the pluralized inputs word).
    int32_t current_y = 0;
    render_amount(current_y, std::string(" ") + input_amount, inputs_label, (uint32_t)BODY_FONT_COLOR);

    // 3. Recipient-spend row — omitted on self-transfers (no external recipient).
    if (num_recipients > 0) {
        current_y += row_advance;
        render_amount(current_y, std::string("-") + spend_amount, recipients_label, (uint32_t)BODY_FONT_COLOR);
    }

    // 4. Fee row.
    current_y += row_advance;
    render_amount(current_y, std::string("-") + fee_amount, fee_label, (uint32_t)BODY_FONT_COLOR);

    // 5. Rule-off line (aligned + sized to the centered block) — a bare
    //    background-colored object standing in for Python's draw.line().
    current_y += row_advance;
    int32_t divider_height = LIST_ITEM_PADDING / 4;
    if (divider_height < 1) divider_height = 1;
    lv_obj_t *divider = lv_obj_create(math_container);
    lv_obj_remove_style_all(divider);
    lv_obj_set_size(divider, content_width, divider_height);
    lv_obj_set_style_bg_color(divider, lv_color_hex(BODY_FONT_COLOR), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_pos(divider, center_offset, current_y);

    // 6. Change-total row, its info word called out in Python's "darkorange".
    current_y += BODY_LINE_SPACING;
    render_amount(current_y, std::string(" ") + change_amount, change_label, 0xff8c00 /* darkorange */);

    // --- Geometry ---

    // Vertical-centering pass — now that the equation's height is known, center
    // the block in the gap between the top nav and the pinned button (Python
    // top-anchors it; here the wider screens have slack to spare), clamped so it
    // never rises into the nav.
    lv_obj_update_layout(screen.screen);
    int32_t math_container_height = lv_obj_get_height(math_container);
    int32_t gap_top    = TOP_NAV_HEIGHT;
    int32_t gap_bottom = bottom_button_top_y(screen);
    int32_t math_container_y = gap_top + (gap_bottom - gap_top - math_container_height) / 2;
    if (math_container_y < gap_top + COMPONENT_PADDING) math_container_y = gap_top + COMPONENT_PADDING;   // clear the nav
    lv_obj_set_y(math_container, math_container_y);

    // --- Navigation + load ---

    // Menu-style default index: an action list always has a selection, so the
    // first button starts focused (the host may override via cfg
    // initial_selected_index).
    bind_screen_navigation(cfg, screen, /*default_initial_index=*/0);

    load_screen_and_cleanup_previous(screen.screen);
}
