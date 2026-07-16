#ifndef SEEDSIGNER_NAVIGATION_H
#define SEEDSIGNER_NAVIGATION_H

#include "lvgl.h"
#include "input_profile.h"
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Sentinel for nav_config_t.initial_body_index meaning "no concrete default focus."
// See that field's comment for how it interacts with overflow/scroll-then-buttons.
#define NAV_INDEX_NONE ((size_t)-1)

typedef enum {
    NAV_ZONE_TOP = 0,
    NAV_ZONE_BODY = 1,
    // Non-focusable body content (status text, or a button list's intro text) is
    // scrolling under joystick control. Entered automatically whenever a vertical
    // screen has such content above its buttons and the body overflows the
    // viewport (see nav_config_t.scroll_then_buttons): DOWN walks the body down a
    // step at a time until the first body button is reachable, then drops into
    // NAV_ZONE_BODY to focus it; UP reverses, finally surfacing the top-nav. No
    // item is highlighted while in this zone.
    NAV_ZONE_SCROLL = 2,
} nav_zone_t;

typedef enum {
    NAV_BODY_VERTICAL = 0,
    NAV_BODY_GRID = 1,
} nav_body_layout_t;

typedef enum {
    NAV_AUX_ENTER = 0,
    NAV_AUX_NOOP = 1,
    NAV_AUX_EMIT = 2,
} nav_aux_action_t;

typedef struct {
    nav_aux_action_t key1;
    nav_aux_action_t key2;
    nav_aux_action_t key3;
} nav_aux_policy_t;

// Shared KEY1/KEY2/KEY3 recognizer — the single spec for mapping a keypad keycode to
// an aux-key index. Returns 1/2/3 for a KEY1/KEY2/KEY3 keycode, 0 otherwise.
// Recognized codes: LV_KEY_F1..F3 when the platform defines them (the forward-compat
// on-device contract; no current build does), and ASCII '1'/'2'/'3' always (what every
// build delivers today). Used by the nav key handler and by the self-input screens
// (seed_mnemonic_entry, seed_add_passphrase, io_test), so all aux-key recognition
// stays byte-equivalent across the corpus.
int nav_aux_key_index(uint32_t key);

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *top_back_btn;
    lv_obj_t *top_power_btn;
    lv_obj_t **body_items;
    size_t body_item_count;
    nav_body_layout_t body_layout;
    nav_aux_policy_t aux_policy;

    // Which body item is focused on load. A CONCRETE index (button lists pass 0; a
    // settings re-render passes the row to restore) stays focused even when the body
    // overflows — it is scrolled into view, so a button is always active.
    // NAV_INDEX_NONE means "no forced default": the first item is focused when the
    // screen fits, but the screen starts UNFOCUSED (read-first) while scrolling is
    // required to reach the buttons (status / warning screens). See nav_bind.
    size_t initial_body_index;
    bool has_input_mode_override;
    input_mode_t input_mode_override;

    // Joystick scrolling (default off: scroll_obj == NULL). bind_screen_navigation
    // turns this on automatically for any vertical screen that has non-focusable
    // upper content (status text, a button list's intro text) above its buttons AND
    // whose body overflows the viewport: it passes the scrollable body here with
    // scroll_then_buttons = true, and the nav state machine inserts a
    // NAV_ZONE_SCROLL step so DOWN scrolls the body progressively before focusing
    // the first button, and UP scrolls back up before entering the top-nav. A pure
    // button list (no upper content) leaves these unset: it scrolls via item-focus
    // navigation (scroll_to_view) instead, so it must NOT get a page-scroll step.
    lv_obj_t *scroll_obj;
    bool scroll_then_buttons;
} nav_config_t;

void nav_bind(const nav_config_t *cfg);

// Route every keypad/encoder indev to `group` — the single input handoff that
// every screen performs when it takes over input on load. Besides attaching the
// group, each indev is latched with lv_indev_wait_release() so a key that is
// still physically HELD from the previous screen is ignored until it is
// released: only a fresh press that begins AFTER this screen is up registers.
//
// Without the latch, a key held across the screen swap "bleeds" onto the freshly
// loaded screen and can fire an action/exit there — e.g. activating an Address
// Explorer row with a still-held KEY1 instantly dismisses the QR that KEY1 just
// opened, because qr_display exits on any non-brightness/-density key. The latch
// is edge-based (not a timed settle) and a no-op when no key is held, so a
// deliberate fast press on the new screen is never dropped. See
// docs/held-key-bleed-across-screen-transition-todo.md for the full analysis.
void attach_keypad_indevs_to_group(lv_group_t *group);

#ifdef __cplusplus
}
#endif

#endif // SEEDSIGNER_NAVIGATION_H
