// seed_sign_message_confirm_message_screen
//
// Python provenance: SeedSignMessageConfirmMessageScreen (seed_screens.py)
//
// "Review Message" step of the message-signing flow: shows the FULL message
// about to be signed as ONE left-aligned wrapped body label (Python TextArea
// with is_text_centered=False) above a bottom-pinned "Next" button; the pressed
// button index returns through the standard navigation callback.
//
// Layout note — documented deviation from Python: Python reflows the message
// into fixed-height PIL pages (reflow_text_into_pages + page_num, titling
// overflow pages "Message (pt x/y)") and re-enters the screen once per page.
// This port uses plain vertical scroll instead of paging: the host passes the
// whole message once; when it overflows the viewport, bind_screen_navigation's
// scroll-then-buttons mode scrolls the content (joystick down / touch swipe)
// until the Next button is revealed and selectable, while a short message just
// shows Next pinned at the bottom. No paging, no per-page hand-off.
//
// Stateless (lifecycle Tier 1): no statics, no timers, no cleanup callback.
//
// cfg:
//   message                   (string, required)    the full message to review
//            (rendered verbatim; an empty string renders no body label).
//   top_nav.title             (string, required)    localized title (Python:
//            _("Review Message"); the paged "Message (pt x/y)" titles have no
//            LVGL counterpart — see the scroll-not-paging note above).
//   top_nav.show_back_button  (bool, default true)   Python ButtonListScreen default.
//   top_nav.show_power_button (bool, default false)  Python ButtonListScreen default.
//   button_list               (array, required, non-empty)  the localized action
//            button(s) (Python: [ButtonOption("Next")]).
//   is_bottom_list            forced true (Python: is_bottom_list = True); a
//            host-supplied value is ignored.
//   is_button_text_centered   (bool, default true)   read by the scaffold; Python
//            forces True, which the scaffold's default already provides.
//   text                      (internal)  overwritten with `message` before the
//            scaffold call (an inert write — see the Config comment); hosts must
//            not supply it.
//   initial_selected_index    (int, optional)        overrides the default initial
//            focus of 0 (navigation layer; Python selected_button).
//   input.mode                (string, optional)     "touch" | "hardware" input-mode
//            override (navigation layer).
//   input.keys.key1/key2/key3 (string, optional)     per-aux-key policy "enter" |
//            "noop" | "emit" (navigation layer).
//   allow_screensaver         (bool, default true)   per-screen screensaver policy
//            (normalized by parse_screen_json_ctx, stamped by the scaffold).

#include "screen_scaffold.h"  // parse_screen_json_ctx / create_top_nav_screen_scaffold / bind_screen_navigation / load_screen_and_cleanup_previous
#include "seedsigner.h"       // seed_sign_message_confirm_message_screen decl, screen_scaffold_t fields
#include "gui_constants.h"    // BODY_FONT, BODY_FONT_COLOR
#include "navigation.h"       // NAV_INDEX_NONE
#include "screen_helpers.h"   // ensure_top_nav_structure, require_top_nav_title, apply_body_tight_line_spacing

#include "lvgl.h"             // lv_label + per-object style setters

#include <nlohmann/json.hpp>  // json (cfg reads + structural-default writes)

#include <stdexcept>          // std::runtime_error (required-field validation)
#include <string>             // std::string

using json = nlohmann::json;


void seed_sign_message_confirm_message_screen(void *ctx_json) {
    // --- Config ---

    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    // Required fields: message is the content this screen exists to put in front
    // of the user before signing; button_list is user-visible CONTENT, which
    // always arrives localized from the host view layer (a string literal baked
    // here would be English-only by construction). One throw per field, before
    // the scaffold exists, so no throw path can leak LVGL objects.
    if (!cfg.contains("message") || !cfg["message"].is_string()) {
        throw std::runtime_error("seed_sign_message_confirm_message_screen: message is required and must be a string");
    }
    if (!cfg.contains("button_list") || !cfg["button_list"].is_array() || cfg["button_list"].empty()) {
        throw std::runtime_error("seed_sign_message_confirm_message_screen: button_list is required and must be a non-empty array");
    }
    std::string message = cfg["message"].get<std::string>();

    // Structural defaults (write-if-absent, never user-visible text). Python
    // ButtonListScreen defaults: show_back_button=True, show_power_button=False
    // (matching the scaffold's own implicit defaults). The localized title is
    // content and must come from the host (Python: _("Review Message")).
    ensure_top_nav_structure(cfg, /*default_show_back_button=*/true,
                                  /*default_show_power_button=*/false);
    require_top_nav_title(cfg, "seed_sign_message_confirm_message_screen");

    cfg["is_bottom_list"] = true;    // forced, not defaulted — Python: is_bottom_list = True

    // Inert config write: the scaffold probes cfg["text"] (its has_intro_text
    // check) only to decide whether a NON-bottom-pinned button list still needs
    // a separate upper_body; the forced is_bottom_list=true above already
    // selects the separate-upper_body flex path on its own, so this assignment
    // changes nothing today. The scaffold never renders cfg["text"] itself —
    // the message is rendered below as this screen's OWN left-aligned label
    // (Python is_text_centered=False; the shared centered body-label form
    // would not match).
    cfg["text"] = message;

    // --- Scaffold ---

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, /*scrollable=*/true);

    // --- Body ---

    // 1. The message body — one left-aligned wrapped label filling upper_body's
    //    content width (derive-internally: the width comes from the container,
    //    not the display resolution). Hand-built rather than
    //    make_body_text_label because that helper centers and Python renders
    //    this TextArea left-aligned (is_text_centered=False). Guarded: an empty
    //    message renders no label, and the upper_body checks keep the label out
    //    of a body that the scaffold did not split.
    if (!message.empty() && screen.upper_body && screen.upper_body != screen.body) {
        lv_obj_t *message_label = lv_label_create(screen.upper_body);
        lv_label_set_text(message_label, message.c_str());
        lv_label_set_long_mode(message_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(message_label, lv_obj_get_content_width(screen.upper_body));
        lv_obj_set_style_text_align(message_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
        lv_obj_set_style_text_color(message_label, lv_color_hex(BODY_FONT_COLOR), LV_PART_MAIN);
        lv_obj_set_style_text_font(message_label, &BODY_FONT, LV_PART_MAIN);
        // Match the PIL reference's tight, ink-based line advance (same as the
        // status / intro body text) instead of LVGL's looser default.
        apply_body_tight_line_spacing(message_label);
    }

    // --- Navigation + load ---

    // NAV_INDEX_NONE (like the status screens): when the message FITS, Next is
    // active; when it OVERFLOWS, start UNFOCUSED at the TOP so the reader sees
    // the message from its beginning and scrolls DOWN through it to reveal +
    // focus the Next button — rather than loading pre-scrolled to the bottom
    // with Next focused. (The host may override via cfg initial_selected_index.)
    bind_screen_navigation(cfg, screen, NAV_INDEX_NONE);

    load_screen_and_cleanup_previous(screen.screen);
}
