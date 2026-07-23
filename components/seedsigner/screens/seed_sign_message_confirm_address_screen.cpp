// seed_sign_message_confirm_address_screen
//
// Python provenance: SeedSignMessageConfirmAddressScreen (seed_screens.py)
//
// The "Confirm Address" step of the message-signing flow: shows WHICH receive
// address the host will sign a message for, so the user can verify it before
// committing. Structurally a bottom-pinned button-list screen (Python:
// is_bottom_list = True, a single "Sign message" action; Python's forced
// is_button_text_centered = True equals the scaffold default, so no override
// is written) with a back button, plus two stacked read-only components in the
// body, in Python's build order:
//
//   1. IconTextLine — the blue DERIVATION glyph + a small gray localized label
//      over the derivation-path value; is_text_centered=True centers the whole
//      icon+text group horizontally.
//   2. FormattedAddress — the receive address in the shared head/middle/tail
//      colored, fixed-width, wrapped form (max_lines=3), a double
//      COMPONENT_PADDING gap below the derivation line (Python screen_y =
//      derivation.screen_y + derivation.height + 2*COMPONENT_PADDING).
//
// Layout notes — documented deviation from Python's vertical model: Python
// TOP-anchors the stack (screen_y measured down from top_nav.height +
// COMPONENT_PADDING); this screen vertically CENTERS the [derivation /
// address] block in the free band between the top nav and the button — the
// same grow/center pattern seed_finalize_screen and psbt_address_details_screen
// use, unlike tools_calc_final_word_done_screen (which keeps Python's top
// anchor). At the 240 reference the two-component stack nearly fills the band,
// so the centered result reads as the top-anchored layout within ~2 px; on the
// taller 480/800 profiles (not diffed against Python) centering balances the
// block instead of leaving a large void above the button.
//
// Both read-only components are shared with the PSBT/xpub screens
// (icon_text_line + formatted_address in components.cpp), so labeled-value
// spacing and address coloring stay identical across screens. The address
// accent here always renders the component default (mainnet orange):
// cfg["network"] is not read and the address format is not inspected, unlike
// psbt_address_details_screen / seed_address_verification_screen.
//
// Lifecycle: stateless (Tier 1) — no statics, no heap ctx; all state is
// widget-tree-owned.
//
// cfg:
//   top_nav.title              (string, required)     localized screen title
//            (Python __post_init__ forces _("Confirm Address"); the host view
//            layer supplies the already-localized string).
//   top_nav.show_back_button   (bool, default true)   Python ButtonListScreen default;
//            the reference shows the back arrow.
//   top_nav.show_power_button  (bool, default false)  Python ButtonListScreen default.
//   derivation_path            (string, required)     the BIP-32 path the address
//            derives from.
//   derivation_path_label      (string, required)     localized small gray label
//            above the path value (Python: _("derivation path")).
//   address                    (string, required)     the receive address the message
//            will be signed for.
//   button_list                (array, required, non-empty)  the localized action
//            buttons (Python: single ButtonOption "Sign message").
//   is_bottom_list             forced true (Python: is_bottom_list = True);
//            a host-supplied value is ignored.
//   initial_selected_index     (int, optional)        overrides the default initial
//            focus of 0 (navigation layer; Python selected_button).
//   input.mode                 (string, optional)     "touch" | "hardware" input-mode
//            override (navigation layer).
//   input.keys.key1/key2/key3  (string, optional)     per-aux-key policy "enter" |
//            "noop" | "emit" (navigation layer).
//   allow_screensaver          (bool, default true)   per-screen screensaver policy
//            (normalized by parse_screen_json_ctx, stamped by the scaffold).

#include "screen_scaffold.h"  // parse_screen_json_ctx / create_top_nav_screen_scaffold / bind_screen_navigation / load_screen_and_cleanup_previous
#include "seedsigner.h"       // seed_sign_message_confirm_address_screen decl, screen_scaffold_t fields
#include "components.h"       // icon_text_line + icon_text_line_opts_t, formatted_address + formatted_address_opts_t, SEEDSIGNER_ICON_COLOR_DEFAULT
#include "gui_constants.h"    // COMPONENT_PADDING, EDGE_PADDING, INFO_COLOR, SeedSignerIconConstants
#include "screen_helpers.h"   // ensure_top_nav_structure, require_top_nav_title, network_color

#include "lvgl.h"             // upper_body flex/margin setters + lv_display_get_horizontal_resolution

#include <nlohmann/json.hpp>  // json (cfg reads + structural-default writes)

#include <stdexcept>          // std::runtime_error (required-field validation)
#include <string>             // std::string

using json = nlohmann::json;


void seed_sign_message_confirm_address_screen(void *ctx_json) {
    // --- Config ---

    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    // Required fields: derivation_path + address are the host-derived data this
    // screen exists to show; derivation_path_label + button_list are user-visible
    // CONTENT, which always arrives localized from the host view layer (a string
    // literal baked here would be English-only by construction). One throw per
    // field, before the scaffold exists, so no throw path can leak LVGL objects.
    if (!cfg.contains("derivation_path") || !cfg["derivation_path"].is_string()) {
        throw std::runtime_error("seed_sign_message_confirm_address_screen: derivation_path is required and must be a string");
    }
    if (!cfg.contains("derivation_path_label") || !cfg["derivation_path_label"].is_string()) {
        throw std::runtime_error("seed_sign_message_confirm_address_screen: derivation_path_label is required and must be a string");
    }
    if (!cfg.contains("address") || !cfg["address"].is_string()) {
        throw std::runtime_error("seed_sign_message_confirm_address_screen: address is required and must be a string");
    }
    if (!cfg.contains("button_list") || !cfg["button_list"].is_array() || cfg["button_list"].empty()) {
        throw std::runtime_error("seed_sign_message_confirm_address_screen: button_list is required and must be a non-empty array");
    }
    std::string derivation_path       = cfg["derivation_path"].get<std::string>();
    std::string derivation_path_label = cfg["derivation_path_label"].get<std::string>();
    std::string address               = cfg["address"].get<std::string>();

    // Structural defaults (write-if-absent, never user-visible text). Python
    // ButtonListScreen defaults: show_back_button=True, show_power_button=False.
    // The localized title itself is content and must come from the host.
    ensure_top_nav_structure(cfg, /*default_show_back_button=*/true,
                                  /*default_show_power_button=*/false);
    require_top_nav_title(cfg, "seed_sign_message_confirm_address_screen");

    cfg["is_bottom_list"] = true;    // forced, not defaulted — Python: is_bottom_list = True

    // --- Scaffold ---

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, /*scrollable=*/false);

    // --- Body ---

    // Vertically center the [derivation / address] block in the gap between the
    // nav and the button — the same grow/center pattern seed_finalize_screen and
    // psbt_address_details_screen use: grow upper_body to claim the gap, center
    // on all flex axes, collapse the scaffold's bottom-list spacer. This
    // deliberately deviates from Python's top-anchored stack (see the banner): at
    // the 240 reference the content nearly fills the gap, so it reads as
    // top-anchored within ~2 px; on the taller 480/800 profiles (not diffed
    // against Python) it keeps the block balanced instead of leaving a large void
    // above the button. Python's inter-component gap (the address sits
    // 2*COMPONENT_PADDING below the derivation line) is preserved as the
    // address's own margin_top; zero the flex row gap so that margin is the only
    // gap. (This grow/center/collapse trio recurs across the family — see
    // docs/screen-conformance-spec.md §10 cluster 12; it stays inline pending the
    // vertical-slack extraction decision at rollout.)
    lv_obj_set_flex_grow(screen.upper_body, 1);
    lv_obj_set_flex_align(screen.upper_body, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(screen.upper_body, 0, LV_PART_MAIN);
    if (screen.button_list_spacer) lv_obj_set_flex_grow(screen.button_list_spacer, 0);

    // 1. Derivation-path line — blue DERIVATION glyph, gray localized label over
    //    the body-font path value (Python: IconTextLine(icon_name=DERIVATION,
    //    icon_color=INFO_COLOR, is_text_centered=True)). is_text_centered centers
    //    the whole icon+text group; the shared icon_text_line leaves the
    //    label/value left-aligned within their column and lets the centered
    //    upper_body place the block.
    icon_text_line_opts_t derivation_opts = {};
    derivation_opts.icon_glyph       = SeedSignerIconConstants::DERIVATION;
    derivation_opts.icon_color       = INFO_COLOR;                          // Python INFO_COLOR (#409CFF)
    derivation_opts.label_text       = derivation_path_label.c_str();
    derivation_opts.value_text       = derivation_path.c_str();
    derivation_opts.label_color      = SEEDSIGNER_ICON_COLOR_DEFAULT;       // -> LABEL_FONT_COLOR (gray)
    derivation_opts.value_color      = SEEDSIGNER_ICON_COLOR_DEFAULT;       // -> BODY_FONT_COLOR
    derivation_opts.is_text_centered = true;                                // Python is_text_centered = True
    icon_text_line(screen.upper_body, &derivation_opts);

    // 2. Formatted receive address — the shared head/middle/tail colored,
    //    fixed-width, wrapped form (Python: FormattedAddress, max_lines=3). Width
    //    is the body's inner column (full display minus the scaffold's edge
    //    padding), so the wrapped block centers on-screen exactly as
    //    psbt_address_details_screen's does. Head/tail carry the NETWORK accent per
    //    D-6: the HOST decides the network and passes cfg["network"]; the screen owns
    //    the palette via network_color() and never infers the network from the
    //    address. Absent network defaults to mainnet.
    const int32_t display_width = lv_display_get_horizontal_resolution(NULL);
    formatted_address_opts_t address_opts = {};
    address_opts.address      = address.c_str();
    address_opts.width        = display_width - 2 * EDGE_PADDING;
    address_opts.max_lines    = 3;                                          // Python max_lines = 3
    address_opts.accent_color = network_color(cfg.value("network", std::string("M")));  // head/tail = network accent
    address_opts.base_color   = SEEDSIGNER_ICON_COLOR_DEFAULT;              // -> LABEL_FONT_COLOR (gray)
    lv_obj_t *address_block = formatted_address(screen.upper_body, &address_opts);
    // Double gap below the derivation line (Python: + 2*COMPONENT_PADDING).
    lv_obj_set_style_margin_top(address_block, 2 * COMPONENT_PADDING, LV_PART_MAIN);

    // --- Navigation + load ---

    // Menu-style default index: an action list always has a selection, so the first
    // (only) button starts focused (the host may override via cfg initial_selected_index).
    bind_screen_navigation(cfg, screen, /*default_initial_index=*/0);

    load_screen_and_cleanup_previous(screen.screen);
}
