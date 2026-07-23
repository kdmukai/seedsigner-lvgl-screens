// psbt_address_details_screen
//
// Python provenance: PSBTAddressDetailsScreen (psbt_screens.py)
//
// PSBT signing-flow detail screen for one recipient output: a bottom-pinned
// button-list screen with a back button, showing the send-amount headline
// (Python: BtcAmount) over the recipient's FULL destination address, wrapped
// to as many lines as it needs, its verifiable head/tail tinted the network
// accent color (Python: FormattedAddress with no line cap).
//
// Layout notes: unlike its sibling psbt_change_details_screen (which TOP-anchors
// its stack under the top nav), this screen vertically CENTERS the
// [amount / address] block in the free band between the top nav and the bottom
// button — Python renders both components into a temp image and pastes it
// centered in that band. Here the same centering is declarative: upper_body
// grows to claim the band (collapsing the scaffold's bottom-list spacer) and
// flex-centers its children, with the flex row gap carrying Python's
// amount->address spacing (FormattedAddress screen_y = btc_amount.height +
// COMPONENT_PADDING).
//
// Lifecycle (stateless, Tier 1): no statics, timers, or heap ctx — all state is
// widget-tree-owned.
//
// cfg:
//   top_nav.title              (string, required)     localized screen title (the host
//            passes "Verify Send Address" etc. for the flow step).
//   top_nav.show_back_button   (bool, default true)   Python ButtonListScreen default.
//   top_nav.show_power_button  (bool, default false)  Python ButtonListScreen default.
//   btc_amount                 (object, optional)     the send-amount headline; sub-keys
//            (primary/secondary/unit/network/icon_color) per btc_amount_from_cfg.
//            Optional by policy: an absent or non-object value skips the headline
//            and centers the address alone.
//   address                    (string, required)     the full destination address.
//   button_list                (array, required, non-empty)  the localized action
//            buttons (Python: "Next").
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

#include "screen_scaffold.h"  // parse_screen_json_ctx / create_top_nav_screen_scaffold / bind_screen_navigation / load_screen_and_cleanup_previous
#include "seedsigner.h"       // psbt_address_details_screen decl
#include "components.h"       // formatted_address + formatted_address_opts_t, SEEDSIGNER_ICON_COLOR_DEFAULT
#include "gui_constants.h"    // COMPONENT_PADDING, EDGE_PADDING
#include "screen_helpers.h"   // ensure_top_nav_structure, require_top_nav_title, btc_amount_from_cfg, network_color

#include "lvgl.h"             // lv_obj flex/style setters + lv_display_get_horizontal_resolution

#include <nlohmann/json.hpp>  // json (cfg reads + structural-default writes)

#include <stdexcept>          // std::runtime_error (required-field validation)
#include <string>             // std::string

using json = nlohmann::json;


void psbt_address_details_screen(void *ctx_json) {
    // --- Config ---

    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    // Required fields: address is the PSBT recipient datum this screen exists to
    // show; button_list is user-visible CONTENT, which always arrives localized
    // from the host view layer (a string literal baked here would be English-only
    // by construction). One throw per field, before the scaffold exists, so no
    // throw path can leak LVGL objects.
    if (!cfg.contains("address") || !cfg["address"].is_string()) {
        throw std::runtime_error("psbt_address_details_screen: address is required and must be a string");
    }
    if (!cfg.contains("button_list") || !cfg["button_list"].is_array() || cfg["button_list"].empty()) {
        throw std::runtime_error("psbt_address_details_screen: button_list is required and must be a non-empty array");
    }
    std::string address = cfg["address"].get<std::string>();

    // Structural defaults (write-if-absent, never user-visible text). Python
    // ButtonListScreen defaults: show_back_button=True, show_power_button=False.
    // The localized title itself is content and must come from the host.
    ensure_top_nav_structure(cfg, /*default_show_back_button=*/true,
                                  /*default_show_power_button=*/false);
    require_top_nav_title(cfg, "psbt_address_details_screen");

    cfg["is_bottom_list"] = true;    // forced, not defaulted — Python: is_bottom_list = True

    // --- Scaffold ---

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, /*scrollable=*/false);

    // --- Body ---

    // Center the [amount / address] block vertically in the gap above the button,
    // the same way seed_finalize_screen centers its fingerprint readout: grow
    // upper_body to claim the gap, center on all flex axes, collapse the scaffold
    // spacer. The COMPONENT_PADDING row gap matches Python's amount->address
    // spacing. (This grow/center/collapse trio recurs across the family — see
    // docs/screen-conformance-spec.md §10 cluster 12; it stays inline pending the
    // vertical-slack extraction decision at rollout.)
    lv_obj_set_flex_grow(screen.upper_body, 1);
    lv_obj_set_flex_align(screen.upper_body, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(screen.upper_body, COMPONENT_PADDING, LV_PART_MAIN);
    if (screen.button_list_spacer) lv_obj_set_flex_grow(screen.button_list_spacer, 0);

    // 1. Send-amount headline (Python: BtcAmount) via the shared PSBT amount
    //    builder — it maps btc_amount.network to the coin-icon color and renders
    //    the host-formatted display strings. Skipped when absent (optional by
    //    policy — see the banner).
    if (cfg.contains("btc_amount") && cfg["btc_amount"].is_object()) {
        btc_amount_from_cfg(screen.upper_body, cfg["btc_amount"]);
    }

    // 2. Fully-wrapped destination address (Python: FormattedAddress with no line
    //    cap) at Python's column width (display width minus the two edge paddings).
    //    The verifiable head/tail runs carry the NETWORK accent color. Per D-6 the
    //    HOST decides the network and passes cfg["network"]; the screen owns the
    //    palette via network_color() and never infers the network from the address.
    //    Absent network defaults to mainnet.
    const int32_t display_width = lv_display_get_horizontal_resolution(NULL);
    formatted_address_opts_t address_opts = {};
    address_opts.address      = address.c_str();
    address_opts.width        = display_width - 2 * EDGE_PADDING;
    address_opts.max_lines    = 0;                                                       // wrap to as many lines as needed
    address_opts.accent_color = network_color(cfg.value("network", std::string("M")));  // head/tail = network accent
    address_opts.base_color   = SEEDSIGNER_ICON_COLOR_DEFAULT;                           // -> LABEL_FONT_COLOR (gray prefix + middle)
    formatted_address(screen.upper_body, &address_opts);

    // --- Navigation + load ---

    // Menu-style default index: an action list always has a selection, so the first
    // button starts focused (the host may override via cfg initial_selected_index).
    bind_screen_navigation(cfg, screen, /*default_initial_index=*/0);

    load_screen_and_cleanup_previous(screen.screen);
}
