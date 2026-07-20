#ifndef SEEDSIGNER_CAMERA_PREVIEW_OVERLAY_H
#define SEEDSIGNER_CAMERA_PREVIEW_OVERLAY_H

#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Camera live-preview overlay — portable spec + renderer
// ---------------------------------------------------------------------------
// The ESP32-P4 camera pipeline shows a landscape, center-cut SQUARE live preview;
// in landscape the left/right gutters around the square are static. This module is
// the *spec home* for the UI drawn OVER that preview, defined declaratively and kept
// separate from rendering so one spec can be realized ≥3 ways (ESP Path A = these
// LVGL widgets; ESP Path B = a manual composite that reads the same spec; Pi Zero
// later). ESP-specific rendering (rotation, gutter stride blits, dummy-draw,
// black-keyed alpha) lives in esp-board-common, NOT here.
//
// Two layers share one display on different clocks: the camera pixels (fast, C-only,
// owned by the camera adapter) and THIS overlay (slow, host-driven). The overlay is a
// PASSIVE view: it never polls the camera or decoder. The host (Python, via the
// binding layer) raises the status bar and advances progress by calling
// camera_preview_overlay_set_scanning()/set_progress() a few times per second.
//
// The overlay is positioned relative to the preview square (square_x/y/w/h within the
// caller-provided parent): in-square elements (status bar, joystick instruction text)
// sit over the camera; the touch back button sits in the parent's top-left gutter,
// OUTSIDE the square, so it never overlaps live pixels and never needs per-frame
// recompositing. The back affordance is INPUT-MODE dependent (input_profile_get_mode):
//   - INPUT_MODE_HARDWARE (joystick): on-preview bottom-center instruction text
//     (matches Python ScanScreen). The overlay OWNS the keys: it puts an invisible
//     focusable sink in its own group and posts SEEDSIGNER_RET_BACK_BUTTON on LEFT or
//     RIGHT, the same event the touch back button posts (Python ScanScreen exits on
//     either). It must own them — the keypad indev is left group-less by the screen
//     swap, so any key the overlay does not claim is discarded by LVGL and the flow
//     cannot be backed out of.
//   - INPUT_MODE_TOUCH: the shared back_button() alone in the top-left gutter; the
//     instruction text is omitted.

// Decode status of the most-recent animated-QR frame — mirrors Python ScanScreen
// FRAME__* so the host forwards one int. Drives the status dot's color/visibility.
typedef enum {
    CAMERA_OVERLAY_FRAME_NONE     = 0,  // no recent frame info → dot hidden
    CAMERA_OVERLAY_FRAME_ADDED    = 1,  // new part decoded → green dot
    CAMERA_OVERLAY_FRAME_REPEATED = 2,  // already-seen part → gray dot
    CAMERA_OVERLAY_FRAME_MISS     = 3,  // nothing decoded → dot hidden
} camera_overlay_frame_status_t;

// Declarative overlay spec — describes WHAT the overlay shows, free of any ESP
// compositing detail. The input mode is read at render time from
// input_profile_get_mode(), so it is intentionally NOT a field here.
typedef struct {
    // Full bottom-line text for HARDWARE/joystick mode (the host composes the whole
    // line, e.g. "< back  |  Scan a QR code", already localized). Ignored in touch
    // mode. NULL/empty → no instruction text.
    const char *instructions_text;

    // Preview-square geometry within the parent (host-supplied: the camera adapter
    // knows where the square sits). The desktop tools pass a centered square = the
    // short display dimension, exposing the landscape gutters.
    int32_t square_x;
    int32_t square_y;
    int32_t square_w;
    int32_t square_h;

    // Initial state. scanning_active=false shows the back affordance; true shows the
    // status bar at progress_percent / frame_status.
    bool                          scanning_active;
    int                           progress_percent;  // 0..100
    camera_overlay_frame_status_t frame_status;
} camera_preview_overlay_spec_t;

// Opaque handle holding the built widgets + cached geometry for live updates.
typedef struct camera_preview_overlay camera_preview_overlay_t;

// Build the overlay onto `parent` (a container spanning the full display; the camera
// square occupies spec->square_* within it). Returns a handle the host updates via the
// functions below. The widgets are children of `parent`; deleting `parent` (e.g. on a
// screen rebuild) frees them — call camera_preview_overlay_destroy() to free the handle.
camera_preview_overlay_t *camera_preview_overlay_create(lv_obj_t *parent,
                                                        const camera_preview_overlay_spec_t *spec);

// Toggle between the back-affordance state and the status-bar state. In hardware mode
// the instruction text and the status bar are mutually exclusive (Python parity); in
// touch mode the gutter back button persists and only the status bar toggles.
void camera_preview_overlay_set_scanning(camera_preview_overlay_t *overlay, bool active);

// Update the status bar from a host decode-progress event (a few per second, never per
// camera frame): resizes the fill, sets the percent label, and colors/hides the status
// dot per frame_status. Implies scanning (raises the bar if not already shown).
void camera_preview_overlay_set_progress(camera_preview_overlay_t *overlay,
                                         int percent,
                                         camera_overlay_frame_status_t frame_status);

// Free the handle struct (does NOT delete the LVGL widgets — they belong to the parent
// tree). Safe to skip if the parent screen is about to be deleted anyway.
void camera_preview_overlay_destroy(camera_preview_overlay_t *overlay);

#ifdef __cplusplus
}
#endif

#endif // SEEDSIGNER_CAMERA_PREVIEW_OVERLAY_H
