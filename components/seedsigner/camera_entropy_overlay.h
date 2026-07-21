#ifndef SEEDSIGNER_CAMERA_ENTROPY_OVERLAY_H
#define SEEDSIGNER_CAMERA_ENTROPY_OVERLAY_H

#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Image-entropy capture overlay — portable spec + renderer
// ---------------------------------------------------------------------------
// Sibling of camera_preview_overlay (QR scan): same PASSIVE-view / preview-square /
// input-mode contract, but models the image-entropy flow's TWO phases instead of a
// scan status bar:
//   - PREVIEW  : live frames are gathered; the user CAPTURES or CANCELS.
//   - CAPTURING: brief transient while the frame is frozen + latched.
//   - CONFIRM  : the frozen final frame is reviewed; the user ACCEPTS or RESHOOTS.
// See camera_preview_overlay.h for the layering / two-clocks rationale.
//
// The back affordance + controls are INPUT-MODE dependent (input_profile_get_mode()):
//   - INPUT_MODE_HARDWARE (joystick; the 240px-high Pi Zero): bottom-center instruction
//     TEXT over the preview, matching the Python PIL screens
//     (ToolsImageEntropyLivePreviewScreen: "< back | click a button";
//      ToolsImageEntropyFinalImageScreen: "< reshoot | accept >"). Joystick keys drive
//     capture/cancel/accept/reshoot; there are NO on-screen buttons. The overlay OWNS
//     those keys (invisible focusable sink in its own group), because the screen swap
//     leaves the keypad indev group-less and LVGL discards anything the overlay does
//     not claim. Map, mirroring the Python screens:
//       PREVIEW  LEFT = cancel(back);  ANYCLICK (click/KEY1-3) = capture
//       CONFIRM  LEFT = reshoot(back); RIGHT or ANYCLICK       = accept
//       CAPTURING — keys swallowed (transient).
//   - INPUT_MODE_TOUCH (the ESP32 boards): the shared back button in the top-left gutter
//     (PREVIEW = cancel, CONFIRM = reshoot), a shutter CAPTURE control in PREVIEW, and a
//     stylized ACCEPT button in CONFIRM.
//
// ALL text is HOST-PROVIDED and already localized — nothing is hardcoded here (mirrors
// camera_preview_overlay's instructions_text). Geometry-only controls (the back arrow,
// the shutter circle) carry no text and need no translation.
//
// Event contract (host reads via poll_for_result): the shared back_button() emits
// SEEDSIGNER_RET_BACK_BUTTON -> "topnav_back"; the shutter + accept controls emit
// seedsigner_lvgl_on_button_selected(...) -> "button_selected". The host runner maps:
// PREVIEW  {button_selected=capture, topnav_back=cancel};
// CONFIRM  {button_selected=accept,  topnav_back=reshoot}.

typedef enum {
    CAMERA_ENTROPY_PHASE_PREVIEW   = 0,  // gathering: capture / cancel
    CAMERA_ENTROPY_PHASE_CAPTURING = 1,  // transient: "Capturing image…"
    CAMERA_ENTROPY_PHASE_CONFIRM   = 2,  // review frozen frame: accept / reshoot
} camera_entropy_phase_t;

// TOUCH-mode PREVIEW capture affordance style.
typedef enum {
    CAMERA_ENTROPY_CAPTURE_RING   = 0,  // shutter: white ring + inner disc + centered icon
    CAMERA_ENTROPY_CAPTURE_SOLID  = 1,  // shutter: single solid white disc + centered icon
    CAMERA_ENTROPY_CAPTURE_BUTTON = 2,  // standard bottom button (like Accept): icon + label
} camera_entropy_capture_style_t;

// Declarative spec — WHAT the overlay shows, free of ESP compositing detail. The input
// mode is read at render time from input_profile_get_mode(), so it is NOT a field here.
typedef struct {
    // Preview-square geometry within the parent (host-supplied), as camera_preview_overlay.
    int32_t square_x;
    int32_t square_y;
    int32_t square_w;
    int32_t square_h;

    // HARDWARE-mode bottom lines (host composes the full localized line). Ignored in
    // touch mode. NULL/empty → omit.
    const char *preview_instructions;   // e.g. "< back  |  click a button"
    const char *confirm_instructions;   // e.g. "< reshoot  |  accept >"

    // Bottom-center accent-color text during the CAPTURING transient (both modes).
    const char *capturing_text;         // e.g. "Capturing image…"

    // TOUCH-mode PREVIEW capture control:
    //   capture_style — RING/SOLID shutter circle, or a standard BUTTON (like Accept).
    //   capture_icon  — glyph shown on the control (host passes the FontAwesome camera;
    //                   rendered via ICON_FONT__SEEDSIGNER, which bakes the FA button
    //                   glyphs). A symbol, not text — needs no translation. NULL → no icon.
    //   capture_label — localized button text, used by the BUTTON style (e.g. "Capture");
    //                   ignored by the shutter styles. NULL → empty.
    camera_entropy_capture_style_t  capture_style;
    const char                     *capture_icon;
    const char                     *capture_label;

    // TOUCH-mode CONFIRM accept button (localized).
    const char *accept_label;

    camera_entropy_phase_t phase;       // initial phase
} camera_entropy_overlay_spec_t;

typedef struct camera_entropy_overlay camera_entropy_overlay_t;

// Build the overlay onto `parent` (spans the full display; the camera square occupies
// spec->square_* within it). Widgets are children of `parent`; deleting `parent` frees
// them. Returns a handle; call ..._destroy() to free the handle struct.
camera_entropy_overlay_t *camera_entropy_overlay_create(lv_obj_t *parent,
                                                        const camera_entropy_overlay_spec_t *spec);

// Switch phase (host calls it on capture() → CAPTURING/CONFIRM, resume() → PREVIEW).
void camera_entropy_overlay_set_phase(camera_entropy_overlay_t *overlay,
                                      camera_entropy_phase_t phase);

// Push the frozen final frame shown in the CONFIRM phase. `rgb565` is a w×h RGB565
// buffer (typically the full display, from image_entropy_process); the overlay COPIES
// it, so the caller may reuse/free its buffer immediately after. The image spans the
// whole display (over the black gutters + the live square) and is visible only while
// phase == CONFIRM. Calling again replaces the previous frame. Safe no-op if overlay
// is NULL. See docs/image-entropy-lvgl-native-contract.md §4.
//
// ⚠ The copy is made with lv_malloc, i.e. out of the LVGL heap. A full-display frame is
// large (480×800×2 = 750 KB) and LVGL pools are typically far smaller — the ESP32-P4
// firmware runs a 128 KB pool — so on such a target this ALWAYS fails and silently
// leaves the previous image. Embedded callers that already hold the frame in large
// memory (PSRAM) should use the _owned variant below instead of this one.
void camera_entropy_overlay_set_confirm_image(camera_entropy_overlay_t *overlay,
                                              const void *rgb565, int32_t w, int32_t h);

// Ownership-transfer form of the above: the overlay ADOPTS `rgb565` and renders directly
// out of it — no LVGL-heap copy, so a full-display frame works regardless of pool size.
// The caller must NOT free or reuse the buffer afterwards; the overlay releases it (on
// replacement, on widget delete) by calling `free_fn(buffer)`. Pass the deallocator that
// matches the allocation — e.g. a wrapper around heap_caps_free() for a PSRAM buffer, so
// this file stays free of any platform allocator dependency. `free_fn == NULL` means
// lv_free. On failure the buffer is released via free_fn rather than leaked.
void camera_entropy_overlay_set_confirm_image_owned(camera_entropy_overlay_t *overlay,
                                                    void *rgb565, int32_t w, int32_t h,
                                                    void (*free_fn)(void *));

// Free the handle struct (does NOT delete the LVGL widgets — they belong to the parent).
void camera_entropy_overlay_destroy(camera_entropy_overlay_t *overlay);

#ifdef __cplusplus
}
#endif

#endif // SEEDSIGNER_CAMERA_ENTROPY_OVERLAY_H
