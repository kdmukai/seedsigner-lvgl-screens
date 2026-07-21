#ifndef SEEDSIGNER_CAMERA_PREVIEW_PILLARBOXED_H
#define SEEDSIGNER_CAMERA_PREVIEW_PILLARBOXED_H

#include "lvgl.h"
#include "camera_preview_overlay.h"   // reuse camera_overlay_frame_status_t
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Pillarboxed camera-preview chrome — portable spec + renderer
// ---------------------------------------------------------------------------
// The sibling of camera_preview_overlay for panels whose scan preview is drawn on a
// NATIVE-PORTRAIT display that is PHYSICALLY MOUNTED LANDSCAPE (the ESP32-P4 4.3" DSI).
// The camera fills a centered square that spans the panel's short (portrait width) axis;
// the two strips above and below it (portrait top/bottom letterbox) present as the LEFT
// and RIGHT pillars once the panel is rotated into its landscape mounting. Unlike
// camera_preview_overlay, NOTHING is drawn over the live camera square — all chrome
// (progress bar, status dot, percent, back button) lives in those pillar strips. Hence
// "pillarboxed", not "overlay".
//
// Everything is authored in PORTRAIT coordinates (the panel's native frame). A caller
// mapping portrait (x,y) -> landscape (lx = panel_h - y, ly = x) can read the intended
// on-screen result: the portrait TOP strip is the landscape RIGHT pillar (progress bar +
// percent + dot), the portrait BOTTOM strip is the landscape LEFT pillar (back button in
// the landscape top-left corner). The one place orientation "shows" is text: a label
// drawn normally in portrait reads sideways once mounted, so the percent readout is
// pre-rotated 90 deg CCW (see the .cpp) to cancel the mount's 90 deg CW. The back
// button instead uses the CHEVRON_DOWN glyph, which needs no rotation — a down chevron
// points "left" once mounted.
//
// Like camera_preview_overlay this is a PASSIVE view: it never polls the camera or
// decoder. The host raises the bar and advances progress via set_scanning()/set_progress()
// a few times per second. Portability: pure LVGL, no board/ESP dependency, so the desktop
// screenshot generator + web runner render it identically to the device (they simulate the
// physical mount by presenting the portrait render rotated to landscape).

// Declarative spec — WHAT the chrome shows, free of any compositing detail. The camera
// square geometry is host-supplied (the adapter knows where the square sits within the
// portrait parent); everything else derives from it + the display resolution.
typedef struct {
    // Camera preview square within the (portrait) parent. On the 4.3 this is the full
    // 480-wide, vertically centered 480x480 (square_y = (800-480)/2 = 160).
    int32_t square_x;
    int32_t square_y;
    int32_t square_w;
    int32_t square_h;

    // Initial state. scanning_active=false hides the status bar (back button persists);
    // true shows the bar at progress_percent / frame_status.
    bool                          scanning_active;
    int                           progress_percent;  // 0..100 (continuous mode only)
    camera_overlay_frame_status_t frame_status;

    // Segmented progress for indexed animated-QR cycles (BBQR / Specter) — the vertical-bar
    // analogue of camera_preview_overlay's fields. total_segments > 0 renders the thickened bar
    // as total_segments discrete per-frame cells (frame 0 at the landscape-bottom anchor, growing
    // toward the top) and fills each decoded frame's own cell, out-of-order aware. Cells wide
    // enough are drawn as OUTLINED boxes (empty from the start, filling green as frames decode);
    // too many segments drops the outlines to bare fills.
    // total_segments <= 0 (default) keeps the continuous fill (UR/fountain/unknown-total).
    int                           total_segments;      // 0 => continuous mode
    const uint8_t                *decoded;             // len == total_segments; nonzero = frame decoded. NULL => none lit
    int                           just_decoded_index;  // 0-based most-recent new frame, or -1 (reserved)
} camera_preview_pillarboxed_spec_t;

// Opaque handle holding the built widgets + cached geometry for live updates.
typedef struct camera_preview_pillarboxed camera_preview_pillarboxed_t;

// Build the chrome onto `parent` (a container spanning the full portrait display; the
// camera square occupies spec->square_* within it). Returns a handle the host updates via
// the functions below. Widgets are children of `parent`; deleting `parent` frees them —
// call camera_preview_pillarboxed_destroy() to free the handle (and cancel any pending
// progress glide).
camera_preview_pillarboxed_t *camera_preview_pillarboxed_create(
    lv_obj_t *parent, const camera_preview_pillarboxed_spec_t *spec);

// Toggle between the back-affordance-only state and the status-bar state. The back button
// persists in both (it lives in a pillar, not over the camera).
void camera_preview_pillarboxed_set_scanning(camera_preview_pillarboxed_t *o, bool active);

// Update from a host decode-progress event (a few per second, never per camera frame):
// grows the vertical fill, repaints the pre-rotated percent, and colors/hides the status
// dot per frame_status. Implies scanning (raises the bar if not already shown).
void camera_preview_pillarboxed_set_progress(camera_preview_pillarboxed_t *o,
                                             int percent,
                                             camera_overlay_frame_status_t frame_status);

// --- Segmented (indexed-cycle) live interface: begin once, then one event per frame ------
// The vertical-bar analogue of the overlay's begin_segments/segment_event. Instead of pushing
// the whole decoded set each call, the host announces the cycle size once and reports each
// decode event; the screen owns the decoded list and derives the pre-rotated percent.

// Announce a segmented scan: the first decoded frame revealed a cycle of `total_segments`
// pieces. Builds the empty N-cell vertical bar (outlined cells, or bare fills if too dense),
// resets the decoded list, and reveals the meter (segmented implies an animated QR). Idempotent
// for the same N; a different N rebuilds.
// total_segments <= 0 is a no-op (use set_progress() for continuous/UR/fountain).
void camera_preview_pillarboxed_begin_segments(camera_preview_pillarboxed_t *o, int total_segments);

// Report one decode event. ADDED fills piece `piece_index` (0-based) green once, advances the
// derived percent, and makes it the current cell — marked by a bright WHITE outline (or, in the
// borderless degenerate, a green perpendicular "burst"). REPEATED passes the re-read index too:
// no percent change, but its already-filled cell becomes the current cell drawn WHITE, so the
// piece stuck in view is visible. MISS updates only the dot (pass piece_index = -1). No-op before
// begin_segments().
void camera_preview_pillarboxed_segment_event(camera_preview_pillarboxed_t *o,
                                              camera_overlay_frame_status_t status,
                                              int piece_index);

// Free the handle struct (does NOT delete the LVGL widgets — they belong to the parent
// tree) and cancel any in-flight progress glide.
void camera_preview_pillarboxed_destroy(camera_preview_pillarboxed_t *o);

#ifdef __cplusplus
}
#endif

#endif // SEEDSIGNER_CAMERA_PREVIEW_PILLARBOXED_H
