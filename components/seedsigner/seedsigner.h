#ifndef SEEDSIGNER_H
#define SEEDSIGNER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Maximum number of buttons the scaffold tracks for a single screen.
// SeedSigner's longest realistic button list is the Settings menu (~10
// entries) plus headroom for future scrollable lists. 32 keeps the scaffold
// state compact (32 pointers = 256 bytes on a 64-bit host) without imposing
// a practical limit on legitimate UIs.
#define SEEDSIGNER_SCAFFOLD_MAX_BUTTONS 32

// Result of create_top_nav_screen_scaffold().
//
// `screen` and `top_nav` are always populated. `top_back_btn` /
// `top_power_btn` are populated when the corresponding TopNav buttons were
// requested.
//
// `body` is the standard scrollable body container that sits beneath the
// TopNav. When `cfg["button_list"]` is provided, `body` is configured as a
// vertical flex column holding [upper_body, (optional spacer), button(s)].
// Otherwise it is the legacy non-flex container.
//
// `upper_body` is a flex-column container the screen owns: callers add
// content (icon, headline, custom widgets, etc.) here and the scaffold
// guarantees it will be followed by an optional flex-grow spacer and the
// button list. When no button_list is configured, `upper_body == body`.
//
// `button_list_spacer` is non-NULL only when both `cfg["button_list"]` is
// present AND `cfg["is_bottom_list"]` is true: it is a flex-grow=1 child
// inserted between `upper_body` and the first button to pin buttons to the
// viewport bottom when upper content fits.
//
// `button_list[]` / `button_list_count` enumerate the buttons created from
// `cfg["button_list"]`. Both are zero / unset when no button list is
// configured (e.g. main_menu_screen, screensaver_screen).
typedef struct {
    lv_obj_t *screen;
    lv_obj_t *top_nav;
    lv_obj_t *title_label;   // top-nav title label, for in-place title updates
    lv_obj_t *top_back_btn;
    lv_obj_t *top_power_btn;
    lv_obj_t *body;
    lv_obj_t *upper_body;
    lv_obj_t *button_list_spacer;
    size_t    button_list_count;
    lv_obj_t *button_list[SEEDSIGNER_SCAFFOLD_MAX_BUTTONS];
} screen_scaffold_t;

// Reserved result codes passed as the `index` argument to
// seedsigner_lvgl_on_button_selected(). Body buttons report their 0-based
// position (0..N-1); these reserved values sit far above any real button count.
// The host checks for them first, then treats `index` as a body-button position
// — the same order SeedSigner's Python Views use.
//
// 1000/1001 mirror RET_CODE__BACK_BUTTON / RET_CODE__POWER_BUTTON in
// seedsigner/.../gui/screens/screen.py so the host reads one int and compares.
#define SEEDSIGNER_RET_BACK_BUTTON          1000u  // == RET_CODE__BACK_BUTTON
#define SEEDSIGNER_RET_POWER_BUTTON         1001u  // == RET_CODE__POWER_BUTTON
#define SEEDSIGNER_RET_SCREENSAVER_DISMISS  1100u  // host-handled, not Python-routed
#define SEEDSIGNER_RET_SPLASH_COMPLETE      1101u  // opening splash finished/dismissed

// Screens
void button_list_screen(void *ctx_json);
void main_menu_screen(void *ctx);
void screensaver_screen(void *ctx_json);
void splash_screen(void *ctx_json);
void large_icon_status_screen(void *ctx_json);
void seed_add_passphrase_screen(void *ctx_json);
void camera_preview_overlay_screen(void *ctx_json);
void keyboard_screen(void *ctx_json);
void seed_mnemonic_entry_screen(void *ctx_json);
void seed_finalize_screen(void *ctx_json);
void loading_screen(void *ctx_json);
void qr_display_screen(void *ctx_json);

// Push the next frame into a live qr_display_screen (host-driven animation, mirroring
// the camera-overlay set_* live-update pattern). Re-encodes + repaints the QR in place
// reusing the screen cfg's qr_mode. `data` may be raw binary (e.g. a CompactSeedQR
// payload). Safe no-op when no qr_display_screen is currently active. Animated QR
// cadence lives in the host (the encode_qr fountain/Specter sequence is Python), so the
// host calls this per frame; a static QR never calls it.
//
// Animation/brightness-tip contract (Python QRDisplayScreen parity): the screen shows the
// brightness panel on START and briefly after each brightness change. The frame driver must
// HOLD (not advance) while qr_display_is_tip_active() is true, then RESTART the sequence when
// it clears — so the tip greets the user and, for a UR fountain, the valuable pure first
// frames are (re)delivered from the start. A brightness change also fires
// seedsigner_lvgl_on_qr_brightness(), the host's cue to restart the sequence.
void qr_display_set_frame(const void *data, size_t len);

// True while the brightness panel is displayed. The host's animation frame driver polls this
// and holds (does not call qr_display_set_frame) while true; when it goes false the driver
// restarts the sequence from its first frame. Safe (returns false) when no QR screen is active.
bool qr_display_is_tip_active(void);

// Optional host hook: qr_display_screen reports its final brightness (31..255) on exit
// so the host can persist SETTING__QR_BRIGHTNESS. The library ships a weak no-op default
// (same pattern as seedsigner_lvgl_on_button_selected / _on_text_entered); a host
// provides a strong definition to persist. Desktop tools may leave it stubbed.
void seedsigner_lvgl_on_qr_brightness(uint8_t brightness);

// Text metrics (shared with components.cpp). Empty vertical space between a
// label's box top and the VISIBLE top of its text — the font's declared ascent
// minus the text's real ink ascent. LVGL anchors a text box by the font ascent
// (which carries leading above the caps); subtract this from a top gap/margin so
// the visible text lands where PIL/Python places it, not at the taller box top.
int32_t text_top_leading(const lv_font_t *font, const char *text);

// Internal: build the screensaver screen object (bouncing logo) WITHOUT loading
// it. `route_dismiss_to_host` controls whether key/touch input fires
// SEEDSIGNER_RET_SCREENSAVER_DISMISS (the legacy Python-driven path); the
// overlay manager passes false and dismisses via its own idle-watch. Shared by
// screensaver_screen() and the overlay manager.
lv_obj_t *ss_build_screensaver_obj(bool route_dismiss_to_host);

// misc
void lv_seedsigner_screen_close(void);

// Enable static-render mode: screens render without animations that would make a
// still capture non-deterministic (e.g. the text-entry cursor is shown without
// blinking). Intended for the screenshot generator; off by default for live use.
void seedsigner_lvgl_set_static_render(bool enabled);


#ifdef __cplusplus
}
#endif

#endif // SEEDSIGNER_H
