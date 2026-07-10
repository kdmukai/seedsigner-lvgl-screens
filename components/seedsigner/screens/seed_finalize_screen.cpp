// seed_finalize_screen
//
// Python provenance: SeedFinalizeScreen (seed_screens.py)
//
// The "Finalize Seed" step shown immediately after a seed is loaded.
// Structurally it is a bottom-pinned button-list screen (host-supplied actions;
// Python builds "Done" and, when a passphrase is allowed, "BIP-39 Passphrase")
// with NO back button; its one body element is a centered fingerprint readout —
// the master-fingerprint hex beneath a small "fingerprint" label, beside a
// large blue fingerprint icon (Python: IconTextLine(icon_name=FINGERPRINT,
// icon_color=INFO_COLOR, icon_size=ICON_FONT_SIZE+12,
// font_size=body+2, is_text_centered=True)). The pressed button index returns
// through the standard navigation callback.
//
// Layout notes: unlike tools_calc_final_word_done_screen (which TOP-anchors its
// readouts under the top-nav), this screen vertically CENTERS the readout in
// the free band between the top-nav and the first button. Python hand-centers
// it by anchoring the IconTextLine's top at the band midpoint minus a fixed
// 30 px (exact centering for a ~60 px block); here the scaffold's flex body
// centers the readout's measured height in the same band — a documented
// few-px deviation when the block height differs from 60 px.
//
// Lifecycle: stateless (Tier 1) — no statics, no heap ctx; all state is
// widget-tree-owned.
//
// cfg:
//   top_nav.title             (string, required)     localized screen title
//            (Python __post_init__ forces _("Finalize Seed"); the host view
//            layer supplies the already-localized string).
//   top_nav.show_back_button  forced false (Python __post_init__:
//            show_back_button = False); a host-supplied value is ignored.
//   top_nav.show_power_button (bool, default false)  Python ButtonListScreen default.
//   fingerprint               (string, required)     the master-fingerprint hex to display.
//   fingerprint_label         (string, required)     localized small label above the
//            fingerprint value (Python: _("fingerprint")).
//   button_list               (array, required, non-empty)  the localized action
//            buttons (Python: "Done", plus "BIP-39 Passphrase" when enabled).
//   is_bottom_list            forced true (Python: is_bottom_list = True);
//            a host-supplied value is ignored.
//   initial_selected_index    (int, optional)        overrides the default initial
//            focus of 0 (navigation layer; Python selected_button).
//   input.mode                (string, optional)     "touch" | "hardware" input-mode
//            override (navigation layer).
//   input.keys.key1/key2/key3 (string, optional)     per-aux-key policy "enter" |
//            "noop" | "emit" (navigation layer).
//   allow_screensaver         (bool, default true)   per-screen screensaver policy
//            (normalized by parse_screen_json_ctx, stamped by the scaffold).

#include "screen_scaffold.h"  // parse_screen_json_ctx / create_top_nav_screen_scaffold / bind_screen_navigation / load_screen_and_cleanup_previous
#include "seedsigner.h"       // seed_finalize_screen decl, screen_scaffold_t fields
#include "components.h"       // icon_text_line + icon_text_line_opts_t, SEEDSIGNER_ICON_COLOR_DEFAULT
#include "gui_constants.h"    // INFO_COLOR, ICON_LARGE_BUTTON_FONT__SEEDSIGNER, seedsigner_latin_font, SeedSignerIconConstants
#include "screen_helpers.h"   // ensure_top_nav_structure, require_top_nav_title

#include "lvgl.h"             // upper_body flex grow/align setters

#include <nlohmann/json.hpp>  // json (cfg reads + structural-default writes)

#include <stdexcept>          // std::runtime_error (required-field validation)
#include <string>             // std::string

using json = nlohmann::json;


void seed_finalize_screen(void *ctx_json) {
    // --- Config ---

    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    // Required fields: fingerprint is the datum this screen exists to show;
    // fingerprint_label + button_list are user-visible CONTENT, which always
    // arrives localized from the host view layer (a string literal baked here
    // would be English-only by construction). One throw per field, before the
    // scaffold exists, so no throw path can leak LVGL objects.
    if (!cfg.contains("fingerprint") || !cfg["fingerprint"].is_string()) {
        throw std::runtime_error("seed_finalize_screen: fingerprint is required and must be a string");
    }
    if (!cfg.contains("fingerprint_label") || !cfg["fingerprint_label"].is_string()) {
        throw std::runtime_error("seed_finalize_screen: fingerprint_label is required and must be a string");
    }
    if (!cfg.contains("button_list") || !cfg["button_list"].is_array() || cfg["button_list"].empty()) {
        throw std::runtime_error("seed_finalize_screen: button_list is required and must be a non-empty array");
    }
    std::string fingerprint       = cfg["fingerprint"].get<std::string>();
    std::string fingerprint_label = cfg["fingerprint_label"].get<std::string>();

    // Structural defaults (write-if-absent, never user-visible text). Python
    // ButtonListScreen defaults: show_back_button=True, show_power_button=False.
    // The localized title itself is content and must come from the host.
    ensure_top_nav_structure(cfg, /*default_show_back_button=*/true,
                                  /*default_show_power_button=*/false);
    require_top_nav_title(cfg, "seed_finalize_screen");

    // Forced (not defaulted) overrides — a host-supplied value is ignored:
    cfg["top_nav"]["show_back_button"] = false;    // Python __post_init__: show_back_button = False
    cfg["is_bottom_list"] = true;                  // Python: is_bottom_list = True

    // --- Scaffold ---

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, /*scrollable=*/false);

    // --- Body ---

    // Python centers the fingerprint IconTextLine in the band between the top-nav
    // and the first button. The scaffold's bottom-list body is a flex column
    // [upper_body][spacer grow=1][buttons]; make upper_body itself the grower and
    // center its child on the main (vertical) axis, then zero the scaffold spacer
    // so upper_body claims the whole gap. Result: the readout sits vertically
    // centered above the buttons. (This grow/center/collapse trio is shared
    // verbatim by the centered-readout screens — slated for extraction at the
    // rollout decision.)
    lv_obj_set_flex_grow(screen.upper_body, 1);
    lv_obj_set_flex_align(screen.upper_body, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if (screen.button_list_spacer) lv_obj_set_flex_grow(screen.button_list_spacer, 0);

    // 1. Fingerprint readout via the shared IconTextLine component — the SAME
    //    widget the xpub-details / review-passphrase screens use, so labeled-value
    //    spacing (gap, leading reclaim) is identical across every screen. Large
    //    blue fingerprint glyph, gray label over the Latin body+2 hex value;
    //    label_font stays unset so the localized label renders in the locale-aware
    //    BODY_FONT. upper_body's center alignment centers the whole block.
    icon_text_line_opts_t fingerprint_opts = {};
    fingerprint_opts.icon_glyph   = SeedSignerIconConstants::FINGERPRINT;
    fingerprint_opts.icon_font    = &ICON_LARGE_BUTTON_FONT__SEEDSIGNER;   // Python icon_size = ICON_FONT_SIZE+12 (~36)
    fingerprint_opts.icon_color   = INFO_COLOR;
    fingerprint_opts.label_text   = fingerprint_label.c_str();
    fingerprint_opts.value_text   = fingerprint.c_str();
    fingerprint_opts.value_font   = seedsigner_latin_font(19);             // Python value font_size = body+2; a fingerprint is always Latin hex
    fingerprint_opts.label_color  = SEEDSIGNER_ICON_COLOR_DEFAULT;         // -> LABEL_FONT_COLOR (gray)
    fingerprint_opts.value_color  = SEEDSIGNER_ICON_COLOR_DEFAULT;         // -> BODY_FONT_COLOR
    fingerprint_opts.is_text_centered = true;
    icon_text_line(screen.upper_body, &fingerprint_opts);

    // --- Navigation + load ---

    // Menu-style default index: an action list always has a selection, so the first
    // button (Done) starts focused (the host may override via cfg initial_selected_index).
    bind_screen_navigation(cfg, screen, /*default_initial_index=*/0);

    load_screen_and_cleanup_previous(screen.screen);
}
