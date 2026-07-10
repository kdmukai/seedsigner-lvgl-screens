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

// Exact character inventory — source of truth mirrored from the Python app
// (seedsigner/src/seedsigner/gui/screens/seed_screens.py:664-673). These five
// disjoint sets total 94 distinct printable characters; the keymaps below must
// reproduce exactly this set (audited at screen build time).
static const char *PASSPHRASE_CHARSET_LOWER  = "abcdefghijklmnopqrstuvwxyz";
static const char *PASSPHRASE_CHARSET_UPPER  = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
static const char *PASSPHRASE_CHARSET_DIGITS = "0123456789";
static const char *PASSPHRASE_CHARSET_SYM1   = "!@#$%&();:,.-+='\"?";
static const char *PASSPHRASE_CHARSET_SYM2   = "^*[]{}_\\|<>/`~";

// Button-matrix control entries. `KBW(n)` is a plain character key of relative
// width n; `KBC(n)` adds the control-key flags (NO_REPEAT | CLICK_TRIG |
// CHECKED) for the control labels below — "abc"/"ABC" (case shift), "1#"
// (symbols), and the LV_SYMBOL_* glyphs — which `lv_keyboard_def_event_cb`
// handles natively. Casts are required because `lv_buttonmatrix_ctrl_t` is an
// enum and C++ (unlike LVGL's own C maps) won't implicitly narrow int → enum.
#define KBW(n) ((lv_buttonmatrix_ctrl_t)(n))
#define KBC(n) ((lv_buttonmatrix_ctrl_t)(LV_KEYBOARD_CTRL_BUTTON_FLAGS | (n)))
// Hidden spacer key: reserves a row's height but is not drawn or navigable. Used
// to add an "empty row" so a short page (digits) keeps normal key proportions
// instead of stretching to fill the keyboard height.
#define KBH(n) ((lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_HIDDEN | LV_BUTTONMATRIX_CTRL_DISABLED | (n)))

// Mode-switch key labels. Our custom keyboard handler
// (passphrase_kb_value_changed) maps each to a target mode, so we choose
// readable labels (the native def_event_cb, which forces "abc"/"ABC"/"1#", is
// removed): ABC→uppercase, abc→lowercase, 123→digits, !?&→symbols.
#define SYM_LABEL "!?&"
#define NUM_LABEL "123"
#define ABC_LABEL "abc"
#define UPPER_LABEL "ABC"

// ===========================================================================
// QWERTY maps — used at >240px height (320/480), where there is room for a full
// 10-wide QWERTY plus a persistent digit row, so digits need no separate page.
// In-grid controls (shift / symbols / OK); these serve both touch and (the
// unlikely) joystick input at >240, which has no side panel.
// ===========================================================================

// Lowercase letters page: digit row, QWERTY letters, shift-to-uppercase ("ABC"),
// backspace, symbols toggle ("!?&"), cursor keys, space, and OK.
static const char * const passphrase_kb_map_lower[] = {
    "1","2","3","4","5","6","7","8","9","0","\n",
    "q","w","e","r","t","y","u","i","o","p","\n",
    "a","s","d","f","g","h","j","k","l","\n",
    UPPER_LABEL,"z","x","c","v","b","n","m",SeedSignerIconConstants::DELETE,"\n",
    SYM_LABEL,SeedSignerIconConstants::SPACE,SeedSignerIconConstants::CHEVRON_LEFT,SeedSignerIconConstants::CHEVRON_RIGHT,SeedSignerIconConstants::CHECK,""
};
static const lv_buttonmatrix_ctrl_t passphrase_kb_ctrl_lower[] = {
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBC(2),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBC(2),
    KBC(2),KBW(6),KBC(1),KBC(1),KBC(2)
};

// Uppercase letters page: mirror of lowercase; "abc" shifts back to lowercase.
static const char * const passphrase_kb_map_upper[] = {
    "1","2","3","4","5","6","7","8","9","0","\n",
    "Q","W","E","R","T","Y","U","I","O","P","\n",
    "A","S","D","F","G","H","J","K","L","\n",
    ABC_LABEL,"Z","X","C","V","B","N","M",SeedSignerIconConstants::DELETE,"\n",
    SYM_LABEL,SeedSignerIconConstants::SPACE,SeedSignerIconConstants::CHEVRON_LEFT,SeedSignerIconConstants::CHEVRON_RIGHT,SeedSignerIconConstants::CHECK,""
};
static const lv_buttonmatrix_ctrl_t passphrase_kb_ctrl_upper[] = {
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBC(2),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBC(2),
    KBC(2),KBW(6),KBC(1),KBC(1),KBC(2)
};

// Symbols page (LV_KEYBOARD_MODE_SPECIAL): all 32 symbols on one page; "abc"
// returns to the lowercase letters page.
static const char * const passphrase_kb_map_special[] = {
    "!","@","#","$","%","&","(",")","\n",
    ";",":",",",".","-","+","=","'","\n",
    "\"","?","^","*","[","]","{","}","\n",
    "_","\\","|","<",">","/","`","~","\n",
    "abc",SeedSignerIconConstants::SPACE,SeedSignerIconConstants::CHEVRON_LEFT,SeedSignerIconConstants::CHEVRON_RIGHT,SeedSignerIconConstants::DELETE,SeedSignerIconConstants::CHECK,""
};
static const lv_buttonmatrix_ctrl_t passphrase_kb_ctrl_special[] = {
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBC(2),KBW(4),KBC(1),KBC(1),KBC(2),KBC(2)
};

// ===========================================================================
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
// ===========================================================================

// --- 240 touch (QWERTY, in-grid controls) ---
static const char * const passphrase_kb_map_lower_240[] = {
    "q","w","e","r","t","y","u","i","o","p","\n",
    "a","s","d","f","g","h","j","k","l","\n",
    UPPER_LABEL,"z","x","c","v","b","n","m",SeedSignerIconConstants::DELETE,"\n",
    NUM_LABEL,SeedSignerIconConstants::SPACE,SeedSignerIconConstants::CHEVRON_LEFT,SeedSignerIconConstants::CHEVRON_RIGHT,SeedSignerIconConstants::CHECK,""
};
static const lv_buttonmatrix_ctrl_t passphrase_kb_ctrl_lower_240[] = {
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBC(2),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBC(2),
    KBC(2),KBW(3),KBC(1),KBC(1),KBC(2)
};

static const char * const passphrase_kb_map_upper_240[] = {
    "Q","W","E","R","T","Y","U","I","O","P","\n",
    "A","S","D","F","G","H","J","K","L","\n",
    ABC_LABEL,"Z","X","C","V","B","N","M",SeedSignerIconConstants::DELETE,"\n",
    NUM_LABEL,SeedSignerIconConstants::SPACE,SeedSignerIconConstants::CHEVRON_LEFT,SeedSignerIconConstants::CHEVRON_RIGHT,SeedSignerIconConstants::CHECK,""
};
static const lv_buttonmatrix_ctrl_t passphrase_kb_ctrl_upper_240[] = {
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBC(2),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBC(2),
    KBC(2),KBW(3),KBC(1),KBC(1),KBC(2)
};

static const char * const passphrase_kb_map_digits_240[] = {
    "1","2","3","4","\n",
    "5","6","7","8","\n",
    "9","0",SeedSignerIconConstants::DELETE,"\n",
    ABC_LABEL,SYM_LABEL,SeedSignerIconConstants::SPACE,SeedSignerIconConstants::CHEVRON_LEFT,SeedSignerIconConstants::CHEVRON_RIGHT,SeedSignerIconConstants::CHECK,""
};
static const lv_buttonmatrix_ctrl_t passphrase_kb_ctrl_digits_240[] = {
    KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBC(2),
    KBC(2),KBC(2),KBW(3),KBC(1),KBC(1),KBC(2)
};

static const char * const passphrase_kb_map_symbols_240[] = {
    "!","@","#","$","%","&","(",")","\n",
    ";",":",",",".","-","+","=","'","\n",
    "\"","?","^","*","[","]","{","}","\n",
    "_","\\","|","<",">","/","`","~","\n",
    ABC_LABEL,NUM_LABEL,SeedSignerIconConstants::SPACE,SeedSignerIconConstants::CHEVRON_LEFT,SeedSignerIconConstants::CHEVRON_RIGHT,SeedSignerIconConstants::DELETE,SeedSignerIconConstants::CHECK,""
};
static const lv_buttonmatrix_ctrl_t passphrase_kb_ctrl_symbols_240[] = {
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBC(2),KBC(2),KBW(3),KBC(1),KBC(1),KBC(2),KBC(2)
};

// --- 240 joystick (no in-grid mode/OK keys; side panel handles those) ---
static const char * const passphrase_kb_map_lower_240hw[] = {
    "a","b","c","d","e","f","g","\n",
    "h","i","j","k","l","m","n","\n",
    "o","p","q","r","s","t","u","\n",
    "v","w","x","y","z",SeedSignerIconConstants::DELETE,"\n",
    SeedSignerIconConstants::SPACE,SeedSignerIconConstants::CHEVRON_LEFT,SeedSignerIconConstants::CHEVRON_RIGHT,""
};
static const lv_buttonmatrix_ctrl_t passphrase_kb_ctrl_lower_240hw[] = {
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBC(2),
    KBW(4),KBC(1),KBC(1)
};

static const char * const passphrase_kb_map_upper_240hw[] = {
    "A","B","C","D","E","F","G","\n",
    "H","I","J","K","L","M","N","\n",
    "O","P","Q","R","S","T","U","\n",
    "V","W","X","Y","Z",SeedSignerIconConstants::DELETE,"\n",
    SeedSignerIconConstants::SPACE,SeedSignerIconConstants::CHEVRON_LEFT,SeedSignerIconConstants::CHEVRON_RIGHT,""
};
static const lv_buttonmatrix_ctrl_t passphrase_kb_ctrl_upper_240hw[] = {
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBC(2),
    KBW(4),KBC(1),KBC(1)
};

static const char * const passphrase_kb_map_digits_240hw[] = {
    "1","2","3","4","\n",
    "5","6","7","8","\n",
    "9","0",SeedSignerIconConstants::DELETE,"\n",
    SeedSignerIconConstants::SPACE,SeedSignerIconConstants::CHEVRON_LEFT,SeedSignerIconConstants::CHEVRON_RIGHT,""
};
static const lv_buttonmatrix_ctrl_t passphrase_kb_ctrl_digits_240hw[] = {
    KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBC(2),
    KBW(4),KBC(1),KBC(1)
};

static const char * const passphrase_kb_map_symbols_240hw[] = {
    "!","@","#","$","%","&","(",")","\n",
    ";",":",",",".","-","+","=","'","\n",
    "\"","?","^","*","[","]","{","}","\n",
    "_","\\","|","<",">","/","`","~","\n",
    SeedSignerIconConstants::SPACE,SeedSignerIconConstants::CHEVRON_LEFT,SeedSignerIconConstants::CHEVRON_RIGHT,SeedSignerIconConstants::DELETE,""
};
static const lv_buttonmatrix_ctrl_t passphrase_kb_ctrl_symbols_240hw[] = {
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),KBW(1),
    KBW(5),KBC(1),KBC(1),KBC(2)
};

// Verify (once, at screen build) that the union of single-character keys across
// the given maps is exactly the 94-char Python inventory — no additions, no
// omissions, no duplicates. Guards against silent drift if a map is edited.
// Run for whichever map set (touch or hardware) the screen is about to use.
static void audit_passphrase_charset(const char * const * const *maps, size_t map_count) {
    std::set<char> expected;
    for (const char *s : { PASSPHRASE_CHARSET_LOWER, PASSPHRASE_CHARSET_UPPER,
                           PASSPHRASE_CHARSET_DIGITS, PASSPHRASE_CHARSET_SYM1,
                           PASSPHRASE_CHARSET_SYM2 }) {
        for (; *s; ++s) expected.insert(*s);
    }

    std::set<char> actual;
    for (size_t m = 0; m < map_count; ++m) {
        const char * const *map = maps[m];
        for (size_t i = 0; map[i][0] != '\0'; ++i) {
            // Single-byte tokens that aren't the row separator or space are the
            // typeable characters; multi-byte LV_SYMBOL_* glyphs and the
            // "abc"/"ABC"/"1#" mode labels are >1 byte and thus skipped.
            if (std::strlen(map[i]) == 1 && map[i][0] != '\n' && map[i][0] != ' ') {
                actual.insert(map[i][0]);
            }
        }
    }

    if (actual != expected) {
        throw std::runtime_error("passphrase keyboard charset does not match the Python inventory");
    }
}

// Hardware/joystick screen state. Lives for the screen's lifetime; freed in
// passphrase_cleanup_cb. Holds what the KEY1/KEY2/KEY3 handlers need plus the
// remembered letter case for returning from the symbols page.
typedef struct {
    lv_obj_t          *kb;
    lv_obj_t          *ta;
    lv_obj_t          *back_btn;
    lv_obj_t          *key1_label;   // KEY1 (case toggle) label
    lv_obj_t          *key2_label;   // KEY2 (symbols toggle) label
    lv_obj_t          *key1_btn;     // KEY1/2/3 side buttons, for the press flash
    lv_obj_t          *key2_btn;
    lv_obj_t          *key3_btn;
    lv_group_t        *group;
    lv_keyboard_mode_t letter_mode;  // LOWER/UPPER to restore when leaving symbols
} passphrase_ctx_t;

// The button-matrix map-inspection helpers (top-row count, row bounds, find
// button) now live in keyboard_core.h (kb_top_row_count / kb_row_bounds /
// kb_find_button), taking the map array directly. Passphrase passes its current
// page's map via lv_keyboard_get_map_array(kb).

// Default joystick selection when arriving at a page fresh — a central key, so
// the first move is short on average: 'k' on the alphabetical letter grid, '6'
// on the digit pad, '.' on the symbol page. Returns a button index (0 if the
// chosen key isn't found in the current map).
static uint32_t passphrase_default_key(lv_obj_t *kb, lv_keyboard_mode_t mode) {
    char ch = 0;
    // The letter layouts differ: the 240 joystick keyboard is alphabetical
    // (a-g / h-n / ...), so 'k' is its center; the QWERTY keyboards (240 touch
    // and the larger profiles) center on 'g' — the middle of the home row.
    // Detect by whether 'a'/'A' is the first key.
    const char * const *map = lv_keyboard_get_map_array(kb);
    bool alphabetical = (kb_find_button(map, 'a') == 0) ||
                        (kb_find_button(map, 'A') == 0);
    switch (mode) {
        case LV_KEYBOARD_MODE_TEXT_LOWER: ch = alphabetical ? 'k' : 'g'; break;
        case LV_KEYBOARD_MODE_TEXT_UPPER: ch = alphabetical ? 'K' : 'G'; break;
        case LV_KEYBOARD_MODE_NUMBER:     ch = '6'; break;
        case LV_KEYBOARD_MODE_SPECIAL:    ch = '.'; break;
        default: break;
    }
    int idx = ch ? kb_find_button(map, ch) : -1;
    return idx >= 0 ? (uint32_t)idx : 0;
}

// Refresh the KEY1/KEY2 side-panel labels to reflect what pressing them does
// next (mirrors the Python passphrase screen's right-panel labels).
static void passphrase_update_labels(passphrase_ctx_t *c) {
    lv_keyboard_mode_t m = lv_keyboard_get_mode(c->kb);
    bool on_letters = (m == LV_KEYBOARD_MODE_TEXT_LOWER || m == LV_KEYBOARD_MODE_TEXT_UPPER);
    if (c->key1_label) {
        // KEY1: on letters, the case to switch TO; otherwise, back to letters.
        const char *t = !on_letters ? ABC_LABEL
                        : (m == LV_KEYBOARD_MODE_TEXT_UPPER ? ABC_LABEL : UPPER_LABEL);
        lv_label_set_text(c->key1_label, t);
    }
    if (c->key2_label) {
        // KEY2 cycles digits <-> symbols: on digits show "!?&", otherwise "123".
        lv_label_set_text(c->key2_label, (m == LV_KEYBOARD_MODE_NUMBER) ? SYM_LABEL : NUM_LABEL);
    }
}

// Switch the keyboard charset/page. lv_keyboard_set_mode leaves btn_id_sel
// unchanged. Lowercase and uppercase share the same layout, so a case swap keeps
// the active key (pressing shift on "g" lands on "G"). For any other page
// (digits/symbols) the index could fall past the end of the shorter map — no
// visible active key, and the joystick would have to wander to find it — so we
// reset the selection to the first key.
static void passphrase_switch_mode(passphrase_ctx_t *c, lv_keyboard_mode_t mode) {
    lv_keyboard_mode_t old = lv_keyboard_get_mode(c->kb);
    bool case_swap =
        (old  == LV_KEYBOARD_MODE_TEXT_LOWER || old  == LV_KEYBOARD_MODE_TEXT_UPPER) &&
        (mode == LV_KEYBOARD_MODE_TEXT_LOWER || mode == LV_KEYBOARD_MODE_TEXT_UPPER);
    lv_keyboard_set_mode(c->kb, mode);
    if (!case_swap) {
        lv_buttonmatrix_set_selected_button(c->kb, passphrase_default_key(c->kb, mode));
    }
}

// KEY1 — on a letters page, toggle case; on the digits/symbols page, return to
// the remembered letters page.
static void passphrase_key1_case(passphrase_ctx_t *c) {
    lv_keyboard_mode_t m = lv_keyboard_get_mode(c->kb);
    if (m == LV_KEYBOARD_MODE_TEXT_LOWER || m == LV_KEYBOARD_MODE_TEXT_UPPER) {
        c->letter_mode = (m == LV_KEYBOARD_MODE_TEXT_UPPER)
                         ? LV_KEYBOARD_MODE_TEXT_LOWER : LV_KEYBOARD_MODE_TEXT_UPPER;
    }
    passphrase_switch_mode(c, c->letter_mode);
    passphrase_update_labels(c);
}

// KEY2 — cycle letters → digits → symbols → digits (the way back to letters is
// KEY1). Remembers the letters case when leaving a letters page.
static void passphrase_key2_cycle(passphrase_ctx_t *c) {
    lv_keyboard_mode_t m = lv_keyboard_get_mode(c->kb);
    if (m == LV_KEYBOARD_MODE_NUMBER) {
        passphrase_switch_mode(c, LV_KEYBOARD_MODE_SPECIAL);
    } else if (m == LV_KEYBOARD_MODE_SPECIAL) {
        passphrase_switch_mode(c, LV_KEYBOARD_MODE_NUMBER);
    } else {
        c->letter_mode = m;
        passphrase_switch_mode(c, LV_KEYBOARD_MODE_NUMBER);
    }
    passphrase_update_labels(c);
}

// The side-button press-flash (kb_flash_side_button) now lives in keyboard_core.

// Hardware-mode key filter on the keyboard. Handles the KEY1/KEY2/KEY3 aux keys
// (case / symbols / confirm) and the top-nav handoff: when the selection is on
// the top row and UP is pressed, focus the back button. The buttonmatrix does
// not wrap UP off the top row, so this is the seam between the keyboard's
// internal navigation and the top-nav zone.
static void passphrase_kb_key_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_KEY) return;
    passphrase_ctx_t *c = (passphrase_ctx_t *)lv_event_get_user_data(e);
    if (!c) return;

    uint32_t key = lv_event_get_key(e);

    // KEY1/KEY2/KEY3 (via the shared nav_aux_key_index recognizer, navigation.h)
    // only act when the side panel is present (240 joystick). At >240 there is no
    // panel — switching/confirm is via the in-grid mode/OK keys.
    if (c->key2_label) {
        int aux = nav_aux_key_index(key);
        if (aux == 1) { kb_flash_side_button(c->key1_btn); passphrase_key1_case(c); return; }
        if (aux == 2) { kb_flash_side_button(c->key2_btn); passphrase_key2_cycle(c); return; }
        if (aux == 3) {
            kb_flash_side_button(c->key3_btn);
            if (c->ta && lv_obj_is_valid(c->ta)) {
                seedsigner_lvgl_on_text_entered(lv_textarea_get_text(c->ta));
            }
            return;
        }
    }

    // The remaining directional keys (UP top-row → back-button handoff, LEFT/RIGHT
    // row-wrap) are the generic keyboard navigation, shared via keyboard_core.
    kb_handle_directional(e, lv_keyboard_get_map_array(c->kb), c->kb, c->back_btn);
}

// Hardware-mode key filter on the back button: DOWN returns focus to the
// keyboard.
static void passphrase_back_key_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_KEY) return;
    passphrase_ctx_t *c = (passphrase_ctx_t *)lv_event_get_user_data(e);
    if (c) kb_back_down_to_matrix(e, c->kb);
}

// Tear down the group + ctx when the screen is destroyed (mirrors
// nav_cleanup_handler / screensaver_cleanup_handler). lv_group_del clears the
// indev's group pointer so no stale reference survives the screen swap.
static void passphrase_cleanup_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;
    passphrase_ctx_t *c = (passphrase_ctx_t *)lv_event_get_user_data(e);
    if (!c) return;
    if (c->group) {
        lv_group_del(c->group);
    }
    lv_free(c);
}

// The side-panel button factory (kb_side_button) now lives in keyboard_core.

// Custom keyboard handler, installed in place of lv_keyboard_def_event_cb so we
// control the in-grid control-key labels (notably the symbols toggle, which the
// native handler would force to "1#"). Handles touch-mode in-grid control keys
// plus character insertion / backspace / cursor for both modes.
static void passphrase_kb_value_changed(lv_event_t *e) {
    lv_obj_t *kb = lv_event_get_target_obj(e);
    passphrase_ctx_t *c = (passphrase_ctx_t *)lv_event_get_user_data(e);
    if (!c) return;

    uint32_t id = lv_keyboard_get_selected_button(kb);
    if (id == LV_BUTTONMATRIX_BUTTON_NONE) return;
    const char *txt = lv_keyboard_get_button_text(kb, id);
    if (!txt) return;

    lv_obj_t *ta = c->ta;

    // In-grid mode-switch keys (present in the touch maps). Each label maps to a
    // target mode; the custom handler owns this since def_event_cb was removed.
    if (std::strcmp(txt, UPPER_LABEL) == 0) {
        c->letter_mode = LV_KEYBOARD_MODE_TEXT_UPPER;
        passphrase_switch_mode(c, LV_KEYBOARD_MODE_TEXT_UPPER);
        passphrase_update_labels(c);
        return;
    }
    if (std::strcmp(txt, ABC_LABEL) == 0) {
        c->letter_mode = LV_KEYBOARD_MODE_TEXT_LOWER;
        passphrase_switch_mode(c, LV_KEYBOARD_MODE_TEXT_LOWER);
        passphrase_update_labels(c);
        return;
    }
    if (std::strcmp(txt, NUM_LABEL) == 0) {
        passphrase_switch_mode(c, LV_KEYBOARD_MODE_NUMBER);
        passphrase_update_labels(c);
        return;
    }
    if (std::strcmp(txt, SYM_LABEL) == 0) {
        passphrase_switch_mode(c, LV_KEYBOARD_MODE_SPECIAL);
        passphrase_update_labels(c);
        return;
    }
    if (std::strcmp(txt, SeedSignerIconConstants::CHECK) == 0) {
        if (ta && lv_obj_is_valid(ta)) {
            seedsigner_lvgl_on_text_entered(lv_textarea_get_text(ta));
        }
        return;
    }

    if (!ta || !lv_obj_is_valid(ta)) return;

    // Editing keys + character insertion (both modes).
    if (std::strcmp(txt, SeedSignerIconConstants::DELETE) == 0) { lv_textarea_delete_char(ta); return; }
    if (std::strcmp(txt, SeedSignerIconConstants::CHEVRON_LEFT) == 0)  { lv_textarea_cursor_left(ta); return; }
    if (std::strcmp(txt, SeedSignerIconConstants::CHEVRON_RIGHT) == 0) { lv_textarea_cursor_right(ta); return; }
    // The space key displays the seedsigner space glyph; insert a real space.
    if (std::strcmp(txt, SeedSignerIconConstants::SPACE) == 0) { lv_textarea_add_text(ta, " "); return; }
    lv_textarea_add_text(ta, txt);
}

// The keyboard theming + per-key SeedSigner icon recolor (kb_style_matrix and
// its draw callback) now live in keyboard_core.

// Parse cfg["initial_mode"] into a starting keyboard mode (for screenshots /
// deep-links into a specific charset page). Defaults to lowercase letters.
static lv_keyboard_mode_t passphrase_initial_mode(const json &cfg) {
    if (cfg.contains("initial_mode") && cfg["initial_mode"].is_string()) {
        std::string m = cfg["initial_mode"].get<std::string>();
        if (m == "upper")   return LV_KEYBOARD_MODE_TEXT_UPPER;
        if (m == "digits")  return LV_KEYBOARD_MODE_NUMBER;
        if (m == "symbols") return LV_KEYBOARD_MODE_SPECIAL;
    }
    return LV_KEYBOARD_MODE_TEXT_LOWER;
}

void seed_add_passphrase_screen(void *ctx_json) {
    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    // Default the title if the caller didn't supply top_nav (keeps minimal
    // scenarios terse).
    if (!cfg.contains("top_nav") || !cfg["top_nav"].is_object()) {
        cfg["top_nav"] = json::object();
    }
    if (!cfg["top_nav"].contains("title")) {
        cfg["top_nav"]["title"] = "Enter Passphrase";
    }

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
        map_lower = passphrase_kb_map_lower;       ctrl_lower = passphrase_kb_ctrl_lower;
        map_upper = passphrase_kb_map_upper;       ctrl_upper = passphrase_kb_ctrl_upper;
        map_special = passphrase_kb_map_special;   ctrl_special = passphrase_kb_ctrl_special;
    } else if (use_side_panel) {
        map_lower = passphrase_kb_map_lower_240hw;     ctrl_lower = passphrase_kb_ctrl_lower_240hw;
        map_upper = passphrase_kb_map_upper_240hw;     ctrl_upper = passphrase_kb_ctrl_upper_240hw;
        map_digits = passphrase_kb_map_digits_240hw;   ctrl_digits = passphrase_kb_ctrl_digits_240hw;
        map_special = passphrase_kb_map_symbols_240hw; ctrl_special = passphrase_kb_ctrl_symbols_240hw;
    } else {
        map_lower = passphrase_kb_map_lower_240;     ctrl_lower = passphrase_kb_ctrl_lower_240;
        map_upper = passphrase_kb_map_upper_240;     ctrl_upper = passphrase_kb_ctrl_upper_240;
        map_digits = passphrase_kb_map_digits_240;   ctrl_digits = passphrase_kb_ctrl_digits_240;
        map_special = passphrase_kb_map_symbols_240; ctrl_special = passphrase_kb_ctrl_symbols_240;
    }

    // Fail fast if a future edit ever breaks the exact-inventory guarantee.
    if (has_digits_page) {
        const char * const * audit_maps[] = { map_lower, map_upper, map_digits, map_special };
        audit_passphrase_charset(audit_maps, 4);
    } else {
        const char * const * audit_maps[] = { map_lower, map_upper, map_special };
        audit_passphrase_charset(audit_maps, 3);
    }

    // No button_list: the body is custom (textarea + keyboard). upper_body == body.
    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, false);
    lv_obj_t *body = screen.body;
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);

    const int32_t content_w = lv_obj_get_content_width(body);
    const int32_t content_h = lv_obj_get_content_height(body);

    // Reserve a right-side strip for the KEY1/KEY2/KEY3 panel only when it is
    // shown (240 + joystick). Python's right_panel_buttons_width = 56, scaled per
    // profile. The text strip and keyboard occupy the remaining width.
    const int32_t panel_w = use_side_panel ? (56 * active_profile().px_multiplier / 100) : 0;
    // The side panel deliberately runs off the right screen edge to line up with
    // the three physical Waveshare buttons (mirrors Python, whose hw buttons
    // overshoot canvas_width by COMPONENT_PADDING). Reclaiming the right
    // EDGE_PADDING gutter widens the keyboard + text-entry strip by that much.
    const int32_t main_w = use_side_panel ? (content_w - panel_w + EDGE_PADDING) : content_w;

    // Per-screen state (both modes). Freed in passphrase_cleanup_cb.
    passphrase_ctx_t *c = (passphrase_ctx_t *)lv_malloc(sizeof(passphrase_ctx_t));
    lv_memzero(c, sizeof(*c));

    // Text-entry strip: the shared one-line, cursor-styled SeedSigner box.
    lv_obj_t *ta = kb_make_text_entry(body, main_w, seedsigner_lvgl_is_static_render());

    std::string initial_text;
    if (cfg.contains("initial_text") && cfg["initial_text"].is_string()) {
        initial_text = cfg["initial_text"].get<std::string>();
        lv_textarea_set_text(ta, initial_text.c_str());
    }
    if (cfg.contains("max_length") && cfg["max_length"].is_number_integer()) {
        int max_length = cfg["max_length"].get<int>();
        if (max_length > 0) {
            lv_textarea_set_max_length(ta, (uint32_t)max_length);
        }
    }
    lv_textarea_set_cursor_pos(ta, LV_TEXTAREA_CURSOR_LAST);

    // Native keyboard with our custom maps. Replace the default event handler
    // with ours so we control the control-key labels (e.g. the symbols toggle).
    lv_obj_t *kb = lv_keyboard_create(body);
    lv_obj_remove_event_cb(kb, lv_keyboard_def_event_cb);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_LOWER, map_lower, ctrl_lower);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_UPPER, map_upper, ctrl_upper);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_SPECIAL, map_special, ctrl_special);
    if (has_digits_page) {
        lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_NUMBER, map_digits, ctrl_digits);
    }
    lv_keyboard_set_textarea(kb, ta);
    kb_style_matrix(kb, &KEYBOARD_FONT);

    lv_keyboard_mode_t start_mode = passphrase_initial_mode(cfg);
    // The digits page only exists at 240; elsewhere fall back to letters.
    if (start_mode == LV_KEYBOARD_MODE_NUMBER && !has_digits_page) {
        start_mode = LV_KEYBOARD_MODE_TEXT_LOWER;
    }
    lv_keyboard_set_mode(kb, start_mode);

    c->kb = kb;
    c->ta = ta;
    c->back_btn = screen.top_back_btn;
    c->letter_mode = (start_mode == LV_KEYBOARD_MODE_TEXT_UPPER)
                     ? LV_KEYBOARD_MODE_TEXT_UPPER : LV_KEYBOARD_MODE_TEXT_LOWER;

    lv_obj_add_event_cb(kb, passphrase_kb_value_changed, LV_EVENT_VALUE_CHANGED, c);

    const int32_t kb_top = BUTTON_HEIGHT + COMPONENT_PADDING;
    const int32_t kb_h = content_h - kb_top;

    // Keyboard fills the area left of any reserved panel strip (main_w ==
    // content_w when there is no panel). lv_obj_align (not set_pos):
    // lv_keyboard_create sets a default BOTTOM_MID anchor in its constructor, so
    // a bare set_pos would offset from the bottom; TOP_LEFT resets the anchor.
    lv_obj_set_size(kb, main_w, kb_h);
    lv_obj_align(kb, LV_ALIGN_TOP_LEFT, 0, kb_top);

    if (use_side_panel) {
        // KEY1/KEY2/KEY3 indicator buttons. Their vertical positions must line up
        // with the device's physical buttons, so they replicate the Python
        // SeedAddPassphraseScreen layout: KEY2 centered on the full canvas,
        // KEY1/KEY3 offset by (3*COMPONENT_PADDING + BUTTON_HEIGHT). Those offsets
        // are canvas-relative; `body` sits TOP_NAV_HEIGHT below the canvas top, so
        // subtract it to get body-local y.
        const int32_t btn_h = BUTTON_HEIGHT;
        const int32_t screen_h = lv_obj_get_height(screen.screen);
        const int32_t spacing = 3 * COMPONENT_PADDING + btn_h;
        const int32_t center_y = (screen_h - btn_h) / 2 - TOP_NAV_HEIGHT;
        // Buttons sit COMPONENT_PADDING right of the keyboard and overshoot the
        // right screen edge by COMPONENT_PADDING (clipped at the body boundary) so
        // they read as aligned with the physical hardware keys.
        const int32_t px = content_w + EDGE_PADDING + COMPONENT_PADDING - panel_w;
        // ABC/123 use the fixed-width keyboard font (Inconsolata), matching the
        // keys and the Python side panel (FIXED_WIDTH_EMPHASIS_FONT_NAME), not the
        // OpenSans body button font.
        const int32_t clipped = COMPONENT_PADDING;  // overshoot off the right edge
        c->key1_btn = kb_side_button(body, px, center_y - spacing, panel_w, btn_h,
                               UPPER_LABEL, &KEYBOARD_FONT, BUTTON_FONT_COLOR, clipped, &c->key1_label);
        c->key2_btn = kb_side_button(body, px, center_y, panel_w, btn_h,
                               NUM_LABEL, &KEYBOARD_FONT, BUTTON_FONT_COLOR, clipped, &c->key2_label);
        c->key3_btn = kb_side_button(body, px, center_y + spacing, panel_w, btn_h,
                               SeedSignerIconConstants::CHECK, &ICON_FONT__SEEDSIGNER, SUCCESS_COLOR, clipped, NULL);
        passphrase_update_labels(c);
    }

    if (hardware) {
        // Localized LVGL-native focus (this screen does NOT use nav_bind): the
        // keyboard is the group-focused object so the buttonmatrix's own
        // directional navigation drives key selection; the back button is the
        // other group member for the top-nav handoff. Used at any size for
        // joystick input; at >240 there is no side panel, so the in-grid mode/OK
        // keys (reached by navigating to them) do the switching/confirming.
        c->group = lv_group_create();
        lv_group_set_wrap(c->group, false);
        lv_group_add_obj(c->group, kb);
        if (screen.top_back_btn) {
            lv_group_add_obj(c->group, screen.top_back_btn);
            // PREPROCESS: run before the buttonmatrix's own key handler so we read
            // the selection BEFORE it moves. Otherwise an UP from the 2nd row first
            // moves the selection into the top row, then our handler sees a top-row
            // selection and wrongly jumps to the back button.
            lv_obj_add_event_cb(kb, passphrase_kb_key_cb,
                                (lv_event_code_t)(LV_EVENT_KEY | LV_EVENT_PREPROCESS), c);
            lv_obj_add_event_cb(screen.top_back_btn, passphrase_back_key_cb, LV_EVENT_KEY, c);
        }
        lv_group_focus_obj(kb);

        // Connect all keypad/encoder indevs to this group.
        kb_connect_indevs(c->group);

        // Pre-select an initial key so the joystick selection is visible from the
        // start. Otherwise btn_id_sel is NONE and it takes an arrow press just to
        // "enter" the keyboard, with no visible cursor until then. Prefer the
        // last-typed key (prefilled, e.g. the "i" of satoshi), else the page's
        // central default key (k / 6). The highlight shows via the focused-key
        // style in live use; static-render (screenshots) has no indev to apply
        // that focus state, so add PRESSED to make the highlight show in the still.
        int sel = initial_text.empty() ? -1 : kb_find_button(lv_keyboard_get_map_array(kb), initial_text.back());
        if (sel < 0) sel = (int)passphrase_default_key(kb, start_mode);
        lv_buttonmatrix_set_selected_button(kb, (uint32_t)sel);
        if (seedsigner_lvgl_is_static_render()) {
            lv_obj_add_state(kb, LV_STATE_PRESSED);
        }
    }

    // Free ctx (+ group, if any) when the screen is destroyed.
    lv_obj_add_event_cb(screen.screen, passphrase_cleanup_cb, LV_EVENT_DELETE, c);

    load_screen_and_cleanup_previous(screen.screen);
}
