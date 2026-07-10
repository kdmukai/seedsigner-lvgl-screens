// seed_add_passphrase_screen
//
// Python provenance: SeedAddPassphraseScreen (seed_screens.py)
//
// BIP-39 passphrase entry on a native lv_keyboard with fully custom keymaps.
// The user types into a one-line text-entry strip; CHECK (an in-grid OK key at
// >240 / 240 touch, the physical KEY3 side button at 240 joystick) commits the
// entered text to the host via seedsigner_lvgl_on_text_entered. The default
// lv_keyboard_def_event_cb is replaced with a custom VALUE_CHANGED handler so
// this screen owns the mode-switch key labels (the native handler would force
// its own "1#"-style labels).
//
// Keymap layout varies by profile height and input mode:
//   - >240 (320/480): full QWERTY plus a persistent digit row, with in-grid
//     controls (case shift / symbols / cursor / OK). One map set serves touch
//     and joystick — no side panel.
//   - 240 touch: QWERTY without the digit row (keys keep their width; digits
//     move to their own NUMBER-mode page), in-grid controls.
//   - 240 joystick (the Pi Zero): ALPHABETICAL letters (matching the Python
//     layout: predictable cursor stepping + wider keys), digits page, and NO
//     in-grid mode/OK keys — the physical KEY1/KEY2/KEY3 side panel switches
//     case/charset and confirms, mirroring Python's hw_button1/2/3 right panel.
//   Python ships only the 240 joystick layout (Pi Zero); the touch and >240
//   QWERTY layouts are this port's additions for the touch-panel targets.
//
// The keymap pages must union to EXACTLY the 94-character Python charset
// inventory; seed_add_passphrase_audit_charset re-verifies that invariant on
// every screen build and throws on drift.
//
// Shared keyboard mechanics (text-entry strip, matrix styling + per-key icon
// recolor, side-button factory + press flash, directional row-wrap and
// back-button handoff, indev group wiring) come from keyboard_core (kb_*).
//
// Lifecycle: Tier 2 (stateful) — one POD ctx (lv_malloc/lv_free) plus one
// LV_EVENT_DELETE cleanup callback on the screen root. This file is the spec's
// §6 rule-1 exemplar (validate-before-allocate): ALL cfg validation — the
// required-field throws and the charset audit — completes before the scaffold
// or ctx exists, so no throw path can leak LVGL objects.
//
// cfg:
//   top_nav.title             (string, required)     localized screen title
//            (Python: _("BIP-39 Passphrase")); host-supplied per the content policy.
//   top_nav.show_back_button  (bool, default true)   Python BaseTopNavScreen default.
//   top_nav.show_power_button (bool, default false)  Python BaseTopNavScreen default.
//   initial_text      (string, optional)  prefill for the text entry (Python:
//            passphrase=""); the cursor lands after the last character.
//   max_length        (int, optional)     textarea max length; applied only when > 0.
//   initial_mode      (string, optional)  starting keymap page: "upper" | "digits" |
//            "symbols"; anything else (or absent) -> lowercase letters. "digits"
//            falls back to lowercase at >240, where digits have no separate page.
//   input.mode        (string, optional)  "touch" | "hardware" — per-screen
//            input-mode override (nav_mode_override_from_cfg, screen_helpers);
//            absent -> the global input profile.
//   allow_screensaver (bool, default true)  read by the parse/scaffold layer,
//            which stamps the screensaver opt-out flag onto the screen root.

#include "screen_scaffold.h"  // parse_screen_json_ctx, create_top_nav_screen_scaffold, load_screen_and_cleanup_previous
#include "seedsigner.h"       // seed_add_passphrase_screen decl, screen_scaffold_t, seedsigner_lvgl_on_text_entered, seedsigner_lvgl_is_static_render
#include "gui_constants.h"    // BUTTON_HEIGHT, COMPONENT_PADDING, EDGE_PADDING, KEYBOARD_FONT, ICON_FONT__SEEDSIGNER, BUTTON_FONT_COLOR, SUCCESS_COLOR, SeedSignerIconConstants, active_profile
#include "input_profile.h"    // input_mode_t, INPUT_MODE_TOUCH/HARDWARE, input_profile_get_mode
#include "keyboard_core.h"    // kb_make_text_entry, kb_style_matrix, kb_side_button, kb_flash_side_button, kb_handle_directional, kb_back_down_to_matrix, kb_connect_indevs, kb_find_button, kb_side_panel_geometry
#include "navigation.h"       // nav_aux_key_index (KEY1/KEY2/KEY3 recognizer)
#include "screen_helpers.h"   // ensure_top_nav_structure, require_top_nav_title, nav_mode_override_from_cfg

#include "lvgl.h"             // lv_keyboard / lv_buttonmatrix / lv_textarea / lv_group + events

#include <nlohmann/json.hpp>  // json (cfg reads + structural-default writes)

#include <cstring>            // std::strcmp / std::strlen (key-label dispatch, charset audit)
#include <set>                // std::set (charset audit)
#include <stdexcept>          // std::runtime_error (validation throws)
#include <string>             // std::string

using json = nlohmann::json;

// Exact character inventory — source of truth mirrored from the Python app
// (SeedAddPassphraseScreen.__post_init__ in seed_screens.py). These five
// disjoint sets total 94 distinct printable characters; the keymaps below must
// reproduce exactly this set (audited at screen build time).
static const char *SEED_ADD_PASSPHRASE_CHARSET_LOWER  = "abcdefghijklmnopqrstuvwxyz";
static const char *SEED_ADD_PASSPHRASE_CHARSET_UPPER  = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
static const char *SEED_ADD_PASSPHRASE_CHARSET_DIGITS = "0123456789";
static const char *SEED_ADD_PASSPHRASE_CHARSET_SYM1   = "!@#$%&();:,.-+='\"?";
static const char *SEED_ADD_PASSPHRASE_CHARSET_SYM2   = "^*[]{}_\\|<>/`~";

// Button-matrix control entries. `KBW(n)` is a plain character key of relative
// width n; `KBC(n)` adds the control-key flags (NO_REPEAT | CLICK_TRIG |
// CHECKED) that give the non-typing keys — the mode-switch labels below plus
// the backspace / cursor / OK icon glyphs — their control-key press behavior
// and CHECKED (dark) styling; the wide space key stays a plain `KBW` key.
// Casts are required because `lv_buttonmatrix_ctrl_t` is an enum and C++
// (unlike LVGL's own C maps) won't implicitly narrow int → enum.
//
// COUNT INVARIANT: each ctrl array must contain exactly one entry per key in
// its map (the "\n" row separators and the "" terminator take none). The
// pairing is purely positional and a count mismatch compiles silently — when
// editing any map below, hand-count its keys against its ctrl array.
#define KBW(n) ((lv_buttonmatrix_ctrl_t)(n))
#define KBC(n) ((lv_buttonmatrix_ctrl_t)(LV_KEYBOARD_CTRL_BUTTON_FLAGS | (n)))

// Mode-switch key labels. Our custom keyboard handler
// (seed_add_passphrase_kb_value_changed_cb) maps each to a target mode, so we
// choose readable labels (the native def_event_cb, which forces
// "abc"/"ABC"/"1#", is removed): ABC→uppercase, abc→lowercase, 123→digits,
// !?&→symbols.
#define SYM_LABEL "!?&"
#define NUM_LABEL "123"
#define ABC_LABEL "abc"
#define UPPER_LABEL "ABC"

// ---------------------------------------------------------------------------
// QWERTY maps — used at >240px height (320/480), where there is room for a full
// 10-wide QWERTY plus a persistent digit row, so digits need no separate page.
// In-grid controls (shift / symbols / OK); these serve both touch and (the
// unlikely) joystick input at >240, which has no side panel.
// ---------------------------------------------------------------------------

// Lowercase letters page: digit row, QWERTY letters, shift-to-uppercase ("ABC"),
// backspace, symbols toggle ("!?&"), cursor keys, space, and OK.
static const char * const seed_add_passphrase_kb_map_lower[] = {
    "1","2","3","4","5","6","7","8","9","0","\n",
    "q","w","e","r","t","y","u","i","o","p","\n",
    "a","s","d","f","g","h","j","k","l","\n",
    UPPER_LABEL,"z","x","c","v","b","n","m",SeedSignerIconConstants::DELETE,"\n",
    SYM_LABEL,SeedSignerIconConstants::SPACE,SeedSignerIconConstants::CHEVRON_LEFT,SeedSignerIconConstants::CHEVRON_RIGHT,SeedSignerIconConstants::CHECK,""
};
static const lv_buttonmatrix_ctrl_t seed_add_passphrase_kb_ctrl_lower[] = {
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBC(2),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBC(2),
    KBC(2),KBW(6),KBC(1),KBC(1),KBC(2)
};

// Uppercase letters page: mirror of lowercase; "abc" shifts back to lowercase.
static const char * const seed_add_passphrase_kb_map_upper[] = {
    "1","2","3","4","5","6","7","8","9","0","\n",
    "Q","W","E","R","T","Y","U","I","O","P","\n",
    "A","S","D","F","G","H","J","K","L","\n",
    ABC_LABEL,"Z","X","C","V","B","N","M",SeedSignerIconConstants::DELETE,"\n",
    SYM_LABEL,SeedSignerIconConstants::SPACE,SeedSignerIconConstants::CHEVRON_LEFT,SeedSignerIconConstants::CHEVRON_RIGHT,SeedSignerIconConstants::CHECK,""
};
static const lv_buttonmatrix_ctrl_t seed_add_passphrase_kb_ctrl_upper[] = {
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBC(2),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBC(2),
    KBC(2),KBW(6),KBC(1),KBC(1),KBC(2)
};

// Symbols page (LV_KEYBOARD_MODE_SPECIAL): all 32 symbols on one page; "abc"
// returns to the lowercase letters page.
static const char * const seed_add_passphrase_kb_map_special[] = {
    "!","@","#","$","%","&","(",")","\n",
    ";",":",",",".","-","+","=","'","\n",
    "\"","?","^","*","[","]","{","}","\n",
    "_","\\","|","<",">","/","`","~","\n",
    "abc",SeedSignerIconConstants::SPACE,SeedSignerIconConstants::CHEVRON_LEFT,SeedSignerIconConstants::CHEVRON_RIGHT,SeedSignerIconConstants::DELETE,SeedSignerIconConstants::CHECK,""
};
static const lv_buttonmatrix_ctrl_t seed_add_passphrase_kb_ctrl_special[] = {
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBC(2),KBW(4),KBC(1),KBC(1),KBC(2),KBC(2)
};

// ---------------------------------------------------------------------------
// 240px maps — digits get their OWN page (NUMBER mode) rather than a top row
// that shrinks the letters (matching the Python rationale). Letter ORDERING
// depends on input mode (the user's call): touch favors QWERTY (thumb-typing
// muscle memory), joystick favors ALPHABETICAL for two reasons:
//   1. Navigation: stepping a joystick cursor to a known letter is easier when
//      the letters are in a predictable A-Z order than scattered QWERTY.
//   2. Key size: a small 240px screen doesn't have the WIDTH for QWERTY, whose
//      top row needs 10 columns (q-w-e-r-t-y-u-i-o-p) and so squeezes the keys
//      narrow. Alphabetical lets us choose the column count — 26 letters over
//      4 rows of ~7 columns — trading a row of height (which we have) for key
//      width (which we don't). This is also why the joystick keyboard matches
//      the Pi Zero Python layout, which is alphabetical for the same reason.
// Pages: letters (lower/upper), digits, symbols — cycled by 123 / !?& / abc.
//
//   *_240    — touch: QWERTY letters, in-grid mode/OK control keys.
//   *_240hw  — joystick: alphabetical letters, NO in-grid mode/OK keys (the side
//              KEY1/KEY2/KEY3 panel switches charset + confirms); cursor / space
//              / backspace stay in-grid.
// ---------------------------------------------------------------------------

// --- 240 touch (QWERTY, in-grid controls) ---
static const char * const seed_add_passphrase_kb_map_lower_240[] = {
    "q","w","e","r","t","y","u","i","o","p","\n",
    "a","s","d","f","g","h","j","k","l","\n",
    UPPER_LABEL,"z","x","c","v","b","n","m",SeedSignerIconConstants::DELETE,"\n",
    NUM_LABEL,SeedSignerIconConstants::SPACE,SeedSignerIconConstants::CHEVRON_LEFT,SeedSignerIconConstants::CHEVRON_RIGHT,SeedSignerIconConstants::CHECK,""
};
static const lv_buttonmatrix_ctrl_t seed_add_passphrase_kb_ctrl_lower_240[] = {
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBC(2),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBC(2),
    KBC(2),KBW(3),KBC(1),KBC(1),KBC(2)
};

static const char * const seed_add_passphrase_kb_map_upper_240[] = {
    "Q","W","E","R","T","Y","U","I","O","P","\n",
    "A","S","D","F","G","H","J","K","L","\n",
    ABC_LABEL,"Z","X","C","V","B","N","M",SeedSignerIconConstants::DELETE,"\n",
    NUM_LABEL,SeedSignerIconConstants::SPACE,SeedSignerIconConstants::CHEVRON_LEFT,SeedSignerIconConstants::CHEVRON_RIGHT,SeedSignerIconConstants::CHECK,""
};
static const lv_buttonmatrix_ctrl_t seed_add_passphrase_kb_ctrl_upper_240[] = {
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBC(2),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBC(2),
    KBC(2),KBW(3),KBC(1),KBC(1),KBC(2)
};

static const char * const seed_add_passphrase_kb_map_digits_240[] = {
    "1","2","3","4","\n",
    "5","6","7","8","\n",
    "9","0",SeedSignerIconConstants::DELETE,"\n",
    ABC_LABEL,SYM_LABEL,SeedSignerIconConstants::SPACE,SeedSignerIconConstants::CHEVRON_LEFT,SeedSignerIconConstants::CHEVRON_RIGHT,SeedSignerIconConstants::CHECK,""
};
static const lv_buttonmatrix_ctrl_t seed_add_passphrase_kb_ctrl_digits_240[] = {
    KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBC(2),
    KBC(2),KBC(2),KBW(3),KBC(1),KBC(1),KBC(2)
};

static const char * const seed_add_passphrase_kb_map_symbols_240[] = {
    "!","@","#","$","%","&","(",")","\n",
    ";",":",",",".","-","+","=","'","\n",
    "\"","?","^","*","[","]","{","}","\n",
    "_","\\","|","<",">","/","`","~","\n",
    ABC_LABEL,NUM_LABEL,SeedSignerIconConstants::SPACE,SeedSignerIconConstants::CHEVRON_LEFT,SeedSignerIconConstants::CHEVRON_RIGHT,SeedSignerIconConstants::DELETE,SeedSignerIconConstants::CHECK,""
};
static const lv_buttonmatrix_ctrl_t seed_add_passphrase_kb_ctrl_symbols_240[] = {
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBC(2),KBC(2),KBW(3),KBC(1),KBC(1),KBC(2),KBC(2)
};

// --- 240 joystick (no in-grid mode/OK keys; side panel handles those) ---
static const char * const seed_add_passphrase_kb_map_lower_240hw[] = {
    "a","b","c","d","e","f","g","\n",
    "h","i","j","k","l","m","n","\n",
    "o","p","q","r","s","t","u","\n",
    "v","w","x","y","z",SeedSignerIconConstants::DELETE,"\n",
    SeedSignerIconConstants::SPACE,SeedSignerIconConstants::CHEVRON_LEFT,SeedSignerIconConstants::CHEVRON_RIGHT,""
};
static const lv_buttonmatrix_ctrl_t seed_add_passphrase_kb_ctrl_lower_240hw[] = {
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBC(2),
    KBW(4),KBC(1),KBC(1)
};

static const char * const seed_add_passphrase_kb_map_upper_240hw[] = {
    "A","B","C","D","E","F","G","\n",
    "H","I","J","K","L","M","N","\n",
    "O","P","Q","R","S","T","U","\n",
    "V","W","X","Y","Z",SeedSignerIconConstants::DELETE,"\n",
    SeedSignerIconConstants::SPACE,SeedSignerIconConstants::CHEVRON_LEFT,SeedSignerIconConstants::CHEVRON_RIGHT,""
};
static const lv_buttonmatrix_ctrl_t seed_add_passphrase_kb_ctrl_upper_240hw[] = {
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBC(2),
    KBW(4),KBC(1),KBC(1)
};

static const char * const seed_add_passphrase_kb_map_digits_240hw[] = {
    "1","2","3","4","\n",
    "5","6","7","8","\n",
    "9","0",SeedSignerIconConstants::DELETE,"\n",
    SeedSignerIconConstants::SPACE,SeedSignerIconConstants::CHEVRON_LEFT,SeedSignerIconConstants::CHEVRON_RIGHT,""
};
static const lv_buttonmatrix_ctrl_t seed_add_passphrase_kb_ctrl_digits_240hw[] = {
    KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBC(2),
    KBW(4),KBC(1),KBC(1)
};

static const char * const seed_add_passphrase_kb_map_symbols_240hw[] = {
    "!","@","#","$","%","&","(",")","\n",
    ";",":",",",".","-","+","=","'","\n",
    "\"","?","^","*","[","]","{","}","\n",
    "_","\\","|","<",">","/","`","~","\n",
    SeedSignerIconConstants::SPACE,SeedSignerIconConstants::CHEVRON_LEFT,SeedSignerIconConstants::CHEVRON_RIGHT,SeedSignerIconConstants::DELETE,""
};
static const lv_buttonmatrix_ctrl_t seed_add_passphrase_kb_ctrl_symbols_240hw[] = {
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(5),KBC(1),KBC(1),KBC(2)
};

namespace {

// Verify (once, at screen build) that the union of single-character keys across
// the given maps is exactly the 94-char Python inventory — no additions, no
// omissions, no duplicates. Guards against silent drift if a map is edited.
// Run for whichever map set (touch or hardware) the screen is about to use.
void seed_add_passphrase_audit_charset(const char * const * const *maps, size_t map_count) {
    std::set<char> expected;
    for (const char *charset : { SEED_ADD_PASSPHRASE_CHARSET_LOWER, SEED_ADD_PASSPHRASE_CHARSET_UPPER,
                                 SEED_ADD_PASSPHRASE_CHARSET_DIGITS, SEED_ADD_PASSPHRASE_CHARSET_SYM1,
                                 SEED_ADD_PASSPHRASE_CHARSET_SYM2 }) {
        for (; *charset; ++charset) expected.insert(*charset);
    }

    std::set<char> actual;
    for (size_t i = 0; i < map_count; ++i) {
        const char * const *map = maps[i];
        for (size_t j = 0; map[j][0] != '\0'; ++j) {
            // Single-byte tokens that aren't the row separator or space are the
            // typeable characters; multi-byte LV_SYMBOL_* glyphs and the
            // "abc"/"ABC"/"1#" mode labels are >1 byte and thus skipped.
            if (std::strlen(map[j]) == 1 && map[j][0] != '\n' && map[j][0] != ' ') {
                actual.insert(map[j][0]);
            }
        }
    }

    if (actual != expected) {
        throw std::runtime_error("seed_add_passphrase_screen: keyboard charset does not match the Python inventory");
    }
}

// Hardware/joystick screen state. Lives for the screen's lifetime; freed in
// seed_add_passphrase_cleanup_cb. Holds what the KEY1/KEY2/KEY3 handlers need
// plus the remembered letter case for returning from the symbols page.
// Allocation idiom: pure POD (raw LVGL pointers + an enum, no C++ members), so
// lv_malloc + lv_memzero + lv_free is correct; a C++ member (string/vector/map)
// would require new/delete for ctor/dtor safety.
struct seed_add_passphrase_ctx_t {
    lv_obj_t          *keyboard;
    lv_obj_t          *text_area;
    lv_obj_t          *back_button;
    lv_obj_t          *key1_label;    // KEY1 (case toggle) label
    lv_obj_t          *key2_label;    // KEY2 (symbols toggle) label
    lv_obj_t          *key1_button;   // KEY1/2/3 side buttons, for the press flash
    lv_obj_t          *key2_button;
    lv_obj_t          *key3_button;
    lv_group_t        *group;
    lv_keyboard_mode_t letter_mode;   // LOWER/UPPER to restore when leaving symbols
};

// Default joystick selection when arriving at a page fresh — a central key, so
// the first move is short on average: 'k' on the alphabetical letter grid, '6'
// on the digit pad, '.' on the symbol page. Returns a button index (0 if the
// chosen key isn't found in the current map).
uint32_t seed_add_passphrase_default_key(lv_obj_t *keyboard, lv_keyboard_mode_t mode) {
    char target_char = 0;
    // The letter layouts differ: the 240 joystick keyboard is alphabetical
    // (a-g / h-n / ...), so 'k' is its center; the QWERTY keyboards (240 touch
    // and the larger profiles) center on 'g' — the middle of the home row.
    // Detect by whether 'a'/'A' is the first key.
    const char * const *map = lv_keyboard_get_map_array(keyboard);
    bool alphabetical = (kb_find_button(map, 'a') == 0) ||
                        (kb_find_button(map, 'A') == 0);
    switch (mode) {
        case LV_KEYBOARD_MODE_TEXT_LOWER: target_char = alphabetical ? 'k' : 'g'; break;
        case LV_KEYBOARD_MODE_TEXT_UPPER: target_char = alphabetical ? 'K' : 'G'; break;
        case LV_KEYBOARD_MODE_NUMBER:     target_char = '6'; break;
        case LV_KEYBOARD_MODE_SPECIAL:    target_char = '.'; break;
        default: break;
    }
    int button_index = target_char ? kb_find_button(map, target_char) : -1;
    return button_index >= 0 ? (uint32_t)button_index : 0;
}

// Refresh the KEY1/KEY2 side-panel labels to reflect what pressing them does
// next (mirrors the Python passphrase screen's right-panel labels).
void seed_add_passphrase_update_labels(seed_add_passphrase_ctx_t *ctx) {
    lv_keyboard_mode_t mode = lv_keyboard_get_mode(ctx->keyboard);
    bool on_letters = (mode == LV_KEYBOARD_MODE_TEXT_LOWER || mode == LV_KEYBOARD_MODE_TEXT_UPPER);
    if (ctx->key1_label) {
        // KEY1: on letters, the case to switch TO; otherwise, back to letters.
        const char *key1_text = !on_letters ? ABC_LABEL
                                : (mode == LV_KEYBOARD_MODE_TEXT_UPPER ? ABC_LABEL : UPPER_LABEL);
        lv_label_set_text(ctx->key1_label, key1_text);
    }
    if (ctx->key2_label) {
        // KEY2 cycles digits <-> symbols: on digits show "!?&", otherwise "123".
        lv_label_set_text(ctx->key2_label, (mode == LV_KEYBOARD_MODE_NUMBER) ? SYM_LABEL : NUM_LABEL);
    }
}

// Switch the keyboard charset/page. lv_keyboard_set_mode leaves btn_id_sel
// unchanged. Lowercase and uppercase share the same layout, so a case swap keeps
// the active key (pressing shift on "g" lands on "G"). For any other page
// (digits/symbols) the index could fall past the end of the shorter map — no
// visible active key, and the joystick would have to wander to find it — so we
// reset the selection to the first key.
void seed_add_passphrase_switch_mode(seed_add_passphrase_ctx_t *ctx, lv_keyboard_mode_t mode) {
    lv_keyboard_mode_t old_mode = lv_keyboard_get_mode(ctx->keyboard);
    bool case_swap =
        (old_mode == LV_KEYBOARD_MODE_TEXT_LOWER || old_mode == LV_KEYBOARD_MODE_TEXT_UPPER) &&
        (mode     == LV_KEYBOARD_MODE_TEXT_LOWER || mode     == LV_KEYBOARD_MODE_TEXT_UPPER);
    lv_keyboard_set_mode(ctx->keyboard, mode);
    if (!case_swap) {
        lv_buttonmatrix_set_selected_button(ctx->keyboard,
                                            seed_add_passphrase_default_key(ctx->keyboard, mode));
    }
}

// KEY1 — on a letters page, toggle case; on the digits/symbols page, return to
// the remembered letters page.
void seed_add_passphrase_key1_case(seed_add_passphrase_ctx_t *ctx) {
    lv_keyboard_mode_t mode = lv_keyboard_get_mode(ctx->keyboard);
    if (mode == LV_KEYBOARD_MODE_TEXT_LOWER || mode == LV_KEYBOARD_MODE_TEXT_UPPER) {
        ctx->letter_mode = (mode == LV_KEYBOARD_MODE_TEXT_UPPER)
                           ? LV_KEYBOARD_MODE_TEXT_LOWER : LV_KEYBOARD_MODE_TEXT_UPPER;
    }
    seed_add_passphrase_switch_mode(ctx, ctx->letter_mode);
    seed_add_passphrase_update_labels(ctx);
}

// KEY2 — cycle letters → digits → symbols → digits (the way back to letters is
// KEY1). Remembers the letters case when leaving a letters page.
void seed_add_passphrase_key2_cycle(seed_add_passphrase_ctx_t *ctx) {
    lv_keyboard_mode_t mode = lv_keyboard_get_mode(ctx->keyboard);
    if (mode == LV_KEYBOARD_MODE_NUMBER) {
        seed_add_passphrase_switch_mode(ctx, LV_KEYBOARD_MODE_SPECIAL);
    } else if (mode == LV_KEYBOARD_MODE_SPECIAL) {
        seed_add_passphrase_switch_mode(ctx, LV_KEYBOARD_MODE_NUMBER);
    } else {
        ctx->letter_mode = mode;
        seed_add_passphrase_switch_mode(ctx, LV_KEYBOARD_MODE_NUMBER);
    }
    seed_add_passphrase_update_labels(ctx);
}

// Hardware-mode key filter on the keyboard. Handles the KEY1/KEY2/KEY3 aux keys
// (case / symbols / confirm) and the top-nav handoff: when the selection is on
// the top row and UP is pressed, focus the back button. The buttonmatrix does
// not wrap UP off the top row, so this is the seam between the keyboard's
// internal navigation and the top-nav zone.
void seed_add_passphrase_kb_key_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_KEY) return;
    seed_add_passphrase_ctx_t *ctx = (seed_add_passphrase_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;

    uint32_t key = lv_event_get_key(e);

    // KEY1/KEY2/KEY3 (via the shared nav_aux_key_index recognizer, navigation.h)
    // only act when the side panel is present (240 joystick). At >240 there is no
    // panel — switching/confirm is via the in-grid mode/OK keys.
    if (ctx->key2_label) {
        int aux_index = nav_aux_key_index(key);
        if (aux_index == 1) {
            kb_flash_side_button(ctx->key1_button);
            seed_add_passphrase_key1_case(ctx);
            return;
        }
        if (aux_index == 2) {
            kb_flash_side_button(ctx->key2_button);
            seed_add_passphrase_key2_cycle(ctx);
            return;
        }
        if (aux_index == 3) {
            kb_flash_side_button(ctx->key3_button);
            if (ctx->text_area && lv_obj_is_valid(ctx->text_area)) {
                seedsigner_lvgl_on_text_entered(lv_textarea_get_text(ctx->text_area));
            }
            return;
        }
    }

    // The remaining directional keys (UP top-row → back-button handoff, LEFT/RIGHT
    // row-wrap) are the generic keyboard navigation, shared via keyboard_core.
    kb_handle_directional(e, lv_keyboard_get_map_array(ctx->keyboard), ctx->keyboard, ctx->back_button);
}

// Hardware-mode key filter on the back button: DOWN returns focus to the
// keyboard.
void seed_add_passphrase_back_key_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_KEY) return;
    seed_add_passphrase_ctx_t *ctx = (seed_add_passphrase_ctx_t *)lv_event_get_user_data(e);
    if (ctx) kb_back_down_to_matrix(e, ctx->keyboard);
}

// Tear down the group + ctx when the screen is destroyed (mirrors
// nav_cleanup_handler / screensaver_cleanup_handler). lv_group_del clears the
// indev's group pointer so no stale reference survives the screen swap.
void seed_add_passphrase_cleanup_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;
    seed_add_passphrase_ctx_t *ctx = (seed_add_passphrase_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;
    if (ctx->group) {
        lv_group_del(ctx->group);
    }
    lv_free(ctx);
}

// Custom keyboard handler, installed in place of lv_keyboard_def_event_cb so we
// control the in-grid control-key labels (notably the symbols toggle, which the
// native handler would force to "1#"). Handles touch-mode in-grid control keys
// plus character insertion / backspace / cursor for both modes.
void seed_add_passphrase_kb_value_changed_cb(lv_event_t *e) {
    lv_obj_t *keyboard = lv_event_get_target_obj(e);
    seed_add_passphrase_ctx_t *ctx = (seed_add_passphrase_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;

    uint32_t button_id = lv_keyboard_get_selected_button(keyboard);
    if (button_id == LV_BUTTONMATRIX_BUTTON_NONE) return;
    const char *text = lv_keyboard_get_button_text(keyboard, button_id);
    if (!text) return;

    lv_obj_t *text_area = ctx->text_area;

    // In-grid mode-switch keys (present in the touch maps). Each label maps to a
    // target mode; the custom handler owns this since def_event_cb was removed.
    if (std::strcmp(text, UPPER_LABEL) == 0) {
        ctx->letter_mode = LV_KEYBOARD_MODE_TEXT_UPPER;
        seed_add_passphrase_switch_mode(ctx, LV_KEYBOARD_MODE_TEXT_UPPER);
        seed_add_passphrase_update_labels(ctx);
        return;
    }
    if (std::strcmp(text, ABC_LABEL) == 0) {
        ctx->letter_mode = LV_KEYBOARD_MODE_TEXT_LOWER;
        seed_add_passphrase_switch_mode(ctx, LV_KEYBOARD_MODE_TEXT_LOWER);
        seed_add_passphrase_update_labels(ctx);
        return;
    }
    if (std::strcmp(text, NUM_LABEL) == 0) {
        seed_add_passphrase_switch_mode(ctx, LV_KEYBOARD_MODE_NUMBER);
        seed_add_passphrase_update_labels(ctx);
        return;
    }
    if (std::strcmp(text, SYM_LABEL) == 0) {
        seed_add_passphrase_switch_mode(ctx, LV_KEYBOARD_MODE_SPECIAL);
        seed_add_passphrase_update_labels(ctx);
        return;
    }
    if (std::strcmp(text, SeedSignerIconConstants::CHECK) == 0) {
        if (text_area && lv_obj_is_valid(text_area)) {
            seedsigner_lvgl_on_text_entered(lv_textarea_get_text(text_area));
        }
        return;
    }

    if (!text_area || !lv_obj_is_valid(text_area)) return;

    // Editing keys + character insertion (both modes).
    if (std::strcmp(text, SeedSignerIconConstants::DELETE) == 0) { lv_textarea_delete_char(text_area); return; }
    if (std::strcmp(text, SeedSignerIconConstants::CHEVRON_LEFT) == 0)  { lv_textarea_cursor_left(text_area); return; }
    if (std::strcmp(text, SeedSignerIconConstants::CHEVRON_RIGHT) == 0) { lv_textarea_cursor_right(text_area); return; }
    // The space key displays the seedsigner space glyph; insert a real space.
    if (std::strcmp(text, SeedSignerIconConstants::SPACE) == 0) { lv_textarea_add_text(text_area, " "); return; }
    lv_textarea_add_text(text_area, text);
}

// Parse cfg["initial_mode"] into a starting keyboard mode (for screenshots /
// deep-links into a specific charset page). Defaults to lowercase letters.
lv_keyboard_mode_t seed_add_passphrase_initial_mode(const json &cfg) {
    if (cfg.contains("initial_mode") && cfg["initial_mode"].is_string()) {
        std::string mode = cfg["initial_mode"].get<std::string>();
        if (mode == "upper")   return LV_KEYBOARD_MODE_TEXT_UPPER;
        if (mode == "digits")  return LV_KEYBOARD_MODE_NUMBER;
        if (mode == "symbols") return LV_KEYBOARD_MODE_SPECIAL;
    }
    return LV_KEYBOARD_MODE_TEXT_LOWER;
}

}  // namespace


void seed_add_passphrase_screen(void *ctx_json) {
    // --- Config ---

    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    // Structural defaults (write-if-absent, never user-visible text). Python
    // BaseTopNavScreen defaults: show_back_button=True, show_power_button=False
    // (the same implicit defaults the scaffold applies). The localized title is
    // content and must come from the host (Python: _("BIP-39 Passphrase")).
    ensure_top_nav_structure(cfg, /*default_show_back_button=*/true,
                                  /*default_show_power_button=*/false);
    require_top_nav_title(cfg, "seed_add_passphrase_screen");

    // Resolve input mode early — it selects letter ordering and the layout.
    bool has_mode_override = false;
    input_mode_t mode_override = INPUT_MODE_TOUCH;
    nav_mode_override_from_cfg(cfg, has_mode_override, mode_override);
    bool hardware = (has_mode_override ? mode_override : input_profile_get_mode()) == INPUT_MODE_HARDWARE;

    // Layout decisions:
    //  - is_240: at 240px there is no room for a number row, so digits get their
    //    own page (keeps letter keys from shrinking); >240 keeps the number row.
    //  - use_side_panel: only at 240 + joystick — the Pi Zero, the one device
    //    with physical KEY1/KEY2/KEY3. Larger screens never show the panel.
    const bool is_240 = (active_profile().height == 240);
    const bool use_side_panel = hardware && is_240;
    const bool has_digits_page = is_240;

    // Keymap set:
    //  - >240 (touch or joystick): QWERTY + number row, in-grid controls.
    //  - 240 touch:   QWERTY (no number row) + digits page, in-grid controls.
    //  - 240 joystick: alphabetical (no number row) + digits page, NO in-grid
    //                  controls (the side panel switches charset + confirms).
    const char * const *map_lower;
    const char * const *map_upper;
    const char * const *map_special;
    const char * const *map_digits = nullptr;
    const lv_buttonmatrix_ctrl_t *ctrl_lower;
    const lv_buttonmatrix_ctrl_t *ctrl_upper;
    const lv_buttonmatrix_ctrl_t *ctrl_special;
    const lv_buttonmatrix_ctrl_t *ctrl_digits = nullptr;
    if (!is_240) {
        map_lower = seed_add_passphrase_kb_map_lower;       ctrl_lower = seed_add_passphrase_kb_ctrl_lower;
        map_upper = seed_add_passphrase_kb_map_upper;       ctrl_upper = seed_add_passphrase_kb_ctrl_upper;
        map_special = seed_add_passphrase_kb_map_special;   ctrl_special = seed_add_passphrase_kb_ctrl_special;
    } else if (use_side_panel) {
        map_lower = seed_add_passphrase_kb_map_lower_240hw;     ctrl_lower = seed_add_passphrase_kb_ctrl_lower_240hw;
        map_upper = seed_add_passphrase_kb_map_upper_240hw;     ctrl_upper = seed_add_passphrase_kb_ctrl_upper_240hw;
        map_digits = seed_add_passphrase_kb_map_digits_240hw;   ctrl_digits = seed_add_passphrase_kb_ctrl_digits_240hw;
        map_special = seed_add_passphrase_kb_map_symbols_240hw; ctrl_special = seed_add_passphrase_kb_ctrl_symbols_240hw;
    } else {
        map_lower = seed_add_passphrase_kb_map_lower_240;     ctrl_lower = seed_add_passphrase_kb_ctrl_lower_240;
        map_upper = seed_add_passphrase_kb_map_upper_240;     ctrl_upper = seed_add_passphrase_kb_ctrl_upper_240;
        map_digits = seed_add_passphrase_kb_map_digits_240;   ctrl_digits = seed_add_passphrase_kb_ctrl_digits_240;
        map_special = seed_add_passphrase_kb_map_symbols_240; ctrl_special = seed_add_passphrase_kb_ctrl_symbols_240;
    }

    // Fail fast if a future edit ever breaks the exact-inventory guarantee.
    // Last validation step: everything that can throw runs BEFORE the scaffold
    // and ctx below exist, so no throw path can leak.
    if (has_digits_page) {
        const char * const * audit_maps[] = { map_lower, map_upper, map_digits, map_special };
        seed_add_passphrase_audit_charset(audit_maps, 4);
    } else {
        const char * const * audit_maps[] = { map_lower, map_upper, map_special };
        seed_add_passphrase_audit_charset(audit_maps, 3);
    }

    // --- Scaffold ---

    // No button_list: the body is custom (textarea + keyboard). upper_body == body.
    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, /*scrollable=*/false);

    // --- Body ---

    // 1. Non-scrolling body + the width budget. The shared KEY1/KEY2/KEY3
    //    side-panel geometry (kb_side_panel_geometry) is computed unconditionally
    //    because panel_width also feeds the keyboard/text-entry strip width; the
    //    panel itself is only built when use_side_panel.
    lv_obj_t *body = screen.body;
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);

    const int32_t content_width  = lv_obj_get_content_width(body);
    const int32_t content_height = lv_obj_get_content_height(body);

    const kb_side_panel_geometry_t side_panel =
        kb_side_panel_geometry(lv_obj_get_height(screen.screen), content_width);

    //    Reserve a right-side strip for the KEY1/KEY2/KEY3 panel only when it is
    //    shown (240 + joystick). Python's right_panel_buttons_width = 56, scaled
    //    per profile. The text strip and keyboard occupy the remaining width.
    const int32_t panel_width = use_side_panel ? side_panel.panel_w : 0;
    //    The side panel deliberately runs off the right screen edge to line up
    //    with the three physical Waveshare buttons (mirrors Python, whose hw
    //    buttons overshoot canvas_width by COMPONENT_PADDING). Reclaiming the
    //    right EDGE_PADDING gutter widens the keyboard + text-entry strip by
    //    that much.
    const int32_t main_width = use_side_panel ? (content_width - panel_width + EDGE_PADDING) : content_width;

    // 2. Per-screen state (both modes; POD — see the ctx struct). Freed in
    //    seed_add_passphrase_cleanup_cb.
    seed_add_passphrase_ctx_t *ctx = (seed_add_passphrase_ctx_t *)lv_malloc(sizeof(seed_add_passphrase_ctx_t));
    lv_memzero(ctx, sizeof(*ctx));

    // 3. Text-entry strip: the shared one-line, cursor-styled SeedSigner box
    //    (Python: text_entry_display at the top of the body).
    lv_obj_t *text_area = kb_make_text_entry(body, main_width, seedsigner_lvgl_is_static_render());

    std::string initial_text;
    if (cfg.contains("initial_text") && cfg["initial_text"].is_string()) {
        initial_text = cfg["initial_text"].get<std::string>();
        lv_textarea_set_text(text_area, initial_text.c_str());
    }
    if (cfg.contains("max_length") && cfg["max_length"].is_number_integer()) {
        int max_length = cfg["max_length"].get<int>();
        if (max_length > 0) {
            lv_textarea_set_max_length(text_area, (uint32_t)max_length);
        }
    }
    lv_textarea_set_cursor_pos(text_area, LV_TEXTAREA_CURSOR_LAST);

    // 4. Native keyboard with our custom maps. Replace the default event handler
    //    with ours so we control the control-key labels (e.g. the symbols toggle).
    lv_obj_t *keyboard = lv_keyboard_create(body);
    lv_obj_remove_event_cb(keyboard, lv_keyboard_def_event_cb);
    lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_TEXT_LOWER, map_lower, ctrl_lower);
    lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_TEXT_UPPER, map_upper, ctrl_upper);
    lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_SPECIAL, map_special, ctrl_special);
    if (has_digits_page) {
        lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_NUMBER, map_digits, ctrl_digits);
    }
    lv_keyboard_set_textarea(keyboard, text_area);
    kb_style_matrix(keyboard, &KEYBOARD_FONT);

    lv_keyboard_mode_t start_mode = seed_add_passphrase_initial_mode(cfg);
    // The digits page only exists at 240; elsewhere fall back to letters.
    if (start_mode == LV_KEYBOARD_MODE_NUMBER && !has_digits_page) {
        start_mode = LV_KEYBOARD_MODE_TEXT_LOWER;
    }
    lv_keyboard_set_mode(keyboard, start_mode);

    ctx->keyboard = keyboard;
    ctx->text_area = text_area;
    ctx->back_button = screen.top_back_btn;
    ctx->letter_mode = (start_mode == LV_KEYBOARD_MODE_TEXT_UPPER)
                       ? LV_KEYBOARD_MODE_TEXT_UPPER : LV_KEYBOARD_MODE_TEXT_LOWER;

    lv_obj_add_event_cb(keyboard, seed_add_passphrase_kb_value_changed_cb, LV_EVENT_VALUE_CHANGED, ctx);

    const int32_t keyboard_top_y = BUTTON_HEIGHT + COMPONENT_PADDING;
    const int32_t keyboard_height = content_height - keyboard_top_y;

    //    Keyboard fills the area left of any reserved panel strip (main_width ==
    //    content_width when there is no panel). lv_obj_align (not set_pos):
    //    lv_keyboard_create sets a default BOTTOM_MID anchor in its constructor,
    //    so a bare set_pos would offset from the bottom; TOP_LEFT resets the
    //    anchor.
    lv_obj_set_size(keyboard, main_width, keyboard_height);
    lv_obj_align(keyboard, LV_ALIGN_TOP_LEFT, 0, keyboard_top_y);

    // 5. KEY1/KEY2/KEY3 side panel (240 joystick only). The indicator buttons'
    //    vertical positions must line up with the device's physical buttons; the
    //    shared kb_side_panel_geometry replicates the Python
    //    SeedAddPassphraseScreen layout (KEY2 centered on the full canvas,
    //    KEY1/KEY3 one `spacing` step above/below, the strip overshooting the
    //    right screen edge by `clipped`).
    if (use_side_panel) {
        const int32_t side_button_height = BUTTON_HEIGHT;

        // ABC/123 use the fixed-width keyboard font (Inconsolata), matching the
        // keys and the Python side panel (FIXED_WIDTH_EMPHASIS_FONT_NAME), not the
        // OpenSans body button font.
        ctx->key1_button = kb_side_button(body, side_panel.x, side_panel.key2_y - side_panel.spacing,
                               side_panel.panel_w, side_button_height,
                               UPPER_LABEL, &KEYBOARD_FONT, BUTTON_FONT_COLOR,
                               side_panel.clipped, &ctx->key1_label);
        ctx->key2_button = kb_side_button(body, side_panel.x, side_panel.key2_y,
                               side_panel.panel_w, side_button_height,
                               NUM_LABEL, &KEYBOARD_FONT, BUTTON_FONT_COLOR,
                               side_panel.clipped, &ctx->key2_label);
        ctx->key3_button = kb_side_button(body, side_panel.x, side_panel.key2_y + side_panel.spacing,
                               side_panel.panel_w, side_button_height,
                               SeedSignerIconConstants::CHECK, &ICON_FONT__SEEDSIGNER, SUCCESS_COLOR,
                               side_panel.clipped, NULL);
        seed_add_passphrase_update_labels(ctx);
    }

    // --- Navigation + load ---

    if (hardware) {
        // Localized LVGL-native focus (this screen does NOT use
        // bind_screen_navigation): the keyboard is the group-focused object so
        // the buttonmatrix's own directional navigation drives key selection;
        // the back button is the other group member for the top-nav handoff.
        // Used at any size for joystick input; at >240 there is no side panel,
        // so the in-grid mode/OK keys (reached by navigating to them) do the
        // switching/confirming.
        // NOTE: this group-binding shape is shared with keyboard_screen /
        // seed_mnemonic_entry_screen — slated for extraction to keyboard_core at
        // the rollout decision (spec §10, cluster 16). Kept inline until then.
        ctx->group = lv_group_create();
        lv_group_set_wrap(ctx->group, false);
        lv_group_add_obj(ctx->group, keyboard);
        if (screen.top_back_btn) {
            lv_group_add_obj(ctx->group, screen.top_back_btn);
            // PREPROCESS: run before the buttonmatrix's own key handler so we read
            // the selection BEFORE it moves. Otherwise an UP from the 2nd row first
            // moves the selection into the top row, then our handler sees a top-row
            // selection and wrongly jumps to the back button.
            lv_obj_add_event_cb(keyboard, seed_add_passphrase_kb_key_cb,
                                (lv_event_code_t)(LV_EVENT_KEY | LV_EVENT_PREPROCESS), ctx);
            lv_obj_add_event_cb(screen.top_back_btn, seed_add_passphrase_back_key_cb, LV_EVENT_KEY, ctx);
        }
        lv_group_focus_obj(keyboard);

        // Connect all keypad/encoder indevs to this group.
        kb_connect_indevs(ctx->group);

        // Pre-select an initial key so the joystick selection is visible from the
        // start. Otherwise btn_id_sel is NONE and it takes an arrow press just to
        // "enter" the keyboard, with no visible cursor until then. Prefer the
        // last-typed key (prefilled, e.g. the "i" of satoshi), else the page's
        // central default key (k / 6). The highlight shows via the focused-key
        // style in live use; static-render (screenshots) has no indev to apply
        // that focus state, so add PRESSED to make the highlight show in the still.
        int selected_index = initial_text.empty()
                             ? -1
                             : kb_find_button(lv_keyboard_get_map_array(keyboard), initial_text.back());
        if (selected_index < 0) selected_index = (int)seed_add_passphrase_default_key(keyboard, start_mode);
        lv_buttonmatrix_set_selected_button(keyboard, (uint32_t)selected_index);
        if (seedsigner_lvgl_is_static_render()) {
            lv_obj_add_state(keyboard, LV_STATE_PRESSED);
        }
    }

    // Free ctx (+ group, if any) when the screen is destroyed.
    lv_obj_add_event_cb(screen.screen, seed_add_passphrase_cleanup_cb, LV_EVENT_DELETE, ctx);

    load_screen_and_cleanup_previous(screen.screen);
}
