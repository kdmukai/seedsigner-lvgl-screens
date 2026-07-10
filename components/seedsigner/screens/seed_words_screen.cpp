#include "seedsigner.h"
#include "screen_scaffold.h"
#include "screen_helpers.h"
#include "components.h"
#include "camera_preview_overlay.h"
#include "camera_entropy_overlay.h"
#include "keyboard_core.h"
#include "gui_constants.h"
#include "navigation.h"
#include "input_profile.h"
#include "font_registry.h"
#include "glyph_runs.h"
#include "locale_loader.h"
#include "locale_picker.h"
#include "overlay_manager.h"

#include "lvgl.h"

#if LV_USE_QRCODE
#include "../../../third_party/lvgl/src/libs/qrcode/qrcodegen.h"
#endif

#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <set>
#include <map>
#include <algorithm>
#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif

using json = nlohmann::json;

// SeedWordsScreen: one page of a seed's words, each numbered in a rounded chip. Paginated
// by the host (one screen render per page). WarningEdgesMixin frames it in pulsing ORANGE
// (DIRE_WARNING_COLOR) — the seed itself is on screen, the most sensitive material there is.
//
// cfg:
//   top_nav.title      — default "Seed Words: {page_index+1}/{num_pages}".
//   words (array, req) — the words shown on THIS page.
//   page_index (int)   — 0-based page number (default 0), for the default numbering.
//   num_pages (int)    — total pages (default 1), for the default title.
//   start_number (int) — 1-based number of the first word on this page. Default
//                        page_index*len(words)+1 (Python's page_index*words_per_page+...).
//   button_list (array)— default ["Done"].
void seed_words_screen(void *ctx_json) {
    const char *json_str = (const char *)ctx_json;
    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    if (!cfg.contains("words") || !cfg["words"].is_array() || cfg["words"].empty()) {
        throw std::runtime_error("seed_words_screen requires a non-empty \"words\" array");
    }
    std::vector<std::string> words;
    for (const auto &w : cfg["words"]) words.push_back(w.get<std::string>());

    int page_index   = cfg.value("page_index", 0);
    int num_pages    = cfg.value("num_pages", 1);
    int start_number = cfg.value("start_number", page_index * (int)words.size() + 1);

    if (!cfg.contains("top_nav") || !cfg["top_nav"].is_object()) cfg["top_nav"] = json::object();
    if (!cfg["top_nav"].contains("title")) {
        cfg["top_nav"]["title"] = "Seed Words: " + std::to_string(page_index + 1) +
                                  "/" + std::to_string(num_pages);
    }
    cfg["is_bottom_list"] = true;
    if (!cfg.contains("button_list")) cfg["button_list"] = json::array({ "Done" });

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, false);

    // Fonts: word = body font at top_nav_title+2 (Python get_top_nav_title_font_size()+2);
    // number = body font at button size. Seed words are BIP-39 wordlist tokens (ASCII), so
    // the Latin body font is an exact match to Python's body font for this content.
    // seedsigner_latin_font() takes an UNSCALED base px and applies the profile multiplier
    // itself, so pass the 240-base sizes (20+2, 18) — NOT the already-scaled *_FONT_SIZE
    // macros, which would double-scale (84 px words at the 800x480 profile).
    const lv_font_t *word_font   = seedsigner_latin_font(20 + 2);
    const lv_font_t *number_font = seedsigner_latin_font(18);

    // Square number chip sized to the widest number ("24"), like Python.
    lv_point_t nsz;
    lv_text_get_size(&nsz, "24", number_font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int32_t box = nsz.x + COMPONENT_PADDING / 2;

    // The word list is one content-sized column, centered horizontally in the body and
    // top-anchored (Python top-anchors the block under the nav). Rows are LEFT-aligned so
    // every chip shares a left edge and every word starts at the same x; the block as a
    // whole centers on the widest word. Grow upper_body to claim the whole gap above the
    // button and collapse the scaffold spacer, so the block is top-anchored in a container
    // sized to fit — otherwise four rows overflow the shrink-wrapped upper_body on the 240
    // body and scroll the first word up under the nav.
    lv_obj_set_flex_grow(screen.upper_body, 1);
    lv_obj_set_flex_align(screen.upper_body, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(screen.upper_body, COMPONENT_PADDING / 2, LV_PART_MAIN);
    if (screen.button_list_spacer) lv_obj_set_flex_grow(screen.button_list_spacer, 0);

    lv_obj_t *list = lv_obj_create(screen.upper_body);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_layout(list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    // Compressed inter-row gap (Python's 1.5*COMPONENT_PADDING reads too loose here, and
    // four rows must fit the 240 body); the word-leading reclaim below makes each row pack
    // to the chip height rather than the taller word line box.
    lv_obj_set_style_pad_row(list, COMPONENT_PADDING, LV_PART_MAIN);
    lv_obj_remove_flag(list, LV_OBJ_FLAG_SCROLLABLE);

    // Collected per row so we can baseline-align the words to their numbers after layout.
    std::vector<lv_obj_t*> num_labels, word_labels;

    for (size_t i = 0; i < words.size(); ++i) {
        // Row: [number chip][word], vertically centered on each other.
        lv_obj_t *row = lv_obj_create(list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_layout(row, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, COMPONENT_PADDING, LV_PART_MAIN);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        // Rounded number chip — dark button-fill, radius 5, the (INFO_COLOR) number centered.
        lv_obj_t *chip = lv_obj_create(row);
        lv_obj_remove_style_all(chip);
        lv_obj_set_size(chip, box, box);
        lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(chip, lv_color_hex(BUTTON_BACKGROUND_COLOR), LV_PART_MAIN);
        lv_obj_set_style_radius(chip, 5, LV_PART_MAIN);
        lv_obj_set_layout(chip, LV_LAYOUT_FLEX);
        lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(chip, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *num = lv_label_create(chip);
        lv_label_set_text(num, std::to_string(start_number + (int)i).c_str());
        lv_obj_set_style_text_font(num, number_font, LV_PART_MAIN);
        lv_obj_set_style_text_color(num, lv_color_hex(INFO_COLOR), LV_PART_MAIN);
        lv_obj_set_style_pad_all(num, 0, LV_PART_MAIN);
        num_labels.push_back(num);

        // The word. Reclaim the FONT's line-height leading (uniform, not per-word) so the
        // row packs to the chip height AND every word gets the same box/baseline — a per-
        // word reclaim would align descender words (ridge, oyster) differently from plain
        // ones (muffin) against the number chip.
        lv_obj_t *word = lv_label_create(row);
        lv_label_set_text(word, words[i].c_str());
        lv_obj_set_style_text_font(word, word_font, LV_PART_MAIN);
        lv_obj_set_style_text_color(word, lv_color_hex(BODY_FONT_COLOR), LV_PART_MAIN);
        lv_obj_set_style_pad_all(word, 0, LV_PART_MAIN);
        reclaim_line_leading_uniform(word, word_font);
        word_labels.push_back(word);
    }

    // WarningEdgesMixin — pulsing orange border (seed on screen).
    add_warning_edges_overlay(screen.screen, DIRE_WARNING_COLOR);

    bind_screen_navigation(cfg, screen, 0);

    // Baseline-align each word to its number: Python draws both at the same baseline_y
    // (word anchor "ls", number "ms"), so descenders drop below a shared baseline. Cross-
    // centering above aligns the BOXES, not the baselines, leaving the words sitting high;
    // shift each word down by the gap between the two text baselines (a label's baseline is
    // its box top + the font ascent = line_height - base_line).
    lv_obj_update_layout(screen.screen);
    const int32_t num_ascent  = (int32_t)lv_font_get_line_height(number_font) - number_font->base_line;
    const int32_t word_ascent = (int32_t)lv_font_get_line_height(word_font)   - word_font->base_line;
    for (size_t i = 0; i < word_labels.size() && i < num_labels.size(); ++i) {
        lv_area_t na, wa;
        lv_obj_get_coords(num_labels[i], &na);
        lv_obj_get_coords(word_labels[i], &wa);
        int32_t delta = (na.y1 + num_ascent) - (wa.y1 + word_ascent);
        if (delta != 0) lv_obj_set_style_translate_y(word_labels[i], delta, LV_PART_MAIN);
    }

    load_screen_and_cleanup_previous(screen.screen);
}
