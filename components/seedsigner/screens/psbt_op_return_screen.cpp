// psbt_op_return_screen
//
// LVGL port of Python's PSBTOpReturnScreen (psbt_screens.py:714). A bottom-list
// ButtonListScreen that shows a transaction's OP_RETURN payload in one of two
// modes, exactly as Python does:
//
//   Mode A — human-readable: the payload decoded as text. Rendered CENTERED (both
//     axes) at the top-nav title font size, in the space above the bottom button.
//
//   Mode B — raw hex fallback: the payload was binary (not valid UTF-8). A small
//     gray "raw hex data" caption over the hex string, hard-wrapped to fit the
//     width in a fixed-width (monospace) font.
//
// PLATFORM CONTRACT: the host (Python) owns the bytes and the UTF-8 heuristic, so
// it decides the mode and passes the already-decoded form — NOT raw bytes:
//   cfg.text (str) present  -> Mode A (human-readable text).
//   cfg.hex  (str) present  -> Mode B (lowercase hex string, no separators).
// The screen owns the width-dependent wrapping (chars-per-line comes from the
// device's own monospace metrics, which only the render layer knows).
//
// cfg:
//   top_nav.title (str) — default "OP_RETURN".
//   button_list (array) — default ["Done"].
//   text (str)          — Mode A payload.
//   hex  (str)          — Mode B payload (takes precedence if both are present).
//   hex_label (str)     — Mode B caption, default "raw hex data" (host localizes).

#include "screen_scaffold.h"   // parse/scaffold/nav/load helpers
#include "seedsigner.h"        // screen_scaffold_t
#include "gui_constants.h"     // TOP_NAV_TITLE_FONT, CANDIDATE_FONT, colors, padding
#include "navigation.h"        // NAV_BODY_VERTICAL, NAV_INDEX_NONE

#include "lvgl.h"

#include <nlohmann/json.hpp>
#include <string>

using json = nlohmann::json;

void psbt_op_return_screen(void *ctx_json) {
    json cfg;
    parse_screen_json_ctx((const char *)ctx_json, cfg);

    if (!cfg.contains("top_nav") || !cfg["top_nav"].is_object()) cfg["top_nav"] = json::object();
    if (!cfg["top_nav"].contains("title")) cfg["top_nav"]["title"] = "OP_RETURN";
    cfg["is_bottom_list"] = true;                                  // Python is_bottom_list = True
    if (!cfg.contains("button_list")) cfg["button_list"] = json::array({ "Done" });

    std::string hex  = cfg.value("hex",  std::string());
    std::string text = cfg.value("text", std::string());
    const bool hex_mode = !hex.empty();

    // is_bottom_list forces the scaffold's separate flex-column upper_body (Mode 3):
    // children stack vertically, centered horizontally, with a flex-grow spacer below
    // that pins the button to the viewport bottom.
    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, true);

    // The readable text column matches Python: full canvas width minus the body's
    // EDGE_PADDING gutters (already baked into the body's content width).
    const int32_t text_w = lv_obj_get_content_width(screen.body);

    if (hex_mode) {
        // ── Mode B: raw hex fallback ──
        // Small gray caption (Python: LABEL_FONT_COLOR / LABEL_FONT_SIZE).
        lv_obj_t *caption = lv_label_create(screen.upper_body);
        lv_label_set_text(caption, cfg.value("hex_label", std::string("raw hex data")).c_str());
        lv_obj_set_style_text_color(caption, lv_color_hex(LABEL_FONT_COLOR), LV_PART_MAIN);
        lv_obj_set_style_text_font(caption, &BODY_FONT, LV_PART_MAIN);

        // Fixed-width metrics: derive chars-per-line from THIS font's cell advance so
        // the hex fills the width without overflowing (Python computes the same from the
        // FIXED_WIDTH font's 'X' box). The only baked monospace sizes are 22/24 px;
        // CANDIDATE_FONT (22 px) packs the most hex per line.
        const lv_font_t *mono = &CANDIDATE_FONT;
        lv_point_t sz10;
        lv_text_get_size(&sz10, "0000000000", mono, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        int32_t adv = sz10.x / 10;
        if (adv < 1) adv = 1;
        int chars_per_line = (int)(text_w / adv);
        if (chars_per_line < 1) chars_per_line = 1;

        // Hard-wrap the hex into fixed-width lines (a continuous hex string has no space
        // break opportunities, so LVGL's word-wrap can't split it — we insert the breaks).
        std::string wrapped;
        for (size_t i = 0; i < hex.size(); i += (size_t)chars_per_line) {
            wrapped += hex.substr(i, (size_t)chars_per_line);
            if (i + (size_t)chars_per_line < hex.size()) wrapped += "\n";
        }

        lv_obj_t *hex_label = lv_label_create(screen.upper_body);
        lv_label_set_text(hex_label, wrapped.c_str());
        lv_obj_set_style_text_align(hex_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_text_color(hex_label, lv_color_hex(BODY_FONT_COLOR), LV_PART_MAIN);
        lv_obj_set_style_text_font(hex_label, mono, LV_PART_MAIN);
        // Python gaps the hex one COMPONENT_PADDING below the caption.
        lv_obj_set_style_margin_top(hex_label, COMPONENT_PADDING, LV_PART_MAIN);

    } else {
        // ── Mode A: human-readable text ──
        // Centered, at the top-nav title font size (Python get_top_nav_title_font_size()).
        lv_obj_t *msg = lv_label_create(screen.upper_body);
        lv_label_set_text(msg, text.c_str());
        lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(msg, text_w);
        lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_text_color(msg, lv_color_hex(BODY_FONT_COLOR), LV_PART_MAIN);
        lv_obj_set_style_text_font(msg, &TOP_NAV_TITLE_FONT, LV_PART_MAIN);

        // Vertically center the message in the gap above the bottom button (Python's
        // TextArea has an explicit height spanning that gap and centers within it). The
        // shift comes out of the flex-grow spacer, so the button stays pinned — clamp to
        // the spacer so we never push the button. (Same technique as large_icon_status.)
        if (screen.button_list_count > 0 && screen.button_list_spacer) {
            lv_obj_update_layout(screen.body);
            lv_area_t text_area, button_area;
            lv_obj_get_coords(msg, &text_area);
            lv_obj_get_coords(screen.button_list[0], &button_area);
            int32_t below_gap = button_area.y1 - text_area.y2;
            int32_t spacer_h = lv_obj_get_height(screen.button_list_spacer);
            int32_t shift = below_gap / 2;
            if (shift > spacer_h) shift = spacer_h;
            if (shift > 0) lv_obj_set_style_margin_top(msg, shift, LV_PART_MAIN);
        }
    }

    // NAV_INDEX_NONE: the Done button is active when the payload fits; a long hex body
    // that overflows must be scrolled through before Done is reachable (read-first),
    // via bind_screen_navigation's scroll-then-buttons auto-detect.
    bind_screen_navigation(cfg, screen,
                           screen.button_list_count > 0 ? screen.button_list : NULL,
                           screen.button_list_count, NAV_BODY_VERTICAL, NAV_INDEX_NONE);

    load_screen_and_cleanup_previous(screen.screen);
}
