// seed_export_xpub_details_screen
//
// Python provenance: SeedExportXpubDetailsScreen (seed_screens.py)
//
// The xpub-export summary: fingerprint, derivation path, and the (truncated)
// xpub — three IconTextLine rows sharing one icon column so their text columns
// align — stacked top-anchored and left-aligned under the top-nav, with the
// host-supplied action button(s) pinned to the viewport bottom.
// WarningEdgesMixin frames the screen in pulsing YELLOW (WARNING_COLOR): an
// xpub leaks viewable transaction history — a privacy caution, not the
// key-material dire warning (orange) used for seed material.
//
// Layout notes: Python spaces consecutive lines int(1.5 * COMPONENT_PADDING)
// apart; here the inter-line gap is a plain COMPONENT_PADDING flex row-gap — a
// documented deviation, because the 1.5x gap would leave the xpub line hugging
// the button on the 240 px body. The xpub is truncated HERE, not host-side, so
// the character budget tracks each display profile's monospace char width (the
// LVGL port of Python's num_chars math).
//
// Lifecycle: stateless Tier 1 — no statics, timers, heap ctx, or cleanup callback.
//
// cfg:
//   top_nav.title             (string, required)     localized screen title
//            (Python: _("Xpub Details")).
//   top_nav.show_back_button  (bool, default true)   Python ButtonListScreen default.
//   top_nav.show_power_button (bool, default false)  Python ButtonListScreen default.
//   fingerprint               (string, required)     BIP-32 master fingerprint (hex),
//            rendered verbatim.
//   derivation_path           (string, required)     the derivation path, rendered
//            verbatim. Required like its fingerprint/xpub siblings: it is
//            security-relevant host data, and a fabricated fallback would show a
//            confident, wrong derivation on a host bug. (Python's dataclass
//            default "m/84'/0'/0'" is a placeholder the view layer always
//            overrides with the real path.)
//   xpub                      (string, required)     the extended pubkey; truncated
//            to one display line here (see Layout notes).
//   fingerprint_label         (string, required)     localized field caption
//            (Python: _("Fingerprint"), short for "BIP-32 Master Fingerprint").
//   derivation_label          (string, required)     localized field caption
//            (Python: _("Derivation"), short for "Derivation Path").
//   xpub_label                (string, required)     localized field caption
//            (Python: _("Xpub")).
//   button_list               (array, required, non-empty)  the localized action
//            button(s) (Python: ButtonOption("Export xpub")).
//   is_bottom_list            forced true (Python: is_bottom_list = True); a
//            host-supplied value is ignored.
//   initial_selected_index    (int, optional)        initial focused-button
//            override; read by the navigation layer (default 0 = first button).
//   input.mode                (string, optional)     "touch" | "hardware" input-mode
//            override (navigation layer).
//   input.keys.key1/key2/key3 (string, optional)     per-aux-key policy "enter" |
//            "noop" | "emit" (navigation layer).
//   allow_screensaver         (bool, default true)   per-screen screensaver policy
//            (normalized by parse_screen_json_ctx, stamped by the scaffold).

#include "screen_scaffold.h"  // parse_screen_json_ctx / create_top_nav_screen_scaffold / add_warning_edges_overlay / bind_screen_navigation / load_screen_and_cleanup_previous
#include "seedsigner.h"       // seed_export_xpub_details_screen decl, screen_scaffold_t
#include "components.h"       // icon_text_line, monospace_char_width, SEEDSIGNER_ICON_COLOR_DEFAULT
#include "gui_constants.h"    // COMPONENT_PADDING, ICON_FONT_SIZE, INFO_COLOR, WARNING_COLOR, CANDIDATE_FONT, KEYBOARD_FONT, SeedSignerIconConstants
#include "screen_helpers.h"   // ensure_top_nav_structure, require_top_nav_title

#include "lvgl.h"             // flex/pad style setters, lv_display_get_horizontal_resolution

#include <nlohmann/json.hpp>  // json (cfg reads + structural-default writes)

#include <stdexcept>          // std::runtime_error (required-field validation)
#include <string>             // std::string

using json = nlohmann::json;


void seed_export_xpub_details_screen(void *ctx_json) {
    // --- Config ---

    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    // Required fields: fingerprint / derivation_path / xpub are the security-
    // relevant read-outs this screen exists to show — a fabricated fallback would
    // display a confident, wrong value on a host bug, so all three fail loudly.
    // The three field captions and button_list are user-visible CONTENT, which
    // always arrives localized from the host view layer (a string literal baked
    // here would be English-only by construction). One throw per field, before
    // the scaffold exists, so no throw path can leak LVGL objects.
    if (!cfg.contains("fingerprint") || !cfg["fingerprint"].is_string()) {
        throw std::runtime_error("seed_export_xpub_details_screen: fingerprint is required and must be a string");
    }
    if (!cfg.contains("derivation_path") || !cfg["derivation_path"].is_string()) {
        throw std::runtime_error("seed_export_xpub_details_screen: derivation_path is required and must be a string");
    }
    if (!cfg.contains("xpub") || !cfg["xpub"].is_string()) {
        throw std::runtime_error("seed_export_xpub_details_screen: xpub is required and must be a string");
    }
    if (!cfg.contains("fingerprint_label") || !cfg["fingerprint_label"].is_string()) {
        throw std::runtime_error("seed_export_xpub_details_screen: fingerprint_label is required and must be a string");
    }
    if (!cfg.contains("derivation_label") || !cfg["derivation_label"].is_string()) {
        throw std::runtime_error("seed_export_xpub_details_screen: derivation_label is required and must be a string");
    }
    if (!cfg.contains("xpub_label") || !cfg["xpub_label"].is_string()) {
        throw std::runtime_error("seed_export_xpub_details_screen: xpub_label is required and must be a string");
    }
    if (!cfg.contains("button_list") || !cfg["button_list"].is_array() || cfg["button_list"].empty()) {
        throw std::runtime_error("seed_export_xpub_details_screen: button_list is required and must be a non-empty array");
    }
    std::string fingerprint       = cfg["fingerprint"].get<std::string>();
    std::string derivation_path   = cfg["derivation_path"].get<std::string>();
    std::string xpub              = cfg["xpub"].get<std::string>();
    std::string fingerprint_label = cfg["fingerprint_label"].get<std::string>();
    std::string derivation_label  = cfg["derivation_label"].get<std::string>();
    std::string xpub_label        = cfg["xpub_label"].get<std::string>();

    // Structural defaults (write-if-absent, never user-visible text). Python
    // ButtonListScreen defaults: show_back_button=True, show_power_button=False.
    // The localized title itself is content and must come from the host.
    ensure_top_nav_structure(cfg, /*default_show_back_button=*/true,
                                  /*default_show_power_button=*/false);
    require_top_nav_title(cfg, "seed_export_xpub_details_screen");

    cfg["is_bottom_list"] = true;    // forced, not defaulted — Python: is_bottom_list = True

    // --- Scaffold ---

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, /*scrollable=*/false);

    // --- Body ---

    // Top-anchored, LEFT-aligned column for the three lines (Python: screen_x =
    // COMPONENT_PADDING, first line COMPONENT_PADDING below the nav). Grow
    // upper_body to claim the whole gap above the button and collapse the
    // scaffold spacer, so the block sits in a container sized to fit rather than
    // a shrink-wrapped one that scrolls its top under the nav. icon_text_line
    // reclaims the LVGL line-height leading so three lines pack at PIL density;
    // the inter-line gap is COMPONENT_PADDING (see the banner's deviation note
    // on Python's 1.5x spacing). This grow/align/collapse trio is kept inline —
    // slated for possible extraction at the rollout decision.
    lv_obj_set_flex_grow(screen.upper_body, 1);
    lv_obj_set_flex_align(screen.upper_body, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_top(screen.upper_body, COMPONENT_PADDING, LV_PART_MAIN);
    lv_obj_set_style_pad_left(screen.upper_body, COMPONENT_PADDING, LV_PART_MAIN);
    lv_obj_set_style_pad_row(screen.upper_body, COMPONENT_PADDING, LV_PART_MAIN);
    if (screen.button_list_spacer) lv_obj_set_flex_grow(screen.button_list_spacer, 0);

    // 1. Fingerprint line — seedsigner FINGERPRINT glyph, body-font value.
    icon_text_line_opts_t fingerprint_opts = {};
    fingerprint_opts.icon_glyph   = SeedSignerIconConstants::FINGERPRINT;
    fingerprint_opts.icon_color   = INFO_COLOR;
    fingerprint_opts.label_text   = fingerprint_label.c_str();
    fingerprint_opts.value_text   = fingerprint.c_str();
    fingerprint_opts.label_color  = SEEDSIGNER_ICON_COLOR_DEFAULT;   // -> LABEL_FONT_COLOR (gray)
    fingerprint_opts.value_color  = SEEDSIGNER_ICON_COLOR_DEFAULT;   // -> BODY_FONT_COLOR
    fingerprint_opts.icon_width   = ICON_FONT_SIZE;                  // shared icon column -> aligned text
    icon_text_line(screen.upper_body, &fingerprint_opts);

    // 2. Derivation line — seedsigner DERIVATION glyph, body-font value.
    icon_text_line_opts_t derivation_opts = {};
    derivation_opts.icon_glyph   = SeedSignerIconConstants::DERIVATION;
    derivation_opts.icon_color   = INFO_COLOR;
    derivation_opts.label_text   = derivation_label.c_str();
    derivation_opts.value_text   = derivation_path.c_str();
    derivation_opts.label_color  = SEEDSIGNER_ICON_COLOR_DEFAULT;    // -> LABEL_FONT_COLOR (gray)
    derivation_opts.value_color  = SEEDSIGNER_ICON_COLOR_DEFAULT;    // -> BODY_FONT_COLOR
    derivation_opts.icon_width   = ICON_FONT_SIZE;                   // same column as fingerprint
    icon_text_line(screen.upper_body, &derivation_opts);

    // 3. Xpub line. Python's icon is FontAwesomeIconConstants.X == the ASCII
    //    letter "X" drawn in the bold FontAwesome font; render it as a bold
    //    monospace "X". The value is the xpub in the fixed-width font (Python
    //    FIXED_WIDTH at body+2; the baked 22 px monospace here), truncated to
    //    fill one line. Truncation is measured HERE so it tracks each display
    //    profile's char width — Python's num_chars math. The trailing "..." is
    //    appended unconditionally — even when the size clamp means the whole
    //    xpub fits — matching Python's f"{xpub[:num_chars]}...".
    const lv_font_t *xpub_font = &CANDIDATE_FONT;   // Inconsolata SemiBold, 22 px @240
    int32_t char_width = monospace_char_width(xpub_font);
    const int32_t display_width = lv_display_get_horizontal_resolution(NULL);
    int xpub_char_count = (int)((display_width - ICON_FONT_SIZE - 2 * COMPONENT_PADDING) / char_width) - 3;  // -3 = "..."
    if (xpub_char_count < 1) xpub_char_count = 1;
    if (xpub_char_count > (int)xpub.size()) xpub_char_count = (int)xpub.size();
    std::string xpub_display = xpub.substr(0, (size_t)xpub_char_count) + "...";

    icon_text_line_opts_t xpub_opts = {};
    xpub_opts.icon_glyph   = "X";                 // Python FontAwesomeIconConstants.X = U+0058
    xpub_opts.icon_font    = &KEYBOARD_FONT;      // bold 24 px monospace X
    xpub_opts.icon_color   = INFO_COLOR;
    xpub_opts.label_text   = xpub_label.c_str();
    xpub_opts.value_text   = xpub_display.c_str();
    xpub_opts.value_font   = xpub_font;
    xpub_opts.label_color  = SEEDSIGNER_ICON_COLOR_DEFAULT;   // -> LABEL_FONT_COLOR (gray)
    xpub_opts.value_color  = SEEDSIGNER_ICON_COLOR_DEFAULT;   // -> BODY_FONT_COLOR
    xpub_opts.icon_width   = ICON_FONT_SIZE;      // same column as fingerprint/derivation -> "X" centers, text aligns
    icon_text_line(screen.upper_body, &xpub_opts);

    // 4. WarningEdgesMixin — pulsing yellow border (privacy-caution YELLOW, not
    //    the dire-warning orange; see the banner).
    add_warning_edges_overlay(screen.screen, WARNING_COLOR);

    // --- Navigation + load ---

    // Menu-style default index: an action list always has a selection, so the first
    // button starts focused (the host may override via cfg initial_selected_index).
    bind_screen_navigation(cfg, screen, /*default_initial_index=*/0);

    load_screen_and_cleanup_previous(screen.screen);
}
