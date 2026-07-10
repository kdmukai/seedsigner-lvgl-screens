// seed_mnemonic_entry_screen
//
// Python provenance: SeedMnemonicEntryScreen (seed_screens.py)
//
// BIP-39 seed-word entry. A 5x6 a-z + DEL lv_buttonmatrix drives a live
// per-keystroke prefix-match state machine against the cfg-passed wordlist: as
// letters are entered, keys that cannot continue any word are visually dimmed
// and a candidate-word panel updates. The matching logic (ports Python's
// calc_possible_words / calc_possible_alphabet plus the trailing-space "live
// slot" float trick) is inherently per-keystroke, so it runs native here — the
// host View cannot intervene mid-word.
//
// Two candidate panels, by input mode:
//   - hardware (Python parity): a right column with a fixed highlight slot at
//     vertical center, dimmed candidates above/below, and KEY1/KEY3 scroll +
//     KEY2 accept (the physical Pi Zero buttons), via kb_side_button.
//   - touch (deliberate divergence from Python, UX review pending): a
//     scrollable candidate list (tap a word to highlight it in place) + a
//     persistent green CHECK button beneath, disabled until a selection
//     exists, tap = accept. No fixed slot, no modal — mirrors the passphrase
//     confirm idiom.
//
// Returns the chosen word via seedsigner_lvgl_on_text_entered() (the same host
// hook the other keyboard screens use); BACK returns RET_CODE__BACK_BUTTON via
// the scaffold's back button. The View loops the screen 12/24x per seed.
//
// Content-policy note: the a-z key caps are keyboard STRUCTURE (Python's
// Keyboard charset), not host-localized content — BIP-39 letters are matched
// against the host-supplied wordlist — so they are not converted to required
// cfg content.
//
// Lifecycle: stateful (Tier 2) — one heap ctx (new/delete: C++ containers) +
// one LV_EVENT_DELETE cleanup callback on the screen root.
//
// cfg:
//   top_nav.title             (string, required)     localized screen title
//            (e.g. "Seed Word #3", derived host-side from the word position).
//   top_nav.show_back_button  (bool, default true)   Python BaseTopNavScreen default.
//   top_nav.show_power_button (bool, default false)  Python BaseTopNavScreen default.
//   wordlist                  (array of strings, required, non-empty)  the selected
//            language's BIP-39 words (English = 2048); the View owns language
//            selection.
//   initial_letters           (string | array of single-char strings, optional)
//            letters already entered for this word (empty/absent = fresh word).
//   initial_selected_word     (string, optional)     pre-select this candidate at
//            load (touch: highlight it + enable CHECK; hardware: move the slot to
//            it). Mainly for static screenshots — runtime selection is a tap /
//            scroll. No-op if the word isn't a current match.
//   input.mode                (string, optional: "touch" | "hardware")  read via
//            nav_mode_override_from_cfg to pick the candidate-panel variant;
//            absent -> the platform input profile decides.
//   allow_screensaver         (bool, default true)   normalized by the parse layer;
//            false stamps the screensaver opt-out flag on the root (scaffold layer).

#include "screen_scaffold.h"  // parse_screen_json_ctx, create_top_nav_screen_scaffold, load_screen_and_cleanup_previous
#include "seedsigner.h"       // screen_scaffold_t fields, seedsigner_lvgl_on_text_entered, seedsigner_lvgl_is_static_render
#include "gui_constants.h"    // KEYBOARD_FONT, CANDIDATE_FONT, ICON_FONT__SEEDSIGNER, SeedSignerIconConstants, paddings/colors/button metrics
#include "input_profile.h"    // input_profile_get_mode, input_mode_t, INPUT_MODE_HARDWARE/INPUT_MODE_TOUCH
#include "keyboard_core.h"    // kb_make_text_entry, kb_style_matrix, kb_find_button, kb_top_row_count, kb_back_down_to_matrix, kb_flash_side_button, kb_side_button, kb_side_panel_geometry, kb_connect_indevs
#include "navigation.h"       // nav_aux_key_index (shared KEY1/2/3 recognizer)
#include "screen_helpers.h"   // ensure_top_nav_structure, require_top_nav_title, nav_mode_override_from_cfg

#include "lvgl.h"             // lv_buttonmatrix / lv_textarea / lv_label, events, groups, draw-task hooks

#include <nlohmann/json.hpp>  // json (cfg reads + structural-default writes)

#include <cstdint>            // intptr_t (candidate index carried in lv_obj user_data)
#include <cstring>            // std::strlen / std::strcmp (key-cap text checks)
#include <stdexcept>          // std::runtime_error (required-field validation)
#include <string>             // std::string
#include <vector>             // std::vector (wordlist, letters, key map)

using json = nlohmann::json;


// The candidate-word panel stays hidden until the prefix reaches this many
// letters. A single letter matches up to ~250 BIP-39 words; on a slower display
// (notably the ESP32-P4) rebuilding that many candidate widgets on the very first
// keystroke stalls the UI for a visible beat — and a 1-letter candidate list has
// little practical value anyway (you can't meaningfully pick a word yet). So the
// panel, and the accept affordance that reads from it, wait for a second letter to
// narrow the field. NOTE: possible_words is still computed at every stage — the
// keyboard's next-letter dimming (possible_alphabet) is derived from it — so only
// the on-screen panel is deferred, not the matching itself.
static const size_t SEED_MNEMONIC_ENTRY_MATCH_MIN_LETTERS = 2;


namespace {

// ---------------------------------------------------------------------------
// Screen context
// ---------------------------------------------------------------------------

// Per-screen state. C++ containers, so new/delete (freed in the cleanup cb —
// lv_malloc would skip the ctors/dtors). The `letters` buffer mirrors Python's
// self.letters: the locked-in letters plus a trailing "live slot" (a single " "
// or the currently-floated letter) so the joystick can preview a letter before
// it is locked in with a click.
struct seed_mnemonic_entry_ctx_t {
    lv_obj_t   *matrix = nullptr;
    lv_obj_t   *text_entry = nullptr;
    lv_obj_t   *back_button = nullptr;
    lv_group_t *group = nullptr;
    bool        hardware = false;

    // Keyboard map (persistent for the buttonmatrix lifetime; ctrl_map mirrors
    // lv_buttonmatrix_set_ctrl_map's vocabulary).
    std::vector<std::string>            key_storage;
    std::vector<const char *>           key_map;
    std::vector<lv_buttonmatrix_ctrl_t> ctrl_map;

    // Matching state.
    std::vector<std::string> wordlist;
    std::vector<std::string> letters;           // locked letters + trailing live slot
    std::vector<int>         possible_words;    // indices into wordlist
    std::string              possible_alphabet; // distinct allowed next letters
    int                      selected_index = 0;

    // Hardware candidate panel.
    lv_obj_t              *highlight_button = nullptr;   // center highlight slot
    lv_obj_t              *highlight_label = nullptr;
    lv_obj_t              *arrow_up = nullptr;
    lv_obj_t              *arrow_down = nullptr;
    std::vector<lv_obj_t *> dim_above;          // nearest-first (sel-1, sel-2, ...)
    std::vector<lv_obj_t *> dim_below;          // nearest-first (sel+1, sel+2, ...)

    // Touch candidate panel.
    lv_obj_t *candidate_list = nullptr;         // scrollable list of candidates
    lv_obj_t *check_button = nullptr;           // persistent green accept
    int       touch_selected = -1;              // index into possible_words
};

// ---------------------------------------------------------------------------
// Matching state machine (ports calc_possible_words / calc_possible_alphabet)
// ---------------------------------------------------------------------------

// The current search prefix: the letters buffer joined and right-trimmed of the
// trailing live slot (Python's "".join(self.letters).strip()).
std::string seed_mnemonic_entry_prefix(const seed_mnemonic_entry_ctx_t *ctx) {
    std::string joined;
    for (const std::string &letter : ctx->letters) joined += letter;
    size_t last_non_space = joined.find_last_not_of(' ');
    return (last_non_space == std::string::npos) ? std::string() : joined.substr(0, last_non_space + 1);
}

bool seed_mnemonic_entry_matches_shown(const seed_mnemonic_entry_ctx_t *ctx) {
    return seed_mnemonic_entry_prefix(ctx).size() >= SEED_MNEMONIC_ENTRY_MATCH_MIN_LETTERS;
}

// possible_words = every wordlist entry starting with the current prefix (reset
// the highlight to the top of the new list).
void seed_mnemonic_entry_calc_possible_words(seed_mnemonic_entry_ctx_t *ctx) {
    std::string prefix = seed_mnemonic_entry_prefix(ctx);
    ctx->possible_words.clear();
    if (!prefix.empty()) {
        for (int i = 0; i < (int)ctx->wordlist.size(); ++i) {
            if (ctx->wordlist[i].rfind(prefix, 0) == 0) ctx->possible_words.push_back(i);
        }
    }
    ctx->selected_index = 0;
}

// possible_alphabet = the distinct set of letters that can follow the locked
// prefix (the (len-1)-th char of every possible word), in first-seen order.
// Recomputed only when a letter is locked in or deleted (not while floating), so
// the joystick can freely float between active letters. With <=1 letter the
// whole alphabet is active and there are no matches yet (Python parity).
void seed_mnemonic_entry_calc_possible_alphabet(seed_mnemonic_entry_ctx_t *ctx) {
    if (ctx->letters.size() > 1) {
        size_t letter_num = ctx->letters.size() - 1;   // == len(search_letters)
        seed_mnemonic_entry_calc_possible_words(ctx);
        std::string alphabet;
        for (int word_index : ctx->possible_words) {
            const std::string &word = ctx->wordlist[word_index];
            if (word.size() >= letter_num + 1) {
                char next_letter = word[letter_num];
                if (alphabet.find(next_letter) == std::string::npos) alphabet += next_letter;
            }
        }
        ctx->possible_alphabet = alphabet;
    } else {
        ctx->possible_alphabet = "abcdefghijklmnopqrstuvwxyz";
        ctx->possible_words.clear();
        ctx->selected_index = 0;
    }
}

// ---------------------------------------------------------------------------
// View sync helpers
// ---------------------------------------------------------------------------

void seed_mnemonic_entry_set_hidden(lv_obj_t *target, bool hidden) {
    if (!target) return;
    if (hidden) lv_obj_add_flag(target, LV_OBJ_FLAG_HIDDEN);
    else        lv_obj_remove_flag(target, LV_OBJ_FLAG_HIDDEN);
}

// Re-dim the keyboard for the current possible_alphabet. Dimming is purely
// VISUAL (seed_mnemonic_entry_dim_draw_cb recolors letter keys not in
// possible_alphabet) — NOT LV_BUTTONMATRIX_CTRL_DISABLED, because disabled keys
// are SKIPPED by the joystick navigation, which strands the cursor when most keys
// are inactive (you could only move along the few active keys). Python lets the
// cursor float freely over dimmed keys (they just don't register a press);
// keeping every key navigable here matches that. So this just forces a redraw.
void seed_mnemonic_entry_apply_dimming(seed_mnemonic_entry_ctx_t *ctx) {
    if (ctx->matrix && lv_obj_is_valid(ctx->matrix)) lv_obj_invalidate(ctx->matrix);
}

// Visual key dimming (LV_EVENT_DRAW_TASK_ADDED): recolor every letter key that
// can't continue any word (not in possible_alphabet) to a recessed dark fill +
// faint text — INCLUDING the selected one. Python's Key.render dims the selected
// inactive key too, marking it only with a highlight OUTLINE (no solid fill); the
// orange border for that comes from the FOCUS/PRESSED border style added to the
// matrix, which this leaves untouched (only fill + label are recolored). Letters
// are plain ASCII, so this never collides with kb_icon_draw_cb (the DEL glyph).
void seed_mnemonic_entry_dim_draw_cb(lv_event_t *e) {
    lv_draw_task_t *task = lv_event_get_draw_task(e);
    lv_draw_dsc_base_t *base = (lv_draw_dsc_base_t *)lv_draw_task_get_draw_dsc(task);
    if (!base || base->part != LV_PART_ITEMS) return;

    seed_mnemonic_entry_ctx_t *ctx = (seed_mnemonic_entry_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;
    lv_obj_t *matrix = lv_event_get_target_obj(e);
    const char *text = lv_buttonmatrix_get_button_text(matrix, base->id1);
    if (!text || std::strlen(text) != 1 || text[0] < 'a' || text[0] > 'z') return;  // letters only
    if (ctx->possible_alphabet.find(text[0]) != std::string::npos) return;          // active key

    lv_draw_label_dsc_t *label_dsc = lv_draw_task_get_label_dsc(task);
    if (label_dsc) { label_dsc->color = lv_color_hex(0x555555); return; }   // gray text (Python "#333"-ish)
    lv_draw_fill_dsc_t *fill_dsc = lv_draw_task_get_fill_dsc(task);
    if (fill_dsc) fill_dsc->color = lv_color_hex(0x141414);                 // recessed dark fill
}

void seed_mnemonic_entry_update_display(seed_mnemonic_entry_ctx_t *ctx) {
    if (!ctx->text_entry || !lv_obj_is_valid(ctx->text_entry)) return;
    std::string prefix = seed_mnemonic_entry_prefix(ctx);
    lv_textarea_set_text(ctx->text_entry, prefix.c_str());
    lv_textarea_set_cursor_pos(ctx->text_entry, LV_TEXTAREA_CURSOR_LAST);
}

// Selected/normal styling for a touch candidate button (selected = SeedSigner
// orange, like the keyboard's selected key).
void seed_mnemonic_entry_style_candidate(lv_obj_t *button, bool selected) {
    lv_obj_set_style_bg_color(button,
        lv_color_hex(selected ? ACCENT_COLOR : BUTTON_BACKGROUND_COLOR), LV_PART_MAIN);
    lv_obj_t *label = lv_obj_get_child(button, 0);
    if (label) lv_obj_set_style_text_color(label,
        lv_color_hex(selected ? BUTTON_SELECTED_FONT_COLOR : BUTTON_FONT_COLOR), LV_PART_MAIN);
}

// Enable/disable the touch accept button (green + clickable once a candidate is
// selected; muted + inert until then).
void seed_mnemonic_entry_set_check_enabled(seed_mnemonic_entry_ctx_t *ctx, bool enabled) {
    if (!ctx->check_button || !lv_obj_is_valid(ctx->check_button)) return;
    if (enabled) {
        lv_obj_add_flag(ctx->check_button, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(ctx->check_button, lv_color_hex(SUCCESS_COLOR), LV_PART_MAIN);
        lv_obj_set_style_text_color(ctx->check_button, lv_color_hex(BUTTON_SELECTED_FONT_COLOR), LV_PART_MAIN);
    } else {
        lv_obj_remove_flag(ctx->check_button, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(ctx->check_button, lv_color_hex(BUTTON_BACKGROUND_COLOR), LV_PART_MAIN);
        lv_obj_set_style_text_color(ctx->check_button, lv_color_hex(0x666666), LV_PART_MAIN);
    }
}

// Forward declaration: candidate rows built in render_matches_touch wire their
// clicks to this callback (defined under Event callbacks below).
void seed_mnemonic_entry_touch_candidate_cb(lv_event_t *e);

// Hardware: refresh the fixed-slot highlight + the dimmed rows above/below it
// from possible_words[selected_index]. Hidden entirely when there are no matches.
void seed_mnemonic_entry_render_matches_hardware(seed_mnemonic_entry_ctx_t *ctx) {
    bool any_matches = seed_mnemonic_entry_matches_shown(ctx) && !ctx->possible_words.empty();
    seed_mnemonic_entry_set_hidden(ctx->highlight_button, !any_matches);
    seed_mnemonic_entry_set_hidden(ctx->arrow_up, !any_matches);
    seed_mnemonic_entry_set_hidden(ctx->arrow_down, !any_matches);
    if (!any_matches) {
        for (lv_obj_t *label : ctx->dim_above) seed_mnemonic_entry_set_hidden(label, true);
        for (lv_obj_t *label : ctx->dim_below) seed_mnemonic_entry_set_hidden(label, true);
        return;
    }
    int count = (int)ctx->possible_words.size();
    int selected = ctx->selected_index;
    if (ctx->highlight_label) lv_label_set_text(ctx->highlight_label, ctx->wordlist[ctx->possible_words[selected]].c_str());
    for (int i = 0; i < (int)ctx->dim_above.size(); ++i) {
        int match_index = selected - 1 - i;
        if (match_index >= 0) { lv_label_set_text(ctx->dim_above[i], ctx->wordlist[ctx->possible_words[match_index]].c_str()); seed_mnemonic_entry_set_hidden(ctx->dim_above[i], false); }
        else                  { seed_mnemonic_entry_set_hidden(ctx->dim_above[i], true); }
    }
    for (int i = 0; i < (int)ctx->dim_below.size(); ++i) {
        int match_index = selected + 1 + i;
        if (match_index < count) { lv_label_set_text(ctx->dim_below[i], ctx->wordlist[ctx->possible_words[match_index]].c_str()); seed_mnemonic_entry_set_hidden(ctx->dim_below[i], false); }
        else                     { seed_mnemonic_entry_set_hidden(ctx->dim_below[i], true); }
    }
}

// Touch: rebuild the scrollable candidate list (one tappable button per match),
// clearing any prior selection and disabling the accept button.
void seed_mnemonic_entry_render_matches_touch(seed_mnemonic_entry_ctx_t *ctx) {
    if (!ctx->candidate_list || !lv_obj_is_valid(ctx->candidate_list)) return;
    lv_obj_clean(ctx->candidate_list);
    ctx->touch_selected = -1;
    seed_mnemonic_entry_set_check_enabled(ctx, false);

    // Deferred until the prefix is long enough (see seed_mnemonic_entry_matches_shown):
    // with a single letter this would build hundreds of candidate rows, stalling the P4.
    if (!seed_mnemonic_entry_matches_shown(ctx)) return;

    const int32_t row_height = (int32_t)(BUTTON_HEIGHT * 3 / 4);
    for (int i = 0; i < (int)ctx->possible_words.size(); ++i) {
        lv_obj_t *row = lv_obj_create(ctx->candidate_list);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, row_height);
        lv_obj_set_style_radius(row, BUTTON_RADIUS / 2, LV_PART_MAIN);
        lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_left(row, COMPONENT_PADDING, LV_PART_MAIN);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_user_data(row, (void *)(intptr_t)i);
        lv_obj_add_event_cb(row, seed_mnemonic_entry_touch_candidate_cb, LV_EVENT_CLICKED, ctx);

        lv_obj_t *label = lv_label_create(row);
        lv_label_set_text(label, ctx->wordlist[ctx->possible_words[i]].c_str());
        lv_obj_set_style_text_font(label, &CANDIDATE_FONT, LV_PART_MAIN);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);

        seed_mnemonic_entry_style_candidate(row, false);
    }
}

void seed_mnemonic_entry_render_matches(seed_mnemonic_entry_ctx_t *ctx) {
    if (ctx->hardware) seed_mnemonic_entry_render_matches_hardware(ctx);
    else               seed_mnemonic_entry_render_matches_touch(ctx);
}

// ---------------------------------------------------------------------------
// State transitions
// ---------------------------------------------------------------------------

// Lock in `letter` (a click / KEY_PRESS on an active letter). The first branch
// (live slot is not " ") assumes the slot already holds the pressed letter and
// just appends a fresh slot — which holds under joystick float, where
// seed_mnemonic_entry_float keeps the slot equal to the hovered key. In touch
// mode with exactly ONE initial letter the slot can hold a DIFFERENT stale
// letter, which this branch locks in while dropping the tapped one (flagged in
// the conformance bug ledger; behavior kept unchanged here). Otherwise (slot is
// a fresh " ") replace the live slot with the letter first. Then recompute the
// alphabet/matches and, if only one letter can follow, pre-select it.
void seed_mnemonic_entry_lock_in(seed_mnemonic_entry_ctx_t *ctx, char letter) {
    if (ctx->letters.empty() || ctx->letters.back() != " ") {
        ctx->letters.push_back(" ");
    } else {
        ctx->letters.back() = std::string(1, letter);
        ctx->letters.push_back(" ");
    }
    seed_mnemonic_entry_calc_possible_alphabet(ctx);
    seed_mnemonic_entry_apply_dimming(ctx);
    if (ctx->possible_alphabet.size() == 1 && ctx->matrix) {
        int button_index = kb_find_button(ctx->key_map.data(), ctx->possible_alphabet[0]);
        if (button_index >= 0) lv_buttonmatrix_set_selected_button(ctx->matrix, (uint32_t)button_index);
    }
    seed_mnemonic_entry_update_display(ctx);
    seed_mnemonic_entry_render_matches(ctx);
}

// Backspace: drop the last locked letter (and the live slot), restoring a fresh
// live slot, then recompute. Ports the KEY_BACKSPACE press branch.
void seed_mnemonic_entry_delete(seed_mnemonic_entry_ctx_t *ctx) {
    if (ctx->letters.size() >= 2) ctx->letters.resize(ctx->letters.size() - 2);
    else                          ctx->letters.clear();
    ctx->letters.push_back(" ");
    seed_mnemonic_entry_calc_possible_alphabet(ctx);
    seed_mnemonic_entry_apply_dimming(ctx);
    seed_mnemonic_entry_update_display(ctx);
    seed_mnemonic_entry_render_matches(ctx);
}

// Joystick float: preview the hovered key in the live slot WITHOUT locking it in.
// A letter updates the matches (calc_possible_words, not the alphabet); hovering
// DEL clears the live slot so the display shows just the locked prefix.
void seed_mnemonic_entry_float(seed_mnemonic_entry_ctx_t *ctx, const char *text) {
    if (!text) return;
    if (std::strcmp(text, SeedSignerIconConstants::DELETE) == 0) {
        if (ctx->letters.empty()) ctx->letters.push_back(" ");
        else                      ctx->letters.back() = " ";
        seed_mnemonic_entry_calc_possible_words(ctx);
        seed_mnemonic_entry_update_display(ctx);
        seed_mnemonic_entry_render_matches(ctx);
        return;
    }
    if (std::strlen(text) == 1 && text[0] >= 'a' && text[0] <= 'z') {
        char letter = text[0];
        if (ctx->possible_alphabet.find(letter) == std::string::npos) return;  // dimmed
        if (ctx->letters.empty()) ctx->letters.push_back(std::string(1, letter));
        else                      ctx->letters.back() = std::string(1, letter);
        seed_mnemonic_entry_calc_possible_words(ctx);
        seed_mnemonic_entry_update_display(ctx);
        seed_mnemonic_entry_render_matches(ctx);
    }
}

// Scroll the hardware candidate highlight; flash the corresponding arrow.
void seed_mnemonic_entry_scroll(seed_mnemonic_entry_ctx_t *ctx, int direction) {
    if (!seed_mnemonic_entry_matches_shown(ctx) || ctx->possible_words.empty()) return;  // panel hidden → nothing to scroll
    ctx->selected_index += direction;
    if (ctx->selected_index < 0) ctx->selected_index = 0;
    if (ctx->selected_index >= (int)ctx->possible_words.size()) ctx->selected_index = (int)ctx->possible_words.size() - 1;
    kb_flash_side_button(direction < 0 ? ctx->arrow_up : ctx->arrow_down);
    seed_mnemonic_entry_render_matches(ctx);
}

// Deliver the chosen word to the host (hardware KEY2 / touch CHECK). One
// chokepoint for both input modes: set the textarea first for visual feedback,
// then hand the word to the host hook.
void seed_mnemonic_entry_accept_word(seed_mnemonic_entry_ctx_t *ctx, int word_index) {
    if (word_index < 0 || word_index >= (int)ctx->wordlist.size()) return;
    const std::string &word = ctx->wordlist[word_index];
    if (ctx->text_entry && lv_obj_is_valid(ctx->text_entry)) lv_textarea_set_text(ctx->text_entry, word.c_str());
    seedsigner_lvgl_on_text_entered(word.c_str());
}

// ---------------------------------------------------------------------------
// Event callbacks
// ---------------------------------------------------------------------------

// Click on a key (joystick ENTER or touch tap): lock in a letter / delete.
void seed_mnemonic_entry_value_changed_cb(lv_event_t *e) {
    lv_obj_t *matrix = lv_event_get_target_obj(e);
    seed_mnemonic_entry_ctx_t *ctx = (seed_mnemonic_entry_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;
    uint32_t button_id = lv_buttonmatrix_get_selected_button(matrix);
    if (button_id == LV_BUTTONMATRIX_BUTTON_NONE) return;
    const char *text = lv_buttonmatrix_get_button_text(matrix, button_id);
    if (!text) return;
    if (std::strcmp(text, SeedSignerIconConstants::DELETE) == 0) { seed_mnemonic_entry_delete(ctx); return; }
    if (std::strlen(text) == 1 && text[0] >= 'a' && text[0] <= 'z') {
        char letter = text[0];
        if (ctx->possible_alphabet.find(letter) == std::string::npos) return;  // dimmed key
        seed_mnemonic_entry_lock_in(ctx, letter);
    }
}

// Hardware PREPROCESS key filter: the aux keys (scroll/accept) and the top-nav
// handoff. Read before the buttonmatrix moves its selection. Directional floats
// are handled by the POST filter (after the move) so they see the new selection.
void seed_mnemonic_entry_keyboard_preprocess_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_KEY) return;
    seed_mnemonic_entry_ctx_t *ctx = (seed_mnemonic_entry_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;
    uint32_t key = lv_event_get_key(e);

    int aux_index = nav_aux_key_index(key);   // shared KEY1/2/3 recognizer (navigation.h)
    if (aux_index == 1) { seed_mnemonic_entry_scroll(ctx, -1); lv_event_stop_processing(e); return; }
    if (aux_index == 3) { seed_mnemonic_entry_scroll(ctx, +1); lv_event_stop_processing(e); return; }
    if (aux_index == 2) {
        // Only accept when the panel is actually shown — otherwise possible_words is
        // populated (for dimming) but no highlighted candidate is visible to accept.
        if (seed_mnemonic_entry_matches_shown(ctx) && !ctx->possible_words.empty()) {
            seed_mnemonic_entry_accept_word(ctx, ctx->possible_words[ctx->selected_index]);
        }
        lv_event_stop_processing(e);
        return;
    }

    // UP on the top row hands focus up to the back button (the buttonmatrix does
    // not wrap UP off the top row).
    if (key == LV_KEY_UP) {
        uint32_t selected = lv_buttonmatrix_get_selected_button(ctx->matrix);
        if (selected != LV_BUTTONMATRIX_BUTTON_NONE && selected < kb_top_row_count(ctx->key_map.data())
            && ctx->back_button && lv_obj_is_valid(ctx->back_button)) {
            lv_group_focus_obj(ctx->back_button);
            lv_event_stop_processing(e);
        }
    }

    // LEFT/RIGHT (and DOWN) fall through to the buttonmatrix's default linear
    // cross-row move — unlike the other keyboard screens, this screen does not
    // route them through kb_handle_directional's same-row LEFT/RIGHT wrap
    // (flagged divergence in the conformance bug ledger; behavior kept unchanged
    // here).
}

// Hardware POST key filter: after the buttonmatrix has moved its selection,
// preview the now-selected key (live float). Ignores ENTER (a click → lock-in via
// VALUE_CHANGED) and any key while focus has handed off to the back button.
void seed_mnemonic_entry_keyboard_post_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_KEY) return;
    seed_mnemonic_entry_ctx_t *ctx = (seed_mnemonic_entry_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;
    uint32_t key = lv_event_get_key(e);
    if (key != LV_KEY_UP && key != LV_KEY_DOWN && key != LV_KEY_LEFT && key != LV_KEY_RIGHT) return;
    if (ctx->group && lv_group_get_focused(ctx->group) != ctx->matrix) return;  // handed off to back
    uint32_t selected = lv_buttonmatrix_get_selected_button(ctx->matrix);
    if (selected == LV_BUTTONMATRIX_BUTTON_NONE) return;
    seed_mnemonic_entry_float(ctx, lv_buttonmatrix_get_button_text(ctx->matrix, selected));
}

// Back button: DOWN returns focus to the keyboard matrix.
void seed_mnemonic_entry_back_key_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_KEY) return;
    seed_mnemonic_entry_ctx_t *ctx = (seed_mnemonic_entry_ctx_t *)lv_event_get_user_data(e);
    if (ctx) kb_back_down_to_matrix(e, ctx->matrix);
}

// Touch: tap a candidate to highlight it in place and enable the accept button.
void seed_mnemonic_entry_touch_candidate_cb(lv_event_t *e) {
    lv_obj_t *button = lv_event_get_target_obj(e);
    seed_mnemonic_entry_ctx_t *ctx = (seed_mnemonic_entry_ctx_t *)lv_event_get_user_data(e);
    if (!ctx || !ctx->candidate_list) return;
    int tapped_index = (int)(intptr_t)lv_obj_get_user_data(button);
    ctx->touch_selected = tapped_index;
    uint32_t child_count = lv_obj_get_child_count(ctx->candidate_list);
    for (uint32_t i = 0; i < child_count; ++i) {
        lv_obj_t *child = lv_obj_get_child(ctx->candidate_list, i);
        seed_mnemonic_entry_style_candidate(child, (int)i == tapped_index);
    }
    seed_mnemonic_entry_set_check_enabled(ctx, true);
}

// Touch: tap CHECK to accept the highlighted candidate.
void seed_mnemonic_entry_check_cb(lv_event_t *e) {
    seed_mnemonic_entry_ctx_t *ctx = (seed_mnemonic_entry_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;
    if (ctx->touch_selected < 0 || ctx->touch_selected >= (int)ctx->possible_words.size()) return;
    seed_mnemonic_entry_accept_word(ctx, ctx->possible_words[ctx->touch_selected]);
}

// LV_EVENT_DELETE teardown on the screen root: release the joystick group, then
// free the ctx.
void seed_mnemonic_entry_cleanup_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;
    seed_mnemonic_entry_ctx_t *ctx = (seed_mnemonic_entry_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;
    if (ctx->group) lv_group_del(ctx->group);
    delete ctx;
}

}  // namespace


void seed_mnemonic_entry_screen(void *ctx_json) {
    // --- Config ---

    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    // Input-mode resolve: cfg input.mode overrides the platform profile. This
    // resolve triple is keyboard-family wiring shared verbatim with
    // keyboard_screen / seed_add_passphrase_screen — slated for extraction at
    // the rollout decision (extraction ledger #16).
    bool has_mode_override = false;
    input_mode_t mode_override = INPUT_MODE_TOUCH;
    nav_mode_override_from_cfg(cfg, has_mode_override, mode_override);
    bool hardware = (has_mode_override ? mode_override : input_profile_get_mode()) == INPUT_MODE_HARDWARE;

    // Structural defaults (write-if-absent, never user-visible text; Python
    // BaseTopNavScreen defaults show_back_button=True / show_power_button=False)
    // + the required localized title (content comes from the host view layer).
    // NOTE: the required `wordlist` is validated further down, after the scaffold
    // and ctx exist — see the flag comment there.
    ensure_top_nav_structure(cfg, /*default_show_back_button=*/true,
                                  /*default_show_power_button=*/false);
    require_top_nav_title(cfg, "seed_mnemonic_entry_screen");

    // --- Scaffold ---

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, /*scrollable=*/false);
    lv_obj_t *body = screen.body;
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);

    const int32_t content_width  = lv_obj_get_content_width(body);
    const int32_t content_height = lv_obj_get_content_height(body);

    // --- Body ---

    // 1. Screen context + matching-state seed.

    seed_mnemonic_entry_ctx_t *ctx = new seed_mnemonic_entry_ctx_t();
    ctx->back_button = screen.top_back_btn;
    ctx->hardware = hardware;

    // wordlist (required). FLAGGED (conformance keeps behavior unchanged): this
    // validation runs AFTER the scaffold and ctx above were created, so the throw
    // path deletes the ctx but LEAKS the never-loaded scaffold screen tree (LVGL
    // objects are not freed by stack unwinding). Hoisting the check above the
    // scaffold would move a validation across LVGL calls, which conformance must
    // not do (the large_icon_status precedent) — left in place.
    if (!cfg.contains("wordlist") || !cfg["wordlist"].is_array() || cfg["wordlist"].empty()) {
        delete ctx;
        throw std::runtime_error("seed_mnemonic_entry_screen: wordlist is required and must be a non-empty array");
    }
    ctx->wordlist.reserve(cfg["wordlist"].size());
    for (const auto &word : cfg["wordlist"]) {
        if (word.is_string()) ctx->wordlist.push_back(word.get<std::string>());
    }

    // initial_letters: accept a string ("ap") or an array (["a","p"]); normalize to
    // a list of single non-space chars.
    if (cfg.contains("initial_letters")) {
        const auto &letters_cfg = cfg["initial_letters"];
        if (letters_cfg.is_string()) {
            for (char letter : letters_cfg.get<std::string>()) if (letter != ' ') ctx->letters.push_back(std::string(1, letter));
        } else if (letters_cfg.is_array()) {
            for (const auto &element : letters_cfg) {
                if (element.is_string()) {
                    std::string letter = element.get<std::string>();
                    if (!letter.empty() && letter != " ") ctx->letters.push_back(letter);
                }
            }
        }
    }

    // Mirror Python's __post_init__ seeding of the live slot: >1 letter locks them
    // all in (append a fresh live slot) and pre-computes the matches; exactly 1
    // letter sits in the live slot (no matches yet); 0 letters = a fresh slot.
    char initial_selected = 'a';
    if (ctx->letters.size() > 1) {
        initial_selected = ctx->letters.back()[0];
        ctx->letters.push_back(" ");
        seed_mnemonic_entry_calc_possible_alphabet(ctx);
    } else if (ctx->letters.size() == 1) {
        initial_selected = ctx->letters[0][0];
        ctx->possible_alphabet = "abcdefghijklmnopqrstuvwxyz";
    } else {
        ctx->letters.push_back(" ");
        ctx->possible_alphabet = "abcdefghijklmnopqrstuvwxyz";
    }

    // 2. Candidate-column geometry. Python sizes the column to the longest BIP-39
    //    word (8 chars, "mushroom") + 2*COMPONENT_PADDING and pins it to the right
    //    screen edge (matches_list_x = canvas_width - column_width). Reclaim the
    //    right EDGE_PADDING gutter so the column reaches the actual screen edge
    //    (content_width + EDGE_PADDING in body-local coords), pushing it as far
    //    right as possible.
    lv_point_t longest_word_size;
    lv_text_get_size(&longest_word_size, "mushroom", &CANDIDATE_FONT, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const int32_t matches_column_width = longest_word_size.x + 2 * COMPONENT_PADDING;
    const int32_t column_gap           = COMPONENT_PADDING;
    const int32_t matches_x            = content_width + EDGE_PADDING - matches_column_width;
    const int32_t keyboard_width       = matches_x - column_gap;

    // 3. Text-entry strip (source of truth for the in-progress word display),
    //    sized to the keyboard width.
    lv_obj_t *text_entry = kb_make_text_entry(body, keyboard_width, seedsigner_lvgl_is_static_render());
    lv_obj_set_width(text_entry, keyboard_width);
    ctx->text_entry = text_entry;

    // 4. Keyboard map: 5x6 a-z, DEL (width 2) on the last row, hidden fillers to
    //    keep the last row's columns aligned with the rows above (Python
    //    left-aligns y/z/DEL with blank space to the right).
    const char *delete_key_glyph = SeedSignerIconConstants::DELETE;
    ctx->key_storage.clear();
    for (int i = 0; i < 26; ++i) ctx->key_storage.push_back(std::string(1, (char)('a' + i)));
    ctx->key_map.clear();
    ctx->ctrl_map.clear();
    int column = 0;
    for (int i = 0; i < 26; ++i) {
        ctx->key_map.push_back(ctx->key_storage[i].c_str());
        ctx->ctrl_map.push_back((lv_buttonmatrix_ctrl_t)1);          // width-1 value key
        if (++column == 6) { ctx->key_map.push_back("\n"); column = 0; }   // 6 cols per row
    }
    // Row 5 already started after 'x' (24 letters = 4 full rows); y,z were appended
    // into the open row. Append DEL (width 2) + two hidden fillers to fill to 6.
    ctx->key_map.push_back(delete_key_glyph);
    ctx->ctrl_map.push_back((lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_NO_REPEAT
                       | LV_BUTTONMATRIX_CTRL_CLICK_TRIG | LV_BUTTONMATRIX_CTRL_CHECKED | 2));
    ctx->key_map.push_back(" ");
    ctx->ctrl_map.push_back((lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_HIDDEN | 1));
    ctx->key_map.push_back(" ");
    ctx->ctrl_map.push_back((lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_HIDDEN | 1));
    ctx->key_map.push_back("");

    lv_obj_t *matrix = lv_buttonmatrix_create(body);
    lv_buttonmatrix_set_map(matrix, ctx->key_map.data());
    lv_buttonmatrix_set_ctrl_map(matrix, ctx->ctrl_map.data());
    kb_style_matrix(matrix, &KEYBOARD_FONT);
    // Inactive keys are dimmed VISUALLY (seed_mnemonic_entry_dim_draw_cb), not via
    // DISABLED, so the joystick can still float over them (Python parity — see
    // seed_mnemonic_entry_apply_dimming). The matrix already sends draw-task events
    // (enabled by kb_style_matrix for its icon recolor), so just add our dimming
    // handler.
    lv_obj_add_event_cb(matrix, seed_mnemonic_entry_dim_draw_cb, LV_EVENT_DRAW_TASK_ADDED, ctx);
    // Orange selection OUTLINE on the focused/pressed key. For an ACTIVE selected
    // key the orange fill (from kb_style_matrix) dominates and the border just
    // rims it; for an INACTIVE selected key the dim draw-cb keeps the dark fill, so
    // only this border marks the cursor — Python's "inactive + selected = highlight
    // outline" behavior.
    const lv_state_t selection_border_states[] = {
        LV_STATE_PRESSED, LV_STATE_FOCUSED, LV_STATE_FOCUS_KEY,
    };
    for (lv_state_t state : selection_border_states) {
        lv_obj_set_style_border_color(matrix, lv_color_hex(ACCENT_COLOR), LV_PART_ITEMS | state);
        lv_obj_set_style_border_opa(matrix, LV_OPA_COVER, LV_PART_ITEMS | state);
        lv_obj_set_style_border_width(matrix, 2, LV_PART_ITEMS | state);
    }
    ctx->matrix = matrix;

    const int32_t keyboard_top = BUTTON_HEIGHT + COMPONENT_PADDING;
    lv_obj_set_size(matrix, keyboard_width, content_height - keyboard_top);
    lv_obj_align(matrix, LV_ALIGN_TOP_LEFT, 0, keyboard_top);
    lv_obj_add_event_cb(matrix, seed_mnemonic_entry_value_changed_cb, LV_EVENT_VALUE_CHANGED, ctx);

    // 5. Candidate panel (hardware fixed-slot column | touch scrollable list).
    //    Hardware soft-buttons (KEY1 up-scroll, KEY2 accept = the centered
    //    highlight, KEY3 down-scroll) sit at the SAME physical Y positions as the
    //    passphrase screen's KEY1/KEY2/KEY3 side panel, so they line up with the
    //    three physical buttons beside the screen; the scroll arrows overshoot the
    //    right edge (clipped) to reinforce that connection — exactly like
    //    seed_add_passphrase_screen. The candidate list runs the full body height
    //    behind the arrows: Python shows up to 3 matches above + 7 below the
    //    centered highlight (highlighted_row=3, num_possible_rows=11), overflowing
    //    past the arrows and clipping at the screen edge, with the arrows drawn ON
    //    TOP.
    const int32_t line_height = (int32_t)lv_font_get_line_height(&CANDIDATE_FONT);
    // Row pitch ≈ Python's matches_list_row_height (word cap height + padding).
    const int32_t row_height       = line_height * 3 / 4 + COMPONENT_PADDING / 2;
    const int32_t highlight_height = (int32_t)(BUTTON_HEIGHT * 3 / 4);

    if (hardware) {
        // Passphrase-aligned anchor slots (body-local), from the shared
        // kb_side_panel_geometry: the KEY2 slot is a BUTTON_HEIGHT box centered on
        // the full screen; KEY1/KEY3 are one `spacing` step above/below it.
        const kb_side_panel_geometry_t side_panel =
            kb_side_panel_geometry(lv_obj_get_height(screen.screen), content_width);
        const int32_t key1_top         = side_panel.key2_y - side_panel.spacing;
        const int32_t key3_top         = side_panel.key2_y + side_panel.spacing;
        const int32_t highlight_center = side_panel.key2_y + BUTTON_HEIGHT / 2;

        // Dimmed candidate rows: up to 3 above + 7 below the highlight, spaced by
        // row_height off its center. They overflow past the arrows and clip at the
        // body edge — created FIRST so the highlight and (last) the arrows draw on
        // top.
        for (int i = 0; i < 3; ++i) {
            lv_obj_t *label = lv_label_create(body);
            lv_obj_set_style_text_font(label, &CANDIDATE_FONT, LV_PART_MAIN);
            lv_obj_set_style_text_color(label, lv_color_hex(0xdddddd), LV_PART_MAIN);
            lv_obj_set_pos(label, matches_x + COMPONENT_PADDING, highlight_center - (i + 1) * row_height - line_height / 2);
            seed_mnemonic_entry_set_hidden(label, true);
            ctx->dim_above.push_back(label);
        }
        for (int i = 0; i < 7; ++i) {
            lv_obj_t *label = lv_label_create(body);
            lv_obj_set_style_text_font(label, &CANDIDATE_FONT, LV_PART_MAIN);
            lv_obj_set_style_text_color(label, lv_color_hex(0xdddddd), LV_PART_MAIN);
            lv_obj_set_pos(label, matches_x + COMPONENT_PADDING, highlight_center + (i + 1) * row_height - line_height / 2);
            seed_mnemonic_entry_set_hidden(label, true);
            ctx->dim_below.push_back(label);
        }

        // Highlight slot (KEY2): the selected candidate, centered in the KEY2 slot,
        // left-aligned fixed-width word in the (on-screen) matches column.
        // The highlight runs COMPONENT_PADDING off the right screen edge on purpose
        // (Python's matches_list_highlight_button width = column + COMPONENT_PADDING):
        // its rounded right corner is clipped at the edge, reinforcing — like the
        // overshooting arrows — that this is the joystick-mode selection.
        lv_obj_t *highlight = lv_obj_create(body);
        lv_obj_set_size(highlight, matches_column_width + COMPONENT_PADDING, highlight_height);
        lv_obj_set_pos(highlight, matches_x, highlight_center - highlight_height / 2);
        lv_obj_set_style_bg_color(highlight, lv_color_hex(ACCENT_COLOR), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(highlight, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(highlight, BUTTON_RADIUS / 2, LV_PART_MAIN);
        lv_obj_set_style_border_width(highlight, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(highlight, 0, LV_PART_MAIN);
        lv_obj_remove_flag(highlight, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
        lv_obj_t *highlight_label = lv_label_create(highlight);
        lv_obj_set_style_text_font(highlight_label, &CANDIDATE_FONT, LV_PART_MAIN);
        lv_obj_set_style_text_color(highlight_label, lv_color_hex(BUTTON_SELECTED_FONT_COLOR), LV_PART_MAIN);
        lv_obj_align(highlight_label, LV_ALIGN_LEFT_MID, COMPONENT_PADDING, 0);
        ctx->highlight_button = highlight;
        ctx->highlight_label = highlight_label;

        // Scroll arrows (KEY1/KEY3), created LAST so they draw OVER the candidate
        // rows (Python parity), centered in their slots and overshooting the right
        // screen edge exactly like the passphrase side panel (same w / x / clipped,
        // all from the shared kb_side_panel_geometry).
        const int32_t arrow_height     = (int32_t)(BUTTON_HEIGHT * 3 / 4);
        const int32_t arrow_slot_inset = (BUTTON_HEIGHT - arrow_height) / 2;   // center within the slot
        ctx->arrow_up = kb_side_button(body, side_panel.x, key1_top + arrow_slot_inset,
                                       side_panel.panel_w, arrow_height,
                                       SeedSignerIconConstants::CHEVRON_UP, &ICON_FONT__SEEDSIGNER,
                                       BODY_FONT_COLOR, side_panel.clipped, /*out_label=*/nullptr);
        ctx->arrow_down = kb_side_button(body, side_panel.x, key3_top + arrow_slot_inset,
                                         side_panel.panel_w, arrow_height,
                                         SeedSignerIconConstants::CHEVRON_DOWN, &ICON_FONT__SEEDSIGNER,
                                         BODY_FONT_COLOR, side_panel.clipped, /*out_label=*/nullptr);
    } else {
        // Touch: a scrollable candidate list above a persistent green CHECK button.
        const int32_t check_height = highlight_height;
        lv_obj_t *list = lv_obj_create(body);
        lv_obj_set_pos(list, matches_x, 0);
        lv_obj_set_size(list, matches_column_width, content_height - check_height - COMPONENT_PADDING);
        lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(list, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(list, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_row(list, COMPONENT_PADDING / 2, LV_PART_MAIN);
        lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_scroll_dir(list, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
        ctx->candidate_list = list;

        lv_obj_t *check_button = lv_obj_create(body);
        lv_obj_set_size(check_button, matches_column_width, check_height);
        lv_obj_set_pos(check_button, matches_x, content_height - check_height);
        lv_obj_set_style_radius(check_button, BUTTON_RADIUS / 2, LV_PART_MAIN);
        lv_obj_set_style_border_width(check_button, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(check_button, 0, LV_PART_MAIN);
        lv_obj_remove_flag(check_button, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *check_label = lv_label_create(check_button);
        lv_label_set_text(check_label, SeedSignerIconConstants::CHECK);
        lv_obj_set_style_text_font(check_label, &ICON_FONT__SEEDSIGNER, LV_PART_MAIN);
        lv_obj_center(check_label);
        lv_obj_add_event_cb(check_button, seed_mnemonic_entry_check_cb, LV_EVENT_CLICKED, ctx);
        ctx->check_button = check_button;
        seed_mnemonic_entry_set_check_enabled(ctx, false);
    }

    // 6. Initial view sync: dimming + the in-progress word + the candidate matches.
    seed_mnemonic_entry_apply_dimming(ctx);
    seed_mnemonic_entry_update_display(ctx);
    seed_mnemonic_entry_render_matches(ctx);

    // Optional: pre-select one of the current candidate words. Mainly for static
    // screenshots of the touch accept affordance (the selection + enabled CHECK is
    // otherwise only reachable by tapping at runtime). Applied after the initial
    // render so the candidate widgets already exist. No-op if the word isn't a
    // current match.
    std::string preselect = cfg.value("initial_selected_word", std::string());
    if (!preselect.empty()) {
        int match_index = -1;
        for (int i = 0; i < (int)ctx->possible_words.size(); ++i) {
            if (ctx->wordlist[ctx->possible_words[i]] == preselect) { match_index = i; break; }
        }
        if (match_index >= 0) {
            if (hardware) {
                ctx->selected_index = match_index;
                seed_mnemonic_entry_render_matches(ctx);
            } else {
                ctx->touch_selected = match_index;
                if (ctx->candidate_list && match_index < (int)lv_obj_get_child_count(ctx->candidate_list)) {
                    seed_mnemonic_entry_style_candidate(lv_obj_get_child(ctx->candidate_list, match_index), true);
                }
                seed_mnemonic_entry_set_check_enabled(ctx, true);
            }
        }
    }

    // --- Navigation + load ---

    // Touch panels have no joystick cursor: a key is chosen by tapping it directly,
    // so NO key is pre-selected on load (the pre-highlighted letter is purely a
    // joystick affordance). LVGL already defaults a buttonmatrix to BUTTON_NONE, but
    // set it explicitly so touch is guaranteed to start blank regardless of any
    // platform default — the hardware branch below installs the cursor on
    // initial_selected. (Mirrors button_list_screen, where initial selection is
    // hardware-mode only.)
    lv_buttonmatrix_set_selected_button(matrix, LV_BUTTONMATRIX_BUTTON_NONE);

    if (hardware) {
        // Hardware group binding (matrix + back button, key filters, indev
        // connect): keyboard-family wiring shared with keyboard_screen /
        // seed_add_passphrase_screen — slated for extraction into keyboard_core
        // at the rollout decision (extraction ledger #16).
        ctx->group = lv_group_create();
        lv_group_set_wrap(ctx->group, false);
        lv_group_add_obj(ctx->group, matrix);
        if (screen.top_back_btn) {
            lv_group_add_obj(ctx->group, screen.top_back_btn);
            lv_obj_add_event_cb(matrix, seed_mnemonic_entry_keyboard_preprocess_cb,
                                (lv_event_code_t)(LV_EVENT_KEY | LV_EVENT_PREPROCESS), ctx);
            lv_obj_add_event_cb(matrix, seed_mnemonic_entry_keyboard_post_cb, LV_EVENT_KEY, ctx);
            lv_obj_add_event_cb(screen.top_back_btn, seed_mnemonic_entry_back_key_cb, LV_EVENT_KEY, ctx);
        }
        lv_group_focus_obj(matrix);
        kb_connect_indevs(ctx->group);

        int initial_button_index = kb_find_button(ctx->key_map.data(), initial_selected);
        if (initial_button_index < 0) initial_button_index = 0;
        lv_buttonmatrix_set_selected_button(matrix, (uint32_t)initial_button_index);
        if (seedsigner_lvgl_is_static_render()) lv_obj_add_state(matrix, LV_STATE_PRESSED);
    }

    lv_obj_add_event_cb(screen.screen, seed_mnemonic_entry_cleanup_cb, LV_EVENT_DELETE, ctx);
    load_screen_and_cleanup_previous(screen.screen);
}
