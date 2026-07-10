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



// ===========================================================================
// seed_mnemonic_entry_screen
// ---------------------------------------------------------------------------
//
// BIP-39 seed-word entry (the LVGL port of Python's SeedMnemonicEntryScreen,
// seed_screens.py). A 5x6 a-z + DEL `lv_buttonmatrix` drives a live prefix-match
// state machine against a cfg-passed `wordlist`: as letters are entered the
// keys that cannot continue any word are dimmed (LV_BUTTONMATRIX_CTRL_DISABLED)
// and a candidate-word panel updates. The matching logic (calc_possible_words /
// calc_possible_alphabet + the trailing-space "live slot" trick) is inherently
// per-keystroke, so it must run native here — the View cannot intervene mid-word.
//
// Two candidate panels, by input mode:
//   - hardware/240 (Python parity): a right column with a fixed highlight slot at
//     vertical center, dimmed candidates above/below, and KEY1/KEY3 scroll +
//     KEY2 accept (the physical Pi Zero buttons), via kb_side_button.
//   - touch (deliberate divergence, UX-review pending): a scrollable candidate
//     list (tap a word to highlight it in place) + a persistent green CHECK
//     button beneath, disabled until a selection exists, tap = accept. No fixed
//     slot, no modal — mirrors the passphrase confirm idiom.
//
// Returns the chosen word via seedsigner_lvgl_on_text_entered() (the same host
// hook the other keyboard screens use); BACK returns RET_CODE__BACK_BUTTON via
// the scaffold's back button. The View loops the screen 12/24x per seed.
//
// cfg:
//   top_nav: { title (e.g. "Seed Word #3"), show_back_button, show_power_button }
//   initial_letters (string | array of single-char strings): letters already
//                   entered for this word (empty/absent = fresh)
//   wordlist (array of strings): the selected language's BIP-39 words (English =
//                   2048). Required; the View owns language selection.
//   initial_selected_word (string, optional): pre-select this candidate at load
//                   (touch: highlight it + enable CHECK; hardware: move the slot to
//                   it). Mainly for static screenshots — runtime selection is a tap/
//                   scroll. No-op if the word isn't a current match.

// Per-screen state. C++ containers, so new/delete (freed in the cleanup cb). The
// `letters` buffer mirrors Python's self.letters: the locked-in letters plus a
// trailing "live slot" (a single " " or the currently-floated letter) so the
// joystick can preview a letter before it is locked in with a click.
struct mnemonic_ctx_t {
    lv_obj_t   *matrix = nullptr;
    lv_obj_t   *ta = nullptr;
    lv_obj_t   *back_btn = nullptr;
    lv_obj_t   *top_nav = nullptr;
    lv_group_t *group = nullptr;
    bool        hardware = false;

    // Keyboard map (persistent for the buttonmatrix lifetime).
    std::vector<std::string>            key_storage;
    std::vector<const char *>           map;
    std::vector<lv_buttonmatrix_ctrl_t> ctrl;

    // Matching state.
    std::vector<std::string> wordlist;
    std::vector<std::string> letters;          // locked letters + trailing live slot
    std::vector<int>         possible_words;    // indices into wordlist
    std::string              possible_alphabet; // distinct allowed next letters
    int                      selected_index = 0;

    // Hardware candidate panel.
    lv_obj_t              *hl_btn = nullptr;    // center highlight slot
    lv_obj_t              *hl_label = nullptr;
    lv_obj_t              *arrow_up = nullptr;
    lv_obj_t              *arrow_down = nullptr;
    std::vector<lv_obj_t *> dim_above;          // nearest-first (sel-1, sel-2, ...)
    std::vector<lv_obj_t *> dim_below;          // nearest-first (sel+1, sel+2, ...)

    // Touch candidate panel.
    lv_obj_t *cand_list = nullptr;              // scrollable list of candidates
    lv_obj_t *check_btn = nullptr;              // persistent green accept
    int       touch_selected = -1;              // index into possible_words
};

// --- matching state machine (ports calc_possible_words / calc_possible_alphabet)

// The current search prefix: the letters buffer joined and right-trimmed of the
// trailing live slot (Python's "".join(self.letters).strip()).
static std::string mnemonic_prefix(const mnemonic_ctx_t *c) {
    std::string s;
    for (const std::string &ch : c->letters) s += ch;
    size_t end = s.find_last_not_of(' ');
    return (end == std::string::npos) ? std::string() : s.substr(0, end + 1);
}

// The candidate-word panel stays hidden until the prefix reaches this many
// letters. A single letter matches up to ~250 BIP-39 words; on a slower display
// (notably the ESP32-P4) rebuilding that many candidate widgets on the very first
// keystroke stalls the UI for a visible beat — and a 1-letter candidate list has
// little practical value anyway (you can't meaningfully pick a word yet). So the
// panel, and the accept affordance that reads from it, wait for a second letter to
// narrow the field. NOTE: possible_words is still computed at every stage — the
// keyboard's next-letter dimming (possible_alphabet) is derived from it — so only
// the on-screen panel is deferred, not the matching itself.
static const size_t MNEMONIC_MATCH_MIN_LETTERS = 2;

static bool mnemonic_matches_shown(const mnemonic_ctx_t *c) {
    return mnemonic_prefix(c).size() >= MNEMONIC_MATCH_MIN_LETTERS;
}

// possible_words = every wordlist entry starting with the current prefix (reset
// the highlight to the top of the new list).
static void mnemonic_calc_possible_words(mnemonic_ctx_t *c) {
    std::string prefix = mnemonic_prefix(c);
    c->possible_words.clear();
    if (!prefix.empty()) {
        for (int i = 0; i < (int)c->wordlist.size(); ++i) {
            if (c->wordlist[i].rfind(prefix, 0) == 0) c->possible_words.push_back(i);
        }
    }
    c->selected_index = 0;
}

// possible_alphabet = the distinct set of letters that can follow the locked
// prefix (the (len-1)-th char of every possible word), in first-seen order.
// Recomputed only when a letter is locked in or deleted (not while floating), so
// the joystick can freely float between active letters. With <=1 letter the
// whole alphabet is active and there are no matches yet (Python parity).
static void mnemonic_calc_possible_alphabet(mnemonic_ctx_t *c) {
    if (c->letters.size() > 1) {
        size_t letter_num = c->letters.size() - 1;   // == len(search_letters)
        mnemonic_calc_possible_words(c);
        std::string alpha;
        for (int wi : c->possible_words) {
            const std::string &w = c->wordlist[wi];
            if (w.size() >= letter_num + 1) {
                char nx = w[letter_num];
                if (alpha.find(nx) == std::string::npos) alpha += nx;
            }
        }
        c->possible_alphabet = alpha;
    } else {
        c->possible_alphabet = "abcdefghijklmnopqrstuvwxyz";
        c->possible_words.clear();
        c->selected_index = 0;
    }
}

// --- view sync helpers ------------------------------------------------------

static void mnemonic_set_hidden(lv_obj_t *o, bool hidden) {
    if (!o) return;
    if (hidden) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else        lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
}

// Re-dim the keyboard for the current possible_alphabet. Dimming is purely
// VISUAL (mnemonic_dim_draw_cb recolors letter keys not in possible_alphabet) —
// NOT LV_BUTTONMATRIX_CTRL_DISABLED, because disabled keys are SKIPPED by the
// joystick navigation, which strands the cursor when most keys are inactive (you
// could only move along the few active keys). Python lets the cursor float freely
// over dimmed keys (they just don't register a press); keeping every key
// navigable here matches that. So this just forces a redraw.
static void mnemonic_apply_dimming(mnemonic_ctx_t *c) {
    if (c->matrix && lv_obj_is_valid(c->matrix)) lv_obj_invalidate(c->matrix);
}

// Visual key dimming (LV_EVENT_DRAW_TASK_ADDED): recolor every letter key that
// can't continue any word (not in possible_alphabet) to a recessed dark fill +
// faint text — INCLUDING the selected one. Python's Key.render dims the selected
// inactive key too, marking it only with a highlight OUTLINE (no solid fill); the
// orange border for that comes from the FOCUS/PRESSED border style added to the
// matrix, which this leaves untouched (only fill + label are recolored). Letters
// are plain ASCII, so this never collides with kb_icon_draw_cb (the DEL glyph).
static void mnemonic_dim_draw_cb(lv_event_t *e) {
    lv_draw_task_t *task = lv_event_get_draw_task(e);
    lv_draw_dsc_base_t *base = (lv_draw_dsc_base_t *)lv_draw_task_get_draw_dsc(task);
    if (!base || base->part != LV_PART_ITEMS) return;

    mnemonic_ctx_t *c = (mnemonic_ctx_t *)lv_event_get_user_data(e);
    if (!c) return;
    lv_obj_t *kb = lv_event_get_target_obj(e);
    const char *txt = lv_buttonmatrix_get_button_text(kb, base->id1);
    if (!txt || std::strlen(txt) != 1 || txt[0] < 'a' || txt[0] > 'z') return;  // letters only
    if (c->possible_alphabet.find(txt[0]) != std::string::npos) return;          // active key

    lv_draw_label_dsc_t *ld = lv_draw_task_get_label_dsc(task);
    if (ld) { ld->color = lv_color_hex(0x555555); return; }   // gray text (Python "#333"-ish)
    lv_draw_fill_dsc_t *fd = lv_draw_task_get_fill_dsc(task);
    if (fd) fd->color = lv_color_hex(0x141414);                // recessed dark fill
}

static void mnemonic_update_display(mnemonic_ctx_t *c) {
    if (!c->ta || !lv_obj_is_valid(c->ta)) return;
    std::string p = mnemonic_prefix(c);
    lv_textarea_set_text(c->ta, p.c_str());
    lv_textarea_set_cursor_pos(c->ta, LV_TEXTAREA_CURSOR_LAST);
}

// Selected/normal styling for a touch candidate button (selected = SeedSigner
// orange, like the keyboard's selected key).
static void mnemonic_style_candidate(lv_obj_t *btn, bool selected) {
    lv_obj_set_style_bg_color(btn,
        lv_color_hex(selected ? ACCENT_COLOR : BUTTON_BACKGROUND_COLOR), LV_PART_MAIN);
    lv_obj_t *lbl = lv_obj_get_child(btn, 0);
    if (lbl) lv_obj_set_style_text_color(lbl,
        lv_color_hex(selected ? BUTTON_SELECTED_FONT_COLOR : BUTTON_FONT_COLOR), LV_PART_MAIN);
}

// Enable/disable the touch accept button (green + clickable once a candidate is
// selected; muted + inert until then).
static void mnemonic_set_check_enabled(mnemonic_ctx_t *c, bool en) {
    if (!c->check_btn || !lv_obj_is_valid(c->check_btn)) return;
    if (en) {
        lv_obj_add_flag(c->check_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(c->check_btn, lv_color_hex(SUCCESS_COLOR), LV_PART_MAIN);
        lv_obj_set_style_text_color(c->check_btn, lv_color_hex(BUTTON_SELECTED_FONT_COLOR), LV_PART_MAIN);
    } else {
        lv_obj_remove_flag(c->check_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(c->check_btn, lv_color_hex(BUTTON_BACKGROUND_COLOR), LV_PART_MAIN);
        lv_obj_set_style_text_color(c->check_btn, lv_color_hex(0x666666), LV_PART_MAIN);
    }
}

static void mnemonic_touch_candidate_cb(lv_event_t *e);  // fwd

// Hardware: refresh the fixed-slot highlight + the dimmed rows above/below it
// from possible_words[selected_index]. Hidden entirely when there are no matches.
static void mnemonic_render_matches_hw(mnemonic_ctx_t *c) {
    bool any = mnemonic_matches_shown(c) && !c->possible_words.empty();
    mnemonic_set_hidden(c->hl_btn, !any);
    mnemonic_set_hidden(c->arrow_up, !any);
    mnemonic_set_hidden(c->arrow_down, !any);
    if (!any) {
        for (lv_obj_t *l : c->dim_above) mnemonic_set_hidden(l, true);
        for (lv_obj_t *l : c->dim_below) mnemonic_set_hidden(l, true);
        return;
    }
    int count = (int)c->possible_words.size();
    int sel = c->selected_index;
    if (c->hl_label) lv_label_set_text(c->hl_label, c->wordlist[c->possible_words[sel]].c_str());
    for (int k = 0; k < (int)c->dim_above.size(); ++k) {
        int idx = sel - 1 - k;
        if (idx >= 0) { lv_label_set_text(c->dim_above[k], c->wordlist[c->possible_words[idx]].c_str()); mnemonic_set_hidden(c->dim_above[k], false); }
        else          { mnemonic_set_hidden(c->dim_above[k], true); }
    }
    for (int k = 0; k < (int)c->dim_below.size(); ++k) {
        int idx = sel + 1 + k;
        if (idx < count) { lv_label_set_text(c->dim_below[k], c->wordlist[c->possible_words[idx]].c_str()); mnemonic_set_hidden(c->dim_below[k], false); }
        else             { mnemonic_set_hidden(c->dim_below[k], true); }
    }
}

// Touch: rebuild the scrollable candidate list (one tappable button per match),
// clearing any prior selection and disabling the accept button.
static void mnemonic_render_matches_touch(mnemonic_ctx_t *c) {
    if (!c->cand_list || !lv_obj_is_valid(c->cand_list)) return;
    lv_obj_clean(c->cand_list);
    c->touch_selected = -1;
    mnemonic_set_check_enabled(c, false);

    // Deferred until the prefix is long enough (see mnemonic_matches_shown): with a
    // single letter this would build hundreds of candidate rows, stalling the P4.
    if (!mnemonic_matches_shown(c)) return;

    const int32_t row_h = (int32_t)(BUTTON_HEIGHT * 3 / 4);
    for (int k = 0; k < (int)c->possible_words.size(); ++k) {
        lv_obj_t *b = lv_obj_create(c->cand_list);
        lv_obj_set_width(b, lv_pct(100));
        lv_obj_set_height(b, row_h);
        lv_obj_set_style_radius(b, BUTTON_RADIUS / 2, LV_PART_MAIN);
        lv_obj_set_style_border_width(b, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(b, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_left(b, COMPONENT_PADDING, LV_PART_MAIN);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_user_data(b, (void *)(intptr_t)k);
        lv_obj_add_event_cb(b, mnemonic_touch_candidate_cb, LV_EVENT_CLICKED, c);

        lv_obj_t *lbl = lv_label_create(b);
        lv_label_set_text(lbl, c->wordlist[c->possible_words[k]].c_str());
        lv_obj_set_style_text_font(lbl, &CANDIDATE_FONT, LV_PART_MAIN);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

        mnemonic_style_candidate(b, false);
    }
}

static void mnemonic_render_matches(mnemonic_ctx_t *c) {
    if (c->hardware) mnemonic_render_matches_hw(c);
    else             mnemonic_render_matches_touch(c);
}

// --- state transitions ------------------------------------------------------

// Lock in `ch` (a click / KEY_PRESS on an active letter). If the live slot was
// already this letter (floated onto it) just append a fresh slot; otherwise
// (touch tap, or a click without floating) replace the live slot with the letter
// first. Then recompute the alphabet/matches and, if only one letter can follow,
// pre-select it.
static void mnemonic_lock_in(mnemonic_ctx_t *c, char ch) {
    if (c->letters.empty() || c->letters.back() != " ") {
        c->letters.push_back(" ");
    } else {
        c->letters.back() = std::string(1, ch);
        c->letters.push_back(" ");
    }
    mnemonic_calc_possible_alphabet(c);
    mnemonic_apply_dimming(c);
    if (c->possible_alphabet.size() == 1 && c->matrix) {
        int bi = kb_find_button(c->map.data(), c->possible_alphabet[0]);
        if (bi >= 0) lv_buttonmatrix_set_selected_button(c->matrix, (uint32_t)bi);
    }
    mnemonic_update_display(c);
    mnemonic_render_matches(c);
}

// Backspace: drop the last locked letter (and the live slot), restoring a fresh
// live slot, then recompute. Ports the KEY_BACKSPACE press branch.
static void mnemonic_delete(mnemonic_ctx_t *c) {
    if (c->letters.size() >= 2) c->letters.resize(c->letters.size() - 2);
    else                        c->letters.clear();
    c->letters.push_back(" ");
    mnemonic_calc_possible_alphabet(c);
    mnemonic_apply_dimming(c);
    mnemonic_update_display(c);
    mnemonic_render_matches(c);
}

// Joystick float: preview the hovered key in the live slot WITHOUT locking it in.
// A letter updates the matches (calc_possible_words, not the alphabet); hovering
// DEL clears the live slot so the display shows just the locked prefix.
static void mnemonic_float(mnemonic_ctx_t *c, const char *txt) {
    if (!txt) return;
    if (std::strcmp(txt, SeedSignerIconConstants::DELETE) == 0) {
        if (c->letters.empty()) c->letters.push_back(" ");
        else                    c->letters.back() = " ";
        mnemonic_calc_possible_words(c);
        mnemonic_update_display(c);
        mnemonic_render_matches(c);
        return;
    }
    if (std::strlen(txt) == 1 && txt[0] >= 'a' && txt[0] <= 'z') {
        char ch = txt[0];
        if (c->possible_alphabet.find(ch) == std::string::npos) return;  // dimmed
        if (c->letters.empty()) c->letters.push_back(std::string(1, ch));
        else                    c->letters.back() = std::string(1, ch);
        mnemonic_calc_possible_words(c);
        mnemonic_update_display(c);
        mnemonic_render_matches(c);
    }
}

// Scroll the hardware candidate highlight; flash the corresponding arrow.
static void mnemonic_scroll(mnemonic_ctx_t *c, int dir) {
    if (!mnemonic_matches_shown(c) || c->possible_words.empty()) return;  // panel hidden → nothing to scroll
    c->selected_index += dir;
    if (c->selected_index < 0) c->selected_index = 0;
    if (c->selected_index >= (int)c->possible_words.size()) c->selected_index = (int)c->possible_words.size() - 1;
    kb_flash_side_button(dir < 0 ? c->arrow_up : c->arrow_down);
    mnemonic_render_matches(c);
}

// Deliver the chosen word to the host (hardware KEY2 / touch CHECK).
static void mnemonic_accept_word(mnemonic_ctx_t *c, int word_index) {
    if (word_index < 0 || word_index >= (int)c->wordlist.size()) return;
    const std::string &word = c->wordlist[word_index];
    if (c->ta && lv_obj_is_valid(c->ta)) lv_textarea_set_text(c->ta, word.c_str());
    seedsigner_lvgl_on_text_entered(word.c_str());
}

// --- event callbacks --------------------------------------------------------

// Click on a key (joystick ENTER or touch tap): lock in a letter / delete.
static void mnemonic_value_changed_cb(lv_event_t *e) {
    lv_obj_t *m = lv_event_get_target_obj(e);
    mnemonic_ctx_t *c = (mnemonic_ctx_t *)lv_event_get_user_data(e);
    if (!c) return;
    uint32_t id = lv_buttonmatrix_get_selected_button(m);
    if (id == LV_BUTTONMATRIX_BUTTON_NONE) return;
    const char *txt = lv_buttonmatrix_get_button_text(m, id);
    if (!txt) return;
    if (std::strcmp(txt, SeedSignerIconConstants::DELETE) == 0) { mnemonic_delete(c); return; }
    if (std::strlen(txt) == 1 && txt[0] >= 'a' && txt[0] <= 'z') {
        char ch = txt[0];
        if (c->possible_alphabet.find(ch) == std::string::npos) return;  // dimmed key
        mnemonic_lock_in(c, ch);
    }
}

// Hardware PREPROCESS key filter: the aux keys (scroll/accept) and the top-nav
// handoff. Read before the buttonmatrix moves its selection. Directional floats
// are handled by the POST filter (after the move) so they see the new selection.
static void mnemonic_kb_preprocess_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_KEY) return;
    mnemonic_ctx_t *c = (mnemonic_ctx_t *)lv_event_get_user_data(e);
    if (!c) return;
    uint32_t key = lv_event_get_key(e);

    int aux = nav_aux_key_index(key);   // shared KEY1/2/3 recognizer (navigation.h)
    if (aux == 1) { mnemonic_scroll(c, -1); lv_event_stop_processing(e); return; }
    if (aux == 3) { mnemonic_scroll(c, +1); lv_event_stop_processing(e); return; }
    if (aux == 2) {
        // Only accept when the panel is actually shown — otherwise possible_words is
        // populated (for dimming) but no highlighted candidate is visible to accept.
        if (mnemonic_matches_shown(c) && !c->possible_words.empty()) {
            mnemonic_accept_word(c, c->possible_words[c->selected_index]);
        }
        lv_event_stop_processing(e);
        return;
    }

    // UP on the top row hands focus up to the back button (the buttonmatrix does
    // not wrap UP off the top row).
    if (key == LV_KEY_UP) {
        uint32_t sel = lv_buttonmatrix_get_selected_button(c->matrix);
        if (sel != LV_BUTTONMATRIX_BUTTON_NONE && sel < kb_top_row_count(c->map.data())
            && c->back_btn && lv_obj_is_valid(c->back_btn)) {
            lv_group_focus_obj(c->back_btn);
            lv_event_stop_processing(e);
        }
    }
}

// Hardware POST key filter: after the buttonmatrix has moved its selection,
// preview the now-selected key (live float). Ignores ENTER (a click → lock-in via
// VALUE_CHANGED) and any key while focus has handed off to the back button.
static void mnemonic_kb_post_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_KEY) return;
    mnemonic_ctx_t *c = (mnemonic_ctx_t *)lv_event_get_user_data(e);
    if (!c) return;
    uint32_t key = lv_event_get_key(e);
    if (key != LV_KEY_UP && key != LV_KEY_DOWN && key != LV_KEY_LEFT && key != LV_KEY_RIGHT) return;
    if (c->group && lv_group_get_focused(c->group) != c->matrix) return;  // handed off to back
    uint32_t sel = lv_buttonmatrix_get_selected_button(c->matrix);
    if (sel == LV_BUTTONMATRIX_BUTTON_NONE) return;
    mnemonic_float(c, lv_buttonmatrix_get_button_text(c->matrix, sel));
}

static void mnemonic_back_key_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_KEY) return;
    mnemonic_ctx_t *c = (mnemonic_ctx_t *)lv_event_get_user_data(e);
    if (c) kb_back_down_to_matrix(e, c->matrix);
}

// Touch: tap a candidate to highlight it in place and enable the accept button.
static void mnemonic_touch_candidate_cb(lv_event_t *e) {
    lv_obj_t *btn = lv_event_get_target_obj(e);
    mnemonic_ctx_t *c = (mnemonic_ctx_t *)lv_event_get_user_data(e);
    if (!c || !c->cand_list) return;
    int k = (int)(intptr_t)lv_obj_get_user_data(btn);
    c->touch_selected = k;
    uint32_t n = lv_obj_get_child_count(c->cand_list);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t *child = lv_obj_get_child(c->cand_list, i);
        mnemonic_style_candidate(child, (int)i == k);
    }
    mnemonic_set_check_enabled(c, true);
}

// Touch: tap CHECK to accept the highlighted candidate.
static void mnemonic_check_cb(lv_event_t *e) {
    mnemonic_ctx_t *c = (mnemonic_ctx_t *)lv_event_get_user_data(e);
    if (!c) return;
    if (c->touch_selected < 0 || c->touch_selected >= (int)c->possible_words.size()) return;
    mnemonic_accept_word(c, c->possible_words[c->touch_selected]);
}

static void mnemonic_cleanup_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;
    mnemonic_ctx_t *c = (mnemonic_ctx_t *)lv_event_get_user_data(e);
    if (!c) return;
    if (c->group) lv_group_del(c->group);
    delete c;
}

void seed_mnemonic_entry_screen(void *ctx_json) {
    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    bool has_mode_override = false;
    input_mode_t mode_override = INPUT_MODE_TOUCH;
    nav_mode_override_from_cfg(cfg, has_mode_override, mode_override);
    bool hardware = (has_mode_override ? mode_override : input_profile_get_mode()) == INPUT_MODE_HARDWARE;

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, false);
    lv_obj_t *body = screen.body;
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);

    const int32_t content_w = lv_obj_get_content_width(body);
    const int32_t content_h = lv_obj_get_content_height(body);

    mnemonic_ctx_t *c = new mnemonic_ctx_t();
    c->back_btn = screen.top_back_btn;
    c->top_nav  = screen.top_nav;
    c->hardware = hardware;

    // wordlist (required).
    if (!cfg.contains("wordlist") || !cfg["wordlist"].is_array() || cfg["wordlist"].empty()) {
        delete c;
        throw std::runtime_error("seed_mnemonic_entry_screen requires a non-empty \"wordlist\" array");
    }
    c->wordlist.reserve(cfg["wordlist"].size());
    for (const auto &w : cfg["wordlist"]) {
        if (w.is_string()) c->wordlist.push_back(w.get<std::string>());
    }

    // initial_letters: accept a string ("ap") or an array (["a","p"]); normalize to
    // a list of single non-space chars.
    if (cfg.contains("initial_letters")) {
        const auto &il = cfg["initial_letters"];
        if (il.is_string()) {
            for (char ch : il.get<std::string>()) if (ch != ' ') c->letters.push_back(std::string(1, ch));
        } else if (il.is_array()) {
            for (const auto &el : il) {
                if (el.is_string()) {
                    std::string s = el.get<std::string>();
                    if (!s.empty() && s != " ") c->letters.push_back(s);
                }
            }
        }
    }

    // Mirror Python's __post_init__ seeding of the live slot: >1 letter locks them
    // all in (append a fresh live slot) and pre-computes the matches; exactly 1
    // letter sits in the live slot (no matches yet); 0 letters = a fresh slot.
    char initial_selected = 'a';
    if (c->letters.size() > 1) {
        initial_selected = c->letters.back()[0];
        c->letters.push_back(" ");
        mnemonic_calc_possible_alphabet(c);
    } else if (c->letters.size() == 1) {
        initial_selected = c->letters[0][0];
        c->possible_alphabet = "abcdefghijklmnopqrstuvwxyz";
    } else {
        c->letters.push_back(" ");
        c->possible_alphabet = "abcdefghijklmnopqrstuvwxyz";
    }

    // --- candidate-column geometry. Python sizes the column to the longest BIP-39
    // word (8 chars, "mushroom") + 2*COMPONENT_PADDING and pins it to the right
    // screen edge (matches_list_x = canvas_width - column_width). Reclaim the right
    // EDGE_PADDING gutter so the column reaches the actual screen edge (content_w +
    // EDGE_PADDING in body-local coords), pushing it as far right as possible.
    lv_point_t msz;
    lv_text_get_size(&msz, "mushroom", &CANDIDATE_FONT, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const int32_t matches_col_w = msz.x + 2 * COMPONENT_PADDING;
    const int32_t col_gap       = COMPONENT_PADDING;
    const int32_t matches_x     = content_w + EDGE_PADDING - matches_col_w;
    const int32_t keyboard_w    = matches_x - col_gap;

    // Text-entry strip (source of truth for the in-progress word display), sized to
    // the keyboard width.
    lv_obj_t *ta = kb_make_text_entry(body, keyboard_w, seedsigner_lvgl_is_static_render());
    lv_obj_set_width(ta, keyboard_w);
    c->ta = ta;

    // --- keyboard map: 5x6 a-z, DEL (width 2) on the last row, hidden fillers to
    // keep the last row's columns aligned with the rows above (Python left-aligns
    // y/z/DEL with blank space to the right).
    const char *DEL = SeedSignerIconConstants::DELETE;
    c->key_storage.clear();
    for (int i = 0; i < 26; ++i) c->key_storage.push_back(std::string(1, (char)('a' + i)));
    c->map.clear();
    c->ctrl.clear();
    int col = 0;
    for (int i = 0; i < 26; ++i) {
        c->map.push_back(c->key_storage[i].c_str());
        c->ctrl.push_back((lv_buttonmatrix_ctrl_t)1);          // width-1 value key
        if (++col == 6) { c->map.push_back("\n"); col = 0; }   // 6 cols per row
    }
    // Row 5 already started after 'x' (24 letters = 4 full rows); y,z were appended
    // into the open row. Append DEL (width 2) + two hidden fillers to fill to 6.
    c->map.push_back(DEL);
    c->ctrl.push_back((lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_NO_REPEAT
                       | LV_BUTTONMATRIX_CTRL_CLICK_TRIG | LV_BUTTONMATRIX_CTRL_CHECKED | 2));
    c->map.push_back(" ");
    c->ctrl.push_back((lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_HIDDEN | 1));
    c->map.push_back(" ");
    c->ctrl.push_back((lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_HIDDEN | 1));
    c->map.push_back("");

    lv_obj_t *matrix = lv_buttonmatrix_create(body);
    lv_buttonmatrix_set_map(matrix, c->map.data());
    lv_buttonmatrix_set_ctrl_map(matrix, c->ctrl.data());
    kb_style_matrix(matrix, &KEYBOARD_FONT);
    // Inactive keys are dimmed VISUALLY (mnemonic_dim_draw_cb), not via DISABLED,
    // so the joystick can still float over them (Python parity — see
    // mnemonic_apply_dimming). The matrix already sends draw-task events (enabled by
    // kb_style_matrix for its icon recolor), so just add our dimming handler.
    lv_obj_add_event_cb(matrix, mnemonic_dim_draw_cb, LV_EVENT_DRAW_TASK_ADDED, c);
    // Orange selection OUTLINE on the focused/pressed key. For an ACTIVE selected
    // key the orange fill (from kb_style_matrix) dominates and the border just
    // rims it; for an INACTIVE selected key the dim draw-cb keeps the dark fill, so
    // only this border marks the cursor — Python's "inactive + selected = highlight
    // outline" behavior.
    const lv_state_t sel_border_states[] = {
        LV_STATE_PRESSED, LV_STATE_FOCUSED, LV_STATE_FOCUS_KEY,
    };
    for (lv_state_t st : sel_border_states) {
        lv_obj_set_style_border_color(matrix, lv_color_hex(ACCENT_COLOR), LV_PART_ITEMS | st);
        lv_obj_set_style_border_opa(matrix, LV_OPA_COVER, LV_PART_ITEMS | st);
        lv_obj_set_style_border_width(matrix, 2, LV_PART_ITEMS | st);
    }
    c->matrix = matrix;

    const int32_t kb_top = BUTTON_HEIGHT + COMPONENT_PADDING;
    lv_obj_set_size(matrix, keyboard_w, content_h - kb_top);
    lv_obj_align(matrix, LV_ALIGN_TOP_LEFT, 0, kb_top);
    lv_obj_add_event_cb(matrix, mnemonic_value_changed_cb, LV_EVENT_VALUE_CHANGED, c);

    // --- candidate panel
    // Hardware soft-buttons (KEY1 up-scroll, KEY2 accept = the centered highlight,
    // KEY3 down-scroll) sit at the SAME physical Y positions as the passphrase
    // screen's KEY1/KEY2/KEY3 side panel, so they line up with the three physical
    // buttons beside the screen; the scroll arrows overshoot the right edge (clipped)
    // to reinforce that connection — exactly like seed_add_passphrase_screen. The
    // candidate list runs the full body height behind the arrows: Python shows up to
    // 3 matches above + 7 below the centered highlight (highlighted_row=3,
    // num_possible_rows=11), overflowing past the arrows and clipping at the screen
    // edge, with the arrows drawn ON TOP.
    const int32_t line_h = (int32_t)lv_font_get_line_height(&CANDIDATE_FONT);
    // Row pitch ≈ Python's matches_list_row_height (word cap height + padding).
    const int32_t row_h  = line_h * 3 / 4 + COMPONENT_PADDING / 2;
    const int32_t hl_h   = (int32_t)(BUTTON_HEIGHT * 3 / 4);

    if (hardware) {
        // Passphrase-aligned anchor slots (body-local). The KEY2 slot is a
        // BUTTON_HEIGHT box centered on the full screen; KEY1/KEY3 are one
        // (3*COMPONENT_PADDING + BUTTON_HEIGHT) step above/below it.
        const int32_t screen_h  = lv_obj_get_height(screen.screen);
        const int32_t spacing   = 3 * COMPONENT_PADDING + BUTTON_HEIGHT;
        const int32_t key2_top  = (screen_h - BUTTON_HEIGHT) / 2 - TOP_NAV_HEIGHT;
        const int32_t key1_top  = key2_top - spacing;
        const int32_t key3_top  = key2_top + spacing;
        const int32_t hl_center = key2_top + BUTTON_HEIGHT / 2;

        // Dimmed candidate rows: up to 3 above + 7 below the highlight, spaced by
        // row_h off its center. They overflow past the arrows and clip at the body
        // edge — created FIRST so the highlight and (last) the arrows draw on top.
        for (int k = 0; k < 3; ++k) {
            lv_obj_t *l = lv_label_create(body);
            lv_obj_set_style_text_font(l, &CANDIDATE_FONT, LV_PART_MAIN);
            lv_obj_set_style_text_color(l, lv_color_hex(0xdddddd), LV_PART_MAIN);
            lv_obj_set_pos(l, matches_x + COMPONENT_PADDING, hl_center - (k + 1) * row_h - line_h / 2);
            mnemonic_set_hidden(l, true);
            c->dim_above.push_back(l);
        }
        for (int k = 0; k < 7; ++k) {
            lv_obj_t *l = lv_label_create(body);
            lv_obj_set_style_text_font(l, &CANDIDATE_FONT, LV_PART_MAIN);
            lv_obj_set_style_text_color(l, lv_color_hex(0xdddddd), LV_PART_MAIN);
            lv_obj_set_pos(l, matches_x + COMPONENT_PADDING, hl_center + (k + 1) * row_h - line_h / 2);
            mnemonic_set_hidden(l, true);
            c->dim_below.push_back(l);
        }

        // Highlight slot (KEY2): the selected candidate, centered in the KEY2 slot,
        // left-aligned fixed-width word in the (on-screen) matches column.
        // The highlight runs COMPONENT_PADDING off the right screen edge on purpose
        // (Python's matches_list_highlight_button width = column + COMPONENT_PADDING):
        // its rounded right corner is clipped at the edge, reinforcing — like the
        // overshooting arrows — that this is the joystick-mode selection.
        lv_obj_t *hl = lv_obj_create(body);
        lv_obj_set_size(hl, matches_col_w + COMPONENT_PADDING, hl_h);
        lv_obj_set_pos(hl, matches_x, hl_center - hl_h / 2);
        lv_obj_set_style_bg_color(hl, lv_color_hex(ACCENT_COLOR), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(hl, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(hl, BUTTON_RADIUS / 2, LV_PART_MAIN);
        lv_obj_set_style_border_width(hl, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(hl, 0, LV_PART_MAIN);
        lv_obj_remove_flag(hl, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
        lv_obj_t *hl_lbl = lv_label_create(hl);
        lv_obj_set_style_text_font(hl_lbl, &CANDIDATE_FONT, LV_PART_MAIN);
        lv_obj_set_style_text_color(hl_lbl, lv_color_hex(BUTTON_SELECTED_FONT_COLOR), LV_PART_MAIN);
        lv_obj_align(hl_lbl, LV_ALIGN_LEFT_MID, COMPONENT_PADDING, 0);
        c->hl_btn = hl;
        c->hl_label = hl_lbl;

        // Scroll arrows (KEY1/KEY3), created LAST so they draw OVER the candidate
        // rows (Python parity), centered in their slots and overshooting the right
        // screen edge exactly like the passphrase side panel (same w / x / clipped).
        const int32_t side_w   = 56 * active_profile().px_multiplier / 100;
        const int32_t side_x   = content_w + EDGE_PADDING + COMPONENT_PADDING - side_w;
        const int32_t clipped  = COMPONENT_PADDING;
        const int32_t arrow_h  = (int32_t)(BUTTON_HEIGHT * 3 / 4);
        const int32_t arrow_dy = (BUTTON_HEIGHT - arrow_h) / 2;   // center within the slot
        c->arrow_up = kb_side_button(body, side_x, key1_top + arrow_dy, side_w, arrow_h,
                                     SeedSignerIconConstants::CHEVRON_UP, &ICON_FONT__SEEDSIGNER,
                                     BODY_FONT_COLOR, clipped, nullptr);
        c->arrow_down = kb_side_button(body, side_x, key3_top + arrow_dy, side_w, arrow_h,
                                       SeedSignerIconConstants::CHEVRON_DOWN, &ICON_FONT__SEEDSIGNER,
                                       BODY_FONT_COLOR, clipped, nullptr);
    } else {
        // Touch: a scrollable candidate list above a persistent green CHECK button.
        const int32_t check_h = hl_h;
        lv_obj_t *list = lv_obj_create(body);
        lv_obj_set_pos(list, matches_x, 0);
        lv_obj_set_size(list, matches_col_w, content_h - check_h - COMPONENT_PADDING);
        lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(list, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(list, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_row(list, COMPONENT_PADDING / 2, LV_PART_MAIN);
        lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_scroll_dir(list, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
        c->cand_list = list;

        lv_obj_t *chk = lv_obj_create(body);
        lv_obj_set_size(chk, matches_col_w, check_h);
        lv_obj_set_pos(chk, matches_x, content_h - check_h);
        lv_obj_set_style_radius(chk, BUTTON_RADIUS / 2, LV_PART_MAIN);
        lv_obj_set_style_border_width(chk, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(chk, 0, LV_PART_MAIN);
        lv_obj_remove_flag(chk, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *chk_lbl = lv_label_create(chk);
        lv_label_set_text(chk_lbl, SeedSignerIconConstants::CHECK);
        lv_obj_set_style_text_font(chk_lbl, &ICON_FONT__SEEDSIGNER, LV_PART_MAIN);
        lv_obj_center(chk_lbl);
        lv_obj_add_event_cb(chk, mnemonic_check_cb, LV_EVENT_CLICKED, c);
        c->check_btn = chk;
        mnemonic_set_check_enabled(c, false);
    }

    // Initial dimming + the in-progress word + the candidate matches.
    mnemonic_apply_dimming(c);
    mnemonic_update_display(c);
    mnemonic_render_matches(c);

    // Optional: pre-select one of the current candidate words. Mainly for static
    // screenshots of the touch accept affordance (the selection + enabled CHECK is
    // otherwise only reachable by tapping at runtime). Applied after the initial
    // render so the candidate widgets already exist. No-op if the word isn't a
    // current match.
    std::string preselect = cfg.value("initial_selected_word", std::string());
    if (!preselect.empty()) {
        int idx = -1;
        for (int k = 0; k < (int)c->possible_words.size(); ++k) {
            if (c->wordlist[c->possible_words[k]] == preselect) { idx = k; break; }
        }
        if (idx >= 0) {
            if (hardware) {
                c->selected_index = idx;
                mnemonic_render_matches(c);
            } else {
                c->touch_selected = idx;
                if (c->cand_list && idx < (int)lv_obj_get_child_count(c->cand_list)) {
                    mnemonic_style_candidate(lv_obj_get_child(c->cand_list, idx), true);
                }
                mnemonic_set_check_enabled(c, true);
            }
        }
    }

    // Touch panels have no joystick cursor: a key is chosen by tapping it directly,
    // so NO key is pre-selected on load (the pre-highlighted letter is purely a
    // joystick affordance). LVGL already defaults a buttonmatrix to BUTTON_NONE, but
    // set it explicitly so touch is guaranteed to start blank regardless of any
    // platform default — the hardware branch below installs the cursor on
    // initial_selected. (Mirrors button_list_screen, where initial selection is
    // hardware-mode only.)
    lv_buttonmatrix_set_selected_button(matrix, LV_BUTTONMATRIX_BUTTON_NONE);

    if (hardware) {
        c->group = lv_group_create();
        lv_group_set_wrap(c->group, false);
        lv_group_add_obj(c->group, matrix);
        if (screen.top_back_btn) {
            lv_group_add_obj(c->group, screen.top_back_btn);
            lv_obj_add_event_cb(matrix, mnemonic_kb_preprocess_cb,
                                (lv_event_code_t)(LV_EVENT_KEY | LV_EVENT_PREPROCESS), c);
            lv_obj_add_event_cb(matrix, mnemonic_kb_post_cb, LV_EVENT_KEY, c);
            lv_obj_add_event_cb(screen.top_back_btn, mnemonic_back_key_cb, LV_EVENT_KEY, c);
        }
        lv_group_focus_obj(matrix);
        kb_connect_indevs(c->group);

        int sel = kb_find_button(c->map.data(), initial_selected);
        if (sel < 0) sel = 0;
        lv_buttonmatrix_set_selected_button(matrix, (uint32_t)sel);
        if (seedsigner_lvgl_is_static_render()) lv_obj_add_state(matrix, LV_STATE_PRESSED);
    }

    lv_obj_add_event_cb(screen.screen, mnemonic_cleanup_cb, LV_EVENT_DELETE, c);
    load_screen_and_cleanup_previous(screen.screen);
}
