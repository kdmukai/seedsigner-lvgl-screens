// psbt_change_details_screen
//
// Python provenance: PSBTChangeDetailsScreen (psbt_screens.py)
//
// PSBT signing-flow detail screen for one change (or self-transfer) output: a
// bottom-pinned button-list screen with a back button, showing a top-anchored
// stack under the top nav:
//
//   1. the output amount headline (Python: BtcAmount at
//      screen_y = top_nav.height + COMPONENT_PADDING).
//   2. a small gray address-type caption — "change address #0" /
//      "receive address #5" (Python: TextArea in LABEL_FONT_COLOR, a
//      COMPONENT_PADDING below the amount).
//   3. the single-line head…tail address, its verifiable head/tail tinted the
//      network accent color (Python: FormattedAddress(max_lines=1) directly
//      under the caption).
//   4. when the host has verified the change address against its own wallet:
//      a green SUCCESS glyph + "Address verified!" line, vertically centered
//      in the free band between the address and the bottom button (Python:
//      IconTextLine(height=available_y) auto-centering).
//
// Layout notes: the stack is TOP-anchored — Python pins it under the top nav
// rather than centering it — and its two gaps are unequal (COMPONENT_PADDING
// above the amount and above the caption, zero above the address), so the flex
// column zeroes pad_row and carries each gap on the child's own margin_top.
// The verified row is built on the SCREEN ROOT (inside the flex upper_body it
// would stack directly under the address) and centered by a post-layout
// measurement pass — the measured-gap variant of the PSBT family's
// "center in the band above the button" techniques.
//
// Lifecycle: Tier 1 (stateless) — no statics, timers, or heap ctx; all state is
// widget-tree-owned.
//
// cfg:
//   top_nav.title              (string, required)     localized screen title (the host
//            passes "Your Change" or "Self-Transfer" depending on the output type).
//   top_nav.show_back_button   (bool, default true)   Python ButtonListScreen default.
//   top_nav.show_power_button  (bool, default false)  Python ButtonListScreen default.
//   btc_amount                 (object, required)     the output amount headline;
//            sub-keys (primary/secondary/unit/network/icon_color) per btc_amount_from_cfg.
//   address                    (string, required)     the change/self-transfer address.
//   address_type_label         (string, required)     localized, pre-composed
//            "change address #N" / "receive address #N" caption (Python composes it
//            from is_change_derivation_path + derivation_path_addr_index via gettext;
//            the LVGL host supplies the finished string).
//   is_verified                (bool, default false)  build the verified-confirmation
//            row (Python: is_change_addr_verified = False).
//   verified_text              (string, required when is_verified)  localized
//            confirmation line (Python: _("Address verified!")); unread when
//            is_verified is false.
//   button_list                (array, required, non-empty)  the localized action
//            buttons (Python: "Next", or the multisig "Verify …" / "Skip …" pair).
//   network                    (string, optional)     device network code ("M"/"T"/"R");
//            selects the address head/tail accent color via network_color()
//            (absent → mainnet "M"). Per D-6 the host decides the network; the screen
//            does no address parsing.
//   initial_selected_index     (int, optional)        overrides the default initial
//            focus of 0 (navigation layer; Python selected_button).
//   input.mode                 (string, optional)     "touch" | "hardware" input-mode
//            override (navigation layer).
//   input.keys.key1/key2/key3  (string, optional)     per-aux-key policy "enter" |
//            "noop" | "emit" (navigation layer).
//   allow_screensaver          (bool, default true)   per-screen screensaver policy
//            (normalized by parse_screen_json_ctx, stamped by the scaffold).

#include "screen_scaffold.h"  // parse_screen_json_ctx / create_top_nav_screen_scaffold / bottom_button_top_y / bind_screen_navigation / load_screen_and_cleanup_previous
#include "seedsigner.h"       // psbt_change_details_screen decl
#include "components.h"       // formatted_address + formatted_address_opts_t, SEEDSIGNER_ICON_COLOR_DEFAULT
#include "gui_constants.h"    // COMPONENT_PADDING, EDGE_PADDING, BODY_FONT, ICON_FONT__SEEDSIGNER, LABEL_FONT_COLOR, BODY_FONT_COLOR, SUCCESS_COLOR, SeedSignerIconConstants
#include "screen_helpers.h"   // ensure_top_nav_structure, require_top_nav_title, btc_amount_from_cfg, network_color

#include "lvgl.h"             // lv_obj/lv_label creation + per-object style setters

#include <nlohmann/json.hpp>  // json (cfg reads + structural-default writes)

#include <stdexcept>          // std::runtime_error (required-field validation)
#include <string>             // std::string

using json = nlohmann::json;


void psbt_change_details_screen(void *ctx_json) {
    // --- Config ---

    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    // Required fields: btc_amount + address are the PSBT output data this screen
    // exists to show; address_type_label and button_list are user-visible CONTENT,
    // which always arrives localized from the host view layer (a string literal
    // baked here would be English-only by construction). One throw per field,
    // before the scaffold exists, so no throw path can leak LVGL objects.
    if (!cfg.contains("btc_amount") || !cfg["btc_amount"].is_object()) {
        throw std::runtime_error("psbt_change_details_screen: btc_amount is required and must be an object");
    }
    if (!cfg.contains("address") || !cfg["address"].is_string()) {
        throw std::runtime_error("psbt_change_details_screen: address is required and must be a string");
    }
    if (!cfg.contains("address_type_label") || !cfg["address_type_label"].is_string()) {
        throw std::runtime_error("psbt_change_details_screen: address_type_label is required and must be a string");
    }
    if (!cfg.contains("button_list") || !cfg["button_list"].is_array() || cfg["button_list"].empty()) {
        throw std::runtime_error("psbt_change_details_screen: button_list is required and must be a non-empty array");
    }

    // Structural flag (never rendered as text): Python is_change_addr_verified
    // defaults to False.
    bool is_verified = cfg.value("is_verified", false);

    // verified_text is content too, but it is only rendered when is_verified — so
    // it is required exactly then (an unverified screen may legitimately omit it).
    if (is_verified && (!cfg.contains("verified_text") || !cfg["verified_text"].is_string())) {
        throw std::runtime_error("psbt_change_details_screen: verified_text is required when is_verified and must be a string");
    }

    std::string address            = cfg["address"].get<std::string>();
    std::string address_type_label = cfg["address_type_label"].get<std::string>();
    std::string verified_text      = is_verified ? cfg["verified_text"].get<std::string>() : std::string();

    // Structural defaults (write-if-absent, never user-visible text). Python
    // ButtonListScreen defaults: show_back_button=True, show_power_button=False.
    // The localized title itself is content and must come from the host.
    ensure_top_nav_structure(cfg, /*default_show_back_button=*/true,
                                  /*default_show_power_button=*/false);
    require_top_nav_title(cfg, "psbt_change_details_screen");

    cfg["is_bottom_list"] = true;    // forced, not defaulted — Python: is_bottom_list = True

    // --- Scaffold ---

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, /*scrollable=*/false);

    // --- Body ---

    const int32_t display_width = lv_display_get_horizontal_resolution(NULL);

    // Top-anchored stack (default upper_body flow, cross-centered). Python spacing:
    // amount COMPONENT_PADDING below the nav, the type caption COMPONENT_PADDING
    // below the amount, and the address directly under the caption. Zero the row
    // gap and carry the caption's gap on its own top margin so the two gaps differ.
    lv_obj_set_style_pad_top(screen.upper_body, COMPONENT_PADDING, LV_PART_MAIN);
    lv_obj_set_style_pad_row(screen.upper_body, 0, LV_PART_MAIN);

    // 1. Output amount headline (Python: BtcAmount) via the shared PSBT amount
    //    builder — it maps btc_amount.network to the coin-icon color and renders
    //    the host-formatted display strings.
    btc_amount_from_cfg(screen.upper_body, cfg["btc_amount"]);

    // 2. Small gray "change address #N" / "receive address #N" caption. Body font
    //    (locale-aware) in the label color, matching the seed_finalize label
    //    precedent.
    lv_obj_t *type_label = lv_label_create(screen.upper_body);
    lv_label_set_text(type_label, address_type_label.c_str());
    lv_obj_set_style_text_font(type_label, &BODY_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(type_label, lv_color_hex(LABEL_FONT_COLOR), LV_PART_MAIN);
    lv_obj_set_style_pad_all(type_label, 0, LV_PART_MAIN);
    lv_obj_set_style_margin_top(type_label, COMPONENT_PADDING, LV_PART_MAIN);

    // 3. Single-line head…tail address (Python: FormattedAddress(max_lines=1)) at
    //    Python's column width (display width minus the two edge paddings). The
    //    verifiable head/tail runs carry the NETWORK accent color. Per D-6 the HOST
    //    decides the network and passes cfg["network"]; the screen owns the palette
    //    via network_color() and never infers the network from the address. Absent
    //    network defaults to mainnet.
    formatted_address_opts_t address_opts = {};
    address_opts.address      = address.c_str();
    address_opts.width        = display_width - 2 * EDGE_PADDING;
    address_opts.max_lines    = 1;
    address_opts.accent_color = network_color(cfg.value("network", std::string("M")));  // head/tail = network accent
    address_opts.base_color   = SEEDSIGNER_ICON_COLOR_DEFAULT;                           // -> LABEL_FONT_COLOR (gray prefix + middle)
    lv_obj_t *address_widget = formatted_address(screen.upper_body, &address_opts);

    // 4. Optional "Address verified!" line — a green success glyph + the
    //    confirmation text, centered in the gap between the address and the button
    //    (Python auto-centers its IconTextLine within that available height).
    //    Built on the non-flex screen root and positioned by measurement in the
    //    Geometry pass below, so it floats mid-gap rather than hugging the address.
    //
    //    Consolidation note: this hand-built icon+text row re-creates what the
    //    shared icon_text_line component provides (docs/screen-conformance-spec.md
    //    §10) and is slated for extraction to a shared component at rollout; it
    //    stays inline here to hold the conformance pixel gate.
    lv_obj_t *verified_row = nullptr;
    if (is_verified) {
        verified_row = lv_obj_create(screen.screen);
        lv_obj_remove_style_all(verified_row);
        lv_obj_set_size(verified_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_layout(verified_row, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(verified_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(verified_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(verified_row, COMPONENT_PADDING / 2, LV_PART_MAIN);
        lv_obj_remove_flag(verified_row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(verified_row, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *verified_icon = lv_label_create(verified_row);
        lv_label_set_text(verified_icon, SeedSignerIconConstants::SUCCESS);
        lv_obj_set_style_text_font(verified_icon, &ICON_FONT__SEEDSIGNER, LV_PART_MAIN);
        lv_obj_set_style_text_color(verified_icon, lv_color_hex(SUCCESS_COLOR), LV_PART_MAIN);
        lv_obj_set_style_pad_all(verified_icon, 0, LV_PART_MAIN);

        lv_obj_t *verified_text_label = lv_label_create(verified_row);
        lv_label_set_text(verified_text_label, verified_text.c_str());
        lv_obj_set_style_text_font(verified_text_label, &BODY_FONT, LV_PART_MAIN);
        lv_obj_set_style_text_color(verified_text_label, lv_color_hex(BODY_FONT_COLOR), LV_PART_MAIN);
        lv_obj_set_style_pad_all(verified_text_label, 0, LV_PART_MAIN);
        // Force SINGLE-LINE. This is a one-line confirmation, but the default
        // LV_LABEL_LONG_WRAP makes the shaped-locale run layer wrap the run to the
        // label's content width (glyph_runs attach_runs) — and inside this tight
        // LV_SIZE_CONTENT row that width is narrow, so a long SHAPED translation
        // (e.g. Thai "ตรวจสอบที่อยู่แล้ว!") wraps and only line 0 survives in the
        // one-line-tall box. CLIP keeps wrap_width 0 so the whole run stays on one
        // line (same fix as the PSBTMath info word). The row stays content-sized,
        // so the icon+text unit still measures/centers as a tight block below.
        lv_label_set_long_mode(verified_text_label, LV_LABEL_LONG_CLIP);
    }

    // --- Geometry ---

    // One measure-and-place pass: with the scaffold + flex content laid out, center
    // the verified row between the address bottom and the first button top —
    // Python's IconTextLine(height=available_y) auto-centering, done here by
    // measuring the two band edges and splitting the difference.
    if (verified_row) {
        lv_obj_update_layout(screen.screen);

        lv_area_t address_area;
        lv_obj_get_coords(address_widget, &address_area);

        int32_t gap_top    = address_area.y2;
        int32_t gap_bottom = bottom_button_top_y(screen);

        int32_t verified_row_height = lv_obj_get_height(verified_row);
        int32_t verified_row_width  = lv_obj_get_width(verified_row);
        lv_obj_set_pos(verified_row,
                       (display_width - verified_row_width) / 2,
                       (gap_top + gap_bottom) / 2 - verified_row_height / 2);
    }

    // --- Navigation + load ---

    // Menu-style default index: an action list always has a selection, so the first
    // button starts focused (the host may override via cfg initial_selected_index).
    bind_screen_navigation(cfg, screen, /*default_initial_index=*/0);

    load_screen_and_cleanup_previous(screen.screen);
}
