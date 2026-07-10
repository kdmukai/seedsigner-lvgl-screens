#include "screen_scaffold.h" // parse_screen_json_ctx
#include "seedsigner.h"      // tools_calc_final_word_screen(), button_list_screen(), text_top_leading()
#include "screen_helpers.h"  // measure_text_ink_extents()
#include "components.h"      // monospace_char_width()
#include "gui_constants.h"   // TOP_NAV_HEIGHT, COMPONENT_PADDING, EDGE_PADDING, BODY_FONT, CANDIDATE_FONT, colors

#include "lvgl.h"

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>
#include <vector>

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// tools_calc_final_word_screen
// ---------------------------------------------------------------------------
//
// Parity target: Python `ToolsCalcFinalWordScreen` (tools_screens.py:259), a
// bottom-pinned `ButtonListScreen` (is_bottom_list=True) with a single "Next"
// button and an absolutely-positioned "final word math" breakdown. The View
// `ToolsCalcFinalWordShowFinalWordView` renders it in two variants:
//   - pick_word  : the user picked a final word (its 11 index bits are shown; the
//                  least-significant `num_checksum` bits are DISCARDED — dimmed —
//                  and replaced by the computed checksum).
//   - coin_flips : the user entered N coin flips (7 here → a 12-word seed); the
//                  discarded slot is shown as dimmed underscores instead of bits.
//
// The breakdown is three labeled blocks, each a centered BODY_FONT caption over a
// centered fixed-width (Inconsolata-SemiBold) 11-column bit display:
//   1. `Your input: "<word|flips>"`  +  the entered 11 (or fewer) entropy bits.
//        keeper bits (the first 11-num_checksum) render white; the trailing slot
//        renders dimmed — real bits (pick_word) or underscores (coin_flips).
//   2. `Checksum`  +  dimmed leading underscores then the ORANGE checksum bits.
//   3. `Final Word: "<word>"`  +  the merged result: white keeper bits then the
//        ORANGE checksum bits.
// The three bit rows share ONE centered 11-column monospace grid: the "keeper"
// segment is left-anchored at bit_display_x; the "checksum" segment is anchored at
// checksum_x = bit_display_x + (11 - num_checksum) * char_width, so the two
// segments abut into one contiguous 11-char string (Python's exact geometry).
//
// Compose strategy (mirrors settings_qr_confirmation_screen.cpp): DELEGATE the
// chrome (top_nav + bottom "Next" button + joystick/touch nav + screen load) to the
// public button_list_screen with an empty intro, then OVERLAY the breakdown labels
// on the freshly-loaded screen at their absolute positions. No duplication of the
// shared scaffold (screen_scaffold.cpp).
//
// FONT NOTE (parity caveat): Python renders the bits in Inconsolata-SemiBold at
// button_font_size("default") + 2 = 20 px. This repo bakes Inconsolata-SemiBold
// only at 22 px (CANDIDATE_FONT) and 24 px (KEYBOARD_FONT) — no 20 px variant — so
// the closest available (CANDIDATE_FONT, 22 px) is used. Its digits are ~2 px taller
// (14 vs 12 px cap) and ~1 px wider (11 vs 10 px advance) than the reference; the row
// RHYTHM and the orange checksum column are anchored to the Python (20 px) metrics so
// the label rows and highlight stay aligned, but the bit glyphs themselves are a hair
// larger. A pixel-exact match would need a 20 px Inconsolata-SemiBold bake.
//
// cfg contract:
//   {
//     "top_nav": { "title": "Final Word Calc", "show_back_button": true },
//     "button_list": ["Next"],                 // OPTIONAL; defaults to ["Next"]
//     "your_input_text":    "Your input: \"satoshi\"",  // host-formatted + localized
//     "final_word_text":    "Final Word: \"say\"",      // host-formatted + localized
//     "checksum_label":     "Checksum",                 // OPTIONAL; defaults to "Checksum"
//     "selected_final_bits": "10111111011",     // entered entropy bits (11 for a word, N for flips)
//     "checksum_bits":       "1111",            // the actual checksum bits (4 for 12-word, 8 for 24)
//     "has_selected_word":   true               // true → discard slot shows real dimmed bits;
//                                               // false (coin flips) → dimmed underscores
//   }

// The Python bit font size is button_font_size("default") + 2 = 20 px at the 240-height
// reference; it does NOT vary by locale (only by display profile). Cap height 12,
// advance 10 at that size. Kept as the layout reference below even though the actual
// glyphs render through the (larger) baked CANDIDATE_FONT — see FONT NOTE above.
static const int PY_BIT_CAP_HEIGHT_240 = 12;   // Inconsolata-SemiBold 20 px digit ink height

// Python caption row height at the 240-height reference: TextArea(height_ignores_below_baseline)
// for BODY_FONT (OpenSans 17) measures 13 px top-to-baseline. Used only for the inter-row
// RHYTHM (every caption anchors this far below the preceding bit row / vice-versa). The
// caption's OWN ink top is placed independently via text_top_leading, so this constant just
// sets spacing. NB: LVGL's measured OpenSans-17 ink ascent is 14 px, 1 px looser than PIL —
// using the PIL value here lands every row exactly on the reference.
static const int PY_BODY_CAP_HEIGHT_240 = 13;

static const int BIT_COLUMNS = 11;             // a BIP-39 word index is 11 bits

// Create a centered, single-line BODY_FONT caption whose VISIBLE ink top lands at the
// canvas-relative `ink_top_y` (Python TextArea.screen_y with height_ignores_below_baseline).
// LVGL anchors a label box by the font ascent (leading above the caps); subtract that
// leading so the visible text lands where PIL draws it (splash/status/settings-qr pattern).
static lv_obj_t *place_centered_caption(lv_obj_t *screen, const char *text, int32_t ink_top_y) {
    lv_obj_t *label = lv_label_create(screen);
    lv_label_set_text(label, text);

    // Python centers on the full canvas; a (canvas - 2*EDGE_PADDING)-wide box centered
    // with centered text is equivalent. CLIP (not WRAP): these captions are single-line.
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(label, lv_obj_get_width(screen) - 2 * EDGE_PADDING);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(BODY_FONT_COLOR), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &BODY_FONT, LV_PART_MAIN);

    const int32_t lead = text_top_leading(&BODY_FONT, text);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, ink_top_y - lead);
    return label;
}

// Left-anchor a run of monospace bit chars at `x` so its VISIBLE ink top lands at
// `ink_top_y` (matches the digit-row rhythm the captions are anchored to).
static lv_obj_t *place_bit_digits(lv_obj_t *screen, const char *text, const lv_font_t *font,
                                  uint32_t color, int32_t x, int32_t ink_top_y) {
    lv_obj_t *label = lv_label_create(screen);
    lv_obj_set_style_pad_all(label, 0, LV_PART_MAIN);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
    lv_label_set_text(label, text);

    const int32_t lead = text_top_leading(font, text);
    lv_obj_set_pos(label, x, ink_top_y - lead);
    return label;
}

// Left-anchor a run of monospace underscore placeholders at `x` so its ink CENTER lands
// at `ink_center_y`. Python draws the discarded/placeholder slots as underscores raised
// to the digit-row's vertical center (so they read like a mid-line rule, not a baseline
// tail); the reference shows the underscore ink at the digit band's vertical center.
static lv_obj_t *place_bit_underscores(lv_obj_t *screen, const char *text, const lv_font_t *font,
                                       uint32_t color, int32_t x, int32_t ink_center_y) {
    lv_obj_t *label = lv_label_create(screen);
    lv_obj_set_style_pad_all(label, 0, LV_PART_MAIN);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
    lv_label_set_text(label, text);

    int32_t asc = 0, desc = 0;
    measure_text_ink_extents(font, text, &asc, &desc);
    // Underscore ink spans [baseline - asc, baseline + desc]; its center is
    // baseline + (desc - asc)/2. Solve for the baseline that lands the center on
    // ink_center_y, then back out the label box top (baseline - font ascent).
    const int32_t font_ascent = (int32_t)lv_font_get_line_height(font) - (int32_t)font->base_line;
    const int32_t baseline = ink_center_y + (asc - desc) / 2;
    lv_obj_set_pos(label, x, baseline - font_ascent);
    return label;
}

void tools_calc_final_word_screen(void *ctx_json) {
    const char *json_str = (const char *)ctx_json;
    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    // --- This screen's own fields (host formats + localizes the caption strings). ---
    const std::string your_input_text = cfg.value("your_input_text", std::string());
    const std::string final_word_text = cfg.value("final_word_text", std::string());
    const std::string checksum_label  = cfg.value("checksum_label",  std::string("Checksum"));
    const std::string selected_final_bits = cfg.value("selected_final_bits", std::string());
    const std::string checksum_bits       = cfg.value("checksum_bits",       std::string());
    const bool has_selected_word          = cfg.value("has_selected_word",   true);

    if (checksum_bits.empty() || selected_final_bits.empty()) {
        throw std::runtime_error(
            "tools_calc_final_word_screen requires \"selected_final_bits\" and \"checksum_bits\"");
    }

    const int num_checksum = (int)checksum_bits.size();
    const int num_keeper    = BIT_COLUMNS - num_checksum;   // entropy bits the user contributes

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

    // --- 1. Chrome via button_list_screen (no intro text), exactly like the reference. ---
    json chrome = cfg;
    chrome.erase("your_input_text");
    chrome.erase("final_word_text");
    chrome.erase("checksum_label");
    chrome.erase("selected_final_bits");
    chrome.erase("checksum_bits");
    chrome.erase("has_selected_word");
    chrome["text"] = "";                 // no flowed intro text; we overlay the breakdown
    if (!chrome.contains("top_nav") || !chrome["top_nav"].is_object()) chrome["top_nav"] = json::object();
    if (!chrome["top_nav"].contains("title")) chrome["top_nav"]["title"] = "Final Word Calc";
    if (!chrome.contains("button_list")) chrome["button_list"] = json::array({ "Next" });
    chrome["is_bottom_list"] = true;

    const std::string chrome_str = chrome.dump();
    button_list_screen((void *)chrome_str.c_str());

    // --- 2. Overlay the breakdown on the just-loaded screen. ---
    lv_obj_t *screen = lv_scr_act();
    const lv_font_t *bit_font = &CANDIDATE_FONT;   // closest baked Inconsolata-SemiBold (22 px)

    const int   px_mult = active_profile().px_multiplier;
    const int32_t comp  = COMPONENT_PADDING;       // already px-scaled

    // Horizontal geometry: center an 11-column monospace grid on the full canvas, then
    // anchor the checksum segment (11 - num_checksum) columns in — Python's exact math,
    // using the actual font's advance so the grid stays monospace.
    const int32_t W = lv_obj_get_width(screen);
    int32_t char_width = monospace_char_width(bit_font, BIT_COLUMNS);
    const int32_t bit_display_width = BIT_COLUMNS * char_width;
    const int32_t bit_display_x     = (W - bit_display_width) / 2;
    const int32_t checksum_x        = bit_display_x + num_keeper * char_width;

    // Vertical geometry: reproduce Python's stacked layout. Every caption row advances
    // by (body cap height + COMPONENT_PADDING); every caption sits 2*COMPONENT_PADDING
    // below the preceding bit row (whose height is the Python 20-px digit cap). The bit
    // rows use the Python cap height for RHYTHM even though the baked glyphs are a hair
    // taller (see FONT NOTE), so the captions/highlight keep the reference spacing.
    const int32_t body_cap = PY_BODY_CAP_HEIGHT_240 * px_mult / 100;  // 13 px at 240 (PIL caption height)
    const int32_t bit_cap  = PY_BIT_CAP_HEIGHT_240 * px_mult / 100;    // 12 px at 240
    const int32_t y_spacer = comp;                                    // body font == default size

    const int32_t y_your_input     = TOP_NAV_HEIGHT + comp - 2;       // Python: top_nav.height + COMPONENT_PADDING - 2
    const int32_t y_first_bits     = y_your_input     + body_cap + y_spacer;
    const int32_t y_checksum_label = y_first_bits     + bit_cap  + 2 * comp;
    const int32_t y_checksum_bits  = y_checksum_label + body_cap + y_spacer;
    const int32_t y_final_label    = y_checksum_bits  + bit_cap  + 2 * comp;
    const int32_t y_final_bits     = y_final_label    + body_cap + y_spacer;

    std::vector<lv_obj_t *> overlay;   // for the taller-profile vertical recenter

    // Row 1: entered entropy.
    overlay.push_back(place_centered_caption(screen, your_input_text.c_str(), y_your_input));
    overlay.push_back(place_bit_digits(screen, keeper_bits.c_str(), bit_font,
                                       (uint32_t)BODY_FONT_COLOR, bit_display_x, y_first_bits));
    if (discard_is_placeholder) {
        overlay.push_back(place_bit_underscores(screen, discard_bits.c_str(), bit_font,
                                                (uint32_t)LABEL_FONT_COLOR, checksum_x,
                                                y_first_bits + bit_cap / 2));
    } else {
        overlay.push_back(place_bit_digits(screen, discard_bits.c_str(), bit_font,
                                           (uint32_t)LABEL_FONT_COLOR, checksum_x, y_first_bits));
    }

    // Row 2: checksum — dimmed leading rule then the orange checksum bits.
    overlay.push_back(place_centered_caption(screen, checksum_label.c_str(), y_checksum_label));
    overlay.push_back(place_bit_underscores(screen, checksum_spacer.c_str(), bit_font,
                                            (uint32_t)LABEL_FONT_COLOR, bit_display_x,
                                            y_checksum_bits + bit_cap / 2));
    overlay.push_back(place_bit_digits(screen, checksum_bits.c_str(), bit_font,
                                       (uint32_t)ACCENT_COLOR, checksum_x, y_checksum_bits));

    // Row 3: the resulting final word — white keeper bits then the orange checksum bits.
    overlay.push_back(place_centered_caption(screen, final_word_text.c_str(), y_final_label));
    overlay.push_back(place_bit_digits(screen, keeper_bits.c_str(), bit_font,
                                       (uint32_t)BODY_FONT_COLOR, bit_display_x, y_final_bits));
    overlay.push_back(place_bit_digits(screen, checksum_bits.c_str(), bit_font,
                                       (uint32_t)ACCENT_COLOR, checksum_x, y_final_bits));

    // On the parity profiles (240x240 and 320x240, px_multiplier == 100) the layout is the
    // exact Python top-anchored one — leave it. On the taller ESP32 profiles (px_multiplier
    // > 100) Python has no reference, so apply the house convention: vertically center the
    // whole breakdown between the top_nav and the bottom "Next" button.
    if (px_mult > 100) {
        lv_obj_update_layout(screen);

        int32_t group_top = INT32_MAX, group_bottom = INT32_MIN;
        for (lv_obj_t *o : overlay) {
            const int32_t top = lv_obj_get_y(o);
            const int32_t bot = top + lv_obj_get_height(o);
            if (top < group_top)    group_top = top;
            if (bot > group_bottom) group_bottom = bot;
        }
        const int32_t group_h = group_bottom - group_top;

        lv_area_t scr_coords;
        lv_obj_get_coords(screen, &scr_coords);

        // Bound the centering region by the actual bottom button: depth-first find the
        // lowest lv_button under the screen (the pinned "Next"), in absolute coords so it
        // shares the overlay's screen frame.
        lv_obj_t *btn = NULL;
        int32_t lowest_y = INT32_MIN;
        std::vector<lv_obj_t *> stack = { screen };
        while (!stack.empty()) {
            lv_obj_t *cur = stack.back();
            stack.pop_back();
            uint32_t n = lv_obj_get_child_cnt(cur);
            for (uint32_t i = 0; i < n; ++i) {
                stack.push_back(lv_obj_get_child(cur, i));
            }
            if (cur != screen && lv_obj_check_type(cur, &lv_button_class)) {
                lv_area_t bc;
                lv_obj_get_coords(cur, &bc);
                if (bc.y1 > lowest_y) { lowest_y = bc.y1; btn = cur; }
            }
        }
        int32_t region_bottom = lv_obj_get_height(screen) - BUTTON_HEIGHT;
        if (btn) {
            lv_area_t bc;
            lv_obj_get_coords(btn, &bc);
            region_bottom = bc.y1 - scr_coords.y1;
        }
        const int32_t region_top  = TOP_NAV_HEIGHT;
        const int32_t desired_top = region_top + (region_bottom - region_top - group_h) / 2;
        const int32_t delta       = desired_top - group_top;
        for (lv_obj_t *o : overlay) {
            lv_obj_set_y(o, lv_obj_get_y(o) + delta);
        }
    }
}
