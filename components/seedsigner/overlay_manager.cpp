// Overlay manager implementation — see overlay_manager.h for the contract.
//
// Everything here runs on the LVGL loop (the dispatcher is an lv_timer). The
// only cross-thread entry points are the producer-facing setters/enqueues,
// which guard shared state with overlay_manager_lock()/unlock().

#include "overlay_manager.h"

#include "lvgl.h"
#include "seedsigner.h"       // ss_build_screensaver_obj
#include "toast_overlay.h"    // toast_overlay_show / _is_active

#include <string.h>           // strncpy / memcpy for the cross-thread toast copy

// Dispatcher period — the screensaver idle-watch granularity. Far below any
// realistic timeout (e.g. 120 s) and far above the per-tick dispatch cost.
#define OVERLAY_DISPATCH_PERIOD_MS 200

static bool        s_inited         = false;
static lv_timer_t *s_dispatch_timer = NULL;

// --- Screensaver state (LVGL thread only) ---------------------------------
static uint32_t    s_ss_timeout_ms    = 0;     // 0 = disabled
static bool        s_ss_active        = false;
static lv_obj_t   *s_ss_saved_screen  = NULL;  // screen restored on dismiss
static lv_group_t *s_ss_saved_group   = NULL;  // indev group restored on dismiss
static lv_obj_t   *s_ss_saver_screen  = NULL;  // the live screensaver screen

// --- Pending toast request (producer thread -> LVGL loop) -----------------
// A producer on any thread stages one toast here under the lock; the dispatcher
// (LVGL thread) drains and realizes it. Strings are copied into fixed buffers so the
// producer's storage need not outlive the call. A newer stage overwrites an
// un-drained one — one toast at a time.
#define OVERLAY_TOAST_TEXT_MAX 256
#define OVERLAY_TOAST_ICON_MAX 16
static bool     s_toast_pending  = false;
static char     s_toast_text[OVERLAY_TOAST_TEXT_MAX];
static char     s_toast_icon[OVERLAY_TOAST_ICON_MAX];
static bool     s_toast_has_icon = false;
static uint32_t s_toast_outline  = 0;
static uint32_t s_toast_font     = 0;
static uint32_t s_toast_duration = 0;

// --- Weak lock hooks ------------------------------------------------------
// Default no-ops: correct for single-threaded hosts (desktop, ESP32 LVGL task,
// web). A host that enqueues from a separate thread overrides these with a real
// mutex (the Pi .so does, for its SD-card detector threads).
extern "C" __attribute__((weak)) void overlay_manager_lock(void) {}
extern "C" __attribute__((weak)) void overlay_manager_unlock(void) {}

// --- indev group helpers --------------------------------------------------
// The navigable group lives on the keypad/encoder indev(s). Saved on activate
// and restored on dismiss so the wake keypress lands on the screensaver's sink,
// not on a button of the restored screen.
static lv_group_t *current_keypad_group(void) {
    lv_indev_t *indev = NULL;
    while ((indev = lv_indev_get_next(indev)) != NULL) {
        lv_indev_type_t t = lv_indev_get_type(indev);
        if (t == LV_INDEV_TYPE_KEYPAD || t == LV_INDEV_TYPE_ENCODER) {
            return lv_indev_get_group(indev);
        }
    }
    return NULL;
}

static void set_keypad_group(lv_group_t *group) {
    lv_indev_t *indev = NULL;
    while ((indev = lv_indev_get_next(indev)) != NULL) {
        lv_indev_type_t t = lv_indev_get_type(indev);
        if (t == LV_INDEV_TYPE_KEYPAD || t == LV_INDEV_TYPE_ENCODER) {
            lv_indev_set_group(indev, group);
        }
    }
}

// --- Screensaver lifecycle ------------------------------------------------
static void screensaver_activate(void) {
    if (s_ss_active) return;

    // Save what we must restore on dismiss.
    s_ss_saved_screen = lv_scr_act();
    s_ss_saved_group  = current_keypad_group();

    // Build a manager-dismissed screensaver (route_dismiss = false: the
    // idle-watch dismisses, the screensaver only swallows the wake input) and
    // load it WITHOUT destroying the saved screen.
    s_ss_saver_screen = ss_build_screensaver_obj(false);
    lv_scr_load(s_ss_saver_screen);

    s_ss_active = true;
}

void overlay_manager_dismiss_screensaver(void) {
    if (!s_ss_active) return;

    lv_obj_t *saver = s_ss_saver_screen;

    // Restore the saved indev group BEFORE deleting the saver, since deleting
    // the saver frees the group it installed.
    set_keypad_group(s_ss_saved_group);
    if (s_ss_saved_screen) {
        lv_scr_load(s_ss_saved_screen);
    }
    if (saver && saver != s_ss_saved_screen) {
        lv_obj_delete(saver);  // its cleanup handler frees the anim timer + group + ctx
    }

    // Reset the idle clock so a residual inactive-time can't immediately re-fire.
    lv_display_trigger_activity(NULL);

    s_ss_active       = false;
    s_ss_saver_screen = NULL;
    s_ss_saved_screen = NULL;
    s_ss_saved_group  = NULL;
}

static void screensaver_dispatch(void) {
    // Disabled: ensure the screensaver is not showing.
    if (s_ss_timeout_ms == 0) {
        if (s_ss_active) overlay_manager_dismiss_screensaver();
        return;
    }

    uint32_t inactive = lv_display_get_inactive_time(NULL);
    if (!s_ss_active) {
        // A toast owns the screen (it composites over the active screen and breaks
        // out of the screensaver): don't cover it. The two overlays are mutually
        // exclusive — Python coordinates them via the renderer lock.
        if (toast_overlay_is_active()) return;
        // Per-screen opt-out: the active screen declares "no screensaver here"
        // by carrying SS_OBJ_FLAG_NO_SCREENSAVER (stamped from the view's
        // allow_screensaver=false). Reading it off lv_scr_act() keeps the policy
        // on the screen with no separate global state. The saver only ever
        // activates from a flag-free screen, so the flag guards activation only.
        if (lv_obj_has_flag(lv_scr_act(), SS_OBJ_FLAG_NO_SCREENSAVER)) return;
        if (inactive >= s_ss_timeout_ms) screensaver_activate();
    } else if (inactive < s_ss_timeout_ms) {
        // Any input reset the idle clock -> wake.
        overlay_manager_dismiss_screensaver();
    }
}

// --- Toast dispatch (LVGL loop) -------------------------------------------
// Drain a staged toast request and realize it on the LVGL thread. Copies the request
// out under the lock, then does all LVGL work (screensaver dismiss + build) off-lock.
static void toast_dispatch(void) {
    char     text[OVERLAY_TOAST_TEXT_MAX];
    char     icon[OVERLAY_TOAST_ICON_MAX];
    bool     has_icon;
    uint32_t outline, font, duration;

    overlay_manager_lock();
    bool have = s_toast_pending;
    if (have) {
        s_toast_pending = false;
        memcpy(text, s_toast_text, sizeof(text));
        memcpy(icon, s_toast_icon, sizeof(icon));
        has_icon = s_toast_has_icon;
        outline  = s_toast_outline;
        font     = s_toast_font;
        duration = s_toast_duration;
    }
    overlay_manager_unlock();
    if (!have) return;

    // Toasts break out of the screensaver (Python parity).
    if (s_ss_active) overlay_manager_dismiss_screensaver();

    toast_overlay_spec_t spec;
    spec.label_text    = text;
    spec.icon_glyph    = has_icon ? icon : NULL;
    spec.outline_color = outline;
    spec.font_color    = font;
    spec.duration_ms   = duration;
    toast_overlay_show(&spec);
}

// --- Dispatcher (LVGL loop) -----------------------------------------------
static void dispatch_cb(lv_timer_t * /*timer*/) {
    // Realize any staged toast FIRST, so the screensaver pass below sees it active
    // and stands down in the same tick (no one-tick flash of the saver).
    toast_dispatch();
    screensaver_dispatch();
}

// --- Public API -----------------------------------------------------------
void overlay_manager_init(void) {
    if (s_inited) return;
    s_dispatch_timer = lv_timer_create(dispatch_cb, OVERLAY_DISPATCH_PERIOD_MS, NULL);
    s_inited = true;
}

void overlay_manager_set_screensaver_timeout(uint32_t ms) {
    overlay_manager_lock();
    s_ss_timeout_ms = ms;
    overlay_manager_unlock();
}

uint32_t overlay_manager_get_screensaver_timeout(void) {
    // Single aligned word: an atomic read; no lock needed.
    return s_ss_timeout_ms;
}

bool overlay_manager_is_screensaver_active(void) {
    return s_ss_active;
}

void overlay_manager_show_toast(const toast_overlay_spec_t *spec) {
    if (!spec) return;

    overlay_manager_lock();
    if (spec->label_text) {
        strncpy(s_toast_text, spec->label_text, OVERLAY_TOAST_TEXT_MAX - 1);
        s_toast_text[OVERLAY_TOAST_TEXT_MAX - 1] = '\0';
    } else {
        s_toast_text[0] = '\0';
    }
    if (spec->icon_glyph && spec->icon_glyph[0]) {
        strncpy(s_toast_icon, spec->icon_glyph, OVERLAY_TOAST_ICON_MAX - 1);
        s_toast_icon[OVERLAY_TOAST_ICON_MAX - 1] = '\0';
        s_toast_has_icon = true;
    } else {
        s_toast_icon[0] = '\0';
        s_toast_has_icon = false;
    }
    s_toast_outline  = spec->outline_color;
    s_toast_font     = spec->font_color;
    s_toast_duration = spec->duration_ms;
    s_toast_pending  = true;
    overlay_manager_unlock();
}
