// tools_calc_final_word_screen
//
// Python provenance: ToolsCalcFinalWordScreen (tools_screens.py)
//
// The "bit math" breakdown step of the Calc-final-word tool: shows how the final
// mnemonic word is assembled from the user's own entropy plus the computed
// checksum. The host view renders it in two variants:
//   - pick_word  : the user picked a final word — its 11 index bits are shown; the
//                  least-significant num_checksum bits are DISCARDED (dimmed) and
//                  replaced by the computed checksum.
//   - coin_flips : the user entered N coin flips (7 for a 12-word seed); the
//                  discarded slot renders as dimmed underscores instead of bits.
//
// The breakdown is three labeled blocks, each a centered BODY_FONT caption over a
// fixed-width (Inconsolata-SemiBold) bit row:
//   1. your_input_text  +  the entered entropy bits: keeper bits (the first
//      11 - num_checksum) in white; the trailing discard slot dimmed — real bits
//      (pick_word) or underscores (coin_flips).
//   2. checksum_label   +  a dimmed leading underscore rule, then the ORANGE
//      checksum bits.
//   3. final_word_text  +  the merged result: white keeper bits, then the ORANGE
//      checksum bits.
// The three bit rows share ONE centered 11-column monospace grid: the keeper
// segment is left-anchored at bit_display_x; the checksum segment is anchored at
// checksum_x = bit_display_x + (11 - num_checksum) * char_width, so the two
// segments abut into one contiguous 11-char string (Python's exact geometry).
// The pressed button's index returns to the host through the chrome's standard
// navigation callback.
//
// Construction (compose-and-overlay tier, docs/screen-conformance-spec.md §8):
// this screen needs PIL-exact
// ABSOLUTE row positions that the scaffold's flex body cannot express, so it
// DELEGATES the chrome (top_nav + bottom-pinned "Next" button + joystick/touch
// nav + screen load) to the public button_list_screen with an empty intro, then
// OVERLAYS the breakdown labels on the freshly-loaded screen. This is a
// documented construction-philosophy divergence from the sibling
// tools_calc_final_word_done_screen, which builds the flow's companion screen
// with the default scaffold + flex upper_body; the compose-and-overlay technique
// (shared only with settings_qr_confirmation_screen) is a contained known
// anti-pattern awaiting the spec's rollout decision — do not add a third consumer.
//
// KNOWN FLAGGED GAP (behavioral, kept as-is): the overlay labels are created
// AFTER the chrome's load_screen_and_cleanup_previous has run, so the load-time
// RTL and glyph-run passes never see them — shaped locales (hi/th) render the
// captions as raw codepoints and RTL (ur) keeps LTR base direction here. Tracked
// in the bug ledger pending the §8 rollout decision.
//
// Lifecycle: stateless (Tier 1) — no heap ctx, no cleanup callback; the overlay
// labels are children of the loaded screen and die with it when the NEXT
// screen's load_screen_and_cleanup_previous deletes it.
//
// FONT NOTE (parity caveat): Python renders the bits in Inconsolata-SemiBold at
// button_font_size("default") + 2 = 20 px. This repo bakes Inconsolata-SemiBold
// only at 22 px (CANDIDATE_FONT) and 24 px (KEYBOARD_FONT) — no 20 px variant —
// so the closest available (CANDIDATE_FONT, 22 px) is used. Its digits are ~2 px
// taller (14 vs 12 px cap) and ~1 px wider (11 vs 10 px advance) than the
// reference; the row RHYTHM and the orange checksum column are anchored to the
// Python (20 px) metrics so the label rows and highlight stay aligned, but the
// bit glyphs themselves are a hair larger. A pixel-exact match would need a
// 20 px Inconsolata-SemiBold bake.
//
// cfg:
//   top_nav.title             (string, required)     localized screen title (read by
//            the delegated button_list_screen scaffold; validated here first so a
//            malformed cfg fails with this screen's name).
//   top_nav.show_back_button  (bool, default true)   Python ButtonListScreen default.
//   top_nav.show_power_button (bool, default false)  Python ButtonListScreen default.
//   button_list               (array, required, non-empty)  the localized action
//            button(s) (Python: a single "Next"); rendered by the delegated chrome.
//   your_input_text           (string, required)     host-formatted + localized row-1
//            caption (Python: _('Your input: "{}"').format(word_or_flips)).
//   checksum_label            (string, required)     localized row-2 caption
//            (Python: _("Checksum")).
//   final_word_text           (string, required)     host-formatted + localized row-3
//            caption (Python: _('Final Word: "{}"').format(final_word)).
//   selected_final_bits       (string, required, non-empty)  the entered entropy bits
//            (11 for a picked word, N for coin flips).
//   checksum_bits             (string, required, non-empty)  the computed checksum
//            bits (4 for a 12-word seed, 8 for 24).
//   has_selected_word         (bool, default true)   true -> the discard slot shows
//            the word's real bits, dimmed; false (coin flips) -> dimmed underscores.
//   text / is_bottom_list are FORCED ("" / true) on the delegated chrome — Python:
//            no intro text; is_bottom_list = True. Every other ButtonListScreen key
//            (initial_selected_index, input.mode / input.keys.*, allow_screensaver,
//            top_nav.icon, ...) passes through to button_list_screen unchanged — see
//            that file's banner for the full chrome contract (scaffold/navigation layer).

#include "screen_scaffold.h"  // parse_screen_json_ctx
#include "seedsigner.h"       // tools_calc_final_word_screen decl, button_list_screen (delegated chrome), text_top_leading
#include "components.h"       // monospace_char_width (11-column grid advance)
#include "gui_constants.h"    // TOP_NAV_HEIGHT, COMPONENT_PADDING, EDGE_PADDING, BUTTON_HEIGHT, BODY_FONT, CANDIDATE_FONT, BODY_FONT_COLOR, LABEL_FONT_COLOR, ACCENT_COLOR, active_profile
#include "screen_helpers.h"   // ensure_top_nav_structure, require_top_nav_title, measure_text_ink_extents

#include "lvgl.h"             // labels, per-object style setters, coords/layout queries

#include <nlohmann/json.hpp>  // json (cfg reads + chrome build)

#include <cstdint>            // INT32_MAX / INT32_MIN (recenter group + DFS bounds)
#include <stdexcept>          // std::runtime_error (required-field validation)
#include <string>             // std::string
#include <vector>             // std::vector (overlay label group + DFS stack)

using json = nlohmann::json;


// Layout-reference constants. The Python bit font size is
// button_font_size("default") + 2 = 20 px at the 240-height reference; it does NOT
// vary by locale (only by display profile). Cap height 12, advance 10 at that size.
// Kept as the layout reference below even though the actual glyphs render through
// the (larger) baked CANDIDATE_FONT — see the FONT NOTE in the banner.
static const int TOOLS_CALC_FINAL_WORD_PY_BIT_CAP_HEIGHT_240 = 12;   // Inconsolata-SemiBold 20 px digit ink height

// Python caption row height at the 240-height reference: TextArea(height_ignores_below_baseline)
// for BODY_FONT (OpenSans 17) measures 13 px top-to-baseline. Used only for the inter-row
// RHYTHM (every caption anchors this far below the preceding bit row / vice-versa). The
// caption's OWN ink top is placed independently via text_top_leading, so this constant just
// sets spacing. NB: LVGL's measured OpenSans-17 ink ascent is 14 px, 1 px looser than PIL —
// using the PIL value here lands every row exactly on the reference.
static const int TOOLS_CALC_FINAL_WORD_PY_BODY_CAP_HEIGHT_240 = 13;

static const int TOOLS_CALC_FINAL_WORD_BIT_COLUMNS = 11;             // a BIP-39 word index is 11 bits


namespace {

// Create a centered, single-line BODY_FONT caption whose VISIBLE ink top lands at the
// canvas-relative `ink_top_y` (Python TextArea.screen_y with height_ignores_below_baseline).
// LVGL anchors a label box by the font ascent (leading above the caps); subtract that
// leading so the visible text lands where PIL draws it (splash/status/settings-qr pattern).
lv_obj_t *tools_calc_final_word_place_centered_caption(lv_obj_t *screen, const char *text,
                                                       int32_t ink_top_y) {
    lv_obj_t *label = lv_label_create(screen);
    lv_label_set_text(label, text);

    // Python centers on the full canvas; a (canvas - 2*EDGE_PADDING)-wide box centered
    // with centered text is equivalent. CLIP (not WRAP): these captions are single-line.
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(label, lv_obj_get_width(screen) - 2 * EDGE_PADDING);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(BODY_FONT_COLOR), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &BODY_FONT, LV_PART_MAIN);

    const int32_t top_leading = text_top_leading(&BODY_FONT, text);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, ink_top_y - top_leading);
    return label;
}

// Left-anchor a run of monospace bit chars at `x` so its VISIBLE ink top lands at
// `ink_top_y` (matches the digit-row rhythm the captions are anchored to).
lv_obj_t *tools_calc_final_word_place_bit_digits(lv_obj_t *screen, const char *text,
                                                 const lv_font_t *font, uint32_t color,
                                                 int32_t x, int32_t ink_top_y) {
    lv_obj_t *label = lv_label_create(screen);
    lv_obj_set_style_pad_all(label, 0, LV_PART_MAIN);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
    lv_label_set_text(label, text);

    const int32_t top_leading = text_top_leading(font, text);
    lv_obj_set_pos(label, x, ink_top_y - top_leading);
    return label;
}

// Left-anchor a run of monospace underscore placeholders at `x` so its ink CENTER lands
// at `ink_center_y`. Python draws the discarded/placeholder slots as underscores raised
// to the digit-row's vertical center (so they read like a mid-line rule, not a baseline
// tail); the reference shows the underscore ink at the digit band's vertical center.
lv_obj_t *tools_calc_final_word_place_bit_underscores(lv_obj_t *screen, const char *text,
                                                      const lv_font_t *font, uint32_t color,
                                                      int32_t x, int32_t ink_center_y) {
    lv_obj_t *label = lv_label_create(screen);
    lv_obj_set_style_pad_all(label, 0, LV_PART_MAIN);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
    lv_label_set_text(label, text);

    int32_t ink_ascent = 0, ink_descent = 0;
    measure_text_ink_extents(font, text, &ink_ascent, &ink_descent);
    // Underscore ink spans [baseline - ink_ascent, baseline + ink_descent]; its center
    // is baseline + (ink_descent - ink_ascent)/2. Solve for the baseline that lands the
    // center on ink_center_y, then back out the label box top (baseline - font ascent).
    const int32_t font_ascent = (int32_t)lv_font_get_line_height(font) - (int32_t)font->base_line;
    const int32_t baseline = ink_center_y + (ink_ascent - ink_descent) / 2;
    lv_obj_set_pos(label, x, baseline - font_ascent);
    return label;
}

}  // namespace


void tools_calc_final_word_screen(void *ctx_json) {
    // --- Config ---

    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    // Required fields: the three captions are user-visible CONTENT, which always
    // arrives host-formatted + localized from the view layer (a string literal
    // baked here would be English-only by construction); the two bit strings are
    // the data this screen exists to visualize; button_list feeds the delegated
    // chrome (validated here first so a malformed cfg fails with this screen's
    // name). One throw per field, before any LVGL work, so no throw path can leak
    // LVGL objects.
    if (!cfg.contains("your_input_text") || !cfg["your_input_text"].is_string()) {
        throw std::runtime_error("tools_calc_final_word_screen: your_input_text is required and must be a string");
    }
    if (!cfg.contains("checksum_label") || !cfg["checksum_label"].is_string()) {
        throw std::runtime_error("tools_calc_final_word_screen: checksum_label is required and must be a string");
    }
    if (!cfg.contains("final_word_text") || !cfg["final_word_text"].is_string()) {
        throw std::runtime_error("tools_calc_final_word_screen: final_word_text is required and must be a string");
    }
    if (!cfg.contains("selected_final_bits") || !cfg["selected_final_bits"].is_string() ||
        cfg["selected_final_bits"].get<std::string>().empty()) {
        throw std::runtime_error("tools_calc_final_word_screen: selected_final_bits is required and must be a non-empty string");
    }
    if (!cfg.contains("checksum_bits") || !cfg["checksum_bits"].is_string() ||
        cfg["checksum_bits"].get<std::string>().empty()) {
        throw std::runtime_error("tools_calc_final_word_screen: checksum_bits is required and must be a non-empty string");
    }
    if (!cfg.contains("button_list") || !cfg["button_list"].is_array() || cfg["button_list"].empty()) {
        throw std::runtime_error("tools_calc_final_word_screen: button_list is required and must be a non-empty array");
    }

    const std::string your_input_text     = cfg["your_input_text"].get<std::string>();
    const std::string checksum_label      = cfg["checksum_label"].get<std::string>();
    const std::string final_word_text     = cfg["final_word_text"].get<std::string>();
    const std::string selected_final_bits = cfg["selected_final_bits"].get<std::string>();
    const std::string checksum_bits       = cfg["checksum_bits"].get<std::string>();

    // Structural flag (write-if-absent read): selects the discard-slot rendering,
    // never rendered as text itself.
    const bool has_selected_word = cfg.value("has_selected_word", true);

    // Structural defaults (write-if-absent, never user-visible text). Python
    // ButtonListScreen defaults: show_back_button=True, show_power_button=False —
    // the same values the delegated scaffold falls back to when the flags are
    // absent, so these writes are representation-only. The localized title itself
    // is content and must come from the host; button_list_screen re-runs this same
    // ensure+require pair on the chrome copy below as a no-op.
    ensure_top_nav_structure(cfg, /*default_show_back_button=*/true,
                                  /*default_show_power_button=*/false);
    require_top_nav_title(cfg, "tools_calc_final_word_screen");

    const int num_checksum = (int)checksum_bits.size();
    const int num_keeper   = TOOLS_CALC_FINAL_WORD_BIT_COLUMNS - num_checksum;   // entropy bits the user contributes

    // Python's bit decomposition (ToolsCalcFinalWordScreen.__post_init__):
    //   keeper  = selected_final_bits[:11 - num_checksum]                 (white, kept)
    //   discard = selected_final_bits[-num_checksum:] (a word)  OR  "_"*num_checksum (coin flips)
    //   checksum_spacer = "_" * (11 - num_checksum)                       (dimmed leading rule)
    // The "final word" row reuses keeper + the orange checksum bits.
    const std::string keeper_bits =
        (int)selected_final_bits.size() >= num_keeper ? selected_final_bits.substr(0, num_keeper)
                                                      : selected_final_bits;
    std::string discard_bits;
    bool discard_is_placeholder;
    if (has_selected_word) {
        discard_bits = (int)selected_final_bits.size() >= num_checksum
                           ? selected_final_bits.substr(selected_final_bits.size() - num_checksum)
                           : selected_final_bits;
        discard_is_placeholder = false;
    } else {
        discard_bits = std::string(num_checksum, '_');
        discard_is_placeholder = true;
    }
    const std::string checksum_spacer(num_keeper, '_');

    // --- Chrome (delegated scaffold + navigation + load) ---

    // Build the ButtonListScreen chrome (top_nav + bottom-pinned "Next" button +
    // joystick/touch nav wiring + the screen load) via the public
    // button_list_screen entry point, exactly like the Python reference's
    // ButtonListScreen base. Preserve every host-provided pass-through key
    // (top_nav / input / initial_selected_index / allow_screensaver); drop this
    // screen's own keys so button_list_screen sees a clean cfg.
    json chrome = cfg;
    chrome.erase("your_input_text");
    chrome.erase("final_word_text");
    chrome.erase("checksum_label");
    chrome.erase("selected_final_bits");
    chrome.erase("checksum_bits");
    chrome.erase("has_selected_word");
    chrome["text"] = "";               // forced: no flowed intro text — the breakdown is overlaid below
    chrome["is_bottom_list"] = true;   // forced, not defaulted — Python: is_bottom_list = True

    const std::string chrome_str = chrome.dump();
    button_list_screen((void *)chrome_str.c_str());

    // --- Overlay body ---

    // The chrome is already LOADED (button_list_screen ends in
    // load_screen_and_cleanup_previous), so the breakdown labels are parented onto
    // the live screen — see the banner's KNOWN FLAGGED GAP: labels added after the
    // load miss the RTL/glyph-run post-passes.
    lv_obj_t *screen = lv_scr_act();
    const lv_font_t *bit_font = &CANDIDATE_FONT;   // closest baked Inconsolata-SemiBold (22 px) — see FONT NOTE

    const int     px_multiplier     = active_profile().px_multiplier;
    const int32_t component_padding = COMPONENT_PADDING;       // already px-scaled

    // Horizontal geometry: center an 11-column monospace grid on the full canvas, then
    // anchor the checksum segment (11 - num_checksum) columns in — Python's exact math,
    // using the actual font's advance so the grid stays monospace.
    const int32_t screen_width = lv_obj_get_width(screen);
    int32_t char_width = monospace_char_width(bit_font, TOOLS_CALC_FINAL_WORD_BIT_COLUMNS);
    const int32_t bit_display_width = TOOLS_CALC_FINAL_WORD_BIT_COLUMNS * char_width;
    const int32_t bit_display_x     = (screen_width - bit_display_width) / 2;
    const int32_t checksum_x        = bit_display_x + num_keeper * char_width;

    // Vertical geometry: reproduce Python's stacked layout. Every caption row advances
    // by (body cap height + COMPONENT_PADDING); every caption sits 2*COMPONENT_PADDING
    // below the preceding bit row (whose height is the Python 20-px digit cap). The bit
    // rows use the Python cap height for RHYTHM even though the baked glyphs are a hair
    // taller (see FONT NOTE), so the captions/highlight keep the reference spacing.
    const int32_t body_cap_height = TOOLS_CALC_FINAL_WORD_PY_BODY_CAP_HEIGHT_240 * px_multiplier / 100;  // 13 px at 240 (PIL caption height)
    const int32_t bit_cap_height  = TOOLS_CALC_FINAL_WORD_PY_BIT_CAP_HEIGHT_240 * px_multiplier / 100;   // 12 px at 240
    const int32_t y_spacer        = component_padding;                                                   // body font == default size

    const int32_t y_your_input     = TOP_NAV_HEIGHT + component_padding - 2;   // Python: top_nav.height + COMPONENT_PADDING - 2
    const int32_t y_first_bits     = y_your_input     + body_cap_height + y_spacer;
    const int32_t y_checksum_label = y_first_bits     + bit_cap_height  + 2 * component_padding;
    const int32_t y_checksum_bits  = y_checksum_label + body_cap_height + y_spacer;
    const int32_t y_final_label    = y_checksum_bits  + bit_cap_height  + 2 * component_padding;
    const int32_t y_final_bits     = y_final_label    + body_cap_height + y_spacer;

    std::vector<lv_obj_t *> overlay;   // the whole breakdown group, for the taller-profile recenter pass

    // 1. Entered entropy: caption + keeper bits + the dimmed discard slot.
    overlay.push_back(tools_calc_final_word_place_centered_caption(screen, your_input_text.c_str(), y_your_input));
    overlay.push_back(tools_calc_final_word_place_bit_digits(screen, keeper_bits.c_str(), bit_font,
                                                             (uint32_t)BODY_FONT_COLOR, bit_display_x, y_first_bits));
    if (discard_is_placeholder) {
        overlay.push_back(tools_calc_final_word_place_bit_underscores(screen, discard_bits.c_str(), bit_font,
                                                                      (uint32_t)LABEL_FONT_COLOR, checksum_x,
                                                                      y_first_bits + bit_cap_height / 2));
    } else {
        overlay.push_back(tools_calc_final_word_place_bit_digits(screen, discard_bits.c_str(), bit_font,
                                                                 (uint32_t)LABEL_FONT_COLOR, checksum_x, y_first_bits));
    }

    // 2. Checksum: caption + dimmed leading rule, then the orange checksum bits.
    overlay.push_back(tools_calc_final_word_place_centered_caption(screen, checksum_label.c_str(), y_checksum_label));
    overlay.push_back(tools_calc_final_word_place_bit_underscores(screen, checksum_spacer.c_str(), bit_font,
                                                                  (uint32_t)LABEL_FONT_COLOR, bit_display_x,
                                                                  y_checksum_bits + bit_cap_height / 2));
    overlay.push_back(tools_calc_final_word_place_bit_digits(screen, checksum_bits.c_str(), bit_font,
                                                             (uint32_t)ACCENT_COLOR, checksum_x, y_checksum_bits));

    // 3. The resulting final word: caption + white keeper bits, then the orange
    //    checksum bits.
    overlay.push_back(tools_calc_final_word_place_centered_caption(screen, final_word_text.c_str(), y_final_label));
    overlay.push_back(tools_calc_final_word_place_bit_digits(screen, keeper_bits.c_str(), bit_font,
                                                             (uint32_t)BODY_FONT_COLOR, bit_display_x, y_final_bits));
    overlay.push_back(tools_calc_final_word_place_bit_digits(screen, checksum_bits.c_str(), bit_font,
                                                             (uint32_t)ACCENT_COLOR, checksum_x, y_final_bits));

    // --- Geometry ---

    // Taller-profile recenter pass. On the parity profiles (240x240 and 320x240,
    // px_multiplier == 100) the layout is the exact Python top-anchored one — leave
    // it. On the taller ESP32 profiles (px_multiplier > 100) Python has no
    // reference, so apply the house convention: vertically center the whole
    // breakdown between the top_nav and the bottom "Next" button.
    if (px_multiplier > 100) {
        lv_obj_update_layout(screen);

        int32_t group_top = INT32_MAX, group_bottom = INT32_MIN;
        for (lv_obj_t *overlay_label : overlay) {
            const int32_t label_top    = lv_obj_get_y(overlay_label);
            const int32_t label_bottom = label_top + lv_obj_get_height(overlay_label);
            if (label_top < group_top)       group_top = label_top;
            if (label_bottom > group_bottom) group_bottom = label_bottom;
        }
        const int32_t group_height = group_bottom - group_top;

        lv_area_t screen_coords;
        lv_obj_get_coords(screen, &screen_coords);

        // Bound the centering region by the actual bottom button: depth-first find
        // the LOWEST lv_button under the screen (the pinned "Next" — with a back
        // button in the top_nav, a find-FIRST walk would stop there instead), in
        // absolute coords so it shares the overlay's screen frame.
        // (Slated for extraction to screen_helpers at the compose-and-overlay
        // rollout decision; keep this local copy inline until then.)
        lv_obj_t *lowest_button = NULL;
        int32_t lowest_y = INT32_MIN;
        std::vector<lv_obj_t *> stack = { screen };
        while (!stack.empty()) {
            lv_obj_t *current = stack.back();
            stack.pop_back();
            uint32_t child_count = lv_obj_get_child_cnt(current);
            for (uint32_t i = 0; i < child_count; ++i) {
                stack.push_back(lv_obj_get_child(current, i));
            }
            if (current != screen && lv_obj_check_type(current, &lv_button_class)) {
                lv_area_t button_coords;
                lv_obj_get_coords(current, &button_coords);
                if (button_coords.y1 > lowest_y) { lowest_y = button_coords.y1; lowest_button = current; }
            }
        }
        int32_t region_bottom = lv_obj_get_height(screen) - BUTTON_HEIGHT;
        if (lowest_button) {
            lv_area_t button_coords;
            lv_obj_get_coords(lowest_button, &button_coords);
            region_bottom = button_coords.y1 - screen_coords.y1;
        }
        const int32_t region_top  = TOP_NAV_HEIGHT;
        const int32_t desired_top = region_top + (region_bottom - region_top - group_height) / 2;
        const int32_t delta       = desired_top - group_top;
        for (lv_obj_t *overlay_label : overlay) {
            lv_obj_set_y(overlay_label, lv_obj_get_y(overlay_label) + delta);
        }
    }
}
