// screensaver_screen
//
// Python provenance: ScreensaverScreen (screen.py)
//
// The idle bouncing-logo screensaver: the SeedSigner logo drifts across an
// otherwise-black screen and bounces off the edges until the user touches or
// presses any key. It returns SEEDSIGNER_RET_SCREENSAVER_DISMISS through
// seedsigner_lvgl_on_button_selected on the legacy (host-routed) path; on the
// overlay-manager path it routes nothing and the manager's idle-watch dismisses.
//
// Layout (chrome-free tier): a bare black root holds a single logo image at its
// native, per-profile size; ALL positioning is imperative — a 16 ms lv_timer
// integrates a float center position and repaints the logo via lv_obj_set_pos
// each frame. There is no top-nav, no flex, and no align.
//
// Deviation from Python: Python steps the logo by a fixed random per-axis
// increment and bounces by negating the axis. This port integrates a sub-pixel
// float velocity and, on every wall/corner hit, re-randomizes BOTH speed and
// departure angle — clamped at least SCREENSAVER_MIN_WALL_ANGLE_RAD off the wall
// surface so the logo never hugs an edge at a grazing angle.
//
// Lifecycle (stateful, Tier 2 — with a SANCTIONED exception): one heap ctx owns
// the spin timer and the keypad group, torn down by screensaver_cleanup_handler
// on LV_EVENT_DELETE. Per the conformance spec (docs/screen-conformance-spec.md
// §6/§8), screensaver_screen loads via bare lv_scr_load and DELIBERATELY DOES
// NOT delete the previous screen: the host save/restores it (save_screen /
// restore_screen), so the otherwise-mandatory load_screen_and_cleanup_previous
// tail is intentionally NOT used here. The builder builds-without-loading so the
// overlay manager can own loading for its own (manager-dismissed) path.
//
// cfg: none — the entry point ignores ctx_json (no JSON surface at all).
// Dismissal behavior is selected by the C bool route_dismiss_to_host on the
// internal builder, not by any config key.

#include "seedsigner.h"        // seedsigner_lvgl_on_button_selected, SEEDSIGNER_RET_SCREENSAVER_DISMISS, ss_build_screensaver_obj / screensaver_screen decls
#include "gui_constants.h"     // seedsigner_logo_for_active_profile
#include "input_profile.h"     // input_profile_get_mode, INPUT_MODE_HARDWARE
#include "navigation.h"        // attach_keypad_indevs_to_group (held-key-safe input handoff)

#include "lvgl.h"              // root/image/timer/group/indev widgets + physics (lv_rand, lv_tick_get, lv_scr_load)

#include <cmath>               // cosf, sinf (velocity from a bounce angle)


// Speed range: 0.07 – 0.18 pixels/ms  (70 – 180 px/s).
static constexpr float SCREENSAVER_SPEED_MIN = 0.07f;
static constexpr float SCREENSAVER_SPEED_MAX = 0.18f;

// Minimum angle from the wall surface on departure (radians). Prevents the logo
// from hugging a wall at a shallow grazing angle.
static constexpr float SCREENSAVER_MIN_WALL_ANGLE_RAD = 25.0f * 3.14159265f / 180.0f;


namespace {

// Pure-POD animation context: lv_malloc + lv_memzero at build, lv_free in the
// LV_EVENT_DELETE cleanup (no C++ members, so there are no ctors/dtors to run).
struct screensaver_ctx_t {
    lv_obj_t   *screen;
    lv_obj_t   *logo_image;
    lv_timer_t *timer;
    lv_group_t *group;
    float       center_x;      // logo center, float for sub-pixel accuracy
    float       center_y;
    float       velocity_x;    // pixels per millisecond
    float       velocity_y;
    uint32_t    last_tick;
    int32_t     screen_width;
    int32_t     screen_height;
    int32_t     logo_width;    // displayed width after zoom
    int32_t     logo_height;   // displayed height after zoom
    bool        route_dismiss; // true: input fires a host dismiss result (legacy
                               // path); false: the overlay manager's idle-watch
                               // dismisses, so input is not host-routed here.
};

// Returns a random float in [lo, hi).
float screensaver_randf(float lo, float hi) {
    uint32_t random_value = lv_rand(0, 0x7fffffffu);
    return lo + (hi - lo) * ((float)random_value / (float)0x7fffffffu);
}

// Pick a random departure angle within the half-plane defined by 'normal_angle'
// (the inward wall normal), clamped so the angle is at least
// SCREENSAVER_MIN_WALL_ANGLE_RAD away from either wall surface edge. This
// eliminates wall-hugging trajectories.
float screensaver_bounce_angle(float normal_angle) {
    float max_offset = (3.14159265f / 2.0f) - SCREENSAVER_MIN_WALL_ANGLE_RAD;
    float offset = screensaver_randf(-max_offset, max_offset);
    return normal_angle + offset;
}

// Per-frame integrator: (legacy path) poll for a dismiss, then advance the float
// center position, bounce off any wall/corner hit with a re-randomized speed and
// departure angle, and repaint the logo via lv_obj_set_pos.
void screensaver_timer_cb(lv_timer_t *timer) {
    screensaver_ctx_t *ctx = (screensaver_ctx_t *)lv_timer_get_user_data(timer);

    // Legacy (Python-driven) path only: poll pointer devices and route a dismiss
    // to the host on touch. In overlay-manager mode (route_dismiss == false) the
    // manager's idle-watch handles dismissal — any input resets
    // lv_display_get_inactive_time() — so the screensaver does not route input.
    if (ctx->route_dismiss) {
        lv_indev_t *indev = NULL;
        while ((indev = lv_indev_get_next(indev)) != NULL) {
            if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER &&
                lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED) {
                seedsigner_lvgl_on_button_selected(SEEDSIGNER_RET_SCREENSAVER_DISMISS, "screensaver_dismiss");
                return;
            }
        }
    }

    uint32_t now     = lv_tick_get();
    uint32_t elapsed = now - ctx->last_tick;
    ctx->last_tick   = now;

    // Clamp elapsed to avoid huge jumps after screen switches or pauses.
    if (elapsed > 200) elapsed = 200;

    ctx->center_x += ctx->velocity_x * (float)elapsed;
    ctx->center_y += ctx->velocity_y * (float)elapsed;

    bool bounced_x = false;
    bool bounced_y = false;
    bool hit_left  = false;
    bool hit_top   = false;

    if (ctx->center_x < 0.0f) {
        ctx->center_x = 0.0f;
        bounced_x = true; hit_left = true;
    } else if (ctx->center_x > (float)ctx->screen_width) {
        ctx->center_x = (float)ctx->screen_width;
        bounced_x = true;
    }

    if (ctx->center_y < 0.0f) {
        ctx->center_y = 0.0f;
        bounced_y = true; hit_top = true;
    } else if (ctx->center_y > (float)ctx->screen_height) {
        ctx->center_y = (float)ctx->screen_height;
        bounced_y = true;
    }

    if (bounced_x || bounced_y) {
        // Inward normal angle for the wall(s) hit.
        // Screen coords: +x = right, +y = down.
        // Left wall  normal: 0          Right wall normal: π
        // Top wall   normal: π/2 (down) Bottom wall normal: -π/2 (up)
        float normal;
        if (bounced_x && bounced_y) {
            // Corner: diagonal normal pointing toward screen interior.
            normal = hit_left
                ? (hit_top ? (3.14159265f / 4.0f)        // top-left  → SE
                           : (-3.14159265f / 4.0f))       // bot-left  → NE
                : (hit_top ? (3.0f * 3.14159265f / 4.0f) // top-right → SW
                           : (-3.0f * 3.14159265f / 4.0f)); // bot-right → NW
        } else if (bounced_x) {
            normal = hit_left ? 0.0f : 3.14159265f;
        } else {
            normal = hit_top ? (3.14159265f / 2.0f) : (-3.14159265f / 2.0f);
        }

        float speed     = screensaver_randf(SCREENSAVER_SPEED_MIN, SCREENSAVER_SPEED_MAX);
        float new_angle = screensaver_bounce_angle(normal);
        ctx->velocity_x = speed * cosf(new_angle);
        ctx->velocity_y = speed * sinf(new_angle);
    }

    lv_obj_set_pos(ctx->logo_image,
                   (int32_t)(ctx->center_x - ctx->logo_width / 2.0f),
                   (int32_t)(ctx->center_y - ctx->logo_height / 2.0f));
}

// Legacy keypad sink handler: any key press fires a host dismiss.
void screensaver_key_handler(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_KEY) return;
    seedsigner_lvgl_on_button_selected(SEEDSIGNER_RET_SCREENSAVER_DISMISS, "screensaver_dismiss");
}

// LV_EVENT_DELETE teardown on the screen root: stop the (not widget-owned) timer,
// delete the keypad group, then free the ctx.
void screensaver_cleanup_handler(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;

    screensaver_ctx_t *ctx = (screensaver_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;

    if (ctx->timer) {
        lv_timer_del(ctx->timer);
        ctx->timer = NULL;
    }
    if (ctx->group) {
        lv_group_del(ctx->group);
        ctx->group = NULL;
    }
    lv_free(ctx);
}

// Build the screensaver screen (bouncing logo) WITHOUT loading it — the caller
// loads it. `route_dismiss_to_host` selects how the screensaver gets dismissed:
//   true  — legacy Python-driven path: a key/touch fires
//           SEEDSIGNER_RET_SCREENSAVER_DISMISS (via seedsigner_lvgl_on_button_selected)
//           and the host runner restores the saved screen.
//   false — overlay-manager path: the manager's idle-watch dismisses on any
//           input (lv_display_get_inactive_time() resets), so input is NOT
//           host-routed here. The keypad sink + group are still installed so the
//           wake keypress is swallowed rather than actuating the restored screen.
lv_obj_t *screensaver_build_impl(bool route_dismiss_to_host) {
    // --- Bare-root build ---

    // Chrome-free tier: a bare black root screen (no top-nav scaffold), nothing scrolls.
    lv_obj_t *screen_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen_root, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen_root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(screen_root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(screen_root, LV_OBJ_FLAG_SCROLLABLE);

    int32_t screen_width  = lv_display_get_horizontal_resolution(NULL);
    int32_t screen_height = lv_display_get_vertical_resolution(NULL);

    // --- Body ---

    // 1. Logo image at native resolution (no zoom). The image is pre-scaled by
    //    png_to_lvgl.py and selected per display profile (px_multiplier) by
    //    seedsigner_logo_for_active_profile().
    const lv_image_dsc_t *logo = seedsigner_logo_for_active_profile();
    int32_t logo_width  = (int32_t)logo->header.w;
    int32_t logo_height = (int32_t)logo->header.h;

    lv_obj_t *logo_image = lv_image_create(screen_root);
    lv_image_set_src(logo_image, logo);
    lv_obj_set_size(logo_image, logo_width, logo_height);

    // 2. Allocate and initialise the animation context, then seed the initial
    //    position, direction, and speed.
    screensaver_ctx_t *ctx = (screensaver_ctx_t *)lv_malloc(sizeof(screensaver_ctx_t));
    lv_memzero(ctx, sizeof(*ctx));
    ctx->screen        = screen_root;
    ctx->logo_image    = logo_image;
    ctx->screen_width  = screen_width;
    ctx->screen_height = screen_height;
    ctx->logo_width    = logo_width;
    ctx->logo_height   = logo_height;
    ctx->route_dismiss = route_dismiss_to_host;

    // Start at screen center.
    ctx->center_x = screen_width / 2.0f;
    ctx->center_y = screen_height / 2.0f;

    // Random initial direction and speed.
    float initial_speed = screensaver_randf(SCREENSAVER_SPEED_MIN, SCREENSAVER_SPEED_MAX);
    float initial_angle = screensaver_randf(0.0f, 2.0f * 3.14159265f);
    ctx->velocity_x = initial_speed * cosf(initial_angle);
    ctx->velocity_y = initial_speed * sinf(initial_angle);

    ctx->last_tick = lv_tick_get();

    // Place logo at its starting position.
    lv_obj_set_pos(logo_image,
                   (int32_t)(ctx->center_x - logo_width / 2.0f),
                   (int32_t)(ctx->center_y - logo_height / 2.0f));

    // 3. Spin driver: a 16 ms timer integrates the position every frame.
    ctx->timer = lv_timer_create(screensaver_timer_cb, 16, ctx);

    // 4. Keypad sink (hardware input mode): any key press dismisses the screensaver.
    if (input_profile_get_mode() == INPUT_MODE_HARDWARE) {
        lv_obj_t *sink = lv_obj_create(screen_root);
        lv_obj_set_size(sink, 1, 1);
        lv_obj_set_pos(sink, 0, 0);
        lv_obj_set_style_opa(sink, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(sink, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(sink, 0, LV_PART_MAIN);
        lv_obj_set_style_outline_width(sink, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(sink, 0, LV_PART_MAIN);
        lv_obj_remove_flag(sink, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

        ctx->group = lv_group_create();
        lv_group_add_obj(ctx->group, sink);
        // Only the legacy path fires a host dismiss on keypress. In manager mode
        // the sink still swallows the wake keypress (no handler installed) while
        // the idle-watch performs the dismiss.
        if (route_dismiss_to_host) {
            lv_obj_add_event_cb(sink, screensaver_key_handler, LV_EVENT_KEY, ctx);
        }

        // Latch held keys on takeover so a wake-key still held from the moment
        // the saver appeared can't bleed onto the restored screen later.
        attach_keypad_indevs_to_group(ctx->group);
    }

    lv_obj_add_event_cb(screen_root, screensaver_cleanup_handler, LV_EVENT_DELETE, ctx);
    return screen_root;
}

}  // namespace


// extern "C" pass-through to the anonymous-namespace builder. The wrapper exists
// only to give the overlay-manager-facing symbol (declared in seedsigner.h) C
// linkage across the extern-"C" boundary; the impl itself stays internal to this TU.
extern "C" lv_obj_t *ss_build_screensaver_obj(bool route_dismiss_to_host) {
    return screensaver_build_impl(route_dismiss_to_host);
}

void screensaver_screen(void * /*ctx_json*/) {
    // Legacy entry point: build with host-routed dismiss and load via bare
    // lv_scr_load, WITHOUT destroying the previous screen. This is the sanctioned
    // §6/§8 lifecycle exception (docs/screen-conformance-spec.md): the caller
    // save/restores the underlying screen (save_screen / restore_screen), so the
    // otherwise-mandatory load_screen_and_cleanup_previous tail is intentionally
    // NOT used here.
    lv_scr_load(screensaver_build_impl(true));
}
