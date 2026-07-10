// button_list_screen
//
// Python provenance: ButtonListScreen (screen.py)
//
// The generic workhorse list screen: a vertical list of buttons built from
// cfg["button_list"], optionally preceded by an intro-text block (cfg["text"]).
// The scaffold builds the buttons; when cfg["text"] is a non-empty string it
// also gives us a separate `upper_body` (above the buttons) for the text. This
// file validates the required key, renders any intro text into upper_body,
// wires navigation, and loads the screen; the activated button's index goes
// back to the host through the navigation layer's select callback.
//
// When intro text overflows the viewport, bind_screen_navigation auto-enables
// scroll-then-buttons joystick navigation: the text scrolls into view before
// the first button takes focus.
//
// Lifecycle: Tier 1 (stateless) — no statics, timers, or heap ctx; all state is
// widget-tree-owned or stack-local.
//
// cfg:
//   button_list               (array, required)      the localized button entries;
//            each is "Label", ["Label", ...] (label at index 0), or a
//            {"label", "icon"?, "right_icon"?, "icon_color"?, "label_color"?}
//            object (read by the scaffold, one button per entry).
//   text                      (string, optional)     localized intro text rendered
//            by this file into upper_body, above the buttons. Non-empty text also
//            makes the scaffold build the separate upper_body — see the two-file
//            invariant note at the render guard below.
//   top_nav.title             (string, required)     localized screen title (read
//            by the scaffold; enforced here via require_top_nav_title).
//   top_nav.show_back_button  (bool, default true)   Python BaseTopNavScreen default.
//   top_nav.show_power_button (bool, default false)  Python BaseTopNavScreen default.
//   top_nav.icon              (string, optional)     icon glyph beside the title
//            (read by the scaffold; Python top_nav_icon_name).
//   top_nav.icon_color        (hex string, optional) title icon color (scaffold).
//   is_bottom_list            (bool, default false)  pin the buttons to the viewport
//            bottom with a flex spacer (scaffold; Python is_bottom_list).
//   is_button_text_centered   (bool, default true)   Python ButtonListScreen default;
//            false left-aligns every button label (scaffold).
//   button_style              (string, default "default")  "default" | "checkbox" |
//            "checked_selection" (scaffold; Python Button_cls).
//   checked_buttons           (int array, optional)  indices drawn checked under the
//            checkbox/radio styles (scaffold; Python checked_buttons).
//   initial_selected_index    (int, optional)        overrides the default initial
//            focus of 0 (navigation layer; Python selected_button).
//   input.mode                (string, optional)     "touch" | "hardware" input-mode
//            override (navigation layer).
//   input.keys.key1/key2/key3 (string, optional)     per-aux-key policy "enter" |
//            "noop" | "emit" (navigation layer).
//   allow_screensaver         (bool, default true)   per-screen screensaver policy
//            (normalized by parse_screen_json_ctx, stamped by the scaffold).

#include "screen_scaffold.h"  // parse_screen_json_ctx / create_top_nav_screen_scaffold / bind_screen_navigation / load_screen_and_cleanup_previous
#include "seedsigner.h"       // button_list_screen decl, screen_scaffold_t
#include "screen_helpers.h"   // ensure_top_nav_structure, require_top_nav_title, make_body_text_label, apply_body_tight_line_spacing

#include "lvgl.h"             // lv_obj_get_content_width (intro-text wrap width)

#include <nlohmann/json.hpp>  // json (cfg reads + structural-default writes)

#include <stdexcept>          // std::runtime_error (required-field validation)
#include <string>             // std::string

using json = nlohmann::json;


void button_list_screen(void *ctx_json) {
    // --- Config ---

    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    // Required field: button_list is the screen's whole reason to exist (the
    // scaffold builds one button per entry, and per-entry shape validation lives
    // in its reader). One throw, before the scaffold exists, so no throw path can
    // leak LVGL objects.
    if (!cfg.contains("button_list") || !cfg["button_list"].is_array()) {
        throw std::runtime_error("button_list_screen: button_list is required and must be an array");
    }

    // Structural defaults (write-if-absent, never user-visible text). Python
    // BaseTopNavScreen defaults: show_back_button=True, show_power_button=False —
    // the same values the scaffold falls back to when the flags are absent, so
    // these writes are representation-only. The localized title itself is content
    // and must come from the host.
    ensure_top_nav_structure(cfg, /*default_show_back_button=*/true,
                                  /*default_show_power_button=*/false);
    require_top_nav_title(cfg, "button_list_screen");

    // --- Scaffold ---

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, /*scrollable=*/true);

    // --- Body ---

    // Optional intro text above the buttons. The scaffold gave us a separate
    // upper_body whenever cfg["text"] is a non-empty string; render the text into
    // it. Wraps to the upper_body content width and uses the standard body
    // font/color, with the SAME tight, ink-based line spacing as the
    // large_icon_status_screen body (not the screen-wide loose default) — otherwise
    // the taller block can tip a short prompt into a marginal overflow that wrongly
    // trips scroll-then-buttons, leaving no button highlighted on load.
    //
    // TWO-FILE INVARIANT: the scaffold decides the separate-upper_body structure
    // with its own has_intro_text predicate (contains + is_string + non-empty, in
    // screen_scaffold.cpp) and leaves upper_body EMPTY for this file to fill; the
    // guard below re-derives that same predicate (plus the upper_body != body
    // structure check). The two sites must agree — if they ever diverge, the intro
    // text is silently dropped instead of rendered.
    if (cfg.contains("text") && cfg["text"].is_string()) {
        std::string text = cfg["text"].get<std::string>();
        if (!text.empty() && screen.upper_body && screen.upper_body != screen.body) {
            lv_obj_t *intro_label = make_body_text_label(
                screen.upper_body, text.c_str(),
                lv_obj_get_content_width(screen.upper_body));
            apply_body_tight_line_spacing(intro_label);
        }
    }

    // --- Navigation + load ---

    // Menu-style default index: default to button index 0 selected when nothing is
    // passed in (an explicit initial_selected_index still overrides — Python's
    // selected_button, default 0). A concrete index keeps a button active even when
    // intro text makes the list overflow — a menu/list always has a selection.
    // (cf. status screens, which pass NAV_INDEX_NONE.)
    bind_screen_navigation(cfg, screen, /*default_initial_index=*/0);

    load_screen_and_cleanup_previous(screen.screen);
}
