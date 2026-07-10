// seed_words_screen
//
// Python provenance: SeedWordsScreen (seed_screens.py)
//
// One page of a seed's words, each numbered inside a rounded chip. Pagination
// is host-driven (one screen render per page; the host passes this page's
// words plus its numbering/start offset). WarningEdgesMixin frames the whole
// screen in a pulsing ORANGE (DIRE_WARNING_COLOR) border — the seed itself is
// on screen, the most sensitive material there is. The bottom-pinned action
// button returns through the standard navigation callback.
//
// Layout notes:
//   - The word list is one content-sized column, centered horizontally on its
//     widest word and TOP-anchored under the nav (Python top-anchors the
//     block). Rows are LEFT-aligned so every chip shares a left edge and every
//     word starts at the same x.
//   - Documented deviation from Python: the inter-row gap is compressed to
//     COMPONENT_PADDING (Python uses 1.5x, which reads too loose here and
//     overflows four rows on the 240 body).
//   - Documented deviation from Python: Python supersamples the whole body
//     onto a temp surface and LANCZOS-downscales + sharpens; this port renders
//     native LVGL labels (the Pi Zero driver's gamma-curve fix superseded
//     supersampling for crispness).
//   - Word/number baselines are matched to Python's shared-baseline anchors by
//     a post-layout translate_y pass (see --- Geometry ---).
//
// Lifecycle: stateless (Tier 1) — no statics, no heap ctx, no cleanup callback.
//
// cfg:
//   top_nav.title             (string, required)     localized page title (Python
//            composes _("Seed Words: {}/{}") from page_index+1 / num_pages via
//            gettext; the host supplies the finished string, so num_pages never
//            reaches this screen).
//   top_nav.show_back_button  (bool, default true)   Python ButtonListScreen default.
//   top_nav.show_power_button (bool, default false)  Python ButtonListScreen default.
//   words                     (array of strings, required, non-empty)  the words
//            shown on THIS page (the host paginates).
//   page_index                (int, default 0)       0-based page number; read only
//            to compute the start_number default below.
//   start_number              (int, default page_index*len(words)+1)  1-based number
//            of the first word on this page. The default mirrors Python's chip
//            numbering str(page_index * words_per_page + index + 1), where Python
//            also derives words_per_page = len(self.words) on the rendered page.
//   button_list               (array, required, non-empty)  the localized action
//            button(s) (Python: "Next" on interior pages, "Done" on the last).
//   is_bottom_list            forced true; a host-supplied value is ignored
//            (Python: is_bottom_list = True dataclass default).
//   initial_selected_index    (int, optional)        overrides the default initial
//            focus of 0 (navigation layer; Python selected_button).
//   input.mode                (string, optional)     "touch" | "hardware" input-mode
//            override (navigation layer).
//   input.keys.key1/key2/key3 (string, optional)     per-aux-key policy "enter" |
//            "noop" | "emit" (navigation layer).
//   allow_screensaver         (bool, default true)   per-screen screensaver policy
//            (normalized by parse_screen_json_ctx, stamped by the scaffold).

#include "screen_scaffold.h"  // parse_screen_json_ctx / create_top_nav_screen_scaffold / add_warning_edges_overlay / bind_screen_navigation / load_screen_and_cleanup_previous
#include "seedsigner.h"       // seed_words_screen decl, screen_scaffold_t fields
#include "components.h"       // reclaim_line_leading_uniform
#include "gui_constants.h"    // COMPONENT_PADDING, BUTTON_BACKGROUND_COLOR, INFO_COLOR, BODY_FONT_COLOR, DIRE_WARNING_COLOR, seedsigner_latin_font
#include "screen_helpers.h"   // ensure_top_nav_structure, require_top_nav_title

#include "lvgl.h"             // lv_obj/lv_label creation, flex layout, per-object style setters, lv_text_get_size, coords

#include <nlohmann/json.hpp>  // json (cfg reads + structural-default writes)

#include <cstddef>            // size_t (row loops)
#include <stdexcept>          // std::runtime_error (required-field validation)
#include <string>             // std::string, std::to_string
#include <vector>             // std::vector (words + per-row label collections)

using json = nlohmann::json;


void seed_words_screen(void *ctx_json) {
    // --- Config ---

    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    // Required fields: words is the seed material this screen exists to show;
    // button_list is user-visible CONTENT, which always arrives localized from
    // the host view layer (a string literal baked here would be English-only by
    // construction). One throw per field, before the scaffold exists, so no
    // throw path can leak LVGL objects.
    if (!cfg.contains("words") || !cfg["words"].is_array() || cfg["words"].empty()) {
        throw std::runtime_error("seed_words_screen: words is required and must be a non-empty array");
    }
    if (!cfg.contains("button_list") || !cfg["button_list"].is_array() || cfg["button_list"].empty()) {
        throw std::runtime_error("seed_words_screen: button_list is required and must be a non-empty array");
    }
    std::vector<std::string> words;
    for (const auto &word : cfg["words"]) words.push_back(word.get<std::string>());

    // Structural numbers (never rendered as-is; they only feed the chip
    // numbering). The start_number default mirrors Python's
    // page_index * words_per_page + index + 1, with words_per_page derived
    // from the rendered page's word count exactly as Python derives it.
    int page_index   = cfg.value("page_index", 0);
    int start_number = cfg.value("start_number", page_index * (int)words.size() + 1);

    // Structural defaults (write-if-absent, never user-visible text). Python
    // ButtonListScreen defaults: show_back_button=True, show_power_button=False.
    // The localized title itself is content and must come from the host.
    ensure_top_nav_structure(cfg, /*default_show_back_button=*/true,
                                  /*default_show_power_button=*/false);
    require_top_nav_title(cfg, "seed_words_screen");

    cfg["is_bottom_list"] = true;    // forced — Python's is_bottom_list=True dataclass default, pinned here

    // --- Scaffold ---

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, /*scrollable=*/false);

    // --- Body ---

    // Fonts: word = body font at top_nav_title+2 (Python get_top_nav_title_font_size()+2);
    // number = body font at button size. Seed words are BIP-39 wordlist tokens (ASCII), so
    // the Latin body font is an exact match to Python's body font for this content.
    // seedsigner_latin_font() takes an UNSCALED base px and applies the profile multiplier
    // itself, so pass the 240-base sizes (20+2, 18) — NOT the already-scaled *_FONT_SIZE
    // macros, which would double-scale (84 px words at the 800x480 profile).
    const lv_font_t *word_font   = seedsigner_latin_font(20 + 2);
    const lv_font_t *number_font = seedsigner_latin_font(18);

    // Square number chip sized to the widest number ("24"), like Python.
    lv_point_t number_size;
    lv_text_get_size(&number_size, "24", number_font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int32_t chip_size = number_size.x + COMPONENT_PADDING / 2;

    // The word list is one content-sized column, centered horizontally in the body and
    // top-anchored (Python top-anchors the block under the nav). Rows are LEFT-aligned so
    // every chip shares a left edge and every word starts at the same x; the block as a
    // whole centers on the widest word. Grow upper_body to claim the whole gap above the
    // button and collapse the scaffold spacer, so the block is top-anchored in a container
    // sized to fit — otherwise four rows overflow the shrink-wrapped upper_body on the 240
    // body and scroll the first word up under the nav. (This grow/center/spacer reshaping
    // is a shared idiom slated for extraction at rollout decision.)
    lv_obj_set_flex_grow(screen.upper_body, 1);
    lv_obj_set_flex_align(screen.upper_body, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(screen.upper_body, COMPONENT_PADDING / 2, LV_PART_MAIN);
    if (screen.button_list_spacer) lv_obj_set_flex_grow(screen.button_list_spacer, 0);

    // 1. The word-list column: one row per word = [number chip][word label].
    lv_obj_t *word_list = lv_obj_create(screen.upper_body);
    lv_obj_remove_style_all(word_list);
    lv_obj_set_size(word_list, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_layout(word_list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(word_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(word_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    // Compressed inter-row gap (Python's 1.5*COMPONENT_PADDING reads too loose here, and
    // four rows must fit the 240 body); the word-leading reclaim below makes each row pack
    // to the chip height rather than the taller word line box.
    lv_obj_set_style_pad_row(word_list, COMPONENT_PADDING, LV_PART_MAIN);
    lv_obj_remove_flag(word_list, LV_OBJ_FLAG_SCROLLABLE);

    // Collected per row so we can baseline-align the words to their numbers after layout.
    std::vector<lv_obj_t*> number_labels, word_labels;

    for (size_t i = 0; i < words.size(); ++i) {
        // Row: [number chip][word], vertically centered on each other.
        lv_obj_t *row = lv_obj_create(word_list);
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
        lv_obj_set_size(chip, chip_size, chip_size);
        lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(chip, lv_color_hex(BUTTON_BACKGROUND_COLOR), LV_PART_MAIN);
        lv_obj_set_style_radius(chip, 5, LV_PART_MAIN);
        lv_obj_set_layout(chip, LV_LAYOUT_FLEX);
        lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(chip, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *number_label = lv_label_create(chip);
        lv_label_set_text(number_label, std::to_string(start_number + (int)i).c_str());
        lv_obj_set_style_text_font(number_label, number_font, LV_PART_MAIN);
        lv_obj_set_style_text_color(number_label, lv_color_hex(INFO_COLOR), LV_PART_MAIN);
        lv_obj_set_style_pad_all(number_label, 0, LV_PART_MAIN);
        number_labels.push_back(number_label);

        // The word. Reclaim the FONT's line-height leading (uniform, not per-word) so the
        // row packs to the chip height AND every word gets the same box/baseline — a per-
        // word reclaim would align descender words (ridge, oyster) differently from plain
        // ones (muffin) against the number chip.
        lv_obj_t *word_label = lv_label_create(row);
        lv_label_set_text(word_label, words[i].c_str());
        lv_obj_set_style_text_font(word_label, word_font, LV_PART_MAIN);
        lv_obj_set_style_text_color(word_label, lv_color_hex(BODY_FONT_COLOR), LV_PART_MAIN);
        lv_obj_set_style_pad_all(word_label, 0, LV_PART_MAIN);
        reclaim_line_leading_uniform(word_label, word_font);
        word_labels.push_back(word_label);
    }

    // 2. WarningEdgesMixin — pulsing orange border (seed on screen).
    add_warning_edges_overlay(screen.screen, DIRE_WARNING_COLOR);

    // --- Geometry ---

    // Baseline-align pass — shift each word down onto its number's baseline.
    // Python draws both at the same baseline_y (word anchor "ls", number "ms"),
    // so descenders drop below a shared baseline. The row's cross-centering above
    // aligns the BOXES, not the baselines, leaving the words sitting high; shift
    // each word down by the gap between the two text baselines (a label's baseline
    // is its box top + the font ascent = line_height - base_line).
    lv_obj_update_layout(screen.screen);
    const int32_t number_ascent = (int32_t)lv_font_get_line_height(number_font) - number_font->base_line;
    const int32_t word_ascent   = (int32_t)lv_font_get_line_height(word_font)   - word_font->base_line;
    for (size_t i = 0; i < word_labels.size() && i < number_labels.size(); ++i) {
        lv_area_t number_area, word_area;
        lv_obj_get_coords(number_labels[i], &number_area);
        lv_obj_get_coords(word_labels[i], &word_area);
        int32_t baseline_delta = (number_area.y1 + number_ascent) - (word_area.y1 + word_ascent);
        if (baseline_delta != 0) lv_obj_set_style_translate_y(word_labels[i], baseline_delta, LV_PART_MAIN);
    }

    // --- Navigation + load ---

    // Menu-style default index: the action list always has a selection, so the
    // first button starts focused (the host may override via cfg
    // initial_selected_index).
    bind_screen_navigation(cfg, screen, /*default_initial_index=*/0);

    load_screen_and_cleanup_previous(screen.screen);
}
