// qr_display_screen
//
// Python provenance: QRDisplayScreen (screen.py)
//
// Full-bleed QR display: encodes a payload with the qrcodegen library bundled in
// LVGL and paints the module matrix at integer scale — black modules on a
// brightness-driven gray field, black gutters filling the rest of the screen.
// Any non-UP/DOWN key (hardware) or the top-right X (touch) exits, reporting
// SEEDSIGNER_RET_BACK_BUTTON with label "qr_display_done" exactly once; the final
// brightness is also pushed through the weak seedsigner_lvgl_on_qr_brightness()
// hook so the host can persist Python's SETTING__QR_BRIGHTNESS.
//
// Chrome-free tier (spec §8 sanctioned scaffold bypass): bare root screen, no
// top-nav, self-owned input; the mandatory load_screen_and_cleanup_previous tail
// still applies.
//
// HOST-PUSH PATTERN (this file is the reference shape). Animation is HOST-DRIVEN:
// Python's encode_qr frame cadence lives host-side (UR fountain frames are
// generated on the fly and cannot be precomputed), so the screen renders the
// initial frame from cfg["qr_data"] and the host pushes every subsequent frame
// via qr_display_set_frame(); qr_display_is_tip_active() lets the host's frame
// driver HOLD on the valuable first frames while the brightness tip is up. The
// idiom: the public extern "C" APIs sit between the anonymous-namespace close and
// the entry point, each carrying its own #if LV_USE_QRCODE/#else availability
// stub, and reach the live screen through the module-scope singleton g_qr_ctx.
//
// Lifecycle (stateful, Tier 2): one lv_malloc'd POD ctx; qr_display_cleanup_cb on
// the screen root's LV_EVENT_DELETE re-checks the event code, frees the toast
// timer / input group / qrcodegen buffers, and clears g_qr_ctx under an identity
// guard — this file is the spec §6 named pattern for the identity-guarded module
// static + DELETE cleanup.
//
// Brightness is a native render concern (Python: QRDisplayThread repaints the QR
// background from a 31..255 counter; KEY_UP/KEY_DOWN step it ±31). HARDWARE mode
// raises a passive hint panel — chevron rows naming what the physical keys do —
// while the keys themselves act. TOUCH mode (no Python equivalent; Python is
// hardware-only) taps the QR to raise a draggable slider flanked by dim/bright
// sun icons, with a top-right X to exit. Panel styling ports Python's
// render_brightness_tip (opacity 224, radius 8). Layout deviation from Python:
// the panel is a content-sized rounded box bottom-centered 2*COMPONENT_PADDING
// above the screen edge, where Python renders a full-width band flush to the
// bottom edge.
//
// The only user-visible strings are brighter_text/darker_text, which arrive
// ALREADY TRANSLATED from the host view layer (nothing user-visible is baked
// here).
//
// cfg:
//   qr_data              (string, required, non-empty)  initial-frame payload,
//            interpreted per data_encoding.
//   qr_mode              (string, default "auto")  numeric|alphanumeric|byte|auto:
//            the qrcodegen encode mode (unknown values throw).
//   data_encoding        (string, default "utf8")  utf8|hex|base64: how qr_data
//            maps to payload bytes (unknown values throw).
//   border               (int, default 2, validated 0..20)  quiet-zone width in
//            modules (Python: part_to_image(..., border=2)).
//   initial_brightness   (int, default 62, clamped 31..255)  starting gray level
//            (Python: SETTING__QR_BRIGHTNESS, default 62).
//   show_brightness_tips (bool, default true)  whether the brightness panel
//            exists at all (Python: SETTING__QR_BRIGHTNESS_TIPS, default enabled).
//   brighter_text        (string, required when show_brightness_tips)  localized
//            hardware hint-row text (Python: _("Brighter")).
//   darker_text          (string, required when show_brightness_tips)  localized
//            hardware hint-row text (Python: _("Darker")).
//   tips_visible         (bool, default false)  gallery/demo aid: raise the panel
//            persistently on load (no auto-hide), including in static stills.
//   input.mode           (string, optional, "touch"|"hardware")  input-mode
//            override, read via nav_mode_override_from_cfg (screen_helpers);
//            absent -> the global input profile decides.
//   test_frames          (array of string, optional)  NOT read by this screen:
//            the desktop runner harness reads it and stands in for the host,
//            pushing each entry through qr_display_set_frame().

#include "screen_scaffold.h"  // parse_screen_json_ctx, load_screen_and_cleanup_previous
#include "seedsigner.h"       // qr_display_screen / qr_display_set_frame / qr_display_is_tip_active / seedsigner_lvgl_on_qr_brightness decls, SEEDSIGNER_RET_BACK_BUTTON, seedsigner_lvgl_on_button_selected, seedsigner_lvgl_is_static_render
#include "gui_constants.h"    // COMPONENT_PADDING, BODY_FONT, BODY_FONT_COLOR, ACCENT_COLOR, INACTIVE_COLOR, ICON_FONT__SEEDSIGNER, ICON_LARGE_BUTTON_FONT__SEEDSIGNER, SeedSignerIconConstants, active_profile
#include "input_profile.h"    // input_mode_t, INPUT_MODE_TOUCH/INPUT_MODE_HARDWARE, input_profile_get_mode
#include "qr_core.h"          // qr_encode_mode_t, QR_ENC_*, qr_decode_payload, qr_encode_bytes, build_gutter_close_button
#include "screen_helpers.h"   // nav_mode_override_from_cfg (input.mode override)

#include "lvgl.h"             // lv_obj / lv_label / lv_slider / lv_group / lv_indev / lv_timer / lv_draw_rect + per-object style setters

#include <nlohmann/json.hpp>  // json (cfg reads)

#include <cstddef>            // size_t (payload/frame lengths)
#include <cstdint>            // uint8_t / uint32_t / int32_t
#include <stdexcept>          // std::runtime_error (required-field validation)
#include <string>             // std::string
#include <vector>             // std::vector (decoded payload bytes)

// Feature-gated last: the qrcodegen encoder bundled inside LVGL.
#if LV_USE_QRCODE
#include "../../../third_party/lvgl/src/libs/qrcode/qrcodegen.h"  // qrcodegen_getSize/getModule, qrcodegen_BUFFER_LEN_MAX
#endif

using json = nlohmann::json;

// Optional host hook: fired on every brightness change and once on exit so the host
// can persist SETTING__QR_BRIGHTNESS (prototype in seedsigner.h). Weak no-op default;
// slated to move to components.cpp (the shared weak-default home) at the
// callback-surface rollout decision.
extern "C" __attribute__((weak)) void seedsigner_lvgl_on_qr_brightness(uint8_t /*brightness*/) {}

#if LV_USE_QRCODE

namespace {

// ---------------------------------------------------------------------------
// Screen state (Tier-2 ctx + host-push singleton)
// ---------------------------------------------------------------------------

// Pure POD (pointers/ints/bools only): allocated with lv_malloc + lv_memzero in
// the entry point and freed with lv_free in qr_display_cleanup_cb (§6 Tier-2
// allocation idiom).
struct qr_display_ctx_t {
    lv_obj_t   *screen;
    lv_obj_t   *qr_obj;         // plain object; QR modules are direct-drawn in qr_draw_cb
    int32_t     qr_side;        // qr_obj edge in px (display short dimension)
    lv_group_t *group;          // hardware keypad group (NULL in touch mode)
    lv_obj_t   *toast;          // brightness toast container (built once, hidden)
    lv_timer_t *toast_timer;    // one-shot auto-hide timer (NULL when idle)

    input_mode_t     input_mode;
    int              brightness;   // 31..255
    int              border;       // quiet-zone modules
    qr_encode_mode_t mode;
    bool             show_tips;
    bool             emitted;      // exit reported once

    // qrcodegen scratch/output, sized for version 40, allocated once. ctx->out holds
    // the CURRENT frame's encoded matrix so a brightness change can repaint without
    // re-encoding.
    uint8_t *tmp;
    uint8_t *out;
    bool     have_frame;           // ctx->out holds a valid encoded QR
};

// The single active QR screen, so qr_display_set_frame() can reach it. LVGL is
// single-threaded (host pushes frames from the same loop), so no locking is needed.
qr_display_ctx_t *g_qr_ctx = nullptr;

// Touch toast stays up longer than the hardware hint (Python's tip duration is
// 1.2 s) so there's time to tap and drag it.
constexpr uint32_t QR_DISPLAY_TOAST_MS_HARDWARE = 1200;
constexpr uint32_t QR_DISPLAY_TOAST_MS_TOUCH    = 3000;

// ---------------------------------------------------------------------------
// QR encode + direct draw
// ---------------------------------------------------------------------------

// brightness (31..255) -> gray, EXACT Python parity: hex(n)[2:] * 3 == (n,n,n).
lv_color_t qr_display_gray(int brightness) {
    uint32_t gray = (uint32_t)(brightness < 0 ? 0 : (brightness > 255 ? 255 : brightness));
    return lv_color_hex((gray << 16) | (gray << 8) | gray);
}

// Encode into ctx->out reusing the screen cfg's mode/scratch — a thin ctx-bound
// wrapper over qr_encode_bytes (qr_display QRs are machine-scanned, so they use
// qrcodegen's fast auto mask — no python-mask parity).
bool qr_encode(qr_display_ctx_t *ctx, const uint8_t *data, size_t len) {
    return qr_encode_bytes(ctx->mode, data, len, ctx->tmp, ctx->out, /*match_python_mask=*/false);
}

// Draw the current ctx->out matrix onto qr_obj's layer (DRAW_MAIN_END, i.e. on top of
// the object's gray background): black scale x scale module blocks (row-runs coalesced
// into single rects), centered with a `border`-module quiet zone.
//
// This REPLACES the old full-screen lv_canvas. On the ESP32 firmware LVGL runs on a
// fixed ~128 KB internal-DRAM pool (LV_USE_BUILTIN_MALLOC); a short-dimension-square
// RGB565 canvas is ~200 KB (480x320) to ~460 KB (800x480) and cannot fit, so lv_malloc
// returned NULL and the screen HARD-FROZE the moment a QR was shown. Direct-drawing the
// modules needs no large buffer at all (same class of fix as psbt_overview_screen). The
// gray quiet-zone field is qr_obj's own background; only the black modules are painted
// here. The layer clip restricts rasterization to the invalidated region, so a partial
// repaint (e.g. the toast hiding) stays cheap.
void qr_draw_cb(lv_event_t *e) {
    qr_display_ctx_t *ctx   = (qr_display_ctx_t *)lv_event_get_user_data(e);
    lv_layer_t       *layer = lv_event_get_layer(e);
    if (!ctx || !layer || !ctx->have_frame) return;

    lv_area_t obj_area;
    lv_obj_get_coords(ctx->qr_obj, &obj_area);  // absolute coords of the gray square

    int size  = qrcodegen_getSize(ctx->out);
    int total = size + 2 * ctx->border;
    int sd    = ctx->qr_side;

    int scale = sd / total;
    if (scale < 1) scale = 1;
    int qr_px = scale * total;
    int off   = (sd - qr_px) / 2;  // center the QR within the square

    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color     = lv_color_black();
    d.bg_opa       = LV_OPA_COVER;
    d.radius       = 0;
    d.border_width = 0;

    // Coalesce each row's lit modules into horizontal runs, offset by the object's
    // absolute position (draw-event coords are screen-absolute, not object-local).
    for (int my = 0; my < size; my++) {
        int mx = 0;
        while (mx < size) {
            if (!qrcodegen_getModule(ctx->out, mx, my)) { mx++; continue; }
            int run = mx;
            while (run < size && qrcodegen_getModule(ctx->out, run, my)) run++;
            lv_area_t a;
            a.x1 = obj_area.x1 + off + (ctx->border + mx)  * scale;
            a.y1 = obj_area.y1 + off + (ctx->border + my)  * scale;
            a.x2 = obj_area.x1 + off + (ctx->border + run) * scale - 1;
            a.y2 = a.y1 + scale - 1;
            lv_draw_rect(layer, &d, &a);
            mx = run;
        }
    }
}

void qr_encode_and_paint(qr_display_ctx_t *ctx, const uint8_t *data, size_t len) {
    if (!ctx || !ctx->qr_obj || len == 0) return;
    if (!qr_encode(ctx, data, len)) return;  // too long to encode — keep the previous frame
    ctx->have_frame = true;
    lv_obj_invalidate(ctx->qr_obj);  // schedules qr_draw_cb to repaint with the new matrix
}

// ---------------------------------------------------------------------------
// Brightness + toast
// ---------------------------------------------------------------------------

void qr_display_hide_toast(qr_display_ctx_t *ctx) {
    if (ctx->toast) lv_obj_add_flag(ctx->toast, LV_OBJ_FLAG_HIDDEN);
}

void qr_display_toast_timer_cb(lv_timer_t *timer) {
    qr_display_ctx_t *ctx = (qr_display_ctx_t *)lv_timer_get_user_data(timer);
    qr_display_hide_toast(ctx);
    ctx->toast_timer = NULL;  // one-shot self-deletes (repeat count 1)
}

void qr_display_show_toast(qr_display_ctx_t *ctx, uint32_t auto_hide_ms) {
    if (!ctx->toast) return;
    lv_obj_remove_flag(ctx->toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(ctx->toast);
    if (ctx->toast_timer) { lv_timer_del(ctx->toast_timer); ctx->toast_timer = NULL; }
    // auto_hide_ms == 0 keeps the toast up persistently (the brightness_tip demo
    // scenario / static stills); a positive value arms a one-shot auto-hide (normal
    // brightness interaction). Stills never animate the auto-hide either.
    if (!seedsigner_lvgl_is_static_render() && auto_hide_ms > 0) {
        ctx->toast_timer = lv_timer_create(qr_display_toast_timer_cb, auto_hide_ms, ctx);
        lv_timer_set_repeat_count(ctx->toast_timer, 1);
    }
}

void qr_display_set_brightness(qr_display_ctx_t *ctx, int brightness) {
    if (brightness < 31) brightness = 31;
    if (brightness > 255) brightness = 255;
    if (brightness == ctx->brightness) return;
    ctx->brightness = brightness;
    // The gray quiet-zone field is qr_obj's background; recolor it (the matrix is
    // unchanged, still in ctx->out). set_style invalidates the object, so qr_draw_cb
    // repaints the black modules on top of the new gray.
    lv_obj_set_style_bg_color(ctx->qr_obj, qr_display_gray(ctx->brightness), LV_PART_MAIN);
    // Real-time change signal: on a brightness change the host RESTARTS an animated sequence
    // (Python restarts the UR fountain so the valuable pure frames are re-delivered from the
    // start) and may persist the value. Fires per change; the host may debounce slider drags.
    seedsigner_lvgl_on_qr_brightness((uint8_t)ctx->brightness);
}

// Report the exit exactly once (emitted latch): final brightness first (the host's
// persist cue), then the BACK_BUTTON navigation event that tears the screen down.
void qr_display_exit(qr_display_ctx_t *ctx) {
    if (ctx->emitted) return;
    ctx->emitted = true;
    seedsigner_lvgl_on_qr_brightness((uint8_t)ctx->brightness);
    seedsigner_lvgl_on_button_selected(SEEDSIGNER_RET_BACK_BUTTON, "qr_display_done");
}

// ---------------------------------------------------------------------------
// Input handlers
// ---------------------------------------------------------------------------

// Hardware/joystick: UP/DOWN adjust brightness (+ hint toast); any other key exits
// (Python _run: any other input exits the screen).
void qr_display_key_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_KEY) return;
    qr_display_ctx_t *ctx = (qr_display_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_UP) {
        qr_display_set_brightness(ctx, ctx->brightness + 31);
        if (ctx->show_tips) qr_display_show_toast(ctx, QR_DISPLAY_TOAST_MS_HARDWARE);
    } else if (key == LV_KEY_DOWN) {
        qr_display_set_brightness(ctx, ctx->brightness - 31);
        if (ctx->show_tips) qr_display_show_toast(ctx, QR_DISPLAY_TOAST_MS_HARDWARE);
    } else {
        qr_display_exit(ctx);
    }
}

// Touch: tapping the QR raises the (interactive) toast.
void qr_display_tap_cb(lv_event_t *e) {
    qr_display_ctx_t *ctx = (qr_display_ctx_t *)lv_event_get_user_data(e);
    if (ctx && ctx->show_tips) qr_display_show_toast(ctx, QR_DISPLAY_TOAST_MS_TOUCH);
}

// Touch: the brightness slider drives the gray live as it is dragged; each change also
// resets the panel's idle auto-hide so it stays up while the user is adjusting.
void qr_display_slider_cb(lv_event_t *e) {
    qr_display_ctx_t *ctx = (qr_display_ctx_t *)lv_event_get_user_data(e);
    lv_obj_t *slider = lv_event_get_target_obj(e);
    if (!ctx || !slider) return;
    qr_display_set_brightness(ctx, (int)lv_slider_get_value(slider));
    qr_display_show_toast(ctx, QR_DISPLAY_TOAST_MS_TOUCH);
}

// Touch: the top-right gutter X exits.
void qr_display_close_cb(lv_event_t *e) {
    qr_display_ctx_t *ctx = (qr_display_ctx_t *)lv_event_get_user_data(e);
    if (ctx) qr_display_exit(ctx);
}

// LV_EVENT_DELETE teardown on the screen root: free the toast timer / input group /
// qrcodegen buffers, clear the host-push singleton under an identity guard, then
// free the ctx.
void qr_display_cleanup_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;
    qr_display_ctx_t *ctx = (qr_display_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;
    if (ctx->toast_timer) lv_timer_del(ctx->toast_timer);
    if (ctx->group)       lv_group_del(ctx->group);
    if (ctx->tmp)         lv_free(ctx->tmp);
    if (ctx->out)         lv_free(ctx->out);
    if (g_qr_ctx == ctx)  g_qr_ctx = nullptr;
    lv_free(ctx);
}

// ---------------------------------------------------------------------------
// Widget builders
// ---------------------------------------------------------------------------

// A small icon label (brightness "sun", chevron, ...) in `font`, body-colored.
lv_obj_t *qr_display_make_icon(lv_obj_t *parent, const char *glyph, const lv_font_t *font) {
    lv_obj_t *icon_label = lv_label_create(parent);
    lv_label_set_text(icon_label, glyph);
    lv_obj_set_style_text_font(icon_label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(icon_label, lv_color_hex(BODY_FONT_COLOR), LV_PART_MAIN);
    return icon_label;
}

// A horizontal, content-sized flex row with the standard inter-item gap.
lv_obj_t *qr_display_make_row(lv_obj_t *parent) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, COMPONENT_PADDING, LV_PART_MAIN);
    return row;
}

// One hardware hint row: a chevron (which physical key) + the translated label. The
// physical KEY_UP/KEY_DOWN do the work; the row tells the user which key does what. Text
// (not an icon) so it reads as a clear instruction; the host passes it already translated.
void qr_display_build_hint_row(lv_obj_t *parent, const char *chevron, const std::string &text) {
    lv_obj_t *row = qr_display_make_row(parent);
    lv_obj_set_style_pad_ver(row, COMPONENT_PADDING / 2, LV_PART_MAIN);
    qr_display_make_icon(row, chevron, &ICON_FONT__SEEDSIGNER);
    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, text.c_str());
    lv_obj_set_style_text_font(label, &BODY_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(BODY_FONT_COLOR), LV_PART_MAIN);
    // Single-line hint, so force CLIP: in this content-sized [chevron][text] row the
    // default LV_LABEL_LONG_WRAP would let the shaped-locale run layer wrap a wide
    // translation and collapse it to line 0 (see glyph-run-single-line-label-wrap-collapse.md).
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
}

// The brightness panel: a rounded semi-transparent box at the bottom, raised on
// interaction. HARDWARE shows passive chevron + translated-text hint rows (physical keys
// act); TOUCH shows a draggable slider flanked by dim/bright suns (a slider is the natural
// touch affordance for a range). brighter_text/darker_text are used only by the hardware hints.
void qr_display_build_toast(qr_display_ctx_t *ctx, const std::string &brighter, const std::string &darker) {
    lv_obj_t *toast = lv_obj_create(ctx->screen);
    lv_obj_remove_style_all(toast);
    lv_obj_set_size(toast, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(toast, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(toast, 224, LV_PART_MAIN);        // Python render_brightness_tip opacity
    lv_obj_set_style_radius(toast, 8 * active_profile().px_multiplier / 100, LV_PART_MAIN);  // Python radius=8, px-scaled
    lv_obj_set_style_pad_all(toast, COMPONENT_PADDING, LV_PART_MAIN);
    lv_obj_set_flex_flow(toast, LV_FLEX_FLOW_COLUMN);
    // LEFT-align rows (cross axis = START) so the hardware hint chevrons line up in a column
    // (the two rows differ in width because "Brighter"/"Darker" differ in length).
    lv_obj_set_flex_align(toast, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(toast, LV_ALIGN_BOTTOM_MID, 0, -COMPONENT_PADDING * 2);
    lv_obj_remove_flag(toast, LV_OBJ_FLAG_SCROLLABLE);

    if (ctx->input_mode == INPUT_MODE_TOUCH) {
        // dim sun (small) | slider | bright sun (large)
        lv_obj_t *row = qr_display_make_row(toast);
        qr_display_make_icon(row, SeedSignerIconConstants::BRIGHTNESS, &ICON_FONT__SEEDSIGNER);
        lv_obj_t *slider = lv_slider_create(row);
        lv_slider_set_range(slider, 31, 255);                 // same 31..255 gray range
        lv_slider_set_value(slider, ctx->brightness, LV_ANIM_OFF);
        lv_obj_set_width(slider, ctx->qr_side / 2);
        lv_obj_set_style_bg_color(slider, lv_color_hex(INACTIVE_COLOR), LV_PART_MAIN);
        lv_obj_set_style_bg_color(slider, lv_color_hex(ACCENT_COLOR), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(slider, lv_color_hex(BODY_FONT_COLOR), LV_PART_KNOB);
        lv_obj_add_event_cb(slider, qr_display_slider_cb, LV_EVENT_VALUE_CHANGED, ctx);
        qr_display_make_icon(row, SeedSignerIconConstants::BRIGHTNESS, &ICON_LARGE_BUTTON_FONT__SEEDSIGNER);
    } else {
        qr_display_build_hint_row(toast, SeedSignerIconConstants::CHEVRON_UP,   brighter);
        qr_display_build_hint_row(toast, SeedSignerIconConstants::CHEVRON_DOWN, darker);
    }

    lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);
    ctx->toast = toast;
}

}  // namespace

#endif  // LV_USE_QRCODE

// ---------------------------------------------------------------------------
// Host-push public APIs (the banner's host-push pattern; each carries its own
// availability stub for builds without LV_USE_QRCODE)
// ---------------------------------------------------------------------------

// Push the next animated-QR frame from the host. See seedsigner.h.
extern "C" void qr_display_set_frame(const void *data, size_t len) {
#if LV_USE_QRCODE
    if (!g_qr_ctx) return;
    qr_encode_and_paint(g_qr_ctx, (const uint8_t *)data, len);
#else
    (void)data; (void)len;
#endif
}

// True while the brightness panel is on screen. The animation frame driver holds (does not
// advance) while this is true, so the tip greets the user on start and the valuable first
// frames are held until it clears. See seedsigner.h.
extern "C" bool qr_display_is_tip_active(void) {
#if LV_USE_QRCODE
    return g_qr_ctx && g_qr_ctx->toast &&
           !lv_obj_has_flag(g_qr_ctx->toast, LV_OBJ_FLAG_HIDDEN);
#else
    return false;
#endif
}


void qr_display_screen(void *ctx_json) {
#if !LV_USE_QRCODE
    // Built without the bundled QR encoder (no shipping build does this). Load a blank
    // screen so the entry point exists and navigation into it does not crash.
    (void)ctx_json;
    lv_obj_t *screen_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen_root, lv_color_black(), LV_PART_MAIN);
    load_screen_and_cleanup_previous(screen_root);
#else
    // --- Config ---

    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    // Required payload: the initial QR frame. Every validation below throws BEFORE
    // any LVGL object or ctx allocation exists, so no throw path can leak.
    if (!cfg.contains("qr_data") || !cfg["qr_data"].is_string() ||
        cfg["qr_data"].get<std::string>().empty()) {
        throw std::runtime_error("qr_display_screen: non-empty qr_data (string) is required");
    }
    std::string qr_data = cfg["qr_data"].get<std::string>();

    // Encode mode (structural default "auto"; unknown values throw).
    std::string qr_mode_string = cfg.value("qr_mode", std::string("auto"));
    qr_encode_mode_t qr_mode;
    if (qr_mode_string == "numeric")            qr_mode = QR_ENC_NUMERIC;
    else if (qr_mode_string == "alphanumeric")  qr_mode = QR_ENC_ALNUM;
    else if (qr_mode_string == "byte")          qr_mode = QR_ENC_BYTE;
    else if (qr_mode_string == "auto")          qr_mode = QR_ENC_AUTO;
    else throw std::runtime_error("qr_display_screen: qr_mode must be numeric|alphanumeric|byte|auto");

    // Payload byte interpretation (structural default "utf8"; unknown values throw).
    std::string data_encoding = cfg.value("data_encoding", std::string("utf8"));
    if (data_encoding != "utf8" && data_encoding != "hex" && data_encoding != "base64")
        throw std::runtime_error("qr_display_screen: data_encoding must be utf8|hex|base64");

    // Quiet-zone width in modules (Python: part_to_image(..., border=2)).
    int border = cfg.value("border", 2);
    if (border < 0 || border > 20)
        throw std::runtime_error("qr_display_screen: border must be 0..20");

    // Starting gray level, clamped to Python's 31..255 brightness range
    // (Python: SETTING__QR_BRIGHTNESS, default 62).
    int brightness = cfg.value("initial_brightness", 62);
    if (brightness < 31) brightness = 31;
    if (brightness > 255) brightness = 255;

    bool show_tips = cfg.value("show_brightness_tips", true);  // Python: SETTING__QR_BRIGHTNESS_TIPS default enabled
    std::string brighter, darker;
    if (show_tips) {
        // Hardware brightness hints use translated TEXT (touch uses an icon slider instead).
        // The host passes brighter_text/darker_text ALREADY TRANSLATED (i18n contract, no
        // strings baked in). Required when tips are on, since the same cfg may render on a
        // hardware profile.
        if (!cfg.contains("brighter_text") || !cfg["brighter_text"].is_string() ||
            !cfg.contains("darker_text")   || !cfg["darker_text"].is_string())
            throw std::runtime_error("qr_display_screen: brighter_text/darker_text (translated) "
                                     "are required when show_brightness_tips is true");
        brighter = cfg["brighter_text"].get<std::string>();
        darker   = cfg["darker_text"].get<std::string>();
    }

    // Input mode: the cfg override (input.mode) wins, else the global input profile.
    bool has_override = false;
    input_mode_t mode_override = INPUT_MODE_TOUCH;
    nav_mode_override_from_cfg(cfg, has_override, mode_override);
    input_mode_t input_mode = has_override ? mode_override : input_profile_get_mode();

    // Decode the initial payload (throws on malformed hex — still before any allocation).
    std::vector<uint8_t> payload = qr_decode_payload(qr_data, data_encoding);

    // --- Bare-root build ---

    // 1. Full-bleed bare root (chrome-free tier: no top-nav scaffold), black gutters
    //    (the QR square itself is gray), nothing scrolls.
    lv_obj_t *screen_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen_root, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen_root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen_root, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(screen_root, 0, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(screen_root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(screen_root, LV_OBJ_FLAG_SCROLLABLE);

    // 2. Tier-2 heap ctx (POD -> lv_malloc + lv_memzero; released by
    //    qr_display_cleanup_cb) + the version-40-sized qrcodegen scratch/output pair.
    qr_display_ctx_t *ctx = (qr_display_ctx_t *)lv_malloc(sizeof(qr_display_ctx_t));
    lv_memzero(ctx, sizeof(*ctx));
    ctx->screen     = screen_root;
    ctx->input_mode = input_mode;
    ctx->brightness = brightness;
    ctx->border     = border;
    ctx->mode       = qr_mode;
    ctx->show_tips  = show_tips;
    ctx->tmp = (uint8_t *)lv_malloc(qrcodegen_BUFFER_LEN_MAX);
    ctx->out = (uint8_t *)lv_malloc(qrcodegen_BUFFER_LEN_MAX);

    // 3. Square QR area sized to the display's short dimension, centered. This is a PLAIN
    //    object, not an lv_canvas: a short-dimension-square RGB565 canvas buffer (~200-460 KB)
    //    overflows the ESP32's ~128 KB LVGL pool and freezes the screen. Its gray background
    //    is the quiet-zone field (so a rare encode failure shows a clean gray, not garbage);
    //    qr_draw_cb paints the black modules on top with no large buffer. See qr_draw_cb.
    int32_t screen_width  = lv_display_get_horizontal_resolution(NULL);
    int32_t screen_height = lv_display_get_vertical_resolution(NULL);
    int32_t short_side = screen_width < screen_height ? screen_width : screen_height;
    ctx->qr_side = short_side;
    ctx->qr_obj  = lv_obj_create(screen_root);
    lv_obj_remove_style_all(ctx->qr_obj);
    lv_obj_set_size(ctx->qr_obj, short_side, short_side);
    lv_obj_center(ctx->qr_obj);
    lv_obj_set_style_bg_color(ctx->qr_obj, qr_display_gray(ctx->brightness), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ctx->qr_obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(ctx->qr_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(ctx->qr_obj, qr_draw_cb, LV_EVENT_DRAW_MAIN_END, ctx);

    // 4. Host-push registration + the initial frame (subsequent frames arrive through
    //    qr_display_set_frame(), which re-encodes into ctx->out and invalidates).
    g_qr_ctx = ctx;

    qr_encode_and_paint(ctx, payload.data(), payload.size());

    // 5. Brightness toast, built hidden; input raises it.
    if (show_tips) qr_display_build_toast(ctx, brighter, darker);

    // --- Input wiring ---

    // Chrome-free tier: input is self-owned — no bind_screen_navigation. Hardware gets a
    // keypad sink; touch gets tap/slider/close affordances.
    if (input_mode == INPUT_MODE_HARDWARE) {
        // Keypad sink in a dedicated group: UP/DOWN = brightness, any other key exits.
        // (This sink+group+indev-assign block is the chrome-free keypad idiom shared with
        // the transcribe/splash/screensaver screens — slated for extraction at the rollout
        // decision, ledger #11.)
        lv_obj_t *sink = lv_obj_create(screen_root);
        lv_obj_set_size(sink, 1, 1);
        lv_obj_set_pos(sink, 0, 0);
        lv_obj_set_style_opa(sink, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(sink, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(sink, 0, LV_PART_MAIN);
        lv_obj_remove_flag(sink, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

        ctx->group = lv_group_create();
        lv_group_add_obj(ctx->group, sink);
        lv_obj_add_event_cb(sink, qr_display_key_cb, LV_EVENT_KEY, ctx);

        lv_indev_t *indev = NULL;
        while ((indev = lv_indev_get_next(indev)) != NULL) {
            if (lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD ||
                lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER) {
                lv_indev_set_group(indev, ctx->group);
            }
        }
    } else {
        // Touch: tap the QR to raise the toast; an explicit top-right X (the shared
        // qr_core gutter close button) to exit — parity screens exit via any key on
        // hardware, but touch has no keys, so it gets an affordance clear of the toast.
        lv_obj_add_flag(ctx->qr_obj, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(ctx->qr_obj, qr_display_tap_cb, LV_EVENT_CLICKED, ctx);
        build_gutter_close_button(ctx->screen, qr_display_close_cb, ctx);
    }

    // --- Load ---

    // Brightness panel visibility on load:
    //  - cfg "tips_visible" (gallery/demo aid): shown persistently (ms=0), incl. static stills.
    //  - otherwise, LIVE only: show it on START and auto-hide (Python parity). The tip greets
    //    the user (joystick brightness isn't obvious) AND — via qr_display_is_tip_active() —
    //    the frame driver HOLDS the animation on the valuable first frames until it clears
    //    (for a UR fountain those first parts are the pure, full-data frames). Static stills
    //    stay clean: seedsigner_lvgl_is_static_render() creates no auto-hide timer, so we skip the on-start show.
    if (show_tips) {
        if (cfg.value("tips_visible", false)) {
            qr_display_show_toast(ctx, 0);
        } else if (!seedsigner_lvgl_is_static_render()) {
            qr_display_show_toast(ctx, ctx->input_mode == INPUT_MODE_HARDWARE
                                           ? QR_DISPLAY_TOAST_MS_HARDWARE : QR_DISPLAY_TOAST_MS_TOUCH);
        }
    }

    lv_obj_add_event_cb(screen_root, qr_display_cleanup_cb, LV_EVENT_DELETE, ctx);
    load_screen_and_cleanup_previous(screen_root);
#endif  // LV_USE_QRCODE
}
