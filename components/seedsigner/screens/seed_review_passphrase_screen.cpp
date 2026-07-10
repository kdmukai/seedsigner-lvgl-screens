// seed_review_passphrase_screen
//
// Python provenance: SeedReviewPassphraseScreen (seed_screens.py)
//
// BIP-39 passphrase review before it is applied to the seed: the EXACT
// passphrase (orange, fixed-width, centered, up to 3 balanced lines) above a
// fingerprint IconTextLine spelling out how the passphrase changes the seed's
// fingerprint ("<without> >> <with>"), over a bottom-pinned action list
// (Python's view supplies EDIT "Edit passphrase" + DONE "Done"). The pressed
// button index returns through the standard navigation callback. No warning
// edges.
//
// Lifecycle: stateless (Tier 1). The only heap state is the space-block
// geometry POD for the hidden-space reveal, owned by the passphrase label via
// its LV_EVENT_DELETE callback.
//
// Layout: bottom-pinned button scaffold, but BOTH content blocks are parented
// to the screen ROOT (not upper_body, which stays empty) and placed by an
// absolute measure-then-set_pos pass under --- Geometry --- — a direct port of
// Python's screen_y coordinate math (the fingerprint line anchors UPWARD from
// the button's top edge; the passphrase centers in the band between the
// top-nav and the fingerprint line), which the scaffold's top-down flex
// stacking cannot express.
//
// Documented deviations from Python:
//   - Fixed passphrase font: Python auto-fits its fixed-width font over a size
//     range (top-nav-title size +8 down to -4, stepping -2); this port renders
//     at the single baked 24 px monospace (KEYBOARD_FONT). A passphrase too
//     long for 3 lines at that size is neither truncated nor shrunk — the
//     lines overflow the screen width (see seed_review_passphrase_wrap).
//   - Hidden-space reveal: Python substitutes ▉ (U+2589) for every space when
//     the passphrase has leading/trailing/doubled spaces; here the real spaces
//     are KEPT and a custom draw callback paints a solid block OVER each one
//     (the ▉ glyph's bake inflated the shared monospace font's line_height,
//     regressing unrelated monospace layouts).
//   - Balanced wrap: Python hard-splits every ceil(len/num_lines) characters;
//     this port balances the line lengths and prefers breaks after special
//     characters (see seed_review_passphrase_wrap).
//
// cfg:
//   top_nav.title             (string, required)     localized title
//            (Python: _("Verify Passphrase")).
//   top_nav.show_back_button  (bool, default true)   Python ButtonListScreen default.
//   top_nav.show_power_button (bool, default false)  Python ButtonListScreen default.
//   passphrase                (string, required)     the passphrase under review,
//            rendered verbatim — every character, spaces included.
//   fingerprint_without       (string, required)     master fingerprint hex without
//            the passphrase applied.
//   fingerprint_with          (string, required)     master fingerprint hex with
//            the passphrase applied.
//   changes_fingerprint_label (string, required)     localized caption above the
//            fingerprint readout (Python: _("changes fingerprint")).
//   button_list               (array, required, non-empty)  the localized action
//            buttons (Python view: EDIT "Edit passphrase", DONE "Done").
//   is_bottom_list            forced true (Python: is_bottom_list = True); a
//            host-supplied value is ignored.
//   initial_selected_index    (int, optional)        overrides the default initial
//            focus of 0 (navigation layer; Python selected_button).
//   input.mode                (string, optional)     "touch" | "hardware" input-mode
//            override (navigation layer).
//   input.keys.key1/key2/key3 (string, optional)     per-aux-key policy "enter" |
//            "noop" | "emit" (navigation layer).
//   allow_screensaver         (bool, default true)   per-screen screensaver policy
//            (normalized by parse_screen_json_ctx, stamped by the scaffold).

#include "screen_scaffold.h"  // parse_screen_json_ctx / create_top_nav_screen_scaffold / bind_screen_navigation / bottom_button_top_y / load_screen_and_cleanup_previous
#include "seedsigner.h"       // seed_review_passphrase_screen decl, screen_scaffold_t fields
#include "components.h"       // icon_text_line, monospace_char_width, SEEDSIGNER_ICON_COLOR_DEFAULT
#include "gui_constants.h"    // COMPONENT_PADDING, EDGE_PADDING, TOP_NAV_HEIGHT, BODY_FONT_SIZE, INFO_COLOR, KEYBOARD_FONT, SeedSignerIconConstants
#include "screen_helpers.h"   // ensure_top_nav_structure, require_top_nav_title, tight_line_space

#include "lvgl.h"             // labels, flex column, lv_draw_rect custom draw, coords/layout queries

#include <nlohmann/json.hpp>  // json (cfg reads + structural-default writes)

#include <algorithm>          // std::min / std::max (balanced hard-cut clamp)
#include <cstddef>            // size_t (substr offsets in the wrap)
#include <stdexcept>          // std::runtime_error (required-field validation)
#include <string>             // std::string
#include <vector>             // std::vector (wrapped passphrase lines)

using json = nlohmann::json;


namespace {

// ---------------------------------------------------------------------------
// Passphrase wrapping
// ---------------------------------------------------------------------------

// Wrap a passphrase into up to `max_lines` lines of at most `max_chars_per_line`
// monospace characters. Two goals beyond Python's naive fixed-width char split:
//   1. Break right AFTER a special (non-alphanumeric) character — dash, punctuation,
//      space — when one sits near the balanced break point, so a human-readable passphrase
//      wraps at its natural separators ("correct-horse-" / ...) instead of mid-word.
//   2. Balance the line lengths (each break targets line_index * length / line_count),
//      like the body-text balanced wrap.
// A random or all-alphabetic passphrase has no special-char breakpoints, so it falls back
// to a balanced hard split by length. EVERY character is preserved — the break character
// stays at the end of its line — since the whole point is to show the exact passphrase.
// OVERFLOW: when the passphrase cannot fit within max_lines * max_chars_per_line
// characters, the line count still caps at max_lines and the surplus lands on the
// EARLIEST lines, which then exceed max_chars_per_line — nothing is ever truncated, and
// there is no font-size fallback, so such lines render wider than the screen.
std::vector<std::string> seed_review_passphrase_wrap(const std::string &passphrase,
                                                     int max_chars_per_line, int max_lines) {
    const int length = (int)passphrase.size();
    if (max_chars_per_line < 1 || length <= max_chars_per_line) return { passphrase };

    int line_count = (length + max_chars_per_line - 1) / max_chars_per_line;   // ceil: fewest lines that can fit
    if (line_count > max_lines) line_count = max_lines;
    if (line_count < 1) line_count = 1;

    auto is_special = [](char character) {
        return !((character >= 'a' && character <= 'z') ||
                 (character >= 'A' && character <= 'Z') ||
                 (character >= '0' && character <= '9'));
    };
    auto distance = [](int position_a, int position_b) {
        int delta = position_a - position_b;
        return delta < 0 ? -delta : delta;
    };

    std::vector<int> cuts;   // start index of each new line
    int previous_cut = 0;
    for (int line_index = 1; line_index < line_count; ++line_index) {
        const int ideal = (int)((long)length * line_index / line_count);   // balanced target cut
        int cut_low  = previous_cut + 1;                                   // ≥1 char on this line
        int cut_high = previous_cut + max_chars_per_line;                  // this line ≤ max_chars_per_line
        // Leave room: the remaining text must fit the remaining lines, ≥1 char each.
        const int remaining_lines = line_count - line_index;
        if (cut_high > length - remaining_lines)                       cut_high = length - remaining_lines;
        if (cut_low  < length - remaining_lines * max_chars_per_line)  cut_low  = length - remaining_lines * max_chars_per_line;
        if (cut_low  < previous_cut + 1)                               cut_low  = previous_cut + 1;
        if (cut_high < cut_low)                                        cut_high = cut_low;

        // Prefer a break just after a special char, closest to `ideal`, within [cut_low, cut_high].
        int best = -1;
        for (int position = cut_low; position <= cut_high && position < length; ++position) {
            if (is_special(passphrase[position - 1]) &&
                (best < 0 || distance(position, ideal) < distance(best, ideal))) {
                best = position;
            }
        }
        int cut = (best >= 0) ? best : std::min(std::max(ideal, cut_low), cut_high);   // else balanced hard cut
        cuts.push_back(cut);
        previous_cut = cut;
    }

    std::vector<std::string> lines;
    int start = 0;
    for (int cut : cuts) {
        lines.push_back(passphrase.substr((size_t)start, (size_t)(cut - start)));
        start = cut;
    }
    lines.push_back(passphrase.substr((size_t)start));
    return lines;
}


// ---------------------------------------------------------------------------
// Hidden-space reveal
// ---------------------------------------------------------------------------

// When a passphrase hides leading/trailing/doubled spaces, every space is marked with a
// SOLID block drawn over it — not a substituted font glyph. Two glyph approaches were
// rejected: the ▉ block (U+2589) inflated the baked monospace line_height, regressing every
// layout that reads it (e.g. psbt_math row spacing); and a hollow open-box / .notdef box
// reads as a "tofu" rendering-error to anyone used to non-Latin glyph fallbacks. The custom
// solid block spans from the tallest printable-ASCII ink down to the baseline, is one
// monospace cell wide, and takes the label's text color: the marker we wanted, at correct
// proportions, with no font-metric side effects and no error-glyph ambiguity.
//
// Allocation idiom: pure POD — lv_malloc'd where the reveal is attached, lv_free'd by the
// label's LV_EVENT_DELETE callback (seed_review_passphrase_space_block_free_cb).
struct seed_review_passphrase_space_block_t {
    int32_t cell_width;   // monospace cell width (space advance)
    int32_t top;          // px from a line's top down to the block top (tallest-ink cap)
    int32_t bottom;       // px from a line's top down to the block bottom (baseline)
    int32_t inset;        // horizontal inset per side (small gap between adjacent blocks)
};

void seed_review_passphrase_space_block_cb(lv_event_t *e) {
    lv_obj_t   *label = lv_event_get_target_obj(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    seed_review_passphrase_space_block_t *space_block =
        (seed_review_passphrase_space_block_t *)lv_event_get_user_data(e);
    if (!label || !layer || !space_block) return;

    const char *text = lv_label_get_text(label);
    if (!text) return;

    lv_area_t label_coords;
    lv_obj_get_coords(label, &label_coords);   // absolute label box; letter positions are label-relative

    lv_draw_rect_dsc_t rect_dsc;
    lv_draw_rect_dsc_init(&rect_dsc);
    rect_dsc.bg_color = lv_obj_get_style_text_color(label, LV_PART_MAIN);  // same orange as the text
    rect_dsc.bg_opa   = LV_OPA_COVER;
    rect_dsc.radius   = 0;

    // Walk by CHARACTER index (lv_label_get_letter_pos is character-, not byte-, indexed):
    // skip UTF-8 continuation bytes so a multi-byte passphrase char can't desync the index.
    uint32_t character_index = 0;
    for (const char *byte = text; *byte; ++byte) {
        if (((unsigned char)*byte & 0xC0) == 0x80) continue;   // UTF-8 continuation byte
        if (*byte == ' ') {
            lv_point_t letter_position;
            lv_label_get_letter_pos(label, character_index, &letter_position);   // top-left of the space's cell
            lv_area_t block_area;
            block_area.x1 = label_coords.x1 + letter_position.x + space_block->inset;
            block_area.x2 = label_coords.x1 + letter_position.x + space_block->cell_width - 1 - space_block->inset;
            block_area.y1 = label_coords.y1 + letter_position.y + space_block->top;
            block_area.y2 = label_coords.y1 + letter_position.y + space_block->bottom - 1;
            if (block_area.x2 >= block_area.x1 && block_area.y2 >= block_area.y1) {
                lv_draw_rect(layer, &rect_dsc, &block_area);
            }
        }
        character_index++;
    }
}

void seed_review_passphrase_space_block_free_cb(lv_event_t *e) {
    lv_free(lv_event_get_user_data(e));
}

}  // namespace


void seed_review_passphrase_screen(void *ctx_json) {
    // --- Config ---

    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    // Required fields: passphrase + the two fingerprints are the security-relevant
    // datums this screen exists to show; changes_fingerprint_label + button_list
    // are user-visible CONTENT, which always arrives localized from the host view
    // layer (a string literal baked here would be English-only by construction).
    // One throw per field, before the scaffold exists, so no throw path can leak
    // LVGL objects.
    if (!cfg.contains("passphrase") || !cfg["passphrase"].is_string()) {
        throw std::runtime_error("seed_review_passphrase_screen: passphrase is required and must be a string");
    }
    if (!cfg.contains("fingerprint_without") || !cfg["fingerprint_without"].is_string()) {
        throw std::runtime_error("seed_review_passphrase_screen: fingerprint_without is required and must be a string");
    }
    if (!cfg.contains("fingerprint_with") || !cfg["fingerprint_with"].is_string()) {
        throw std::runtime_error("seed_review_passphrase_screen: fingerprint_with is required and must be a string");
    }
    if (!cfg.contains("changes_fingerprint_label") || !cfg["changes_fingerprint_label"].is_string()) {
        throw std::runtime_error("seed_review_passphrase_screen: changes_fingerprint_label is required and must be a string");
    }
    if (!cfg.contains("button_list") || !cfg["button_list"].is_array() || cfg["button_list"].empty()) {
        throw std::runtime_error("seed_review_passphrase_screen: button_list is required and must be a non-empty array");
    }
    std::string passphrase                = cfg["passphrase"].get<std::string>();
    std::string fingerprint_without       = cfg["fingerprint_without"].get<std::string>();
    std::string fingerprint_with          = cfg["fingerprint_with"].get<std::string>();
    std::string changes_fingerprint_label = cfg["changes_fingerprint_label"].get<std::string>();

    // Structural defaults (write-if-absent, never user-visible text). Python
    // ButtonListScreen defaults: show_back_button=True, show_power_button=False.
    // The localized title itself is content and must come from the host.
    ensure_top_nav_structure(cfg, /*default_show_back_button=*/true,
                                  /*default_show_power_button=*/false);
    require_top_nav_title(cfg, "seed_review_passphrase_screen");

    cfg["is_bottom_list"] = true;    // forced, not defaulted — Python: is_bottom_list = True

    // --- Scaffold ---

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, /*scrollable=*/false);

    // --- Body ---

    // Both content blocks below are parented to screen.screen (the ROOT), not
    // upper_body: their placement is Python's absolute screen_y coordinate math
    // (the --- Geometry --- pass), and inside the scaffold's flex column an
    // lv_obj_set_pos would be overridden by flex. upper_body stays empty; the
    // scaffold's flex spacer still pins the buttons to the viewport bottom.

    const int32_t display_width = lv_display_get_horizontal_resolution(NULL);

    // 1. Fingerprint-change readout: FINGERPRINT glyph + localized caption over
    //    "<without> >> <with>", centered as a block (positioned by measurement in
    //    the --- Geometry --- pass).
    std::string fingerprint_value = fingerprint_without + " >> " + fingerprint_with;
    icon_text_line_opts_t fingerprint_opts = {};
    fingerprint_opts.icon_glyph   = SeedSignerIconConstants::FINGERPRINT;
    fingerprint_opts.icon_color   = INFO_COLOR;
    fingerprint_opts.label_text   = changes_fingerprint_label.c_str();
    fingerprint_opts.value_text   = fingerprint_value.c_str();
    fingerprint_opts.label_color  = SEEDSIGNER_ICON_COLOR_DEFAULT;   // -> LABEL_FONT_COLOR (gray)
    fingerprint_opts.value_color  = SEEDSIGNER_ICON_COLOR_DEFAULT;   // -> BODY_FONT_COLOR
    fingerprint_opts.is_text_centered = true;
    lv_obj_t *fingerprint_line = icon_text_line(screen.screen, &fingerprint_opts);

    // 2. The passphrase itself: orange, fixed-width, centered, up to 3 balanced
    //    lines. The size is fixed at the baked 24 px monospace — a documented
    //    deviation (see banner): Python auto-fits a font-size range; this port
    //    ships one baked size. If the passphrase has leading/trailing or doubled
    //    spaces (show_spaces), every space is marked with a solid block drawn over
    //    it (seed_review_passphrase_space_block_cb) so they can't hide.
    const lv_font_t *passphrase_font = &KEYBOARD_FONT;      // Inconsolata SemiBold, 24 px @240
    const bool show_spaces =
        (!passphrase.empty() && (passphrase.front() == ' ' || passphrase.back() == ' ')) ||
        (passphrase.find("  ") != std::string::npos);

    int32_t passphrase_char_width = monospace_char_width(passphrase_font);

    int max_chars_per_line = (int)((display_width - 2 * EDGE_PADDING) / passphrase_char_width);
    if (max_chars_per_line < 1) max_chars_per_line = 1;
    std::vector<std::string> lines =
        seed_review_passphrase_wrap(passphrase, max_chars_per_line, 3);  // Python max_lines = 3

    // Join the wrapped lines into one '\n'-separated string. Real spaces are KEPT (they carry
    // the correct monospace width and keep centering right); when show_spaces, a solid block is
    // drawn over each in seed_review_passphrase_space_block_cb. A SINGLE multi-line label (vs
    // one label per line) lets tight_line_space() set a uniform, Python-tight line advance;
    // per-line labels stacked at the font's loose declared line_height left too much air
    // between wrapped lines.
    std::string joined;
    for (const std::string &line : lines) {
        if (!joined.empty()) joined += "\n";
        joined += line;
    }

    // Centered container holding the passphrase block (positioned by measurement in
    // the --- Geometry --- pass).
    lv_obj_t *passphrase_column = lv_obj_create(screen.screen);
    lv_obj_remove_style_all(passphrase_column);
    lv_obj_set_size(passphrase_column, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_layout(passphrase_column, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(passphrase_column, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(passphrase_column, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(passphrase_column, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *passphrase_label = lv_label_create(passphrase_column);
    lv_label_set_text(passphrase_label, joined.c_str());
    lv_obj_set_style_text_font(passphrase_label, passphrase_font, LV_PART_MAIN);
    lv_obj_set_style_text_color(passphrase_label, lv_color_hex(0xFFA500), LV_PART_MAIN);  // PIL "orange" — slated for a named gui_constants color at the consolidation rollout
    lv_obj_set_style_text_align(passphrase_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);    // center each line
    lv_obj_set_style_pad_all(passphrase_label, 0, LV_PART_MAIN);
    // Uniform, Python-tight inter-line advance (ink height + 2) instead of the font's loose
    // declared line_height. Measured across the whole block, so descenders never clip.
    lv_obj_set_style_text_line_space(passphrase_label, tight_line_space(passphrase_font, joined.c_str(), 2),
                                     LV_PART_MAIN);

    // 3. Reveal hidden spaces with a solid block over each space (one cell wide, on
    //    the baseline, in the label's orange). Size it to the TALLEST ink in the
    //    character set — measured over every printable ASCII glyph, since special
    //    characters ({}[]()| etc.) reach higher than capitals or digits — so the
    //    block matches the tallest character, not just a digit. See
    //    seed_review_passphrase_space_block_cb.
    if (show_spaces) {
        int32_t ascent  = passphrase_font->line_height - passphrase_font->base_line;   // baseline offset from line top
        int32_t max_top = 0;                                                           // tallest ink above the baseline
        for (uint32_t codepoint = 0x21; codepoint <= 0x7E; ++codepoint) {
            lv_font_glyph_dsc_t glyph_dsc;
            if (!lv_font_get_glyph_dsc(passphrase_font, &glyph_dsc, codepoint, 0)) continue;
            int32_t top = (int32_t)glyph_dsc.ofs_y + (int32_t)glyph_dsc.box_h;
            if (top > max_top) max_top = top;
        }
        seed_review_passphrase_space_block_t *space_block =
            (seed_review_passphrase_space_block_t *)lv_malloc(sizeof(seed_review_passphrase_space_block_t));
        space_block->cell_width = passphrase_char_width;
        space_block->top        = ascent - max_top;   // tallest ink in the set (special chars included)
        space_block->bottom     = ascent;             // baseline, aligned with the text
        space_block->inset      = passphrase_char_width / 8 > 0 ? passphrase_char_width / 8 : 1;   // ~1/8-cell gap (▉ was 7/8 fill)
        lv_obj_add_event_cb(passphrase_label, seed_review_passphrase_space_block_cb,      LV_EVENT_DRAW_MAIN_END, space_block);
        lv_obj_add_event_cb(passphrase_label, seed_review_passphrase_space_block_free_cb, LV_EVENT_DELETE,        space_block);
    }

    // --- Geometry ---

    // Absolute placement pass (one measure-then-place; ports Python's screen_y
    // math). lv_obj_update_layout settles the scaffold's flex-laid button so both
    // reads below are final:
    //   - Fingerprint anchor: button_top - COMPONENT_PADDING - BODY_FONT_SIZE*2.5
    //     (Python's IconTextLine screen_y), centered horizontally.
    //   - Passphrase centering: the column centers in the band between the top-nav
    //     bottom and the fingerprint line, clamped so it never starts above the band.
    lv_obj_update_layout(screen.screen);
    int32_t button_top = bottom_button_top_y(screen);

    int32_t fingerprint_height = lv_obj_get_height(fingerprint_line);
    int32_t fingerprint_width  = lv_obj_get_width(fingerprint_line);
    int32_t fingerprint_y = button_top - COMPONENT_PADDING - (int32_t)(BODY_FONT_SIZE * 2.5);
    lv_obj_set_pos(fingerprint_line, (display_width - fingerprint_width) / 2, fingerprint_y);

    int32_t passphrase_height = lv_obj_get_height(passphrase_column);
    int32_t passphrase_width  = lv_obj_get_width(passphrase_column);
    int32_t region_top = TOP_NAV_HEIGHT;
    int32_t passphrase_y = region_top + (fingerprint_y - region_top - passphrase_height) / 2;
    if (passphrase_y < region_top) passphrase_y = region_top;
    lv_obj_set_pos(passphrase_column, (display_width - passphrase_width) / 2, passphrase_y);

    // --- Navigation + load ---

    // Menu-style default index: an action list always has a selection, so the first
    // button starts focused (the host may override via cfg initial_selected_index).
    bind_screen_navigation(cfg, screen, /*default_initial_index=*/0);

    load_screen_and_cleanup_previous(screen.screen);
}
