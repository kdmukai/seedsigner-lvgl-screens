// ---------------------------------------------------------------------------
// tools_calc_final_word_done_screen
// ---------------------------------------------------------------------------
//
// The final step of the "Calc final word" tool (the LVGL port of Python's
// ToolsCalcFinalWordDoneScreen, tools_screens.py). Structurally it is a
// bottom-pinned ButtonListScreen (Load seed / Discard) with a back button;
// above the buttons it shows two elements:
//
//   1. the freshly-derived FINAL WORD, large and centered, wrapped in quotes
//      (Python: TextArea(text=f'"{final_word}"', font_size=26, centered)).
//   2. a centered fingerprint readout — the master-fingerprint hex under a
//      small "fingerprint" label beside a blue fingerprint icon (Python:
//      IconTextLine(icon=FINGERPRINT, icon_color=INFO_COLOR, label="fingerprint",
//      value=fingerprint, is_text_centered=True)).
//
// The title is the ordinal word label — "12th Word" for a 12-word mnemonic,
// "24th Word" for a 24-word mnemonic (Python derives this from
// mnemonic_word_length in __post_init__). The View supplies the localized string
// via top_nav.title; absent that we derive the English default from
// mnemonic_word_length so a bare cfg still renders.
//
// Layout note: unlike seed_finalize_screen (which vertically CENTERS its single
// readout in the gap), this screen is TOP-anchored — the word sits a
// COMPONENT_PADDING below the top-nav and the fingerprint line 3*COMPONENT_PADDING
// below the word (Python's absolute screen_y math), with the scaffold's bottom
// spacer pushing the buttons to the viewport bottom.
//
// cfg:
//   top_nav: { title, show_power_button }. show_back_button defaults true
//            (Python ButtonListScreen default; the reference shows the back arrow).
//   final_word (string, required): the derived final mnemonic word (unquoted;
//            this screen adds the surrounding quotes, matching Python).
//   fingerprint (string, required): the master-fingerprint hex to display.
//   fingerprint_label (string, optional): the small label above the value;
//            defaults to "fingerprint" (localized upstream by the View / scenario).
//   mnemonic_word_length (int, optional, default 12): drives the default title
//            ordinal when top_nav.title is not supplied.
//   button_list (array): the action buttons; defaults to Load seed / Discard,
//            with Discard's label in red (Python DISCARD button_label_color="red").

#include "screen_scaffold.h"  // parse_screen_json_ctx / create_top_nav_screen_scaffold / bind_screen_navigation / load_screen_and_cleanup_previous
#include "seedsigner.h"       // tools_calc_final_word_done_screen decl, text_top_leading
#include "components.h"       // icon_text_line, SEEDSIGNER_ICON_COLOR_DEFAULT
#include "gui_constants.h"    // COMPONENT_PADDING, BODY_FONT_COLOR, INFO_COLOR, seedsigner_latin_font, SeedSignerIconConstants

#include "lvgl.h"

#include <stdexcept>
#include <string>


void tools_calc_final_word_done_screen(void *ctx_json) {
    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    // final_word + fingerprint are required (this is a post-derivation results screen).
    if (!cfg.contains("final_word") || !cfg["final_word"].is_string()) {
        throw std::runtime_error("tools_calc_final_word_done_screen requires a \"final_word\" string");
    }
    if (!cfg.contains("fingerprint") || !cfg["fingerprint"].is_string()) {
        throw std::runtime_error("tools_calc_final_word_done_screen requires a \"fingerprint\" string");
    }
    std::string final_word        = cfg["final_word"].get<std::string>();
    std::string fingerprint       = cfg["fingerprint"].get<std::string>();
    std::string fingerprint_label = cfg.value("fingerprint_label", std::string("fingerprint"));

    // Force the ToolsCalcFinalWordDoneScreen shape onto the scaffold cfg: a titled,
    // back-button-bearing, bottom-pinned button list.
    if (!cfg.contains("top_nav") || !cfg["top_nav"].is_object()) cfg["top_nav"] = json::object();
    if (!cfg["top_nav"].contains("title")) {
        // Python __post_init__ derives the title from mnemonic_word_length. Default 12.
        int word_length = cfg.value("mnemonic_word_length", 12);
        cfg["top_nav"]["title"] = (word_length == 12) ? "12th Word" : "24th Word";
    }
    cfg["is_bottom_list"] = true;    // Python: is_bottom_list = True
    if (!cfg.contains("button_list")) {
        // Python: [LOAD ("Load seed"), DISCARD ("Discard", label_color=red)].
        cfg["button_list"] = json::array({
            json("Load seed"),
            json{ {"label", "Discard"}, {"label_color", "#ff0000"} },
        });
    }

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, false);

    // The bottom-list body is a flex column [upper_body][spacer grow=1][buttons].
    // Leave upper_body ungrown (top-anchored) so the word + fingerprint sit just
    // below the top-nav (Python's absolute screen_y anchoring) while the spacer
    // pins the buttons to the bottom. Zero the flex row-gap so the inter-element
    // spacing is ONLY the explicit margins set below.
    lv_obj_set_style_pad_row(screen.upper_body, 0, LV_PART_MAIN);

    // 1. The final word — large, centered, quote-wrapped. Python uses the default
    //    (OpenSans) body font at size 26; a BIP-39 word is always Latin, so the
    //    Latin face is faithful and covers it. Anchored a COMPONENT_PADDING below
    //    the top-nav (Python screen_y = top_nav.height + COMPONENT_PADDING),
    //    reclaiming the font's top leading so the visible caps land where PIL puts
    //    them rather than a leading-height lower.
    const lv_font_t *word_font = seedsigner_latin_font(26);
    std::string quoted_word = "\"" + final_word + "\"";

    lv_obj_t *word_label = lv_label_create(screen.upper_body);
    lv_label_set_text(word_label, quoted_word.c_str());
    lv_obj_set_style_text_font(word_label, word_font, LV_PART_MAIN);
    lv_obj_set_style_text_color(word_label, lv_color_hex(BODY_FONT_COLOR), LV_PART_MAIN);
    lv_obj_set_style_text_align(word_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(word_label, 0, LV_PART_MAIN);
    int32_t word_top_gap = COMPONENT_PADDING - text_top_leading(word_font, quoted_word.c_str());
    if (word_top_gap < 0) word_top_gap = 0;
    lv_obj_set_style_margin_top(word_label, word_top_gap, LV_PART_MAIN);

    // 2. Fingerprint readout via the shared IconTextLine component (the SAME widget
    //    seed_finalize / xpub-details use). Python here uses the IconTextLine
    //    DEFAULTS — icon_size = ICON_FONT_SIZE (24 px), value/label at the body font
    //    size — so we leave icon_font/value_font/label_font unset and only color the
    //    icon blue. upper_body's cross-axis centering centers the whole block.
    //    Python spaces it 3*COMPONENT_PADDING below the word.
    icon_text_line_opts_t fp = {};
    fp.icon_glyph       = SeedSignerIconConstants::FINGERPRINT;
    fp.icon_color       = INFO_COLOR;
    fp.label_text       = fingerprint_label.c_str();
    fp.value_text       = fingerprint.c_str();
    fp.label_color      = SEEDSIGNER_ICON_COLOR_DEFAULT;   // -> LABEL_FONT_COLOR (gray)
    fp.value_color      = SEEDSIGNER_ICON_COLOR_DEFAULT;   // -> BODY_FONT_COLOR
    fp.is_text_centered = true;
    lv_obj_t *fp_row = icon_text_line(screen.upper_body, &fp);
    if (fp_row) {
        // Python's 3*COMPONENT_PADDING is measured from the word TextArea's tight box
        // bottom to the IconTextLine's box top. LVGL's word label box is a full
        // line-height tall (overshooting the visible glyph bottom), and the fp block's
        // top line (the "fingerprint" label) carries its own leading above the caps.
        // Reclaim both so the VISIBLE gap between the word and the readout matches PIL.
        int32_t fp_gap = 3 * COMPONENT_PADDING
                         - text_top_leading(word_font, quoted_word.c_str())
                         - text_top_leading(&BODY_FONT, fingerprint_label.c_str());
        if (fp_gap < 0) fp_gap = 0;
        lv_obj_set_style_margin_top(fp_row, fp_gap, LV_PART_MAIN);
    }

    bind_screen_navigation(
        cfg,
        screen,
        screen.button_list_count > 0 ? screen.button_list : NULL,
        screen.button_list_count,
        NAV_BODY_VERTICAL,
        0   // default the first action button (Load seed) selected, like button_list_screen
    );

    load_screen_and_cleanup_previous(screen.screen);
}
