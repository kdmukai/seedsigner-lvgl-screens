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
    int                           progress_percent;  // 0..100 (continuous mode only)
    camera_overlay_frame_status_t frame_status;

    // Segmented progress for indexed animated-QR cycles (BBQR / Specter): when
    // total_segments > 0 the track renders as total_segments discrete per-frame cells on a
    // thickened bar instead of the continuous fill, and each decoded frame fills its own cell
    // (out-of-order aware — a middle-first scan fills the middle cell). total_segments
    // <= 0 (the default) keeps the continuous fill, which is what UR/fountain codes and
    // unknown-total scans use (no fixed cycle of discrete parts to map to cells).
    //
    // When the cells are wide enough they are drawn as OUTLINED boxes: every cell is visible
    // from the start as an empty slot with a light-gray outline, and its interior fills green as
    // its frame decodes — the outline staying a distinct gray so the cell grid still reads once
    // filled. When too many segments make the cells too narrow for an outline, the outlines are
    // dropped and the cells degrade to bare green fills that appear as their frames decode.
    //
    // The track is partitioned by cumulative floor (cell i spans [i*W/N, (i+1)*W/N)), so
    // cell widths differ by at most 1px and always sum to the full track — never subpixel,
    // never overflow. In the borderless degenerate, if total_segments exceeds the track width
    // in px the surplus cells collapse to zero width and are skipped: up to track_width frames
    // get a 1px cell, and any beyond that simply don't render — far past any real BBQR.
    int                           total_segments;      // 0 => continuous mode
    const uint8_t                *decoded;             // len == total_segments; nonzero = frame decoded. NULL => none lit
    int                           just_decoded_index;  // 0-based most-recent new frame, or -1 (reserved: recent-cell emphasis)
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

// --- Segmented (indexed-cycle) live interface: begin once, then one event per frame ------
// For animated QRs with a fixed, indexed cycle (BBQR / Specter). Instead of pushing the
// whole decoded set each call, the host announces the cycle size once and then reports each
// decode event; the SCREEN owns the decoded list and derives the percent. This mirrors the
// app's scan loop 1:1 (decoder.add() -> one DecodeQRStatus per frame; total_segments known
// after the first frame; the just-added piece index available from the decoder).

// Announce a segmented scan: the first decoded frame revealed a cycle of `total_segments`
// pieces. Builds the empty N-cell bar and resets the decoded list. Idempotent for the same
// N (keeps progress); a different N rebuilds. Implies scanning. total_segments <= 0 is a
// no-op — continuous/UR/fountain scans use set_progress() above.
void camera_preview_overlay_begin_segments(camera_preview_overlay_t *overlay, int total_segments);

// Report one decode event (a few/sec). `status` sets the most-recent-frame dot, and for a
// decoded piece `piece_index` (0-based) marks the "current" cell — the piece the camera is on,
// emphasized differently for new vs re-read:
//   ADDED    -> `piece_index` is a NEWLY decoded piece: fills its cell green once, advances the
//               derived percent (idempotent if already decoded), and makes it the current cell —
//               marked by a bright WHITE outline (or, in the borderless degenerate, a green
//               perpendicular "burst").
//   REPEATED -> `piece_index` is an already-seen piece being re-read: no percent change, but
//               its (already-filled) cell becomes the current cell drawn WHITE — so the user
//               sees which piece is stuck in view. Pass the re-read index, not -1.
//   MISS/NONE-> nothing decoded; updates the dot only (pass piece_index = -1; current stays).
// As the cursor advances, the prior current cell settles back to plain decoded green. No-op
// before begin_segments().
void camera_preview_overlay_segment_event(camera_preview_overlay_t *overlay,
                                          camera_overlay_frame_status_t status,
                                          int piece_index);

// Free the handle struct (does NOT delete the LVGL widgets — they belong to the parent
// tree). Safe to skip if the parent screen is about to be deleted anyway.
void camera_preview_overlay_destroy(camera_preview_overlay_t *overlay);

#ifdef __cplusplus
}
#endif

#endif // SEEDSIGNER_CAMERA_PREVIEW_OVERLAY_H
