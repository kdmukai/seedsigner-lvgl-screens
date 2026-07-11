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
void opening_splash_screen(void *ctx_json);
void large_icon_status_screen(void *ctx_json);
void seed_add_passphrase_screen(void *ctx_json);
void camera_preview_overlay_screen(void *ctx_json);
void camera_entropy_overlay_screen(void *ctx_json);
void keyboard_screen(void *ctx_json);
void seed_mnemonic_entry_screen(void *ctx_json);
void seed_finalize_screen(void *ctx_json);
void seed_export_xpub_details_screen(void *ctx_json);
void seed_review_passphrase_screen(void *ctx_json);
void seed_words_screen(void *ctx_json);
void loading_spinner_screen(void *ctx_json);
void qr_display_screen(void *ctx_json);
// Zoomed, pannable SeedQR transcription view (parity with Python
// SeedTranscribeSeedQRZoomedInScreen). Renders the QR oversized (fat round modules),
// dims all but one centered A-F/1-6 zone, and steps a full zone per joystick press /
// touch swipe (hardware exits on click via a "click to exit" hint; touch exits via a
// top-right X). Direct-draws only the viewport modules (no oversized canvas) to stay
// within the ESP32 LVGL pool. Fixed registration patterns (finder/alignment) render as
// solid squares to match the pre-printed paper templates; data modules are round dots.
// cfg: qr_data / qr_mode / data_encoding / num_modules / initial_zone_x / initial_zone_y / exit_text.
void seed_transcribe_zoomed_qr_screen(void *ctx_json);
void psbt_overview_screen(void *ctx_json);
void psbt_address_details_screen(void *ctx_json);
void psbt_change_details_screen(void *ctx_json);
void psbt_math_screen(void *ctx_json);
void settings_locale_picker_screen(void *ctx_json);
void multisig_wallet_descriptor_screen(void *ctx_json);
void seed_sign_message_confirm_address_screen(void *ctx_json);
void settings_qr_confirmation_screen(void *ctx_json);
void seed_sign_message_confirm_message_screen(void *ctx_json);
void seed_address_verification_screen(void *ctx_json);
// Update the in-place "checking…" progress line on a live seed_address_verification_screen
// while the host's background brute-force worker scans derivation indexes. The host pushes
// already-localized text (e.g. "Checking address 1240"); the library holds no strings.
// Safe no-op when no such screen is active — the same host-driven live-push contract as
// qr_display_set_frame(); the host owns the worker/match logic and closes the screen.
void seed_address_verification_set_progress(const char *progress_text);
// Tools > Calc final word: the "final word math" breakdown (parity with Python
// ToolsCalcFinalWordScreen). Shows the user's entered entropy bits, the checksum
// bits (orange), and the merged final word — over three centered monospace bit rows.
void tools_calc_final_word_screen(void *ctx_json);
void tools_calc_final_word_done_screen(void *ctx_json);
// The "whole QR" overview step of the SeedQR hand-transcription flow (parity with
// Python SeedTranscribeSeedQRWholeQRScreen). Direct-draws the full SeedQR/CompactSeedQR
// grid (python-qrcode mask parity) with a pulsing orange WarningEdges border.
void seed_transcribe_whole_qr_screen(void *ctx_json);
// Address Explorer address list (parity with Python ToolsAddressExplorerAddressListScreen).
// A bottom-pinned, left-aligned, fixed-width (monospace) button list of derived addresses,
// each shown as "{index}:{head}...{tail}"; the selected row reveals its full address. A
// trailing "Next N" button (right chevron) pages forward. cfg: addresses / start_index /
// initial_selected_index / next_label / top_nav.title.
void tools_address_explorer_address_list_screen(void *ctx_json);

// Simple info screens — a title over centered/anchored body text, no button list.
//   reset_screen                    (Python ResetScreen): "Restarting" + wipe notice; no nav buttons.
//   power_off_not_required_screen   (Python PowerOffNotRequiredScreen): "Just Unplug It" + back button.
//   donate_screen                   (Python DonateScreen): paragraph + accent "seedsigner.com" 28px line.
void reset_screen(void *ctx_json);
void power_off_not_required_screen(void *ctx_json);
void donate_screen(void *ctx_json);

// PSBT OP_RETURN payload (parity with Python PSBTOpReturnScreen). Bottom-list screen
// showing the payload either as centered human-readable text (cfg.text) or as a
// monospace hard-wrapped hex dump with a "raw hex data" caption (cfg.hex). The host
// decides the mode (it owns the bytes + UTF-8 heuristic); the screen owns the wrap.
void psbt_op_return_screen(void *ctx_json);

// I/O self-test (parity with Python IOTestScreen). A D-pad pictogram (joystick
// directions + click) plus KEY1/KEY2/KEY3 controls, over a live camera square. This
// screen is a PASSIVE chrome overlay for the camera (pixels are blitted by the board
// adapter, NOT delivered through here), but it OWNS its input: like Python's self-test,
// each physical control lights up when actuated (the screen reads the keypad indev
// directly — joystick dirs / click / KEY1-3 — and flashes the matching control). No back
// button (Python show_back_button = False); the user leaves via KEY3. KEY1/KEY2/KEY3 are
// also forwarded to the host (seedsigner_lvgl_on_aux_key) so it can grab a frame / clear
// / exit.
//
// A hardware-input diagnostic → meaningful on the Pi Zero (real joystick + keys); on the
// touchscreen build the app simply does not navigate to it (no touch variant here).
typedef enum {
    IO_TEST_CAPTURE_IDLE      = 0,  // no capture; KEY2 label blank
    IO_TEST_CAPTURE_CAPTURING = 1,  // "Capturing image…" band shown
    IO_TEST_CAPTURE_CAPTURED  = 2,  // a still frame is displayed; KEY2 label = "Clear"
} io_test_capture_state_t;

void io_test_screen(void *ctx_json);

// Reflect the camera capture phase (the host's async single-frame grab): show/hide the
// "Capturing…" band and toggle the KEY2 "Clear" label. Safe no-op when inactive.
void io_test_set_capture_state(io_test_capture_state_t state);

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

// Optional host hook: qr_display_screen reports the selected animated-QR density in integer
// pixels-per-module (3..6) whenever the on-screen density slider changes, and once on exit.
// It is the host's cue to re-resolve max_fragment_len from its resolution-keyed table, rebuild
// the encoder, and RESTART the UR fountain (same contract as _on_qr_brightness), then persist
// SETTING__QR_DENSITY. The density->fragment lookup stays host-side (single source of truth);
// this callback carries only px/module. Ships a weak no-op default; a host overrides to react.
void seedsigner_lvgl_on_qr_density(uint8_t px_per_module);

// Host callback: a body button (or a dismiss/complete sentinel) was selected. `index`
// is the 0-based body-button index or a SEEDSIGNER_RET_* value; `label` is its text.
// Ships a weak no-op default (in components.cpp); the host provides a strong override.
// Declared here so every screen TU sees one declaration (was a per-file forward decl).
void seedsigner_lvgl_on_button_selected(uint32_t index, const char *label);

// Host callback: the user committed entered text (keyboard / passphrase / mnemonic
// entry). Ships a weak no-op default (in components.cpp); host provides a strong one.
void seedsigner_lvgl_on_text_entered(const char *text);

// Host callback: an aux key (KEY1/KEY2/KEY3) was forwarded to the host — by the nav
// layer when the key's policy is NAV_AUX_EMIT, or by io_test_screen's self-owned
// input. `key_name` is "KEY1" / "KEY2" / "KEY3". Ships a weak no-op default (in
// components.cpp); interactive hosts provide a strong override — and MUST on
// MinGW/PE, where a weak default only resolves references from its own TU (the
// cross-TU link failure behind commit 7f89913).
void seedsigner_lvgl_on_aux_key(const char *key_name);

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

// Read the static-render flag. Screens that adapt to it (keyboards, loading,
// qr_display, splash, transcribe) call this getter rather than reaching a
// file-static, so their definitions can live in their own translation units.
bool seedsigner_lvgl_is_static_render(void);


#ifdef __cplusplus
}
#endif

#endif // SEEDSIGNER_H
