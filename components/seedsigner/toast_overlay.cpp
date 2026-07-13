// Toast overlay implementation — see toast_overlay.h for the contract.
//
// The banner is a single lv_obj on the display's TOP LAYER (lv_layer_top()) holding
// an optional icon label and a wrapped message label. It ports the geometry of
// Python's ToastOverlay.render() (gui/toast.py): a full-width, bottom-pinned rounded
// rectangle (black fill, colored 2px outline), a leading icon inset from the left,
// and a left-aligned message that grows the banner when it wraps to multiple lines.
//
// Everything here runs on the LVGL loop. There is at most one live toast (a new show
// replaces the old one, mirroring Python's one-toast-at-a-time Controller).

#include "toast_overlay.h"

#include "lvgl.h"

#include "gui_constants.h"     // EDGE_PADDING, COMPONENT_PADDING, BODY_FONT, TOP_NAV_ICON_FONT__SEEDSIGNER, active_profile()
#include "input_profile.h"     // input_profile_get_mode() — hardware vs touch dismissal
#include "seedsigner.h"        // seedsigner_lvgl_is_static_render()
#include "screen_helpers.h"    // apply_rtl_text_to_labels()
#include "glyph_runs.h"        // apply_glyph_runs_to_labels()
#include "font_registry.h"     // seedsigner_locale_is_rtl()

// --- Live toast state (LVGL thread only) ----------------------------------
static lv_obj_t   *s_toast_obj   = NULL;  // the banner container on the top layer
static lv_timer_t *s_toast_timer = NULL;  // periodic auto-dismiss + hardware input watch

// Dismissal bookkeeping, captured when the toast is shown.
static uint32_t s_duration_ms       = 0;      // 0 = no auto timeout (stay until dismissed)
static uint32_t s_show_tick         = 0;      // lv_tick_get() at show, for the timeout
static uint32_t s_last_input_at_show = 0;     // absolute tick of the last input as of show
static bool     s_hardware_mode      = false; // input mode captured at show
static bool     s_flying_off         = false; // a touch swipe-dismiss fly-off anim is running
static int32_t  s_press_x            = 0;      // touch press origin, for tap-vs-swipe on release
static int32_t  s_press_y            = 0;

// How often the watch timer wakes to check the timeout / for new hardware input.
#define TOAST_WATCH_PERIOD_MS 100

// Fixed duration of the touch swipe-dismiss fly-off. The user's swipe only picks the
// direction; the banner then flies off on its own at this fixed pace, ignoring the
// rest of the drag.
#define TOAST_FLYOFF_MS 200

// Minimum press→release travel (px, either axis) to read the touch as a swipe rather
// than a tap. Comfortably above tap jitter, well below any deliberate flick.
#define TOAST_SWIPE_MIN_PX 24

// A hardware press is only treated as a "dismiss" input once the last-input tick has
// advanced past the show-time baseline by more than this margin — it absorbs the
// sub-tick skew between the two clock reads so the very press that triggered the
// toast can't instantly close it. Any deliberate later press clears it by far more.
#define TOAST_INPUT_DISMISS_MARGIN_MS 100

// --- Teardown -------------------------------------------------------------

// Absolute tick of the most recent input, derived from LVGL's idle clock.
static uint32_t last_input_tick(void) {
    return lv_tick_get() - lv_display_get_inactive_time(NULL);
}

void toast_overlay_dismiss(void) {
    // Drop the timer first so it can't fire against a half-torn-down toast, then the
    // banner. Null s_toast_obj BEFORE deleting so the LV_EVENT_DELETE handler (which
    // also nulls it) is a harmless no-op rather than a re-entrant surprise.
    if (s_toast_timer) {
        lv_timer_delete(s_toast_timer);
        s_toast_timer = NULL;
    }
    s_flying_off = false;
    if (s_toast_obj) {
        lv_obj_t *obj = s_toast_obj;
        s_toast_obj = NULL;
        lv_obj_delete(obj);
    }
}

// Deferred dismiss — used from input event callbacks, where deleting the event's own
// target object inline is unsafe; lv_async_call runs it after the event dispatch.
static void toast_async_dismiss_cb(void * /*unused*/) {
    toast_overlay_dismiss();
}

// The banner was deleted (by dismiss(), the fly-off completion, or an external teardown
// such as lv_display_delete on a desktop-runner resize). Keep our statics consistent and
// drop any running fly-off animation so its exec_cb can't fire against the freed object.
static void toast_obj_deleted_cb(lv_event_t *e) {
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    lv_anim_delete(obj, NULL);
    s_toast_obj  = NULL;
    s_flying_off = false;
    if (s_toast_timer) {
        lv_timer_delete(s_toast_timer);
        s_toast_timer = NULL;
    }
}

// --- Dismissal drivers ----------------------------------------------------

// Periodic watch: auto-timeout, plus (hardware mode only) dismiss on any new key /
// joystick press. The press is observed via the idle clock, NOT consumed — it still
// reaches the underlying screen's indev, so a press both hides the toast and drives
// the screen, exactly like Python's has_any_input() check.
static void toast_watch_cb(lv_timer_t * /*t*/) {
    uint32_t since_show = lv_tick_elaps(s_show_tick);

    if (s_duration_ms > 0 && since_show >= s_duration_ms) {
        toast_overlay_dismiss();
        return;
    }

    if (s_hardware_mode) {
        // A new press advances the last-input tick past the show-time baseline.
        int32_t advanced = (int32_t)(last_input_tick() - s_last_input_at_show);
        if (advanced > TOAST_INPUT_DISMISS_MARGIN_MS) {
            toast_overlay_dismiss();
        }
    }
}

// Fly-off animation exec callbacks — slide the banner via a style translate (does not
// disturb its layout position, which stays put behind the offset).
static void toast_anim_translate_x_cb(void *var, int32_t v) {
    lv_obj_set_style_translate_x((lv_obj_t *)var, v, LV_PART_MAIN);
}
static void toast_anim_translate_y_cb(void *var, int32_t v) {
    lv_obj_set_style_translate_y((lv_obj_t *)var, v, LV_PART_MAIN);
}

// Fly-off finished (banner fully off-screen): dismiss for real. Deferred out of the
// animation callback context so the object delete + lv_anim_delete don't run while the
// anim engine is mid-iteration over this very animation.
static void toast_flyoff_completed_cb(lv_anim_t * /*a*/) {
    lv_async_call(toast_async_dismiss_cb, NULL);
}

// Start the touch swipe-dismiss fly-off: the banner slides off `dir` at a FIXED pace
// (TOAST_FLYOFF_MS), independent of the drag's speed or continued direction, and is
// deleted once it clears the edge. `dir` is the swipe's dominant cardinal direction.
static void toast_start_flyoff(lv_dir_t dir) {
    if (s_flying_off || !s_toast_obj) return;
    s_flying_off = true;

    // The fly-off owns dismissal now — retire the auto-timeout / input watch so it
    // can't also fire mid-flight.
    if (s_toast_timer) {
        lv_timer_delete(s_toast_timer);
        s_toast_timer = NULL;
    }

    lv_obj_t *obj = s_toast_obj;
    int32_t disp_w = lv_display_get_horizontal_resolution(NULL);
    int32_t disp_h = lv_display_get_vertical_resolution(NULL);
    int32_t x = lv_obj_get_x(obj), y = lv_obj_get_y(obj);
    int32_t w = lv_obj_get_width(obj), h = lv_obj_get_height(obj);
    const int32_t margin = 8;  // clear the edge fully (no 1px sliver on the last frame)

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_duration(&a, TOAST_FLYOFF_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);   // accelerate away — a flung feel
    lv_anim_set_completed_cb(&a, toast_flyoff_completed_cb);

    switch (dir) {
        case LV_DIR_LEFT:
            lv_anim_set_exec_cb(&a, toast_anim_translate_x_cb);
            lv_anim_set_values(&a, 0, -(x + w + margin));
            break;
        case LV_DIR_RIGHT:
            lv_anim_set_exec_cb(&a, toast_anim_translate_x_cb);
            lv_anim_set_values(&a, 0, (disp_w - x) + margin);
            break;
        case LV_DIR_TOP:
            lv_anim_set_exec_cb(&a, toast_anim_translate_y_cb);
            lv_anim_set_values(&a, 0, -(y + h + margin));
            break;
        case LV_DIR_BOTTOM:
        default:
            lv_anim_set_exec_cb(&a, toast_anim_translate_y_cb);
            lv_anim_set_values(&a, 0, (disp_h - y) + margin);
            break;
    }
    lv_anim_start(&a);
}

// Touch dismissal: we classify tap vs swipe OURSELVES from the press→release
// displacement rather than lean on LVGL's built-in gesture. That gesture resets its
// accumulator on any slow frame (per-frame move < a few px) and needs ~50px, so the
// natural deceleration right before lifting a finger/mouse wipes the accumulation and
// the whole thing lands as a click — swipes were never recognized. The press→release
// delta is immune to that: LV_EVENT_RELEASED fires on the banner "in every case" (even
// when the finger slid off it), so the delta is the FULL swipe vector regardless of how
// small the banner is.
static void toast_pressed_cb(lv_event_t * /*e*/) {
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    s_press_x = p.x;
    s_press_y = p.y;
}

static void toast_released_cb(lv_event_t * /*e*/) {
    if (s_flying_off || !s_toast_obj) return;

    lv_indev_t *indev = lv_indev_active();
    if (!indev) {  // no indev to measure — treat as a tap
        lv_async_call(toast_async_dismiss_cb, NULL);
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int32_t dx = p.x - s_press_x;
    int32_t dy = p.y - s_press_y;
    int32_t adx = dx < 0 ? -dx : dx;
    int32_t ady = dy < 0 ? -dy : dy;

    // Below the swipe threshold in both axes → a tap: dismiss immediately (no fly-off).
    if (adx < TOAST_SWIPE_MIN_PX && ady < TOAST_SWIPE_MIN_PX) {
        lv_async_call(toast_async_dismiss_cb, NULL);
        return;
    }

    // Otherwise fling off in the dominant cardinal direction of the swipe.
    lv_dir_t dir;
    if (adx >= ady) dir = (dx > 0) ? LV_DIR_RIGHT : LV_DIR_LEFT;
    else            dir = (dy > 0) ? LV_DIR_BOTTOM : LV_DIR_TOP;
    toast_start_flyoff(dir);
}

// --- Show -----------------------------------------------------------------
void toast_overlay_show(const toast_overlay_spec_t *spec) {
    if (!spec) return;

    // One toast at a time: replace whatever is currently showing.
    toast_overlay_dismiss();

    const bool     hardware = (input_profile_get_mode() == INPUT_MODE_HARDWARE);
    const int      pm       = active_profile().px_multiplier;
    const int32_t  disp_w   = lv_display_get_horizontal_resolution(NULL);
    const int32_t  edge     = EDGE_PADDING;       // Python EDGE_PADDING (8 base), scaled
    const int32_t  comp     = COMPONENT_PADDING;  // Python COMPONENT_PADDING (8 base), scaled

    // Python outline_thickness is a flat 2px (Pi-Zero-only); scale it for the taller
    // profiles so the outline stays visible, with a 2px floor.
    int32_t border = (2 * pm) / 100;
    if (border < 2) border = 2;
    const int32_t radius = (8 * pm) / 100;  // Python rounded_rectangle radius=8

    // Python default banner height = ICON_TOAST_FONT_SIZE (30) + 2 * EDGE_PADDING.
    const int32_t default_h = (30 * pm) / 100 + 2 * edge;

    // --- Banner container on the top layer ---
    lv_obj_t *toast = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(toast);
    lv_obj_set_width(toast, disp_w);
    lv_obj_set_style_bg_color(toast, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(toast, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(toast, radius, LV_PART_MAIN);
    lv_obj_set_style_border_color(toast, lv_color_hex(spec->outline_color), LV_PART_MAIN);
    lv_obj_set_style_border_width(toast, border, LV_PART_MAIN);
    lv_obj_set_style_border_opa(toast, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(toast, 0, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(toast, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(toast, LV_OBJ_FLAG_SCROLLABLE);
    // Not clickable by default → taps fall through to the screen below. Touch mode
    // re-enables it (further down) so a tap ON the banner can dismiss it.
    lv_obj_remove_flag(toast, LV_OBJ_FLAG_CLICKABLE);

    // --- Optional leading icon (Python: drawn in `color`, inset outline+EDGE_PADDING) ---
    int32_t icon_delta_x = 0;  // Python: icon.width + icon.screen_x (0 when no icon)
    lv_obj_t *icon = NULL;
    if (spec->icon_glyph && spec->icon_glyph[0]) {
        icon = lv_label_create(toast);
        lv_label_set_text(icon, spec->icon_glyph);
        // Nearest baked seedsigner-icon size to Python's ICON_TOAST_FONT_SIZE (30):
        // the top-nav contextual icon font (26 base). The icon font is baked with each
        // glyph vertically centered in its box, so LV_ALIGN_LEFT_MID centers the ink.
        lv_obj_set_style_text_font(icon, &TOP_NAV_ICON_FONT__SEEDSIGNER, LV_PART_MAIN);
        lv_obj_set_style_text_color(icon, lv_color_hex(spec->outline_color), LV_PART_MAIN);
        lv_obj_update_layout(toast);
        int32_t icon_x = border + edge;
        icon_delta_x = lv_obj_get_width(icon) + icon_x;
    }

    // --- Message label (Python TextArea: left-aligned, wraps to the remaining width) ---
    int32_t label_x = icon_delta_x + comp;
    int32_t label_w = disp_w - icon_delta_x - 2 * comp - 2 * border;
    if (label_w < 1) label_w = 1;

    lv_obj_t *label = lv_label_create(toast);
    lv_obj_set_style_text_font(label, &BODY_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(spec->font_color), LV_PART_MAIN);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, label_w);
    lv_label_set_text(label, spec->label_text ? spec->label_text : "");

    // Localization passes (same order as load_screen_and_cleanup_previous): flip RTL
    // base direction, then swap any translated codepoint text for its pre-shaped glyph
    // run. No-ops for LTR / non-shaping locales (English renders straight through).
    if (seedsigner_locale_is_rtl()) {
        apply_rtl_text_to_labels(toast);
    }
    apply_glyph_runs_to_labels(toast);

    // Grow the banner for a wrapped multi-line message (Python's height override).
    lv_obj_update_layout(toast);
    int32_t label_h = lv_obj_get_height(label);
    int32_t toast_h = default_h;
    if (label_h + 2 * edge > toast_h) toast_h = label_h + 2 * edge;
    lv_obj_set_height(toast, toast_h);

    // Vertically center icon + label; pin the banner flush to the display bottom.
    if (icon) lv_obj_align(icon, LV_ALIGN_LEFT_MID, border + edge, 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, label_x, 0);
    lv_obj_align(toast, LV_ALIGN_BOTTOM_MID, 0, 0);

    // Keep our statics consistent if the banner is deleted out from under us.
    lv_obj_add_event_cb(toast, toast_obj_deleted_cb, LV_EVENT_DELETE, NULL);

    // --- Dismissal wiring ---
    s_hardware_mode      = hardware;
    s_duration_ms        = spec->duration_ms;
    s_show_tick          = lv_tick_get();
    s_last_input_at_show = last_input_tick();
    s_flying_off         = false;

    if (hardware) {
        // Any key/joystick press dismisses — handled by the watch timer's idle-clock
        // check (non-consuming). The banner stays click-through.
    } else {
        // Touch: press + release on the banner classify tap (dismiss) vs swipe (fly-off
        // in the swipe direction). Clickable so the press lands on the banner; taps
        // OUTSIDE the bottom band fall through to the screen.
        lv_obj_add_flag(toast, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(toast, toast_pressed_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(toast, toast_released_cb, LV_EVENT_RELEASED, NULL);
    }

    // The auto-timeout + hardware input watch runs off a periodic timer. Skipped in
    // static-render mode so a screenshot capture keeps the banner up deterministically.
    if (!seedsigner_lvgl_is_static_render()) {
        s_toast_timer = lv_timer_create(toast_watch_cb, TOAST_WATCH_PERIOD_MS, NULL);
    }

    s_toast_obj = toast;
}

bool toast_overlay_is_active(void) {
    return s_toast_obj != NULL;
}
