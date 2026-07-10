#include "screen_scaffold.h"   // parse/scaffold/nav/load helpers (defined in screen_scaffold.cpp)
#include "components.h"        // icon_text_line()
#include "gui_constants.h"     // colors, scaled layout macros, fonts, seedsigner_latin_font()
#include "navigation.h"        // NAV_BODY_VERTICAL

#include "lvgl.h"

#include <string>

// ---------------------------------------------------------------------------
// MultisigWalletDescriptorScreen (Python seed_screens.py:MultisigWalletDescriptorScreen)
//
// A ButtonListScreen (is_bottom_list = True) confirming that a multisig wallet
// descriptor was loaded. Two centered IconTextLine readouts sit top-anchored under
// the nav, with a single action button ("OK") pinned at the bottom:
//
//   Policy                       (gray label, body-2 = 15px)
//   1 of 2                       (white value, 20px OpenSans)
//
//   Signing Keys                 (gray label, body-2 = 15px)
//   22bde1a9 73c5da0a            (white value, 24px Inconsolata SemiBold — monospace)
//
// Layout math mirrors the Python class exactly at the 240x240 reference:
//   - component 1 screen_y = top_nav.height (48) → content begins right under the nav.
//   - component 2 screen_y = component1 bottom + 2*COMPONENT_PADDING (a 16px gap).
//   - the label/value pairing and its internal spacing come from the shared
//     icon_text_line() component (Python IconTextLine), reused verbatim.
//
// The signing-keys VALUE is the fixed-width emphasis font (Python
// FIXED_WIDTH_EMPHASIS_FONT_NAME = "Inconsolata-SemiBold" @24px) — the profile's
// KEYBOARD_FONT, whose 240 base IS inconsolata_semibold_24_4bpp, so the hex
// fingerprints render in monospace exactly like Python (and psbt_math's digits).
//
// cfg:
//   top_nav.title            — default "Descriptor Loaded".
//   policy (str)             — the signing policy string, e.g. "1 of 2" (host-formatted).
//   fingerprints (str[])     — master fingerprints; space-joined into the value line,
//                              matching Python's " ".join(fingerprints). Alternatively a
//                              pre-joined `signing_keys` string may be supplied directly.
//   policy_label (str)       — host-localized "Policy" label   (default "Policy").
//   signing_keys_label (str) — host-localized "Signing Keys"   (default "Signing Keys").
//   button_list (array)      — default ["OK"].
// ---------------------------------------------------------------------------
void multisig_wallet_descriptor_screen(void *ctx_json) {
    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    // --- Data ------------------------------------------------------------------
    // Policy string (host formats "{threshold} of {n}", e.g. "1 of 2").
    std::string policy = cfg.value("policy", std::string(""));

    // Signing-keys value = the space-joined fingerprints (Python " ".join(fingerprints)).
    // Accept a pre-joined "signing_keys" string too, for hosts that format it upstream.
    std::string signing_keys;
    if (cfg.contains("signing_keys") && cfg["signing_keys"].is_string()) {
        signing_keys = cfg["signing_keys"].get<std::string>();
    } else if (cfg.contains("fingerprints") && cfg["fingerprints"].is_array()) {
        for (const auto &fp : cfg["fingerprints"]) {
            if (!fp.is_string()) continue;
            if (!signing_keys.empty()) signing_keys += " ";
            signing_keys += fp.get<std::string>();
        }
    }

    // Field labels (host-localized; default to the English strings).
    std::string policy_label       = cfg.value("policy_label",       std::string("Policy"));
    std::string signing_keys_label = cfg.value("signing_keys_label", std::string("Signing Keys"));

    // --- Scaffold: a bottom-pinned single-button list (Python is_bottom_list = True). ---
    if (!cfg.contains("top_nav") || !cfg["top_nav"].is_object()) cfg["top_nav"] = json::object();
    if (!cfg["top_nav"].contains("title")) cfg["top_nav"]["title"] = "Descriptor Loaded";
    cfg["is_bottom_list"] = true;
    if (!cfg.contains("button_list")) cfg["button_list"] = json::array({ "OK" });

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, false);

    // --- Body layout -----------------------------------------------------------
    // Both readouts stack in the scaffold's upper_body (a flex column, cross-axis
    // centered). Python places component 1 at screen_y = top_nav.height (no extra top
    // pad) and component 2 a 2*COMPONENT_PADDING gap below component 1, so:
    //   - pad_top = 0                     → first line hugs the nav bottom (y = 48).
    //   - pad_row = 2 * COMPONENT_PADDING → the 16px (base) inter-component gap.
    // icon_text_line reclaims each label/value's LVGL line leading, but the text
    // line-box still leaves ~3px (at the 240 reference) more space below the last row
    // than PIL does; trim that residual so component 2's label lands on the Python
    // baseline (measured: without it, row 2 sat 3px low). Scaled via COMPONENT_PADDING
    // so taller profiles (larger fonts → larger residual) trim proportionally.
    const int32_t leading_residual = (3 * COMPONENT_PADDING) / 8;   // 3px @240; scales w/ profile
    lv_obj_set_style_pad_top(screen.upper_body, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(screen.upper_body, 2 * COMPONENT_PADDING - leading_residual, LV_PART_MAIN);

    // Profiles that share the 240-tall reference height (240x240, 320x240) TOP-ANCHOR
    // the block to match the Python reference: upper_body stays content-sized and the
    // scaffold's flex-grow spacer pushes the button to the bottom. The taller profiles
    // (480x320, 800x480) have vertical slack Python never renders, so the house
    // convention centers the body in that gap: grow upper_body to fill and center its
    // rows on the main axis, collapsing the scaffold spacer.
    if (lv_display_get_vertical_resolution(NULL) > 240) {
        lv_obj_set_flex_grow(screen.upper_body, 1);
        lv_obj_set_flex_align(screen.upper_body, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        if (screen.button_list_spacer) lv_obj_set_flex_grow(screen.button_list_spacer, 0);
    }

    // seedsigner_latin_font() takes the UNSCALED base (240-reference) px and applies the
    // active profile's multiplier itself. Python's IconTextLine label is
    // get_body_font_size() - 2 = 17 - 2 = 15 (base); the value fonts are Python's literal
    // 20 (Policy) and 24 (Signing Keys, carried by KEYBOARD_FONT below).
    const int label_base_px = 15;   // Python body_font_size(17) - 2

    // Component 1 — Policy. No icon; label + value centered (Python is_text_centered).
    // label: body-2 = 15px OpenSans (Python IconTextLine label = body_size - 2).
    // value: 20px OpenSans (Python font_size = 20).
    icon_text_line_opts_t policy_line = {};
    policy_line.label_text       = policy_label.c_str();
    policy_line.label_font       = seedsigner_latin_font(label_base_px);
    policy_line.value_text       = policy.c_str();
    policy_line.value_font       = seedsigner_latin_font(20);
    policy_line.label_color      = SEEDSIGNER_ICON_COLOR_DEFAULT;   // -> LABEL_FONT_COLOR (gray)
    policy_line.value_color      = SEEDSIGNER_ICON_COLOR_DEFAULT;   // -> BODY_FONT_COLOR (white)
    policy_line.is_text_centered = true;
    icon_text_line(screen.upper_body, &policy_line);

    // Component 2 — Signing Keys. No icon; label + value centered.
    // label: body-2 = 15px OpenSans.
    // value: the space-joined fingerprints in FIXED-WIDTH emphasis (Python 24px
    // Inconsolata-SemiBold) = the profile KEYBOARD_FONT (24px base monospace).
    icon_text_line_opts_t keys_line = {};
    keys_line.label_text       = signing_keys_label.c_str();
    keys_line.label_font       = seedsigner_latin_font(label_base_px);
    keys_line.value_text       = signing_keys.c_str();
    keys_line.value_font       = &KEYBOARD_FONT;                   // Inconsolata SemiBold, 24px @240
    keys_line.label_color      = SEEDSIGNER_ICON_COLOR_DEFAULT;   // -> LABEL_FONT_COLOR (gray)
    keys_line.value_color      = SEEDSIGNER_ICON_COLOR_DEFAULT;   // -> BODY_FONT_COLOR (white)
    keys_line.is_text_centered = true;
    // Wrap the space-joined fingerprints across lines (Python auto_line_break = True): a
    // 2-of-3 / 3-of-5 policy lists 3–5 fingerprints that overflow one monospace line, so
    // fix the value to the body's inner width and let it wrap (a 1-of-2 still fits on one
    // line). Without this the list clips to a single centered line.
    keys_line.value_wrap_width = lv_display_get_horizontal_resolution(NULL) - 2 * EDGE_PADDING;
    icon_text_line(screen.upper_body, &keys_line);

    bind_screen_navigation(cfg, screen, 0);

    load_screen_and_cleanup_previous(screen.screen);
}
