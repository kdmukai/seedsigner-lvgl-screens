// reset_screen
//
// Python provenance: ResetScreen (screen.py)
//
// Shown while SeedSigner restarts and wipes its in-memory data. A plain title
// over one vertically-centered body message, with NO back button and NO power
// button — the app restarts on its own, so there is nothing for the user to
// dismiss. The screen takes no input and returns no result. Stateless
// (lifecycle Tier 1): no statics, no timers, no heap ctx.
//
// Layout: no button_list -> scaffold Mode 1 (body == upper_body, a plain scroll
// container below the top nav) holding a single centered, wrapped body label —
// matching Python's full-height TextArea vertical centering. Structurally the
// minimal member of the simple-text trio (reset / power_off_not_required /
// donate); the trio keeps three independent skeletons — a shared simple-text
// helper is slated for extraction at rollout decision.
//
// cfg:
//   text                      (string, required)     localized restart/wipe message,
//            rendered as the single centered body block (Python: _("SeedSigner is
//            restarting.\n\nAll in-memory data will be wiped.")).
//   top_nav.title             (string, required)     localized screen title (read by
//            the scaffold; enforced here via require_top_nav_title; Python:
//            _("Restarting")).
//   top_nav.show_back_button  (bool, default false)  Python ResetScreen sets
//            show_back_button = False — nothing to navigate back to mid-restart.
//   top_nav.show_power_button (bool, default false)  Python BaseTopNavScreen default.
//   input.mode                (string, optional)     "touch" | "hardware" input-mode
//            override (navigation layer).
//   input.keys.key1/key2/key3 (string, optional)     per-aux-key policy "enter" |
//            "noop" | "emit" (navigation layer).
//   initial_selected_index    (int, optional)        read by the navigation layer
//            but moot here — the screen registers no body focusables.
//   allow_screensaver         (bool, default true)   per-screen screensaver policy
//            (normalized by parse_screen_json_ctx, stamped by the scaffold).

#include "screen_scaffold.h"  // parse_screen_json_ctx / create_top_nav_screen_scaffold / bind_screen_navigation / load_screen_and_cleanup_previous
#include "seedsigner.h"       // reset_screen decl, screen_scaffold_t
#include "navigation.h"       // NAV_BODY_VERTICAL, NAV_INDEX_NONE
#include "screen_helpers.h"   // ensure_top_nav_structure, require_top_nav_title, make_body_text_label, apply_body_tight_line_spacing

#include "lvgl.h"             // lv_obj_get_content_width (body wrap width), lv_obj_align

#include <nlohmann/json.hpp>  // json (cfg reads + structural-default writes)

#include <stdexcept>          // std::runtime_error (required-field validation)
#include <string>             // std::string

using json = nlohmann::json;


void reset_screen(void *ctx_json) {
    // --- Config ---

    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    // Required field: the restart/wipe message is user-visible CONTENT, which
    // always arrives localized from the host view layer (a string literal baked
    // here would be English-only by construction). One throw, before the
    // scaffold exists, so no throw path can leak LVGL objects.
    if (!cfg.contains("text") || !cfg["text"].is_string()) {
        throw std::runtime_error("reset_screen: text is required and must be a string");
    }
    std::string text = cfg["text"].get<std::string>();

    // Structural defaults (write-if-absent, never user-visible text). Python
    // ResetScreen sets show_back_button = False and inherits show_power_button =
    // False — no nav buttons at all: the app restarts on its own. The localized
    // title itself is content and must come from the host.
    ensure_top_nav_structure(cfg, /*default_show_back_button=*/false,
                                  /*default_show_power_button=*/false);
    require_top_nav_title(cfg, "reset_screen");

    // --- Scaffold ---

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, /*scrollable=*/true);

    // --- Body ---

    // 1. The restart/wipe message. No button_list -> scaffold Mode 1: body ==
    //    upper_body, a plain (non-flex) scroll container spanning the area below
    //    the top nav. Render one centered, wrapped body label and vertically
    //    center it in that area — matching Python's full-height TextArea, which
    //    vertically centers a body that fits (is_text_centered=True;
    //    text_offset_y = (height - total_text_height) / 2).
    lv_obj_t *label = make_body_text_label(screen.body, text.c_str(),
                                           lv_obj_get_content_width(screen.body));
    apply_body_tight_line_spacing(label);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);

    // --- Navigation + load ---

    // No-buttons contract: nothing on this screen is focusable (no body items,
    // and both top-nav buttons are hidden by default), so nav binds with the
    // literal NULL, 0, NAV_INDEX_NONE form — a deliberately different contract
    // from the scaffold-buttons overload; do not migrate it there (extraction
    // ledger #2, docs/screen-conformance-spec.md). The call still runs so the
    // screen gets the standard input plumbing (aux-key policy, mode override,
    // and any host-shown top-nav button).
    bind_screen_navigation(cfg, screen, NULL, 0, NAV_BODY_VERTICAL, NAV_INDEX_NONE);

    load_screen_and_cleanup_previous(screen.screen);
}
