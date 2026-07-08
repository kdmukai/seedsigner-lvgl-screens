#include "screen_scaffold.h" // parse/scaffold/nav/load helpers (defined in seedsigner.cpp)
#include "seedsigner.h"     // screen_scaffold_t, this screen's decl (extern "C")
#include "components.h"      // icon_text_line(), formatted_address()
#include "gui_constants.h"   // colors, COMPONENT_PADDING, INFO_COLOR, SeedSignerIconConstants
#include "navigation.h"      // nav_body_layout_t, NAV_BODY_VERTICAL

#include "lvgl.h"

#include <nlohmann/json.hpp>
#include <string>

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// SeedSignMessageConfirmAddressScreen (Python seed_screens.py:1648)
//
// The "Confirm Address" step of the message-signing flow: shows WHICH receive
// address the host will sign a message for, so the user can verify it before
// committing. Structurally a bottom-pinned ButtonListScreen (is_bottom_list =
// True, is_button_text_centered = True) with a single action button, and two
// stacked read-only components pinned under the top nav (Python top-anchors the
// stack — screen_y is measured down from top_nav.height, NOT vertically
// centered like SeedFinalize / PSBTAddressDetails):
//
//   [1] IconTextLine  — the blue DERIVATION glyph + a small gray "derivation
//       path" label over the derivation-path value. is_text_centered=True →
//       the whole icon+text group is horizontally centered. Python screen_y =
//       top_nav.height + COMPONENT_PADDING.
//   [2] FormattedAddress — the receive address in the shared head/middle/tail
//       colored, fixed-width, wrapped form. Python screen_y = derivation.screen_y
//       + derivation.height + 2*COMPONENT_PADDING (a double gap below the line).
//
// Both read-only components are shared with the PSBT/xpub screens
// (icon_text_line + formatted_address in components.cpp), so labeled-value
// spacing and address coloring stay identical across every screen.
//
// cfg:
//   top_nav.title            — screen title (default "Confirm Address").
//   derivation_path (str, req.)     — the BIP-32 path the address derives from.
//   derivation_path_label (str)     — the small gray label above the path;
//                                     defaults to "derivation path" (the View
//                                     supplies the localized string).
//   address (str, req.)      — the receive address the message will be signed for.
//   button_list (array)      — action buttons (default ["Sign message"]).
// ---------------------------------------------------------------------------

// The scaffold/navigation helpers (parse_screen_json_ctx, create_top_nav_screen_scaffold,
// bind_screen_navigation, load_screen_and_cleanup_previous) are declared in
// screen_scaffold.h and defined (with external linkage) in seedsigner.cpp.
void seed_sign_message_confirm_address_screen(void *ctx_json) {
    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    // Required data (host-derived): the derivation path + the receive address.
    if (!cfg.contains("derivation_path") || !cfg["derivation_path"].is_string()) {
        throw std::runtime_error("seed_sign_message_confirm_address_screen requires a \"derivation_path\" string");
    }
    if (!cfg.contains("address") || !cfg["address"].is_string()) {
        throw std::runtime_error("seed_sign_message_confirm_address_screen requires an \"address\" string");
    }
    std::string derivation_path  = cfg["derivation_path"].get<std::string>();
    std::string derivation_label = cfg.value("derivation_path_label", std::string("derivation path"));
    std::string address          = cfg["address"].get<std::string>();

    // Force the SeedSignMessageConfirmAddressScreen shape onto the scaffold cfg: a
    // titled, bottom-pinned, single-button list (Python is_bottom_list = True). The
    // View supplies the localized title + button label; default both so a bare cfg
    // still renders.
    if (!cfg.contains("top_nav") || !cfg["top_nav"].is_object()) cfg["top_nav"] = json::object();
    if (!cfg["top_nav"].contains("title")) cfg["top_nav"]["title"] = "Confirm Address";
    cfg["is_bottom_list"] = true;                                  // Python: is_bottom_list = True
    if (!cfg.contains("button_list")) cfg["button_list"] = json::array({ "Sign message" });

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, false, nullptr);

    // Top-anchored stack. The scaffold's upper_body is already a flex column with
    // main-axis START (top) and cross-axis CENTER (horizontal) — exactly Python's
    // "pin the stack under the nav, centered horizontally". Keep the bottom spacer
    // growing so the action button stays pinned at the viewport bottom.
    //
    // Vertically center the [derivation / address] block in the gap between the nav and
    // the button — the SAME upper_body-grow + center pattern seed_finalize and
    // psbt_address_details use. At the 240 reference the content nearly fills that gap, so
    // this reads as Python's top-anchored stack (within ~2 px); on the taller 480/800
    // profiles (not diffed against Python) it keeps the block balanced instead of leaving
    // a large void above the button. The Python inter-line gap (address sits
    // 2*COMPONENT_PADDING below the derivation line) is preserved as the address's own
    // margin_top; zero the inter-child row gap so that margin is the only gap.
    lv_obj_set_flex_grow(screen.upper_body, 1);
    lv_obj_set_flex_align(screen.upper_body, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(screen.upper_body, 0, LV_PART_MAIN);
    if (screen.button_list_spacer) lv_obj_set_flex_grow(screen.button_list_spacer, 0);

    // [1] Derivation-path line — blue DERIVATION glyph, gray "derivation path" label
    // over the body-font path value. is_text_centered centers the whole icon+text
    // group (the shared icon_text_line leaves the label/value left-aligned within
    // their column and lets this centered upper_body place the block).
    icon_text_line_opts_t dv = {};
    dv.icon_glyph       = SeedSignerIconConstants::DERIVATION;
    dv.icon_color       = INFO_COLOR;                          // Python INFO_COLOR (#409CFF)
    dv.label_text       = derivation_label.c_str();
    dv.value_text       = derivation_path.c_str();
    dv.label_color      = SEEDSIGNER_ICON_COLOR_DEFAULT;       // -> LABEL_FONT_COLOR (gray)
    dv.value_color      = SEEDSIGNER_ICON_COLOR_DEFAULT;       // -> BODY_FONT_COLOR
    dv.is_text_centered = true;                                // Python is_text_centered = True
    icon_text_line(screen.upper_body, &dv);

    // [2] Formatted receive address — shared head/middle/tail colored, fixed-width,
    // wrapped form (Python FormattedAddress, max_lines=3). Width is the body's inner
    // column (full display minus the scaffold's edge padding), so the wrapped block
    // centers on-screen exactly as the psbt_address_details screen's does. Mainnet
    // accent (default) matches the reference; the host derives the address.
    const int32_t W = lv_display_get_horizontal_resolution(NULL);
    formatted_address_opts_t fo = {};
    fo.address      = address.c_str();
    fo.width        = W - 2 * EDGE_PADDING;
    fo.max_lines    = 3;                                       // Python max_lines = 3
    fo.accent_color = SEEDSIGNER_ICON_COLOR_DEFAULT;          // -> ACCENT_COLOR (mainnet orange)
    fo.base_color   = SEEDSIGNER_ICON_COLOR_DEFAULT;          // -> LABEL_FONT_COLOR (gray)
    lv_obj_t *addr = formatted_address(screen.upper_body, &fo);
    // Double gap below the derivation line (Python: + 2*COMPONENT_PADDING).
    lv_obj_set_style_margin_top(addr, 2 * COMPONENT_PADDING, LV_PART_MAIN);

    bind_screen_navigation(
        cfg,
        screen,
        screen.button_list_count > 0 ? screen.button_list : NULL,
        screen.button_list_count,
        NAV_BODY_VERTICAL,
        0   // default the first (only) action button selected, like button_list_screen
    );

    load_screen_and_cleanup_previous(screen.screen);
}
