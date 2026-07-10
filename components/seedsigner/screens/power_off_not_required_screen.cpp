// power_off_not_required_screen
//
// Python provenance: PowerOffNotRequiredScreen (screen.py)
//
// Shown in place of a power-off flow on hardware that needs no shutdown
// sequence: one centered, wrapped body message reassuring the user that the
// device can simply be unplugged. No action buttons — the top-nav back button
// is the only focusable, and its press returns through the standard
// navigation callback.
//
// Layout note: Python renders the message as a full-height TextArea
// (screen_y = top_nav.height, height = canvas_height - top_nav.height), which
// centers the text vertically in the band below the top-nav; here one label
// is LV_ALIGN_CENTER-ed in the scaffold body for the same rendering.
//
// Lifecycle: Tier 1 (stateless) — no statics, no heap ctx, no cleanup callback.
//
// Skeleton note: reset_screen / donate_screen share this simple-text screen
// shape — a shared helper is slated for extraction at the rollout decision
// (spec extraction ledger #10); each file keeps its own copy until then.
//
// cfg:
//   top_nav.title             (string, required)     localized title
//            (Python: _("Just Unplug It")).
//   top_nav.show_back_button  (bool, default true)   Python: show_back_button = True.
//   top_nav.show_power_button (bool, default false)  Python BaseTopNavScreen default.
//   text                      (string, required)     localized body message
//            (Python: _("It is safe to disconnect power at any time.")).
//   initial_selected_index    (int, optional)        read by the navigation layer
//            but moot here — the screen registers no body focusables.
//   input.mode                (string, optional)     "touch" | "hardware" input-mode
//            override (navigation layer).
//   input.keys.key1/key2/key3 (string, optional)     per-aux-key policy "enter" |
//            "noop" | "emit" (navigation layer).
//   allow_screensaver         (bool, default true)   per-screen screensaver policy
//            (normalized by parse_screen_json_ctx, stamped by the scaffold).

#include "screen_scaffold.h"  // parse_screen_json_ctx / create_top_nav_screen_scaffold / bind_screen_navigation / load_screen_and_cleanup_previous
#include "seedsigner.h"       // power_off_not_required_screen decl, screen_scaffold_t
#include "navigation.h"       // NAV_BODY_VERTICAL, NAV_INDEX_NONE (no-buttons nav bind)
#include "screen_helpers.h"   // ensure_top_nav_structure, require_top_nav_title, make_body_text_label, apply_body_tight_line_spacing

#include "lvgl.h"             // lv_obj_align / lv_obj_get_content_width (centered body label)

#include <nlohmann/json.hpp>  // json (cfg reads + structural-default writes)

#include <stdexcept>          // std::runtime_error (required-field validation)
#include <string>             // std::string

using json = nlohmann::json;


void power_off_not_required_screen(void *ctx_json) {
    // --- Config ---

    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    // Required field: the body message is user-visible CONTENT, which always
    // arrives localized from the host view layer (a string literal baked here
    // would be English-only by construction). Throw before the scaffold
    // exists, so no throw path can leak LVGL objects.
    if (!cfg.contains("text") || !cfg["text"].is_string()) {
        throw std::runtime_error("power_off_not_required_screen: text is required and must be a string");
    }
    std::string text = cfg["text"].get<std::string>();

    // Structural defaults (write-if-absent, never user-visible text). Python:
    // show_back_button = True; show_power_button inherits the BaseTopNavScreen
    // default (False). The localized title itself is content and must come
    // from the host.
    ensure_top_nav_structure(cfg, /*default_show_back_button=*/true,
                                  /*default_show_power_button=*/false);
    require_top_nav_title(cfg, "power_off_not_required_screen");

    // --- Scaffold ---

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, /*scrollable=*/true);

    // --- Body ---

    // 1. The reassurance message. No button_list -> scaffold Mode 1 (body ==
    //    upper_body). One centered, wrapped body label, vertically centered to
    //    match Python's full-height TextArea (see the layout note in the banner).
    lv_obj_t *label = make_body_text_label(screen.body, text.c_str(),
                                           lv_obj_get_content_width(screen.body));
    apply_body_tight_line_spacing(label);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);

    // --- Navigation + load ---

    // No body items: NULL/0/NAV_INDEX_NONE is the no-buttons nav contract
    // (deliberately NOT the 3-arg scaffold-buttons overload) — only the
    // top-nav back button is focusable.
    bind_screen_navigation(cfg, screen, NULL, 0, NAV_BODY_VERTICAL, NAV_INDEX_NONE);

    load_screen_and_cleanup_previous(screen.screen);
}
