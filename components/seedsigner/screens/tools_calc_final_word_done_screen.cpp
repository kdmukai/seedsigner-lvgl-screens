// tools_calc_final_word_done_screen
//
// Python provenance: ToolsCalcFinalWordDoneScreen (tools_screens.py)
//
// The final step of the "Calc final word" tool: a bottom-pinned button-list
// screen (host-supplied actions; Python builds LOAD "Load seed" + DISCARD
// "Discard" with a red label) with a back button; above the buttons it shows
// two elements:
//
//   1. the freshly-derived FINAL WORD, large and centered, wrapped in quotes
//      (Python: TextArea(text=f'"{final_word}"', font_size=26, centered)).
//   2. a centered fingerprint readout — the master-fingerprint hex under a
//      small "fingerprint" label beside a blue fingerprint icon (Python:
//      IconTextLine(icon=FINGERPRINT, icon_color=INFO_COLOR,
//      label_text=_("fingerprint"), value_text=fingerprint,
//      is_text_centered=True)).
//
// The title is the ordinal word label — Python derives "12th Word" / "24th Word"
// from mnemonic_word_length in __post_init__ and localizes both via gettext, so
// the host view layer supplies the already-localized string as top_nav.title.
// is_bottom_list is forced true (Python: is_bottom_list = True); a host-supplied
// value is ignored.
//
// Layout note: unlike seed_finalize_screen (which vertically CENTERS its single
// readout in the gap), this screen is TOP-anchored — the word sits a
// COMPONENT_PADDING below the top-nav and the fingerprint line 3*COMPONENT_PADDING
// below the word (Python's absolute screen_y math), with the scaffold's bottom
// spacer pushing the buttons to the viewport bottom.
//
// Lifecycle: Tier 1 (stateless) — no statics, timers, or heap ctx; all state is
// widget-tree-owned or stack-local.
//
// cfg:
//   top_nav.title             (string, required)     localized ordinal title
//            ("12th Word" / "24th Word", derived host-side from the mnemonic length).
//   top_nav.show_back_button  (bool, default true)   Python ButtonListScreen default;
//            the reference shows the back arrow.
//   top_nav.show_power_button (bool, default false)  Python ButtonListScreen default.
//   final_word                (string, required)     the derived final mnemonic word
//            (unquoted; this screen adds the surrounding quotes, matching Python).
//   fingerprint               (string, required)     the master-fingerprint hex to display.
//   fingerprint_label         (string, required)     localized small label above the
//            fingerprint value (Python: _("fingerprint")).
//   button_list               (array, required, non-empty)  the localized action
//            buttons (Python: LOAD "Load seed", DISCARD "Discard" with label_color red).
//   initial_selected_index    (int, optional)        overrides the default initial
//            focus of 0 (navigation layer; Python selected_button).
//   input.mode                (string, optional)     "touch" | "hardware" input-mode
//            override (navigation layer).
//   input.keys.key1/key2/key3 (string, optional)     per-aux-key policy "enter" |
//            "noop" | "emit" (navigation layer).
//   allow_screensaver         (bool, default true)   per-screen screensaver policy
//            (normalized by parse_screen_json_ctx, stamped by the scaffold).

#include "screen_scaffold.h"  // parse_screen_json_ctx / create_top_nav_screen_scaffold / bind_screen_navigation / load_screen_and_cleanup_previous
#include "seedsigner.h"       // tools_calc_final_word_done_screen decl, text_top_leading
#include "components.h"       // icon_text_line, SEEDSIGNER_ICON_COLOR_DEFAULT
#include "gui_constants.h"    // COMPONENT_PADDING, BODY_FONT, BODY_FONT_COLOR, INFO_COLOR, seedsigner_latin_font, SeedSignerIconConstants
#include "screen_helpers.h"   // ensure_top_nav_structure, require_top_nav_title

#include "lvgl.h"             // lv_label + per-object style setters

#include <nlohmann/json.hpp>  // json (cfg reads + structural-default writes)

#include <stdexcept>          // std::runtime_error (required-field validation)
#include <string>             // std::string

using json = nlohmann::json;


void tools_calc_final_word_done_screen(void *ctx_json) {
    // --- Config ---

    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    // Required fields: final_word + fingerprint are the post-derivation results
    // this screen exists to show; fingerprint_label + button_list are user-visible
    // CONTENT, which always arrives localized from the host view layer (a string
    // literal baked here would be English-only by construction). One throw per
    // field, before the scaffold exists, so no throw path can leak LVGL objects.
    if (!cfg.contains("final_word") || !cfg["final_word"].is_string()) {
        throw std::runtime_error("tools_calc_final_word_done_screen: final_word is required and must be a string");
    }
    if (!cfg.contains("fingerprint") || !cfg["fingerprint"].is_string()) {
        throw std::runtime_error("tools_calc_final_word_done_screen: fingerprint is required and must be a string");
    }
    if (!cfg.contains("fingerprint_label") || !cfg["fingerprint_label"].is_string()) {
        throw std::runtime_error("tools_calc_final_word_done_screen: fingerprint_label is required and must be a string");
    }
    if (!cfg.contains("button_list") || !cfg["button_list"].is_array() || cfg["button_list"].empty()) {
        throw std::runtime_error("tools_calc_final_word_done_screen: button_list is required and must be a non-empty array");
    }
    std::string final_word        = cfg["final_word"].get<std::string>();
    std::string fingerprint       = cfg["fingerprint"].get<std::string>();
    std::string fingerprint_label = cfg["fingerprint_label"].get<std::string>();

    // Structural defaults (write-if-absent, never user-visible text). Python
    // ButtonListScreen defaults: show_back_button=True, show_power_button=False.
    // The localized title itself is content and must come from the host.
    ensure_top_nav_structure(cfg, /*default_show_back_button=*/true,
                                  /*default_show_power_button=*/false);
    require_top_nav_title(cfg, "tools_calc_final_word_done_screen");

    cfg["is_bottom_list"] = true;    // forced, not defaulted — Python: is_bottom_list = True

    // --- Scaffold ---

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, /*scrollable=*/false);

    // --- Body ---

    // The bottom-list body is a flex column [upper_body][spacer grow=1][buttons].
    // Leave upper_body ungrown (top-anchored) so the word + fingerprint sit just
    // below the top-nav (Python's absolute screen_y anchoring) while the spacer
    // pins the buttons to the bottom. Zero the flex row-gap so the inter-element
    // spacing is ONLY the explicit margins set below.
    lv_obj_set_style_pad_row(screen.upper_body, 0, LV_PART_MAIN);

    // 1. The final word — large, centered, quote-wrapped. Python uses the default
    //    (OpenSans) body font at size 26; a BIP-39 word is always Latin, so the
    //    Latin face is faithful and covers it. Anchored a COMPONENT_PADDING below
    //    the top-nav (Python screen_y = top_nav.height + COMPONENT_PADDING),
    //    reclaiming the font's top leading so the visible caps land where PIL puts
    //    them rather than a leading-height lower.
    const lv_font_t *word_font = seedsigner_latin_font(26);
    std::string quoted_word = "\"" + final_word + "\"";

    lv_obj_t *word_label = lv_label_create(screen.upper_body);
    lv_label_set_text(word_label, quoted_word.c_str());
    lv_obj_set_style_text_font(word_label, word_font, LV_PART_MAIN);
    lv_obj_set_style_text_color(word_label, lv_color_hex(BODY_FONT_COLOR), LV_PART_MAIN);
    lv_obj_set_style_text_align(word_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(word_label, 0, LV_PART_MAIN);
    int32_t word_top_gap = COMPONENT_PADDING - text_top_leading(word_font, quoted_word.c_str());
    if (word_top_gap < 0) word_top_gap = 0;
    lv_obj_set_style_margin_top(word_label, word_top_gap, LV_PART_MAIN);

    // 2. Fingerprint readout via the shared IconTextLine component (the SAME widget
    //    seed_finalize / xpub-details use). Python here uses the IconTextLine
    //    DEFAULTS — icon_size = ICON_FONT_SIZE (24 px), value/label at the body font
    //    size — so we leave icon_font/value_font/label_font unset and only color the
    //    icon blue. upper_body's cross-axis centering centers the whole block.
    //    Python spaces it 3*COMPONENT_PADDING below the word.
    icon_text_line_opts_t fingerprint_opts = {};
    fingerprint_opts.icon_glyph       = SeedSignerIconConstants::FINGERPRINT;
    fingerprint_opts.icon_color       = INFO_COLOR;
    fingerprint_opts.label_text       = fingerprint_label.c_str();
    fingerprint_opts.value_text       = fingerprint.c_str();
    fingerprint_opts.label_color      = SEEDSIGNER_ICON_COLOR_DEFAULT;   // -> LABEL_FONT_COLOR (gray)
    fingerprint_opts.value_color      = SEEDSIGNER_ICON_COLOR_DEFAULT;   // -> BODY_FONT_COLOR
    fingerprint_opts.is_text_centered = true;
    lv_obj_t *fingerprint_row = icon_text_line(screen.upper_body, &fingerprint_opts);
    if (fingerprint_row) {
        // Python's 3*COMPONENT_PADDING is measured from the word TextArea's tight box
        // bottom to the IconTextLine's box top. LVGL's word label box is a full
        // line-height tall (overshooting the visible glyph bottom), and the
        // fingerprint block's top line (the label) carries its own leading above the
        // caps. Reclaim both so the VISIBLE gap between the word and the readout
        // matches PIL.
        int32_t fingerprint_gap = 3 * COMPONENT_PADDING
                                  - text_top_leading(word_font, quoted_word.c_str())
                                  - text_top_leading(&BODY_FONT, fingerprint_label.c_str());
        if (fingerprint_gap < 0) fingerprint_gap = 0;
        lv_obj_set_style_margin_top(fingerprint_row, fingerprint_gap, LV_PART_MAIN);
    }

    // --- Navigation + load ---

    // Menu-style default index: an action list always has a selection, so the first
    // button starts focused (the host may override via cfg initial_selected_index).
    bind_screen_navigation(cfg, screen, /*default_initial_index=*/0);

    load_screen_and_cleanup_previous(screen.screen);
}
