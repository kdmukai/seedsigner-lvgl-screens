// seed_transcribe_seedqr_format_screen
//
// Python provenance: SeedTranscribeSeedQRFormatScreen (seed_screens.py, a
// ButtonListScreen subclass), reached via SeedTranscribeSeedQRFormatView.
//
// The "which SeedQR format?" chooser in the hand-transcription flow: a bottom-pinned
// [Standard, Compact] button list under two label/value explanation rows that tell the
// user what each format encodes. The two labeled rows are what make this need its own
// entry point — the generic button_list_screen has only a single plain intro-text block,
// not a pair of caption/value rows.
//
//   Standard : BIP-39 wordlist indices
//   Compact  : Raw entropy bits
//
// Each row is Python's IconTextLine WITHOUT an icon (a small gray caption above a white
// body-font value, left-justified). Both the captions and the button labels are localized
// by the host (the button labels are dynamic per seed length — Python "Standard: 25x25" /
// "Compact: 21x21" for 12 words, "…29x29" / "…25x25" for 24), so all user-visible strings
// arrive via cfg.
//
// Documented deviation from Python: both value rows word-wrap to the content width, where
// Python sets auto_line_break only on the first row. Wrapping both keeps a long localized
// explanation on-screen instead of clipping it; the short English defaults are single-line
// either way, so en is unchanged.
//
// Lifecycle: Tier 1 (stateless) — no statics, timers, or heap ctx; all state is
// widget-tree-owned. Ends with load_screen_and_cleanup_previous.
//
// cfg:
//   top_nav.title             (string, required)     localized screen title
//            (Python: _("SeedQR Format")).
//   top_nav.show_back_button  (bool, default true)   Python ButtonListScreen default.
//   top_nav.show_power_button (bool, default false)  Python ButtonListScreen default.
//   button_list               (array, required, non-empty)  the localized choices
//            (Python: "Standard: NxN", "Compact: MxM").
//   is_bottom_list            forced true (Python: is_bottom_list = True); a
//            host-supplied value is ignored.
//   standard_label            (string, required)     row-1 caption (Python: _("Standard")).
//   standard_text             (string, required)     row-1 value (Python: _("BIP-39 wordlist indices")).
//   compact_label             (string, required)     row-2 caption (Python: _("Compact")).
//   compact_text              (string, required)     row-2 value (Python: _("Raw entropy bits")).
//   initial_selected_index    (int, optional)        overrides the default focus of 0
//            (navigation layer; Python selected_button).
//   input.mode                (string, optional)     "touch" | "hardware" input-mode
//            override (navigation layer).
//   input.keys.key1/key2/key3 (string, optional)     per-aux-key policy "enter" |
//            "noop" | "emit" (navigation layer).
//   allow_screensaver         (bool, default true)   per-screen screensaver policy
//            (normalized by parse_screen_json_ctx, stamped by the scaffold).

#include "screen_scaffold.h"  // parse_screen_json_ctx / create_top_nav_screen_scaffold / bind_screen_navigation / load_screen_and_cleanup_previous
#include "seedsigner.h"       // seed_transcribe_seedqr_format_screen decl, screen_scaffold_t
#include "components.h"       // icon_text_line + icon_text_line_opts_t, SEEDSIGNER_ICON_COLOR_DEFAULT
#include "gui_constants.h"    // EDGE_PADDING, COMPONENT_PADDING
#include "screen_helpers.h"   // ensure_top_nav_structure, require_top_nav_title

#include "lvgl.h"             // upper_body flex grow/align/pad setters, lv_display_get_horizontal_resolution

#include <nlohmann/json.hpp>  // json (cfg reads + structural-default writes)

#include <stdexcept>          // std::runtime_error (required-field validation)
#include <string>             // std::string

using json = nlohmann::json;


void seed_transcribe_seedqr_format_screen(void *ctx_json) {
    // --- Config ---

    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    // Required fields — the two explanation rows' captions/values and the button list are
    // all user-visible CONTENT that always arrives localized from the host view layer (a
    // string literal baked here would be English-only by construction). One throw per
    // field, before the scaffold exists, so no throw path can leak LVGL objects.
    if (!cfg.contains("button_list") || !cfg["button_list"].is_array() || cfg["button_list"].empty()) {
        throw std::runtime_error("seed_transcribe_seedqr_format_screen: button_list is required and must be a non-empty array");
    }
    if (!cfg.contains("standard_label") || !cfg["standard_label"].is_string()) {
        throw std::runtime_error("seed_transcribe_seedqr_format_screen: standard_label is required and must be a string");
    }
    if (!cfg.contains("standard_text") || !cfg["standard_text"].is_string()) {
        throw std::runtime_error("seed_transcribe_seedqr_format_screen: standard_text is required and must be a string");
    }
    if (!cfg.contains("compact_label") || !cfg["compact_label"].is_string()) {
        throw std::runtime_error("seed_transcribe_seedqr_format_screen: compact_label is required and must be a string");
    }
    if (!cfg.contains("compact_text") || !cfg["compact_text"].is_string()) {
        throw std::runtime_error("seed_transcribe_seedqr_format_screen: compact_text is required and must be a string");
    }
    std::string standard_label = cfg["standard_label"].get<std::string>();
    std::string standard_text  = cfg["standard_text"].get<std::string>();
    std::string compact_label  = cfg["compact_label"].get<std::string>();
    std::string compact_text   = cfg["compact_text"].get<std::string>();

    // Structural defaults (write-if-absent, never user-visible text). Python
    // ButtonListScreen defaults: show_back_button=True, show_power_button=False. The
    // localized title is content and must come from the host.
    ensure_top_nav_structure(cfg, /*default_show_back_button=*/true,
                                  /*default_show_power_button=*/false);
    require_top_nav_title(cfg, "seed_transcribe_seedqr_format_screen");

    cfg["is_bottom_list"] = true;    // forced, not defaulted — Python: is_bottom_list = True

    // --- Scaffold ---

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, /*scrollable=*/false);

    // --- Body ---

    // Two left-aligned caption/value rows, top-anchored under the nav (Python screen_x =
    // EDGE_PADDING, row 1 at top_nav.height + COMPONENT_PADDING). Grow upper_body to claim
    // the band above the bottom-pinned buttons and collapse the scaffold spacer; the
    // 2*COMPONENT_PADDING flex row-gap matches Python's inter-row spacing (row 2 screen_y =
    // row1 + height + 2*COMPONENT_PADDING). Same header idiom as
    // tools_address_explorer_address_type_screen, minus the icons.
    lv_obj_set_flex_grow(screen.upper_body, 1);
    lv_obj_set_flex_align(screen.upper_body, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_top(screen.upper_body, COMPONENT_PADDING, LV_PART_MAIN);
    lv_obj_set_style_pad_left(screen.upper_body, EDGE_PADDING, LV_PART_MAIN);
    lv_obj_set_style_pad_row(screen.upper_body, 2 * COMPONENT_PADDING, LV_PART_MAIN);
    // Collapse the scaffold's flex spacer so upper_body (not the spacer) claims the whole
    // band above the bottom-pinned buttons — otherwise both grow=1 and split the free space,
    // leaving upper_body too short to contain the second row (it would clip). Same as the
    // seed_export_xpub / address-type header idiom.
    if (screen.button_list_spacer) lv_obj_set_flex_grow(screen.button_list_spacer, 0);

    // Value wrap width: from the content's left edge (EDGE_PADDING) to a matching right
    // margin, so a long localized explanation wraps instead of clipping at the screen edge.
    const int32_t display_width = lv_display_get_horizontal_resolution(NULL);
    const int32_t value_wrap_width = display_width - 2 * EDGE_PADDING;

    // 1. Standard row — small gray caption over the white body-font value, no icon,
    //    left-justified (Python IconTextLine, icon_name omitted, is_text_centered=False).
    icon_text_line_opts_t standard_opts = {};
    standard_opts.label_text       = standard_label.c_str();
    standard_opts.value_text       = standard_text.c_str();
    standard_opts.label_color      = SEEDSIGNER_ICON_COLOR_DEFAULT;   // -> LABEL_FONT_COLOR (gray)
    standard_opts.value_color      = SEEDSIGNER_ICON_COLOR_DEFAULT;   // -> BODY_FONT_COLOR (white)
    standard_opts.is_text_centered = false;
    standard_opts.value_wrap_width = value_wrap_width;
    icon_text_line(screen.upper_body, &standard_opts);

    // 2. Compact row — same treatment.
    icon_text_line_opts_t compact_opts = {};
    compact_opts.label_text       = compact_label.c_str();
    compact_opts.value_text       = compact_text.c_str();
    compact_opts.label_color      = SEEDSIGNER_ICON_COLOR_DEFAULT;    // -> LABEL_FONT_COLOR (gray)
    compact_opts.value_color      = SEEDSIGNER_ICON_COLOR_DEFAULT;    // -> BODY_FONT_COLOR (white)
    compact_opts.is_text_centered = false;
    compact_opts.value_wrap_width = value_wrap_width;
    icon_text_line(screen.upper_body, &compact_opts);

    // --- Navigation + load ---

    // Menu-style default index: a choice list always has a selection, so the first button
    // (Standard) starts focused (the host may override via initial_selected_index).
    bind_screen_navigation(cfg, screen, /*default_initial_index=*/0);

    load_screen_and_cleanup_previous(screen.screen);
}
