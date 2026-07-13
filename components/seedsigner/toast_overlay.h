#ifndef SEEDSIGNER_TOAST_OVERLAY_H
#define SEEDSIGNER_TOAST_OVERLAY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Toast overlay — portable native LVGL transient banner
// ---------------------------------------------------------------------------
// A pop-up notification banner pinned to the bottom of the display: a black
// rounded rectangle with a colored outline, an optional leading severity icon,
// and a left-aligned wrapped message. It ports Python's gui/toast.py ToastOverlay
// (the render() in that file) to a native LVGL surface.
//
// WHY THIS EXISTS (the bug it fixes): on the Pi Zero the underlying screen is now
// drawn by LVGL, but the toast was still a PIL component that blitted a whole
// framebuffer. With no live PIL canvas to composite onto, the PIL toast painted its
// banner over BLACK — blacking out the LVGL UI behind it. Rendering the toast as a
// native LVGL widget makes it composite over the live screen instead.
//
// HOW IT COMPOSITES: the banner is built on the display's TOP LAYER
// (lv_layer_top()), which floats above the active screen and is NOT cleared by
// lv_screen_load() — so the toast overlays whatever screen is showing and survives
// screen swaps. It joins no input group (it never steals keypad/encoder focus), so
// input keeps flowing to the underlying screen — the "non-focus-stealing" contract.
//
// DISMISSAL (auto + by input, matching Python's "any input hides the toast"):
//   - Auto: after spec->duration_ms (0 = stay until dismissed/replaced).
//   - HARDWARE input mode (joystick + keys): ANY key/joystick press dismisses the
//     toast. The press is NOT consumed — it still drives the underlying screen (the
//     toast watches LVGL's idle clock rather than grabbing the key), so a press both
//     hides the toast and acts on the screen, exactly like Python.
//   - TOUCH input mode: a TAP on the banner, or a swipe FLICK across it, dismisses
//     it. The banner is clickable in touch mode only so a tap lands on it; taps
//     OUTSIDE the bottom band fall through to the screen untouched.
// The input mode is read once (input_profile_get_mode()) when the toast is shown.
//
// POLICY-FREE: this module renders WHAT it is told (text, icon glyph, colors,
// duration). The severity -> (icon, color) mapping is the HOST's policy (Python's
// InfoToast / SuccessToast / WarningToast / ... subclasses); the host passes the
// resolved icon glyph and colors. The desktop-tools wrapper (toast_overlay_screen)
// carries an equivalent mapping so the screenshot corpus can render each severity.
//
// SCREENSAVER COEXISTENCE: the toast and the screensaver are both cross-cutting
// overlays owned by overlay_manager, and they are mutually exclusive there — the
// dispatcher suppresses screensaver activation while a toast is showing, and a toast
// enqueued while the screensaver is up dismisses it first (Python: "new toast
// notifications break out of the Screensaver"). See overlay_manager.{h,cpp}.
//
// THREADING: every function here is LVGL-thread only (they build/delete widgets and
// timers). Cross-thread producers (e.g. the Pi's SD-card detector thread) must NOT
// call these directly — they go through overlay_manager_show_toast(), which marshals
// the request onto the LVGL loop.

// Declarative description of one toast. The strings are only read during the
// toast_overlay_show() call (the widget copies what it needs), so a caller may pass
// stack/temporary buffers.
typedef struct {
    // Message text. Required (NULL/empty renders an empty banner). May contain '\n'
    // for explicit line breaks; long lines also soft-wrap to the banner width.
    const char *label_text;

    // Optional leading icon: a seedsigner-icon-font UTF-8 glyph (e.g.
    // SeedSignerIconConstants::SUCCESS) or NULL for a text-only toast (Python
    // DefaultToast). Drawn in outline_color, matching Python (icon_color == color).
    const char *icon_glyph;

    // Banner outline + icon color, 0xRRGGBB (Python ToastOverlay.color).
    uint32_t outline_color;

    // Message text color, 0xRRGGBB (Python ToastOverlay.font_color). Note the SD-card
    // toasts use the same green for both; the severity toasts use white text over a
    // colored outline.
    uint32_t font_color;

    // Auto-dismiss after this many milliseconds. 0 = stay until the host calls
    // toast_overlay_dismiss() (or a newer toast replaces it) — the Python
    // "keep_running until a condition" case (e.g. RemoveSDCard until the card is
    // pulled). Ignored in static-render mode (the banner stays put for a
    // deterministic screenshot).
    uint32_t duration_ms;
} toast_overlay_spec_t;

// Show a toast on the top layer, REPLACING any currently-showing toast (Python runs
// one toast at a time). Builds the banner immediately so a single render pass (the
// screenshot generator) captures it, and — unless in static-render mode — arms a
// one-shot auto-dismiss timer for spec->duration_ms. LVGL-thread only.
void toast_overlay_show(const toast_overlay_spec_t *spec);

// Dismiss the current toast immediately, freeing its widget + timer (no-op if none
// is showing). LVGL-thread only.
void toast_overlay_dismiss(void);

// True while a toast banner is on screen. overlay_manager reads this to suppress the
// screensaver; the desktop wrapper does not need it.
bool toast_overlay_is_active(void);

#ifdef __cplusplus
}
#endif

#endif // SEEDSIGNER_TOAST_OVERLAY_H
