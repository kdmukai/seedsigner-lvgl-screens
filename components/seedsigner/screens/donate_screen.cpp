// donate_screen
//
// Python provenance: DonateScreen (settings_screens.py)
//
// The Settings "Donate" appeal: a wrapped body paragraph asking for community
// funding, then a larger accent-colored URL line ("seedsigner.com") below it.
// The screen has no body focusables — only the top-nav back button — and
// returns only through the top-nav (back / power) navigation callbacks.
// Lifecycle Tier 1 (stateless): no statics, timers, or heap ctx; the
// process-lifetime seedsigner_latin_font(28) instance is memoized by design
// (see gui_constants.h) and is never torn down by screens.
//
// Layout note: unlike its simple-text siblings reset_screen /
// power_off_not_required_screen (which vertically CENTER a single body label),
// Donate is TOP-anchored at explicit offsets, mirroring Python's screen_y math:
//   - paragraph at top_nav.height + 3*COMPONENT_PADDING
//   - URL line one COMPONENT_PADDING below the paragraph.
//
// The URL line renders at 28 px via the runtime seedsigner_latin_font(28) — the
// memoized OpenSans tiny_ttf rasterizer for body-relative sizes the profile
// roles don't cover — so no new font bake is needed. (Python renders this line
// with supersampling_factor=1, i.e. a plain render, which the tiny_ttf path
// matches.)
//
// cfg:
//   text                      (string, required)     localized body paragraph
//            (Python localizes it via gettext: "SeedSigner is 100% free & open
//            source, ...").
//   url                       (string, required)     the donation URL line
//            (Python hardcodes "seedsigner.com"; here it is host-supplied like
//            all user-visible content).
//   top_nav.title             (string, required)     localized screen title (read
//            by the scaffold; enforced here via require_top_nav_title; Python:
//            _("Donate")).
//   top_nav.show_back_button  (bool, default true)   Python BaseTopNavScreen default.
//   top_nav.show_power_button (bool, default false)  Python BaseTopNavScreen default.
//   top_nav.icon              (string, optional)     icon glyph beside the title
//            (read by the scaffold).
//   top_nav.icon_color        (hex string, optional) title icon color (scaffold).
//   input.mode                (string, optional)     "touch" | "hardware" input-mode
//            override (navigation layer).
//   input.keys.key1/key2/key3 (string, optional)     per-aux-key policy "enter" |
//            "noop" | "emit" (navigation layer).
//   initial_selected_index    (int, optional)        read by the navigation layer
//            but moot here — the screen registers no body focusables.
//   allow_screensaver         (bool, default true)   per-screen screensaver policy
//            (normalized by parse_screen_json_ctx, stamped by the scaffold).

#include "screen_scaffold.h"  // parse_screen_json_ctx / create_top_nav_screen_scaffold / bind_screen_navigation / load_screen_and_cleanup_previous
#include "seedsigner.h"       // donate_screen decl, screen_scaffold_t
#include "gui_constants.h"    // COMPONENT_PADDING, ACCENT_COLOR, seedsigner_latin_font
#include "navigation.h"       // NAV_BODY_VERTICAL, NAV_INDEX_NONE (no-buttons bind form)
#include "screen_helpers.h"   // ensure_top_nav_structure, require_top_nav_title, make_body_text_label, apply_body_tight_line_spacing

#include "lvgl.h"             // lv_label + per-object style setters, lv_obj_align_to / update_layout placement

#include <nlohmann/json.hpp>  // json (cfg reads + structural-default writes)

#include <stdexcept>          // std::runtime_error (required-field validation)
#include <string>             // std::string

using json = nlohmann::json;


void donate_screen(void *ctx_json) {
    // --- Config ---

    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    // Required fields: both rendered strings are user-visible CONTENT, which
    // always arrives from the host view layer (the paragraph localized via
    // gettext; the URL passed through verbatim) — a string literal baked here
    // would be English-only by construction. One throw per field, before the
    // scaffold exists, so no throw path can leak LVGL objects.
    if (!cfg.contains("text") || !cfg["text"].is_string()) {
        throw std::runtime_error("donate_screen: text is required and must be a string");
    }
    if (!cfg.contains("url") || !cfg["url"].is_string()) {
        throw std::runtime_error("donate_screen: url is required and must be a string");
    }
    std::string body_text = cfg["text"].get<std::string>();
    std::string url_text  = cfg["url"].get<std::string>();

    // Structural defaults (write-if-absent, never user-visible text). Python
    // BaseTopNavScreen defaults: show_back_button=True, show_power_button=False.
    // The localized title itself is content and must come from the host.
    ensure_top_nav_structure(cfg, /*default_show_back_button=*/true,
                                  /*default_show_power_button=*/false);
    require_top_nav_title(cfg, "donate_screen");

    // --- Scaffold ---

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, /*scrollable=*/true);

    // --- Body ---

    // Scaffold Mode 1 (no button_list): body == upper_body, a plain scroll container.
    const int32_t body_width = lv_obj_get_content_width(screen.body);

    // 1. Body paragraph — centered, wrapped, top-anchored 3*COMPONENT_PADDING below
    //    the top nav (Python screen_y = top_nav.height + 3*COMPONENT_PADDING).
    lv_obj_t *paragraph_label = make_body_text_label(screen.body, body_text.c_str(), body_width);
    apply_body_tight_line_spacing(paragraph_label);
    lv_obj_align(paragraph_label, LV_ALIGN_TOP_MID, 0, 3 * COMPONENT_PADDING);

    // 2. Accent URL line — 28 px OpenSans in ACCENT_COLOR, centered, one
    //    COMPONENT_PADDING below the paragraph. lv_obj_align_to reads the target's
    //    CURRENT coords, so the paragraph's layout must resolve first.
    lv_obj_update_layout(screen.body);

    lv_obj_t *url_label = lv_label_create(screen.body);
    lv_label_set_text(url_label, url_text.c_str());
    lv_obj_set_width(url_label, body_width);
    lv_obj_set_style_text_align(url_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(url_label, lv_color_hex(ACCENT_COLOR), LV_PART_MAIN);
    lv_obj_set_style_text_font(url_label, seedsigner_latin_font(28), LV_PART_MAIN);
    lv_obj_align_to(url_label, paragraph_label, LV_ALIGN_OUT_BOTTOM_MID, 0, COMPONENT_PADDING);

    // --- Navigation + load ---

    // No-buttons nav contract: only the top-nav back button is focusable, so this
    // keeps the literal (NULL, 0, NAV_INDEX_NONE) form — a different contract from
    // the scaffold-buttons overload, not a candidate for it.
    bind_screen_navigation(cfg, screen, NULL, 0, NAV_BODY_VERTICAL, NAV_INDEX_NONE);

    load_screen_and_cleanup_previous(screen.screen);
}
