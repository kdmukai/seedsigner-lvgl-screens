// multisig_wallet_descriptor_screen
//
// Python provenance: MultisigWalletDescriptorScreen (seed_screens.py)
//
// A ButtonListScreen (is_bottom_list forced true) confirming that a multisig
// wallet descriptor was loaded. Two centered IconTextLine readouts sit under
// the top-nav, with the host-supplied action button (Python: single "OK")
// pinned at the bottom:
//
//   Policy                       (gray label, body-2 = 15px)
//   1 of 2                       (white value, 20px OpenSans)
//
//   Signing Keys                 (gray label, body-2 = 15px)
//   22bde1a9 73c5da0a            (white value, 24px Inconsolata SemiBold — monospace)
//
// Layout mirrors the Python class exactly at the 240x240 reference:
//   - component 1 screen_y = top_nav.height (48) → content begins right under the nav.
//   - component 2 screen_y = component 1 bottom + 2*COMPONENT_PADDING (a 16px gap).
//   - the label/value pairing and its internal spacing come from the shared
//     icon_text_line() component (Python IconTextLine), reused verbatim.
//
// The signing-keys VALUE is the fixed-width emphasis font (Python
// FIXED_WIDTH_EMPHASIS_FONT_NAME = "Inconsolata-SemiBold" @24px) — the profile's
// KEYBOARD_FONT, whose 240 base IS inconsolata_semibold_24_4bpp, so the hex
// fingerprints render in monospace exactly like Python (and psbt_math's digits).
// It wraps at the body width (Python auto_line_break = True) so a 3-of-5
// policy's fingerprint list flows onto extra lines.
//
// Documented deviation from Python: profiles taller than the 240 reference
// center the readout block in the extra vertical slack (see the centering
// block in --- Body --- below); Python only ever renders the top-anchored
// 240-tall layout.
//
// Lifecycle: stateless (Tier 1) — no statics, no heap ctx; all state is
// widget-tree-owned or stack-local.
//
// cfg:
//   policy             (string, required)    the signing policy readout, e.g.
//            "1 of 2" (host-formatted "{threshold} of {n}").
//   fingerprints       (string array)        master fingerprints; space-joined into
//            the signing-keys value, matching Python's " ".join(fingerprints).
//            Non-string entries are skipped (render the rest).
//   signing_keys       (string)              pre-joined alternative to `fingerprints`
//            for hosts that format the value upstream; wins when both are present.
//            At least ONE of signing_keys/fingerprints is required.
//   policy_label       (string, required)    localized "Policy" field label
//            (Python: _("Policy")).
//   signing_keys_label (string, required)    localized "Signing Keys" field label
//            (Python: _("Signing Keys")).
//   button_list        (array, required, non-empty)  the localized action button(s)
//            (Python: single "OK"; read by the scaffold, one button per entry).
//   top_nav.title      (string, required)    localized screen title (read by the
//            scaffold; Python: _("Descriptor Loaded")); enforced here via
//            require_top_nav_title.
//   top_nav.show_back_button  (bool, default true)   Python ButtonListScreen default.
//   top_nav.show_power_button (bool, default false)  Python ButtonListScreen default.
//   is_bottom_list     forced true (Python: is_bottom_list = True); a host-supplied
//            value is ignored.
//   initial_selected_index    (int, optional)     overrides the default initial
//            focus of 0 (navigation layer; Python selected_button).
//   input.mode                (string, optional)  "touch" | "hardware" input-mode
//            override (navigation layer).
//   input.keys.key1/key2/key3 (string, optional)  per-aux-key policy "enter" |
//            "noop" | "emit" (navigation layer).
//   allow_screensaver         (bool, default true) per-screen screensaver policy
//            (normalized by parse_screen_json_ctx, stamped by the scaffold).

#include "screen_scaffold.h"  // parse_screen_json_ctx / create_top_nav_screen_scaffold / bind_screen_navigation / load_screen_and_cleanup_previous
#include "seedsigner.h"       // multisig_wallet_descriptor_screen decl, screen_scaffold_t
#include "components.h"       // icon_text_line, SEEDSIGNER_ICON_COLOR_DEFAULT
#include "gui_constants.h"    // COMPONENT_PADDING, EDGE_PADDING, KEYBOARD_FONT, seedsigner_latin_font
#include "screen_helpers.h"   // ensure_top_nav_structure, require_top_nav_title

#include "lvgl.h"             // flex/pad style setters, display-resolution queries

#include <nlohmann/json.hpp>  // json (cfg reads + structural-default writes)

#include <stdexcept>          // std::runtime_error (required-field validation)
#include <string>             // std::string

using json = nlohmann::json;


void multisig_wallet_descriptor_screen(void *ctx_json) {
    // --- Config ---

    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    // Required fields: policy and the fingerprints are the descriptor facts this
    // confirmation exists to show; the two field labels and button_list are
    // user-visible CONTENT, which always arrives localized from the host view
    // layer (a string literal baked here would be English-only by construction).
    // One throw per field, before the scaffold exists, so no throw path can leak
    // LVGL objects. The signing-keys value is dual-form: hosts send EITHER the
    // "fingerprints" array (space-joined below, Python " ".join(fingerprints))
    // OR a pre-joined "signing_keys" string — at least one must be present.
    if (!cfg.contains("policy") || !cfg["policy"].is_string()) {
        throw std::runtime_error("multisig_wallet_descriptor_screen: policy is required and must be a string");
    }
    const bool has_signing_keys_string = cfg.contains("signing_keys") && cfg["signing_keys"].is_string();
    const bool has_fingerprints_array  = cfg.contains("fingerprints") && cfg["fingerprints"].is_array();
    if (!has_signing_keys_string && !has_fingerprints_array) {
        throw std::runtime_error("multisig_wallet_descriptor_screen: signing_keys (string) or fingerprints (array) is required");
    }
    if (!cfg.contains("policy_label") || !cfg["policy_label"].is_string()) {
        throw std::runtime_error("multisig_wallet_descriptor_screen: policy_label is required and must be a string");
    }
    if (!cfg.contains("signing_keys_label") || !cfg["signing_keys_label"].is_string()) {
        throw std::runtime_error("multisig_wallet_descriptor_screen: signing_keys_label is required and must be a string");
    }
    if (!cfg.contains("button_list") || !cfg["button_list"].is_array() || cfg["button_list"].empty()) {
        throw std::runtime_error("multisig_wallet_descriptor_screen: button_list is required and must be a non-empty array");
    }

    // Policy string (host formats "{threshold} of {n}", e.g. "1 of 2").
    std::string policy = cfg["policy"].get<std::string>();

    // Signing-keys value = the space-joined fingerprints (Python " ".join(fingerprints)).
    // The pre-joined "signing_keys" form wins when both are present.
    std::string signing_keys;
    if (has_signing_keys_string) {
        signing_keys = cfg["signing_keys"].get<std::string>();
    } else {
        for (const auto &fingerprint : cfg["fingerprints"]) {
            if (!fingerprint.is_string()) continue;   // skip malformed entries; render the rest
            if (!signing_keys.empty()) signing_keys += " ";
            signing_keys += fingerprint.get<std::string>();
        }
    }

    // Field labels (host-localized; Python _("Policy") / _("Signing Keys")).
    std::string policy_label       = cfg["policy_label"].get<std::string>();
    std::string signing_keys_label = cfg["signing_keys_label"].get<std::string>();

    // Structural defaults (write-if-absent, never user-visible text). Python
    // ButtonListScreen defaults: show_back_button=True, show_power_button=False —
    // the same values the scaffold falls back to when the flags are absent, so
    // these writes are representation-only. The localized title itself is content
    // and must come from the host.
    ensure_top_nav_structure(cfg, /*default_show_back_button=*/true,
                                  /*default_show_power_button=*/false);
    require_top_nav_title(cfg, "multisig_wallet_descriptor_screen");

    cfg["is_bottom_list"] = true;    // forced, not defaulted — Python: is_bottom_list = True

    // --- Scaffold ---

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, /*scrollable=*/false);

    // --- Body ---

    // Both readouts stack in the scaffold's upper_body (a flex column, cross-axis
    // centered). Python places component 1 at screen_y = top_nav.height (no extra top
    // pad) and component 2 a 2*COMPONENT_PADDING gap below component 1, so:
    //   - pad_top = 0                     → first line hugs the nav bottom (y = 48).
    //   - pad_row = 2 * COMPONENT_PADDING → the 16px (base) inter-component gap.
    // icon_text_line reclaims each label/value's LVGL line leading, but the text
    // line-box still leaves ~3px (at the 240 reference) more space below the last row
    // than PIL does; trim that residual so component 2's label lands on the Python
    // baseline (measured: without it, row 2 sat 3px low). The (3 * COMPONENT_PADDING) / 8
    // form ties the trim to the profile's padding scale, but integer division makes it
    // PLATEAU rather than track the multiplier linearly: COMPONENT_PADDING 8 (240 base)
    // → 3, ~10 (the 133% profile) → still 3, 16 (200%) → 6. Only the 240-tall reference
    // is pixel-gated; taller profiles just inherit an about-right trim.
    const int32_t leading_residual = (3 * COMPONENT_PADDING) / 8;   // 3px at the 240 base
    lv_obj_set_style_pad_top(screen.upper_body, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(screen.upper_body, 2 * COMPONENT_PADDING - leading_residual, LV_PART_MAIN);

    // Profiles sharing the 240-tall reference height (240x240, 320x240) TOP-ANCHOR the
    // readouts for pixel parity with Python; the taller profiles (480x320, 800x480)
    // have vertical slack Python never renders, so this screen centers the block in
    // that gap: grow upper_body to fill, center its rows on the main axis, and
    // collapse the scaffold's bottom spacer. The taller-than-reference alignment
    // policy is an open per-screen decision — see the §11 vertical-slack policy in
    // docs/screen-conformance-spec.md.
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

    // 1. Policy readout. No icon; label + value centered (Python is_text_centered).
    //    label: body-2 = 15px OpenSans (Python IconTextLine label = body_size - 2).
    //    value: 20px OpenSans (Python font_size = 20).
    icon_text_line_opts_t policy_line = {};
    policy_line.label_text       = policy_label.c_str();
    policy_line.label_font       = seedsigner_latin_font(label_base_px);
    policy_line.value_text       = policy.c_str();
    policy_line.value_font       = seedsigner_latin_font(20);
    policy_line.label_color      = SEEDSIGNER_ICON_COLOR_DEFAULT;   // -> LABEL_FONT_COLOR (gray)
    policy_line.value_color      = SEEDSIGNER_ICON_COLOR_DEFAULT;   // -> BODY_FONT_COLOR (white)
    policy_line.is_text_centered = true;
    icon_text_line(screen.upper_body, &policy_line);

    // 2. Signing Keys readout. No icon; label + value centered.
    //    label: body-2 = 15px OpenSans.
    //    value: the space-joined fingerprints in FIXED-WIDTH emphasis (Python 24px
    //    Inconsolata-SemiBold) = the profile KEYBOARD_FONT (24px base monospace).
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

    // --- Navigation + load ---

    // Menu-style default index: an action list always has a selection, so the first
    // button starts focused (the host may override via cfg initial_selected_index).
    bind_screen_navigation(cfg, screen, /*default_initial_index=*/0);

    load_screen_and_cleanup_previous(screen.screen);
}
