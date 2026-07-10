// psbt_op_return_screen
//
// Python provenance: PSBTOpReturnScreen (psbt_screens.py)
//
// PSBT signing-flow detail screen for a transaction's OP_RETURN output: a
// bottom-pinned button-list screen with a back button, showing the payload in
// one of two modes, mirroring Python's try/except decode split:
//
//   Mode A — human-readable text: the payload decoded as UTF-8 text, rendered
//     CENTERED (both axes) at the top-nav title font size in the free band
//     between the top nav and the bottom button (Python: TextArea with an
//     explicit height spanning that band).
//
//   Mode B — raw hex fallback: the payload was binary (not valid UTF-8). A
//     small gray caption over the hex string, hard-wrapped to the display
//     width in a fixed-width (monospace) font (Python: a LABEL_FONT_COLOR
//     TextArea, then a FIXED_WIDTH_FONT TextArea with hand-computed breaks).
//
// PLATFORM CONTRACT: the host (Python view layer) owns the payload bytes and
// the UTF-8 heuristic (bytes.decode(errors="strict")), so it decides the mode
// and passes the already-decoded form — never raw bytes:
//   cfg.text present -> Mode A (human-readable text).
//   cfg.hex  present -> Mode B (lowercase hex string, no separators).
// The screen owns the width-dependent wrapping: chars-per-line comes from the
// device's own monospace metrics, which only the render layer knows.
//
// Layout notes: the body scrolls (scrollable=true) — unlike the sibling PSBT
// detail screens (psbt_address_details / psbt_change_details / psbt_math) —
// because a long hex dump legitimately overflows the viewport; NAV_INDEX_NONE
// makes it read-first (scroll through the payload before the button activates).
// Mode A vertically centers via the spacer-clamped margin-shift technique from
// large_icon_status_screen; Mode B is top-anchored under the top nav. The text
// column width is derived from the scaffold body's content width. Stateless
// (lifecycle Tier 1): no timers, host-push state, or cleanup callbacks.
//
// cfg:
//   top_nav.title             (string, required)     localized screen title
//            (Python: _("OP_RETURN") passed by PSBTOpReturnView).
//   top_nav.show_back_button  (bool, default true)   Python ButtonListScreen default;
//            read by the scaffold's top-nav builder.
//   top_nav.show_power_button (bool, default false)  Python ButtonListScreen default;
//            read by the scaffold's top-nav builder.
//   text                      (string)               Mode A payload: the OP_RETURN
//            bytes decoded as UTF-8, host-supplied.
//   hex                       (string)               Mode B payload: lowercase hex,
//            no separators. ONE OF text/hex is required (both absent-or-empty
//            throws); a non-empty hex takes precedence and selects Mode B.
//   hex_label                 (string, default "raw hex data")  Mode B caption
//            (Python: _("raw hex data")). English default RETAINED — a documented
//            content-policy deviation: the raw_hex scenario does not supply the
//            key, so converting it to require-and-throw would break the pixel
//            gate; revisit once the scenario/host always sends the localized
//            caption.
//   button_list               (array, required, non-empty)  the localized action
//            buttons (Python: "Next"); built by the scaffold. is_bottom_list is
//            forced true (Python: is_bottom_list = True).
//   initial_selected_index    (int, optional)        overrides the default initial
//            focus of 0 (navigation layer; Python selected_button).
//   input.mode                (string, optional)     "touch" | "hardware" input-mode
//            override (navigation layer).
//   input.keys.key1/key2/key3 (string, optional)     per-aux-key policy "enter" |
//            "noop" | "emit" (navigation layer).
//   allow_screensaver         (bool, default true)   per-screen screensaver policy
//            (normalized by parse_screen_json_ctx, stamped by the scaffold).

#include "screen_scaffold.h"  // parse_screen_json_ctx / create_top_nav_screen_scaffold / bind_screen_navigation / load_screen_and_cleanup_previous
#include "seedsigner.h"       // psbt_op_return_screen decl, screen_scaffold_t
#include "components.h"       // monospace_char_width
#include "gui_constants.h"    // COMPONENT_PADDING, TOP_NAV_TITLE_FONT, CANDIDATE_FONT, BODY_FONT, LABEL_FONT_COLOR, BODY_FONT_COLOR
#include "navigation.h"       // NAV_INDEX_NONE
#include "screen_helpers.h"   // ensure_top_nav_structure, require_top_nav_title

#include "lvgl.h"             // lv_label creation, per-object style setters, layout/coords queries

#include <nlohmann/json.hpp>  // json (cfg reads + structural-default writes)

#include <stdexcept>          // std::runtime_error (required-field validation)
#include <string>             // std::string

using json = nlohmann::json;


void psbt_op_return_screen(void *ctx_json) {
    // --- Config ---

    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    // Required fields: the OP_RETURN payload arrives pre-decoded from the host in
    // exactly one of its two forms (see the PLATFORM CONTRACT above) — neither
    // supplied (both absent or empty) is a host wiring error, surfaced as a throw
    // rather than a silently blank screen. button_list is user-visible CONTENT,
    // which always arrives localized from the host view layer (a string literal
    // baked here would be English-only by construction). All throws fire before
    // the scaffold exists, so no throw path can leak LVGL objects.
    std::string hex  = cfg.value("hex",  std::string());
    std::string text = cfg.value("text", std::string());
    if (hex.empty() && text.empty()) {
        throw std::runtime_error("psbt_op_return_screen: one of text or hex is required");
    }
    if (!cfg.contains("button_list") || !cfg["button_list"].is_array() || cfg["button_list"].empty()) {
        throw std::runtime_error("psbt_op_return_screen: button_list is required and must be a non-empty array");
    }

    // Mode selection: a non-empty hex wins. The host never sends both, but the
    // contract's tiebreak is explicit.
    const bool hex_mode = !hex.empty();

    // Structural defaults (write-if-absent, never user-visible text). Python
    // ButtonListScreen defaults: show_back_button=True, show_power_button=False.
    // The localized title itself is content and must come from the host.
    ensure_top_nav_structure(cfg, /*default_show_back_button=*/true,
                                  /*default_show_power_button=*/false);
    require_top_nav_title(cfg, "psbt_op_return_screen");

    cfg["is_bottom_list"] = true;    // forced, not defaulted — Python: is_bottom_list = True

    // --- Scaffold ---

    // is_bottom_list forces the scaffold's separate flex-column upper_body: children
    // stack vertically, centered horizontally, with a flex-grow spacer below that
    // pins the button to the viewport bottom. scrollable=true — unlike the sibling
    // PSBT detail screens — because a long hex dump legitimately overflows the
    // viewport and must page-scroll.
    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, /*scrollable=*/true);

    // --- Body ---

    // The readable text column matches Python: full canvas width minus the body's
    // EDGE_PADDING gutters, already baked into the body's content width
    // (derive-internally rule — no display-width recomputation).
    const int32_t text_width = lv_obj_get_content_width(screen.body);

    if (hex_mode) {
        // --- Mode B: raw hex fallback ---

        // 1. Small gray caption above the dump (Python: TextArea in
        //    LABEL_FONT_COLOR at the label font size). The caption string is the
        //    kept English default documented in the banner's cfg table.
        lv_obj_t *caption = lv_label_create(screen.upper_body);
        lv_label_set_text(caption, cfg.value("hex_label", std::string("raw hex data")).c_str());
        lv_obj_set_style_text_color(caption, lv_color_hex(LABEL_FONT_COLOR), LV_PART_MAIN);
        lv_obj_set_style_text_font(caption, &BODY_FONT, LV_PART_MAIN);

        // 2. The hex payload, hard-wrapped to the column. Fixed-width metrics:
        //    derive chars-per-line from THIS font's cell advance so the hex fills
        //    the width without overflowing (Python computes the same from the
        //    FIXED_WIDTH font's 'X' box). The only baked monospace sizes are
        //    22/24 px; CANDIDATE_FONT (22 px) packs the most hex per line.
        const lv_font_t *monospace_font = &CANDIDATE_FONT;
        int32_t char_advance = monospace_char_width(monospace_font);
        int chars_per_line = (int)(text_width / char_advance);
        if (chars_per_line < 1) chars_per_line = 1;

        // Insert the line breaks ourselves: a continuous hex string has no space
        // break opportunities, so LVGL's word-wrap can never split it.
        std::string wrapped_hex;
        for (size_t i = 0; i < hex.size(); i += (size_t)chars_per_line) {
            wrapped_hex += hex.substr(i, (size_t)chars_per_line);
            if (i + (size_t)chars_per_line < hex.size()) wrapped_hex += "\n";
        }

        lv_obj_t *hex_payload_label = lv_label_create(screen.upper_body);
        lv_label_set_text(hex_payload_label, wrapped_hex.c_str());
        lv_obj_set_style_text_align(hex_payload_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_text_color(hex_payload_label, lv_color_hex(BODY_FONT_COLOR), LV_PART_MAIN);
        lv_obj_set_style_text_font(hex_payload_label, monospace_font, LV_PART_MAIN);
        // Python gaps the hex one COMPONENT_PADDING below the caption.
        lv_obj_set_style_margin_top(hex_payload_label, COMPONENT_PADDING, LV_PART_MAIN);

    } else {
        // --- Mode A: human-readable text ---

        // 1. The payload as centered wrapped text at the top-nav title font size
        //    (Python: get_top_nav_title_font_size()).
        lv_obj_t *message_label = lv_label_create(screen.upper_body);
        lv_label_set_text(message_label, text.c_str());
        lv_label_set_long_mode(message_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(message_label, text_width);
        lv_obj_set_style_text_align(message_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_text_color(message_label, lv_color_hex(BODY_FONT_COLOR), LV_PART_MAIN);
        lv_obj_set_style_text_font(message_label, &TOP_NAV_TITLE_FONT, LV_PART_MAIN);

        // Vertical-centering pass — the file's one measure-and-place pass, Mode A
        // only. Center the message in the gap above the bottom button (Python's
        // TextArea has an explicit height spanning that gap and centers within it).
        // The shift comes out of the flex-grow spacer, so the button stays pinned —
        // clamp to the spacer height so we never push the button. Same technique
        // as large_icon_status_screen's vertical-centering pass, minus its
        // shaped-run drawn-height correction: the payload is arbitrary transaction
        // data, never in the pre-shaped locale catalog, so no glyph run ever
        // attaches to this label. The band edges are measured directly from the
        // label's and button_list[0]'s own coords (not the shared
        // bottom_button_top_y helper) — the documented variant site
        // (docs/screen-conformance-spec.md §10 #8), kept verbatim for the pixel
        // gate.
        if (screen.button_list_count > 0 && screen.button_list_spacer) {
            lv_obj_update_layout(screen.body);
            lv_area_t text_area, button_area;
            lv_obj_get_coords(message_label, &text_area);
            lv_obj_get_coords(screen.button_list[0], &button_area);
            int32_t below_gap = button_area.y1 - text_area.y2;
            int32_t spacer_height = lv_obj_get_height(screen.button_list_spacer);
            int32_t shift = below_gap / 2;
            if (shift > spacer_height) shift = spacer_height;
            if (shift > 0) lv_obj_set_style_margin_top(message_label, shift, LV_PART_MAIN);
        }
    }

    // --- Navigation + load ---

    // NAV_INDEX_NONE (read-first): the action button is active when the payload
    // fits; a long hex body that overflows must be scrolled through before the
    // button is reachable, via bind_screen_navigation's scroll-then-buttons
    // auto-detect. (The sibling PSBT detail screens use the menu-style default 0.)
    bind_screen_navigation(cfg, screen, NAV_INDEX_NONE);

    load_screen_and_cleanup_previous(screen.screen);
}
