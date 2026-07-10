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



// ============================ PSBT detail screens ============================
// The transaction-review leaf screens: the per-recipient / change-address readouts
// and the fee "math". Each is a bottom-pinned ButtonListScreen (Python parity) whose
// body composes the shared btc_amount + formatted_address components. The host formats
// every value (denomination, digit grouping, address derivation); these screens only
// lay the pieces out — the same host-formats / C-renders split as btc_amount, so the
// two platforms can never disagree on how a number rounds or an address truncates.










// Wrap a passphrase into up to `max_lines` lines of at most `max_cpl` monospace chars.
// Two goals beyond Python's naive fixed-width char split:
//   1. Break right AFTER a special (non-alphanumeric) character — dash, punctuation,
//      space — when one sits near the balanced break point, so a human-readable passphrase
//      wraps at its natural separators ("correct-horse-" / ...) instead of mid-word.
//   2. Balance the line lengths (each break targets k*len/num_lines), like the body-text
//      balanced wrap.
// A random or all-alphabetic passphrase has no special-char breakpoints, so it falls back
// to a balanced hard split by length. EVERY character is preserved — the break character
// stays at the end of its line — since the whole point is to show the exact passphrase.
static std::vector<std::string> wrap_passphrase(const std::string& p, int max_cpl, int max_lines) {
    const int len = (int)p.size();
    if (max_cpl < 1 || len <= max_cpl) return { p };

    int num_lines = (len + max_cpl - 1) / max_cpl;   // ceil: fewest lines that can fit
    if (num_lines > max_lines) num_lines = max_lines;
    if (num_lines < 1) num_lines = 1;

    auto is_special = [](char c) {
        return !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'));
    };
    auto dist = [](int a, int b) { int d = a - b; return d < 0 ? -d : d; };

    std::vector<int> cuts;   // start index of each new line
    int prev = 0;
    for (int k = 1; k < num_lines; ++k) {
        const int ideal = (int)((long)len * k / num_lines);   // balanced target cut
        int lo = prev + 1;                                    // ≥1 char on this line
        int hi = prev + max_cpl;                              // this line ≤ max_cpl
        // Leave room: the remaining text must fit the remaining lines, ≥1 char each.
        const int rem_lines = num_lines - k;
        if (hi > len - rem_lines)            hi = len - rem_lines;
        if (lo < len - rem_lines * max_cpl)  lo = len - rem_lines * max_cpl;
        if (lo < prev + 1)                   lo = prev + 1;
        if (hi < lo)                         hi = lo;

        // Prefer a break just after a special char, closest to `ideal`, within [lo, hi].
        int best = -1;
        for (int pos = lo; pos <= hi && pos < len; ++pos) {
            if (is_special(p[pos - 1]) && (best < 0 || dist(pos, ideal) < dist(best, ideal))) {
                best = pos;
            }
        }
        int cut = (best >= 0) ? best : std::min(std::max(ideal, lo), hi);   // else balanced hard cut
        cuts.push_back(cut);
        prev = cut;
    }

    std::vector<std::string> lines;
    int start = 0;
    for (int c : cuts) { lines.push_back(p.substr((size_t)start, (size_t)(c - start))); start = c; }
    lines.push_back(p.substr((size_t)start));
    return lines;
}


// --- review-passphrase space reveal ----------------------------------------
// When a passphrase hides leading/trailing/doubled spaces, every space is marked with a
// SOLID block drawn over it — not a substituted font glyph. Two glyph approaches were
// rejected: the ▉ block (U+2589) inflated the baked monospace line_height, regressing every
// layout that reads it (e.g. psbt_math row spacing); and a hollow open-box / .notdef box
// reads as a "tofu" rendering-error to anyone used to non-Latin glyph fallbacks. A custom
// solid block is digit-height, one monospace cell wide, sits on the baseline, and takes the
// label's text color: the marker we wanted, at correct proportions, with no font-metric
// side effects and no error-glyph ambiguity.
struct pp_space_block_t {
    int32_t cell_w;   // monospace cell width (space advance)
    int32_t top;      // px from a line's top down to the block top (digit cap)
    int32_t bottom;   // px from a line's top down to the block bottom (baseline)
    int32_t inset;    // horizontal inset per side (small gap between adjacent blocks)
};

static void passphrase_space_block_cb(lv_event_t *e) {
    lv_obj_t         *lbl   = lv_event_get_target_obj(e);
    lv_layer_t       *layer = lv_event_get_layer(e);
    pp_space_block_t *sb    = (pp_space_block_t *)lv_event_get_user_data(e);
    if (!lbl || !layer || !sb) return;

    const char *txt = lv_label_get_text(lbl);
    if (!txt) return;

    lv_area_t lc;
    lv_obj_get_coords(lbl, &lc);   // absolute label box; letter positions are label-relative

    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_obj_get_style_text_color(lbl, LV_PART_MAIN);  // same orange as the text
    d.bg_opa   = LV_OPA_COVER;
    d.radius   = 0;

    // Walk by CHARACTER index (lv_label_get_letter_pos is character-, not byte-, indexed):
    // skip UTF-8 continuation bytes so a multi-byte passphrase char can't desync the index.
    uint32_t char_id = 0;
    for (const char *p = txt; *p; ++p) {
        if (((unsigned char)*p & 0xC0) == 0x80) continue;   // UTF-8 continuation byte
        if (*p == ' ') {
            lv_point_t pos;
            lv_label_get_letter_pos(lbl, char_id, &pos);    // top-left of the space's cell
            lv_area_t a;
            a.x1 = lc.x1 + pos.x + sb->inset;
            a.x2 = lc.x1 + pos.x + sb->cell_w - 1 - sb->inset;
            a.y1 = lc.y1 + pos.y + sb->top;
            a.y2 = lc.y1 + pos.y + sb->bottom - 1;
            if (a.x2 >= a.x1 && a.y2 >= a.y1) lv_draw_rect(layer, &d, &a);
        }
        char_id++;
    }
}

static void passphrase_space_block_free_cb(lv_event_t *e) {
    lv_free(lv_event_get_user_data(e));
}


// SeedReviewPassphraseScreen: shows the entered BIP-39 passphrase (orange, fixed-width,
// centered, up to 3 lines) above a fingerprint IconTextLine that spells out how the
// passphrase CHANGES the seed's fingerprint (without >> with). No warning edges.
//
// cfg:
//   top_nav.title                 — default "Verify Passphrase".
//   passphrase (str, req.)        — the passphrase to review.
//   fingerprint_without (str,req) — fingerprint before the passphrase.
//   fingerprint_with (str, req.)  — fingerprint after the passphrase.
//   changes_fingerprint_label(str)— host-localized "changes fingerprint" label.
//   button_list (array)           — default ["Done"].
void seed_review_passphrase_screen(void *ctx_json) {
    const char *json_str = (const char *)ctx_json;
    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    if (!cfg.contains("passphrase") || !cfg["passphrase"].is_string()) {
        throw std::runtime_error("seed_review_passphrase_screen requires a \"passphrase\" string");
    }
    std::string passphrase = cfg["passphrase"].get<std::string>();
    std::string fp_without = cfg.value("fingerprint_without", std::string(""));
    std::string fp_with    = cfg.value("fingerprint_with",    std::string(""));
    std::string changes_label = cfg.value("changes_fingerprint_label",
                                          std::string("changes fingerprint"));

    if (!cfg.contains("top_nav") || !cfg["top_nav"].is_object()) cfg["top_nav"] = json::object();
    if (!cfg["top_nav"].contains("title")) cfg["top_nav"]["title"] = "Verify Passphrase";
    cfg["is_bottom_list"] = true;
    if (!cfg.contains("button_list")) cfg["button_list"] = json::array({ "Done" });

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, false);

    const int32_t W = lv_display_get_horizontal_resolution(NULL);

    // The fingerprint readout: FINGERPRINT glyph + "changes fingerprint" over
    // "<without> >> <with>", centered as a block (positioned by measurement below).
    std::string fp_value = fp_without + " >> " + fp_with;
    icon_text_line_opts_t fo = {};
    fo.icon_glyph   = SeedSignerIconConstants::FINGERPRINT;
    fo.icon_color   = INFO_COLOR;
    fo.label_text   = changes_label.c_str();
    fo.value_text   = fp_value.c_str();
    fo.label_color  = SEEDSIGNER_ICON_COLOR_DEFAULT;
    fo.value_color  = SEEDSIGNER_ICON_COLOR_DEFAULT;
    fo.is_text_centered = true;
    lv_obj_t *fp_line = icon_text_line(screen.screen, &fo);

    // The passphrase itself: orange, fixed-width, centered, up to 3 balanced lines. The
    // user fixed the size at the baked 24 px monospace (Python auto-fits a range; we don't).
    // If the passphrase has leading/trailing or doubled spaces (show_spaces), every space is
    // marked with a solid block drawn over it (passphrase_space_block_cb) so they can't hide.
    const lv_font_t *pp_font = &KEYBOARD_FONT;      // Inconsolata SemiBold, 24 px @240
    const bool show_spaces =
        (!passphrase.empty() && (passphrase.front() == ' ' || passphrase.back() == ' ')) ||
        (passphrase.find("  ") != std::string::npos);

    lv_point_t ppsz;
    lv_text_get_size(&ppsz, "0000000000", pp_font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int32_t pp_char_w = ppsz.x / 10;
    if (pp_char_w < 1) pp_char_w = 1;

    int max_cpl = (int)((W - 2 * EDGE_PADDING) / pp_char_w);
    if (max_cpl < 1) max_cpl = 1;
    std::vector<std::string> lines = wrap_passphrase(passphrase, max_cpl, 3);  // Python max_lines = 3

    // Join the wrapped lines into one '\n'-separated string. Real spaces are KEPT (they carry
    // the correct monospace width and keep centering right); when show_spaces, a solid block is
    // drawn over each in passphrase_space_block_cb. A SINGLE multi-line label (vs one label per
    // line) lets tight_line_space() set a uniform, Python-tight line advance; per-line labels
    // stacked at the font's loose declared line_height left too much air between wrapped lines.
    std::string joined;
    for (const std::string &line : lines) {
        if (!joined.empty()) joined += "\n";
        joined += line;
    }

    // Centered container holding the passphrase block (positioned by measurement below).
    lv_obj_t *pp_col = lv_obj_create(screen.screen);
    lv_obj_remove_style_all(pp_col);
    lv_obj_set_size(pp_col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_layout(pp_col, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(pp_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(pp_col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(pp_col, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *pp_lbl = lv_label_create(pp_col);
    lv_label_set_text(pp_lbl, joined.c_str());
    lv_obj_set_style_text_font(pp_lbl, pp_font, LV_PART_MAIN);
    lv_obj_set_style_text_color(pp_lbl, lv_color_hex(0xFFA500), LV_PART_MAIN);  // PIL "orange"
    lv_obj_set_style_text_align(pp_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);    // center each line
    lv_obj_set_style_pad_all(pp_lbl, 0, LV_PART_MAIN);
    // Uniform, Python-tight inter-line advance (ink height + 2) instead of the font's loose
    // declared line_height. Measured across the whole block, so descenders never clip.
    lv_obj_set_style_text_line_space(pp_lbl, tight_line_space(pp_font, joined.c_str(), 2),
                                     LV_PART_MAIN);

    // Reveal hidden spaces with a solid block over each space (one cell wide, on the baseline,
    // in the label's orange). Size it to the TALLEST ink in the character set — measured over
    // every printable glyph, since special characters ({}[]()| etc.) reach higher than capitals
    // or digits — so the block matches the tallest character, not just a digit. See
    // passphrase_space_block_cb.
    if (show_spaces) {
        int32_t ascent  = pp_font->line_height - pp_font->base_line;   // baseline offset from line top
        int32_t max_top = 0;                                            // tallest ink above the baseline
        for (uint32_t cp = 0x21; cp <= 0x7E; ++cp) {
            lv_font_glyph_dsc_t g;
            if (!lv_font_get_glyph_dsc(pp_font, &g, cp, 0)) continue;
            int32_t top = (int32_t)g.ofs_y + (int32_t)g.box_h;
            if (top > max_top) max_top = top;
        }
        pp_space_block_t *sb = (pp_space_block_t *)lv_malloc(sizeof(pp_space_block_t));
        sb->cell_w = pp_char_w;
        sb->top    = ascent - max_top;   // tallest ink in the set (special chars included)
        sb->bottom = ascent;             // baseline, aligned with the text
        sb->inset  = pp_char_w / 8 > 0 ? pp_char_w / 8 : 1;   // ~1/8-cell gap (▉ was 7/8 fill)
        lv_obj_add_event_cb(pp_lbl, passphrase_space_block_cb,      LV_EVENT_DRAW_MAIN_END, sb);
        lv_obj_add_event_cb(pp_lbl, passphrase_space_block_free_cb, LV_EVENT_DELETE,        sb);
    }

    bind_screen_navigation(cfg, screen, 0);

    // Position both blocks now that sizes are known. The fingerprint line sits just above
    // the button (Python: button_top - COMPONENT_PADDING - body_font_size*2.5); the
    // passphrase centers in the gap between the top nav and the fingerprint line.
    lv_obj_update_layout(screen.screen);
    int32_t button_top = lv_display_get_vertical_resolution(NULL) - BUTTON_HEIGHT;
    if (screen.button_list_count > 0 && lv_obj_is_valid(screen.button_list[0])) {
        lv_area_t ba; lv_obj_get_coords(screen.button_list[0], &ba);
        button_top = ba.y1;
    }

    int32_t fp_h = lv_obj_get_height(fp_line);
    int32_t fp_w = lv_obj_get_width(fp_line);
    int32_t fp_y = button_top - COMPONENT_PADDING - (int32_t)(BODY_FONT_SIZE * 2.5);
    lv_obj_set_pos(fp_line, (W - fp_w) / 2, fp_y);

    int32_t pp_h = lv_obj_get_height(pp_col);
    int32_t pp_w = lv_obj_get_width(pp_col);
    int32_t region_top = TOP_NAV_HEIGHT;
    int32_t pp_y = region_top + (fp_y - region_top - pp_h) / 2;
    if (pp_y < region_top) pp_y = region_top;
    lv_obj_set_pos(pp_col, (W - pp_w) / 2, pp_y);

    load_screen_and_cleanup_previous(screen.screen);
}
