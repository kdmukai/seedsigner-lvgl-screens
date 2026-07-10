// seed_transcribe_zoomed_qr_screen
//
// Python provenance: SeedTranscribeSeedQRZoomedInScreen (seed_screens.py)
//
// The zoomed-in, pannable step of the SeedQR hand-transcription flow. A hand
// transcribes a SeedQR onto a paper template by reading it one "zone" (a 5x5 or
// 7x7 block of modules) at a time. This screen renders the QR OVERSIZED — each
// module is a fat 24 px (base) circle — and dims everything except the one
// centered zone, with an accent-outlined window plus A-F / 1-6 zone-coordinate
// labels so the reader always knows which template cell they are on. The joystick
// (or a touch swipe) steps one whole zone at a time; the joystick click (hardware)
// or a top-right X button (touch) exits, reporting SEEDSIGNER_RET_BACK_BUTTON via
// seedsigner_lvgl_on_button_selected exactly once.
//
// OVERSIZED WITHOUT AN OVERSIZED BUFFER. Python builds a giant square bitmap
// ((border+num_modules+border)*24 px per side = ~0.95-1.6 MB RGB565) and crops a
// display-sized window out of it each pan step, alpha-compositing a full-canvas
// zone mask on top. That buffer alone overflows the ESP32's fixed ~128 KB LVGL
// pool and would hard-freeze the device (the identical canvas-OOM class fixed on
// qr_display_screen and psbt_overview_screen — see docs/qr-display-screen-esp32-
// canvas-oom.md). So there is NO oversized bitmap here: the module matrix (from the
// bundled qrcodegen, shared with qr_display_screen via qr_encode_bytes) is
// direct-drawn per-module in a DRAW_MAIN_END callback, and only the modules that
// fall inside the viewport are painted — panning is just a change of draw offset.
// The dimming mask, the accent window outline, and the zone-label bars are plain
// LVGL widgets (rectangles + labels), so LVGL recomposites them fringe-free with no
// per-frame memcpy.
//
// Geometry mirrors Python exactly at the 240 px reference (pixels_per_module = 24)
// and scales with the profile px_multiplier (~31 px at 320-height, 48 px at 480), so
// the zoom stays physically consistent across displays. Data modules are round dots;
// the fixed registration patterns (finder eyes + alignment block) are SOLID squares,
// matching both the qrcode lib's CircleModuleDrawer and — crucially — the pre-printed
// registration blocks on the paper SeedQR templates the transcriber fills in. Light
// #bbb gridlines overlay the whole content (incl. the solid blocks) as a counting
// guide. Rendering fidelity note: Python's rounded modules come from resizing a
// low-res StyledPilImage (soft, blurry circles); the direct-draw path paints crisp
// dots/squares instead — same visual language, sharper. The module matrix is
// pixel-identical for SeedQR (numeric); CompactSeedQR (byte) may pick a different
// mask than Python's qrcode lib, which is harmless (the transcribed QR still scans,
// and the confirm-scan step verifies it).
//
// Chrome-free tier: no top-nav scaffold — a bare root screen holds the QR field,
// the dim mask, the zone window, and the coordinate bars; the mandatory
// load_screen_and_cleanup_previous tail still applies.
//
// Lifecycle (stateful, Tier 2): one heap ctx owning the keypad group (hardware
// mode), the qrcodegen buffers, and any in-flight pan animation; the
// LV_EVENT_DELETE cleanup callback on the screen root tears all of it down.
//
// cfg:
//   qr_data         (string, required, non-empty)  the SeedQR payload — a digit
//            stream (standard SeedQR) or the CompactSeedQR entropy bytes, carried
//            per data_encoding.
//   qr_mode         (string, default "numeric")    qrcodegen segment mode:
//            numeric | alphanumeric | byte | auto. SeedQR is numeric, CompactSeedQR
//            is byte; "numeric" is this screen's domain default (qr_display, which
//            serves arbitrary payloads, defaults "auto").
//   data_encoding   (string, default "utf8")       JSON transport encoding of
//            qr_data: utf8 | hex | base64 (JSON cannot carry raw bytes, so binary
//            CompactSeedQR payloads arrive hex-encoded).
//   exit_text       (string, required)             localized exit hint (Python:
//            _("click to exit")), supplied already translated by the host view
//            layer. Rendered only in hardware input mode (touch exits via the
//            gutter X) but required in both — one uniform contract.
//   initial_zone_x  (int, default 0)               starting zone column, clamped to
//            [0, zones-1] (Python dataclass default 0; the screenshot generator /
//            host uses it to frame an interesting cell).
//   initial_zone_y  (int, default 0)               starting zone row, same clamp.
//   input.mode      (string, optional)             "touch" | "hardware" input-mode
//            override, read via nav_mode_override_from_cfg (screen_helpers);
//            absent -> the platform input profile decides.
//
// Note: Python's num_modules field (present in the scenarios) is deliberately NOT
// read — the module count is derived from the encoded matrix (qrcodegen_getSize).

#include "screen_scaffold.h"  // parse_screen_json_ctx, load_screen_and_cleanup_previous
#include "seedsigner.h"       // seed_transcribe_zoomed_qr_screen decl, SEEDSIGNER_RET_BACK_BUTTON, seedsigner_lvgl_on_button_selected, seedsigner_lvgl_is_static_render
#include "gui_constants.h"    // ACCENT_COLOR, KEYBOARD_FONT, BODY_FONT, BODY_FONT_COLOR, BACKGROUND_COLOR, BUTTON_SELECTED_FONT_COLOR, COMPONENT_PADDING, active_profile
#include "input_profile.h"    // input_mode_t, INPUT_MODE_TOUCH/HARDWARE, input_profile_get_mode
#include "qr_core.h"          // qr_encode_mode_t, qr_decode_payload, qr_encode_bytes, build_gutter_close_button
#include "screen_helpers.h"   // nav_mode_override_from_cfg

#include "lvgl.h"             // widgets, direct-draw layer API, lv_anim, lv_group/lv_indev

#include <nlohmann/json.hpp>  // json (cfg reads)

#include <stdexcept>          // std::runtime_error (required-field validation)
#include <string>             // std::string
#include <vector>             // std::vector (decoded QR payload bytes)

// Feature-gated last: LVGL's bundled qrcodegen supplies the module matrix. This
// file lives one level deeper (screens/), so the repo-root-relative reach needs an
// extra ../ vs. the component-root copy.
#if LV_USE_QRCODE
#include "../../../third_party/lvgl/src/libs/qrcode/qrcodegen.h"
#endif

using json = nlohmann::json;


#if LV_USE_QRCODE

// One zone-step of pan is animated over this long on live runners (snap on stills).
// Deliberately unhurried: the eye has to follow the sliding window to the next grid
// cell, so a slow ease-in-out slide reads far better than a quick snap.
constexpr uint32_t SEED_TRANSCRIBE_ZOOMED_QR_PAN_MS = 600;

// The 4-module quiet zone Python bakes around the oversized QR. It exists only so the
// viewport never runs off the matrix at an edge zone; we reproduce it as a draw offset
// (the white qr_field background IS the quiet-zone field — no modules are drawn there).
constexpr int SEED_TRANSCRIBE_ZOOMED_QR_BORDER_MODULES = 4;

namespace {

// ---------------------------------------------------------------------------
// Screen context
// ---------------------------------------------------------------------------

// Screen context — pure POD (pointers/ints/bools only), so it is allocated with
// lv_malloc + lv_memzero and released with lv_free in the LV_EVENT_DELETE cleanup
// callback (any C++ member would require new/delete for ctor/dtor correctness).
struct seed_transcribe_zoomed_qr_ctx_t {
    lv_obj_t   *screen;
    lv_obj_t   *qr_field;       // full-screen white field; modules direct-drawn in the draw cb
    lv_obj_t   *column_label;   // "1".."6" in the top accent bar (current column)
    lv_obj_t   *row_label;      // "A".."F" in the left accent bar (current row)
    lv_group_t *group;          // hardware keypad group (NULL in touch mode)

    input_mode_t input_mode;
    bool         emitted;       // exit reported once

    // QR + zone geometry (all in screen px unless noted).
    int size;                   // qrcodegen module count per side
    int modules_per_zone;       // 7 for a 21-module QR, else 5
    int pixels_per_module;      // 24 * px_multiplier / 100
    int zones;                  // ceil(size / modules_per_zone): zone count per axis (square QR)
    int zone_px;                // modules_per_zone * pixels_per_module: the highlighted window edge
    int window_offset_x;        // top-left of the centered zone window
    int window_offset_y;
    int screen_width, screen_height;

    // Current zone + the pan offset (top-left of the conceptual oversized crop).
    // Module (module_x,module_y) is painted at screen
    // (border+module_x)*pixels_per_module - pan_x, (border+module_y)*pixels_per_module - pan_y.
    int current_zone_x, current_zone_y;
    int pan_x, pan_y;
    bool animating;             // ignore nav input mid-slide (Python's pan is blocking)
    lv_anim_t *pan_animation;

    // Touch swipe (press->release delta; LVGL's built-in gesture detection is velocity-
    // gated and didn't fire reliably here). A drag past swipe_threshold steps one zone.
    int press_x, press_y;
    int swipe_threshold;

    uint8_t *encode_scratch;    // qrcodegen scratch buffer (one-shot encode at build time)
    uint8_t *qr_matrix;         // encoded qrcodegen module matrix
    bool     have_frame;
};

// ---------------------------------------------------------------------------
// Geometry helpers
// ---------------------------------------------------------------------------

// The pan offset that centers zone `zone_index` in the window (Python's cur_pixel_x/y).
int seed_transcribe_zoomed_qr_pan_for_zone(seed_transcribe_zoomed_qr_ctx_t *ctx,
                                           int zone_index, int window_offset) {
    return zone_index * ctx->modules_per_zone * ctx->pixels_per_module
           + SEED_TRANSCRIBE_ZOOMED_QR_BORDER_MODULES * ctx->pixels_per_module - window_offset;
}

// Is module (module_x,module_y) part of a fixed registration pattern (a finder "eye"
// or the alignment block)? Those are drawn as SOLID squares, not round dots — the
// SeedQR paper templates come with the registration blocks pre-printed in their normal
// square QR form, so the screen must match them (only the DATA modules are the fat
// dots the transcriber fills in). The qrcode lib's CircleModuleDrawer does the same:
// square eyes, round data. Alignment position follows Python's square-off (module 16
// for a 25-module QR, 20 for 29; version-1 21-module QRs have no alignment pattern).
bool seed_transcribe_zoomed_qr_is_registration(int module_x, int module_y, int size) {
    bool finder = (module_x < 7 && module_y < 7) ||                 // top-left
                  (module_x >= size - 7 && module_y < 7) ||         // top-right
                  (module_x < 7 && module_y >= size - 7);           // bottom-left
    int align = (size == 25) ? 16 : (size == 29) ? 20 : -1;
    bool alignment = align >= 0 &&
                     module_x >= align && module_x < align + 5 &&
                     module_y >= align && module_y < align + 5;
    return finder || alignment;
}

// ---------------------------------------------------------------------------
// Direct-draw viewport
// ---------------------------------------------------------------------------

// Direct-draw the visible modules + the transcription gridlines onto qr_field's layer
// (on top of its white background). Only modules whose cell intersects the object
// area are painted, so the pass is bounded by the viewport (~10x10 cells) no matter
// how large the QR is. Panning changes pan_x/pan_y and invalidates the object.
void seed_transcribe_zoomed_qr_draw_cb(lv_event_t *e) {
    seed_transcribe_zoomed_qr_ctx_t *ctx   = (seed_transcribe_zoomed_qr_ctx_t *)lv_event_get_user_data(e);
    lv_layer_t                      *layer = lv_event_get_layer(e);
    if (!ctx || !layer || !ctx->have_frame) return;

    lv_area_t field_area;
    lv_obj_get_coords(ctx->qr_field, &field_area);  // absolute; the object fills the screen

    const int pixels_per_module = ctx->pixels_per_module;
    const int border            = SEED_TRANSCRIBE_ZOOMED_QR_BORDER_MODULES;
    const int size              = ctx->size;

    // Two black module styles: DATA modules are round dots; the fixed registration
    // patterns (finder eyes + alignment block) are SOLID squares so adjacent cells
    // tile into the connected shapes the paper templates pre-print (see
    // seed_transcribe_zoomed_qr_is_registration). Full-cell squares mean neighbours
    // abut seamlessly.
    lv_draw_rect_dsc_t dot;
    lv_draw_rect_dsc_init(&dot);
    dot.bg_color = lv_color_black();
    dot.bg_opa   = LV_OPA_COVER;
    dot.radius   = LV_RADIUS_CIRCLE;
    lv_draw_rect_dsc_t square = dot;
    square.radius = 0;

    for (int module_y = 0; module_y < size; module_y++) {
        int cell_y = field_area.y1 + (border + module_y) * pixels_per_module - ctx->pan_y;
        if (cell_y + pixels_per_module <= field_area.y1 || cell_y > field_area.y2) continue;  // vertical cull
        for (int module_x = 0; module_x < size; module_x++) {
            if (!qrcodegen_getModule(ctx->qr_matrix, module_x, module_y)) continue;
            int cell_x = field_area.x1 + (border + module_x) * pixels_per_module - ctx->pan_x;
            if (cell_x + pixels_per_module <= field_area.x1 || cell_x > field_area.x2) continue;  // horizontal cull
            if (seed_transcribe_zoomed_qr_is_registration(module_x, module_y, size)) {
                // Registration blocks fill the whole cell so neighbours tile into one
                // solid shape (the gridlines overlay them as a guide, by design).
                lv_area_t module_area = { cell_x, cell_y,
                                          cell_x + pixels_per_module - 1, cell_y + pixels_per_module - 1 };
                lv_draw_rect(layer, &square, &module_area);
            } else {
                // Data dots are inset by 1px so the 1px gridlines (drawn on top, at each
                // cell's top+left edge) FRAME the dot instead of clipping it — the whole
                // circle renders. Crisper + more accurate than Python's zoomed AA blur.
                lv_area_t module_area = { cell_x + 1, cell_y + 1,
                                          cell_x + pixels_per_module - 1, cell_y + pixels_per_module - 1 };
                lv_draw_rect(layer, &dot, &module_area);
            }
        }
    }

    // Light gridlines at every content-module boundary (Python draws them ON TOP of
    // the modules, in #bbb, spanning only the content area — never the quiet zone).
    // They give the transcriber a per-cell grid to count against.
    lv_draw_rect_dsc_t grid;
    lv_draw_rect_dsc_init(&grid);
    grid.bg_color = lv_color_hex(0xBBBBBB);
    grid.bg_opa   = LV_OPA_COVER;

    int content_top    = field_area.y1 + border * pixels_per_module - ctx->pan_y;
    int content_bottom = field_area.y1 + (border + size) * pixels_per_module - ctx->pan_y;
    int content_left   = field_area.x1 + border * pixels_per_module - ctx->pan_x;
    int content_right  = field_area.x1 + (border + size) * pixels_per_module - ctx->pan_x;
    int vertical_line_top    = LV_MAX(content_top, field_area.y1);
    int vertical_line_bottom = LV_MIN(content_bottom, field_area.y2);
    int horizontal_line_left  = LV_MAX(content_left, field_area.x1);
    int horizontal_line_right = LV_MIN(content_right, field_area.x2);
    // i runs border..border+size INCLUSIVE: size+1 boundaries close every cell, incl. the
    // far-right column edge and far-bottom row edge (a bare `< border+size` left those open).
    for (int i = border; i <= border + size; i++) {
        int gridline_x = field_area.x1 + i * pixels_per_module - ctx->pan_x;   // vertical line
        if (gridline_x >= field_area.x1 && gridline_x <= field_area.x2 &&
            vertical_line_top <= vertical_line_bottom) {
            lv_area_t line_area = { gridline_x, vertical_line_top, gridline_x, vertical_line_bottom };
            lv_draw_rect(layer, &grid, &line_area);
        }
        int gridline_y = field_area.y1 + i * pixels_per_module - ctx->pan_y;   // horizontal line
        if (gridline_y >= field_area.y1 && gridline_y <= field_area.y2 &&
            horizontal_line_left <= horizontal_line_right) {
            lv_area_t line_area = { horizontal_line_left, gridline_y, horizontal_line_right, gridline_y };
            lv_draw_rect(layer, &grid, &line_area);
        }
    }
}

// ---------------------------------------------------------------------------
// Pan machinery
// ---------------------------------------------------------------------------

// Rewrite the two zone-coordinate labels for the current zone ("3" / "C").
void seed_transcribe_zoomed_qr_update_labels(seed_transcribe_zoomed_qr_ctx_t *ctx) {
    char column_text[4]; lv_snprintf(column_text, sizeof(column_text), "%d", ctx->current_zone_x + 1);
    char row_text[2] = { (char)('A' + ctx->current_zone_y), 0 };
    lv_label_set_text(ctx->column_label, column_text);
    lv_label_set_text(ctx->row_label, row_text);
}

void seed_transcribe_zoomed_qr_pan_exec_x(void *var, int32_t value) {
    seed_transcribe_zoomed_qr_ctx_t *ctx = (seed_transcribe_zoomed_qr_ctx_t *)var;
    ctx->pan_x = value;
    lv_obj_invalidate(ctx->qr_field);
}
void seed_transcribe_zoomed_qr_pan_exec_y(void *var, int32_t value) {
    seed_transcribe_zoomed_qr_ctx_t *ctx = (seed_transcribe_zoomed_qr_ctx_t *)var;
    ctx->pan_y = value;
    lv_obj_invalidate(ctx->qr_field);
}
void seed_transcribe_zoomed_qr_pan_ready(lv_anim_t *animation) {
    seed_transcribe_zoomed_qr_ctx_t *ctx = (seed_transcribe_zoomed_qr_ctx_t *)animation->var;
    ctx->animating     = false;
    ctx->pan_animation = NULL;
    // Reveal the destination coordinate only once the window has ARRIVED — updating the
    // A-F/1-6 labels mid-slide would show the target cell before the eye gets there.
    seed_transcribe_zoomed_qr_update_labels(ctx);
}

// Step by one zone in (delta_zone_x, delta_zone_y) if that stays on the QR, then slide
// the pan offset to the new zone (snapping on static stills). The zone-coordinate labels
// update on ARRIVAL (in seed_transcribe_zoomed_qr_pan_ready), not here, so they don't
// jump ahead of the window. Nav input is ignored while a slide is in flight, mirroring
// Python's blocking show_image_pan.
void seed_transcribe_zoomed_qr_step(seed_transcribe_zoomed_qr_ctx_t *ctx,
                                    int delta_zone_x, int delta_zone_y) {
    if (ctx->animating) return;
    int next_zone_x = ctx->current_zone_x + delta_zone_x;
    int next_zone_y = ctx->current_zone_y + delta_zone_y;
    if (next_zone_x < 0 || next_zone_x >= ctx->zones ||
        next_zone_y < 0 || next_zone_y >= ctx->zones) return;
    if (next_zone_x == ctx->current_zone_x && next_zone_y == ctx->current_zone_y) return;

    ctx->current_zone_x = next_zone_x;
    ctx->current_zone_y = next_zone_y;

    int target_pan_x = seed_transcribe_zoomed_qr_pan_for_zone(ctx, next_zone_x, ctx->window_offset_x);
    int target_pan_y = seed_transcribe_zoomed_qr_pan_for_zone(ctx, next_zone_y, ctx->window_offset_y);

    if (seedsigner_lvgl_is_static_render()) {
        ctx->pan_x = target_pan_x;
        ctx->pan_y = target_pan_y;
        seed_transcribe_zoomed_qr_update_labels(ctx);   // no animation on stills: reflect the zone immediately
        lv_obj_invalidate(ctx->qr_field);
        return;
    }

    // Only one axis changes per step; animate that one.
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, ctx);
    lv_anim_set_time(&animation, SEED_TRANSCRIBE_ZOOMED_QR_PAN_MS);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_in_out);  // gentle start+stop = easy to track
    lv_anim_set_ready_cb(&animation, seed_transcribe_zoomed_qr_pan_ready);
    if (delta_zone_x) {
        lv_anim_set_values(&animation, ctx->pan_x, target_pan_x);
        lv_anim_set_exec_cb(&animation, seed_transcribe_zoomed_qr_pan_exec_x);
    } else {
        lv_anim_set_values(&animation, ctx->pan_y, target_pan_y);
        lv_anim_set_exec_cb(&animation, seed_transcribe_zoomed_qr_pan_exec_y);
    }
    ctx->animating     = true;
    ctx->pan_animation = lv_anim_start(&animation);
}

// ---------------------------------------------------------------------------
// Exit + input callbacks
// ---------------------------------------------------------------------------

void seed_transcribe_zoomed_qr_exit(seed_transcribe_zoomed_qr_ctx_t *ctx) {
    if (ctx->emitted) return;
    ctx->emitted = true;
    seedsigner_lvgl_on_button_selected(SEEDSIGNER_RET_BACK_BUTTON, "seed_transcribe_zoomed_qr_done");
}

// Hardware joystick: arrows step one zone; any other key (a click) exits — Python's
// KEYS__LEFT_RIGHT_UP_DOWN pan / KEYS__ANYCLICK exit.
void seed_transcribe_zoomed_qr_key_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_KEY) return;
    seed_transcribe_zoomed_qr_ctx_t *ctx = (seed_transcribe_zoomed_qr_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;
    switch (lv_event_get_key(e)) {
        case LV_KEY_RIGHT: seed_transcribe_zoomed_qr_step(ctx, +1,  0); break;
        case LV_KEY_LEFT:  seed_transcribe_zoomed_qr_step(ctx, -1,  0); break;
        case LV_KEY_DOWN:  seed_transcribe_zoomed_qr_step(ctx,  0, +1); break;
        case LV_KEY_UP:    seed_transcribe_zoomed_qr_step(ctx,  0, -1); break;
        default:           seed_transcribe_zoomed_qr_exit(ctx);         break;
    }
}

// Touch swipe = one full zone step, detected as a press->release delta (LVGL's built-in
// LV_EVENT_GESTURE is velocity-gated and did not fire reliably for a slow drag here, so we
// measure the drag ourselves). Content-drag / scroll sense: dragging the QR LEFT reveals
// the zone to its RIGHT, like panning a map. Full-step only — never free positioning. A
// drag shorter than swipe_threshold is a tap and does nothing (touch exits via the X).
void seed_transcribe_zoomed_qr_press_cb(lv_event_t *e) {
    seed_transcribe_zoomed_qr_ctx_t *ctx = (seed_transcribe_zoomed_qr_ctx_t *)lv_event_get_user_data(e);
    lv_indev_t *indev = lv_indev_active();
    if (!ctx || !indev) return;
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    ctx->press_x = point.x;
    ctx->press_y = point.y;
}
void seed_transcribe_zoomed_qr_release_cb(lv_event_t *e) {
    seed_transcribe_zoomed_qr_ctx_t *ctx = (seed_transcribe_zoomed_qr_ctx_t *)lv_event_get_user_data(e);
    lv_indev_t *indev = lv_indev_active();
    if (!ctx || !indev) return;
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    int delta_x = point.x - ctx->press_x;
    int delta_y = point.y - ctx->press_y;
    int absolute_delta_x = LV_ABS(delta_x), absolute_delta_y = LV_ABS(delta_y);
    if (absolute_delta_x < ctx->swipe_threshold &&
        absolute_delta_y < ctx->swipe_threshold) return;  // tap, not a swipe
    if (absolute_delta_x >= absolute_delta_y) {
        if (delta_x < 0) seed_transcribe_zoomed_qr_step(ctx, +1,  0);   // drag left  -> next column right
        else             seed_transcribe_zoomed_qr_step(ctx, -1,  0);
    } else {
        if (delta_y < 0) seed_transcribe_zoomed_qr_step(ctx,  0, +1);   // drag up    -> next row below
        else             seed_transcribe_zoomed_qr_step(ctx,  0, -1);
    }
}
void seed_transcribe_zoomed_qr_close_cb(lv_event_t *e) {
    seed_transcribe_zoomed_qr_ctx_t *ctx = (seed_transcribe_zoomed_qr_ctx_t *)lv_event_get_user_data(e);
    if (ctx) seed_transcribe_zoomed_qr_exit(ctx);
}

// LV_EVENT_DELETE teardown on the screen root: kill any in-flight pan animation,
// delete the hardware keypad group, free the qrcodegen buffers, free the ctx.
void seed_transcribe_zoomed_qr_cleanup_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;
    seed_transcribe_zoomed_qr_ctx_t *ctx = (seed_transcribe_zoomed_qr_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;
    if (ctx->pan_animation)  lv_anim_delete(ctx, NULL);
    if (ctx->group)          lv_group_del(ctx->group);
    if (ctx->encode_scratch) lv_free(ctx->encode_scratch);
    if (ctx->qr_matrix)      lv_free(ctx->qr_matrix);
    lv_free(ctx);
}

// ---------------------------------------------------------------------------
// Widget micro-builders
// ---------------------------------------------------------------------------

// One flat, non-interactive dimming panel (black at Python's 226/255 opacity). The
// four of them frame the transparent zone window; kept out of the input path so taps
// and swipes reach the QR object beneath.
lv_obj_t *seed_transcribe_zoomed_qr_dim_rect(lv_obj_t *parent, int x, int y, int w, int h) {
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_remove_style_all(panel);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_style_bg_color(panel, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(panel, 226, LV_PART_MAIN);
    lv_obj_remove_flag(panel, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    return panel;
}

// An accent zone-coordinate bar (top = column number, left = row letter) with a
// centered fixed-width label in the selected-font (dark) color, per Python.
lv_obj_t *seed_transcribe_zoomed_qr_bar(lv_obj_t *parent, int x, int y, int w, int h,
                                        lv_obj_t **out_label) {
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_size(bar, w, h);
    lv_obj_set_style_bg_color(bar, lv_color_hex(ACCENT_COLOR), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(bar, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    lv_obj_t *label = lv_label_create(bar);
    lv_obj_set_style_text_font(label, &KEYBOARD_FONT, LV_PART_MAIN);  // Inconsolata-SemiBold (fixed-width emphasis)
    lv_obj_set_style_text_color(label, lv_color_hex(BUTTON_SELECTED_FONT_COLOR), LV_PART_MAIN);
    lv_obj_center(label);
    *out_label = label;
    return bar;
}

}  // namespace

#endif  // LV_USE_QRCODE


void seed_transcribe_zoomed_qr_screen(void *ctx_json) {
#if !LV_USE_QRCODE
    // Built without the bundled QR encoder (no shipping build does this). Load a
    // blank screen so the entry point exists and navigation does not crash.
    (void)ctx_json;
    lv_obj_t *screen_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen_root, lv_color_black(), LV_PART_MAIN);
    load_screen_and_cleanup_previous(screen_root);
#else
    // --- Config ---

    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    // Required-field validation: one throw per field, before any allocation, so no
    // throw path can leak the ctx or LVGL objects.
    if (!cfg.contains("qr_data") || !cfg["qr_data"].is_string() ||
        cfg["qr_data"].get<std::string>().empty()) {
        throw std::runtime_error("seed_transcribe_zoomed_qr_screen: qr_data is required and must be a non-empty string");
    }
    std::string qr_data = cfg["qr_data"].get<std::string>();

    // Encoding mode: SeedQR is numeric, CompactSeedQR is byte. Structural default
    // "numeric" (the SeedQR native mode; qr_display, serving arbitrary payloads,
    // defaults "auto" instead).
    std::string qr_mode_string = cfg.value("qr_mode", std::string("numeric"));
    qr_encode_mode_t qr_mode;
    if (qr_mode_string == "numeric")            qr_mode = QR_ENC_NUMERIC;
    else if (qr_mode_string == "alphanumeric")  qr_mode = QR_ENC_ALNUM;
    else if (qr_mode_string == "byte")          qr_mode = QR_ENC_BYTE;
    else if (qr_mode_string == "auto")          qr_mode = QR_ENC_AUTO;
    else throw std::runtime_error("seed_transcribe_zoomed_qr_screen: qr_mode must be numeric|alphanumeric|byte|auto");

    std::string data_encoding = cfg.value("data_encoding", std::string("utf8"));
    if (data_encoding != "utf8" && data_encoding != "hex" && data_encoding != "base64")
        throw std::runtime_error("seed_transcribe_zoomed_qr_screen: data_encoding must be utf8|hex|base64");

    // exit_text is user-visible CONTENT: it always arrives already localized from the
    // host view layer (Python: _("click to exit")) — an English fallback baked here
    // would ship untranslated. Required in both input modes even though only hardware
    // mode renders it (one uniform contract; touch exits via the gutter X).
    if (!cfg.contains("exit_text") || !cfg["exit_text"].is_string()) {
        throw std::runtime_error("seed_transcribe_zoomed_qr_screen: exit_text is required and must be a string");
    }
    std::string exit_text = cfg["exit_text"].get<std::string>();

    bool has_override = false;
    input_mode_t input_mode_override = INPUT_MODE_TOUCH;
    nav_mode_override_from_cfg(cfg, has_override, input_mode_override);
    input_mode_t input_mode = has_override ? input_mode_override : input_profile_get_mode();

    // --- QR encode + zone geometry ---

    std::vector<uint8_t> payload = qr_decode_payload(qr_data, data_encoding);

    seed_transcribe_zoomed_qr_ctx_t *ctx =
        (seed_transcribe_zoomed_qr_ctx_t *)lv_malloc(sizeof(seed_transcribe_zoomed_qr_ctx_t));
    lv_memzero(ctx, sizeof(*ctx));
    ctx->input_mode = input_mode;
    ctx->encode_scratch = (uint8_t *)lv_malloc(qrcodegen_BUFFER_LEN_MAX);
    ctx->qr_matrix      = (uint8_t *)lv_malloc(qrcodegen_BUFFER_LEN_MAX);

    // Encode once (this is a static QR — no host frame push). match_python_mask=true so the
    // hand-transcribed pattern is pixel-identical to a Pi Zero (see qr_python_lost_point). On
    // failure we still build the screen so navigation doesn't crash; the field stays white.
    ctx->have_frame = qr_encode_bytes(qr_mode, payload.data(), payload.size(),
                                      ctx->encode_scratch, ctx->qr_matrix,
                                      /*match_python_mask=*/true);
    ctx->size = ctx->have_frame ? qrcodegen_getSize(ctx->qr_matrix) : 21;

    // Zone geometry. modules_per_zone follows Python: 7 for a 21-module QR (fills the
    // 240 screen nicely), 5 otherwise. pixels_per_module scales with the profile.
    ctx->modules_per_zone = (ctx->size == 21) ? 7 : 5;
    // pixels_per_module = Python's fixed 24 at the 240 reference, scaled by the profile
    // multiplier (same truncating base*mult/100 the layout constants use).
    ctx->pixels_per_module = (int)(24 * active_profile().px_multiplier / 100.0);
    ctx->zones   = (ctx->size + ctx->modules_per_zone - 1) / ctx->modules_per_zone;   // ceil
    ctx->zone_px = ctx->modules_per_zone * ctx->pixels_per_module;
    ctx->screen_width  = lv_display_get_horizontal_resolution(NULL);
    ctx->screen_height = lv_display_get_vertical_resolution(NULL);
    ctx->window_offset_x = (ctx->screen_width - ctx->zone_px) / 2;
    ctx->window_offset_y = (ctx->screen_height - ctx->zone_px) / 2;

    // Initial zone (structural defaults — Python dataclass initial_zone_x/y = 0; the
    // screenshot generator / host uses these to frame an interesting cell).
    int initial_zone_x = cfg.value("initial_zone_x", 0);
    int initial_zone_y = cfg.value("initial_zone_y", 0);
    ctx->current_zone_x = LV_CLAMP(0, initial_zone_x, ctx->zones - 1);
    ctx->current_zone_y = LV_CLAMP(0, initial_zone_y, ctx->zones - 1);
    ctx->pan_x = seed_transcribe_zoomed_qr_pan_for_zone(ctx, ctx->current_zone_x, ctx->window_offset_x);
    ctx->pan_y = seed_transcribe_zoomed_qr_pan_for_zone(ctx, ctx->current_zone_y, ctx->window_offset_y);

    // --- Bare-root build ---

    // Full-bleed bare root (chrome-free: no top-nav scaffold). The QR field is white
    // (its quiet zone), black elsewhere is never seen (the field fills the screen).
    lv_obj_t *screen_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen_root, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen_root, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(screen_root, 0, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(screen_root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(screen_root, LV_OBJ_FLAG_SCROLLABLE);
    ctx->screen = screen_root;

    // 1) The QR field: white background, modules direct-drawn on top in the draw cb.
    ctx->qr_field = lv_obj_create(screen_root);
    lv_obj_remove_style_all(ctx->qr_field);
    lv_obj_set_size(ctx->qr_field, ctx->screen_width, ctx->screen_height);
    lv_obj_set_pos(ctx->qr_field, 0, 0);
    lv_obj_set_style_bg_color(ctx->qr_field, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ctx->qr_field, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(ctx->qr_field, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(ctx->qr_field, seed_transcribe_zoomed_qr_draw_cb, LV_EVENT_DRAW_MAIN_END, ctx);

    // 2) Dimming mask: four opaque-ish black panels framing the transparent zone
    //    window. The far panels are sized to the REMAINDER past the hole (not a second
    //    window_offset_x/window_offset_y) so an odd (width-zone_px)/(height-zone_px)
    //    can't truncate into a 1px gap that leaks the white QR field as a stray line
    //    (seen at 480x320).
    int hole_right  = ctx->window_offset_x + ctx->zone_px;   // right edge of the hole
    int hole_bottom = ctx->window_offset_y + ctx->zone_px;   // bottom edge of the hole
    seed_transcribe_zoomed_qr_dim_rect(screen_root, 0, 0,
                                       ctx->screen_width, ctx->window_offset_y);                        // top
    seed_transcribe_zoomed_qr_dim_rect(screen_root, 0, hole_bottom,
                                       ctx->screen_width, ctx->screen_height - hole_bottom);            // bottom
    seed_transcribe_zoomed_qr_dim_rect(screen_root, 0, ctx->window_offset_y,
                                       ctx->window_offset_x, ctx->zone_px);                             // left
    seed_transcribe_zoomed_qr_dim_rect(screen_root, hole_right, ctx->window_offset_y,
                                       ctx->screen_width - hole_right, ctx->zone_px);                   // right

    // 3) Accent window outline (1px) around the highlighted zone.
    lv_obj_t *outline = lv_obj_create(screen_root);
    lv_obj_remove_style_all(outline);
    lv_obj_set_pos(outline, ctx->window_offset_x, ctx->window_offset_y);
    lv_obj_set_size(outline, ctx->zone_px, ctx->zone_px);
    lv_obj_set_style_bg_opa(outline, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_color(outline, lv_color_hex(ACCENT_COLOR), LV_PART_MAIN);
    lv_obj_set_style_border_width(outline, 1, LV_PART_MAIN);
    lv_obj_remove_flag(outline, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    // 4) Zone-coordinate bars + labels: a column-number bar across the top and a
    //    row-letter bar down the left, both aligned to the zone window's extent.
    seed_transcribe_zoomed_qr_bar(screen_root, ctx->window_offset_x, 0,
                                  ctx->zone_px, ctx->pixels_per_module, &ctx->column_label);  // top: "1".."6"
    seed_transcribe_zoomed_qr_bar(screen_root, 0, ctx->window_offset_y,
                                  ctx->pixels_per_module, ctx->zone_px, &ctx->row_label);     // left: "A".."F"
    seed_transcribe_zoomed_qr_update_labels(ctx);

    // 5) Exit affordance + input. The two input modes get DIFFERENT exit affordances:
    //
    //    HARDWARE: a keypad sink (arrows pan a zone, any other key exits) + a bottom
    //    "click to exit" text plate — the joystick click is the exit, so the text tells
    //    the user what to press (Python parity; hardware has no on-screen button).
    //
    //    TOUCH: a transparent full-screen catcher (swipe = one-zone step; no tap-to-exit
    //    so a stray tap mid-transcription can't drop the user out) + an explicit top-right
    //    gutter "X" button. No bottom text — the X is the affordance.
    if (input_mode == INPUT_MODE_HARDWARE) {
        lv_obj_t *exit_hint_label = lv_label_create(screen_root);
        lv_label_set_text(exit_hint_label, exit_text.c_str());
        lv_obj_set_style_text_font(exit_hint_label, &BODY_FONT, LV_PART_MAIN);
        lv_obj_set_style_text_color(exit_hint_label, lv_color_hex(BODY_FONT_COLOR), LV_PART_MAIN);
        lv_obj_set_style_bg_color(exit_hint_label, lv_color_hex(BACKGROUND_COLOR), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(exit_hint_label, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(exit_hint_label, COMPONENT_PADDING, LV_PART_MAIN);
        lv_obj_set_style_pad_ver(exit_hint_label, COMPONENT_PADDING / 4, LV_PART_MAIN);
        lv_obj_align(exit_hint_label, LV_ALIGN_BOTTOM_MID, 0, 0);

        lv_obj_t *sink = lv_obj_create(screen_root);
        lv_obj_set_size(sink, 1, 1);
        lv_obj_set_pos(sink, 0, 0);
        lv_obj_set_style_bg_opa(sink, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(sink, 0, LV_PART_MAIN);
        lv_obj_remove_flag(sink, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

        ctx->group = lv_group_create();
        lv_group_add_obj(ctx->group, sink);
        lv_obj_add_event_cb(sink, seed_transcribe_zoomed_qr_key_cb, LV_EVENT_KEY, ctx);

        lv_indev_t *indev = NULL;
        while ((indev = lv_indev_get_next(indev)) != NULL) {
            if (lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD ||
                lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER) {
                lv_indev_set_group(indev, ctx->group);
            }
        }
    } else {
        // Swipe distance that counts as a zone step: ~1/8 of the short screen edge,
        // floored so a small drag on a tiny display still needs deliberate movement.
        ctx->swipe_threshold = LV_MAX(30, LV_MIN(ctx->screen_width, ctx->screen_height) / 8);

        lv_obj_t *catcher = lv_obj_create(screen_root);
        lv_obj_remove_style_all(catcher);
        lv_obj_set_size(catcher, ctx->screen_width, ctx->screen_height);
        lv_obj_set_pos(catcher, 0, 0);
        lv_obj_add_flag(catcher, LV_OBJ_FLAG_CLICKABLE);   // must be pressable to see the drag
        lv_obj_remove_flag(catcher, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(catcher, seed_transcribe_zoomed_qr_press_cb, LV_EVENT_PRESSED, ctx);
        lv_obj_add_event_cb(catcher, seed_transcribe_zoomed_qr_release_cb, LV_EVENT_RELEASED, ctx);

        build_gutter_close_button(screen_root, seed_transcribe_zoomed_qr_close_cb, ctx);  // top-right X -> exit
    }

    // --- Cleanup + load ---

    lv_obj_add_event_cb(screen_root, seed_transcribe_zoomed_qr_cleanup_cb, LV_EVENT_DELETE, ctx);
    load_screen_and_cleanup_previous(screen_root);
#endif  // LV_USE_QRCODE
}
