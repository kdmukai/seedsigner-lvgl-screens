#include "seedsigner.h"
#include "screen_scaffold.h"
#include "screen_helpers.h"
#include "components.h"
#include "camera_preview_overlay.h"
#include "camera_entropy_overlay.h"
#include "keyboard_core.h"
#include "gui_constants.h"
#include "navigation.h"
#include "input_profile.h"
#include "font_registry.h"
#include "glyph_runs.h"
#include "locale_loader.h"
#include "locale_picker.h"
#include "overlay_manager.h"

#include "lvgl.h"

#if LV_USE_QRCODE
#include "../../../third_party/lvgl/src/libs/qrcode/qrcodegen.h"
#endif

#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <set>
#include <map>
#include <algorithm>
#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif

using json = nlohmann::json;







// ---------------------------------------------------------------------------
// loading_spinner_screen — the animated "processing" spinner (LVGL port of Python's
// LoadingScreenThread, screen.py)
// ---------------------------------------------------------------------------
//
// Shown while the host CPU runs a long, blocking task (seed generation, PSBT
// signing, …). Python spins a background THREAD that repaints a comet arc around
// the Bitcoin logo while the main thread works. On LVGL the host thread is busy
// inside the task and can't drive frames, so the animation is SELF-DRIVEN by an
// lv_anim on the LVGL timer: as long as the platform keeps ticking lv_timer_handler
// (a dedicated display task on both the ESP32 and Pi Zero backends), the comet keeps
// rotating with zero host involvement.
//
// The screen takes no input and returns no result — it is a pure visual. The host
// shows it, does its work, then loads the next screen (which cleans this one up via
// load_screen_and_cleanup_previous). Deleting the screen auto-cancels the anim (its
// var is a child arc object), so no explicit teardown is needed.
//
// Layout (ports LoadingScreenThread): a centered Bitcoin logo (the baked
// btc_logo_* asset — orange disc + white tilted ₿) with a two-tone "comet" orbiting
// it: a bright 45° leading arc (#ff9416) trailed by a dim 45° arc (#80490b), the two
// rotating together. Optional status text sits above the spinner.
//
// cfg (all optional):
//   text : a short status line shown above the spinner (e.g. "Loading…").

// Comet spin driver. Advances by REAL elapsed time (so the rate is consistent), but
// CLAMPS the per-frame step: a plain wall-clock lv_anim would, on a starved frame, skip
// straight to the time-correct angle (a visible jump); clamping caps how far one frame
// can move, so under extreme load the spin instead visibly SLOWS DOWN — and smoothly
// speeds back up as frames recover — a natural "still working" signal, never a leap.
// Rotation is accumulated in millidegrees for smoothness at high frame rates. A lv_timer
// is NOT auto-freed with its objects, so the screen's DELETE handler tears it down.
static const uint32_t LOADING_SPIN_PERIOD_MS     = 490;    // one revolution at healthy fps (~2 rev/s)
static const uint32_t LOADING_SPIN_TICK_MS       = 33;     // update cadence (~30 fps)
static const int32_t  LOADING_SPIN_MAX_STEP_MDEG = 60000;  // clamp: at most 60° advance per frame

struct loading_spin_ctx_t {
    lv_obj_t   *bright;
    lv_obj_t   *dim;
    lv_timer_t *timer;
    uint32_t    last_tick;       // lv_tick at the previous update
    int32_t     rotation_mdeg;   // accumulated rotation, millidegrees [0, 360000)
};

static void loading_spin_timer_cb(lv_timer_t *timer) {
    loading_spin_ctx_t *c = (loading_spin_ctx_t *)lv_timer_get_user_data(timer);
    if (!c || !lv_obj_is_valid(c->bright)) return;

    uint32_t now = lv_tick_get();
    uint32_t dt  = now - c->last_tick;   // ms since last update (uint wrap-safe)
    c->last_tick = now;

    // Wall-clock advance, clamped so a long (starved) frame slows the spin rather than
    // skipping ahead. The clamp sits just above the generator's 70 ms GIF frame step, so
    // the captured GIF stays the healthy-rate loop while only real on-device stalls slow it.
    int32_t adv = (int32_t)((uint64_t)dt * 360000u / LOADING_SPIN_PERIOD_MS);
    if (adv > LOADING_SPIN_MAX_STEP_MDEG) adv = LOADING_SPIN_MAX_STEP_MDEG;
    c->rotation_mdeg = (c->rotation_mdeg + adv) % 360000;

    int32_t deg = c->rotation_mdeg / 1000;
    lv_arc_set_rotation(c->bright, deg);
    if (lv_obj_is_valid(c->dim)) lv_arc_set_rotation(c->dim, deg);
}

static void loading_cleanup_cb(lv_event_t *e) {
    loading_spin_ctx_t *c = (loading_spin_ctx_t *)lv_event_get_user_data(e);
    if (!c) return;
    if (c->timer) lv_timer_delete(c->timer);
    delete c;
}

// Build one comet segment as a bare indicator-only arc (no track, no knob, not
// interactive) spanning [start,end] degrees, in `color`, at the shared orbit radius.
static lv_obj_t *loading_make_comet_arc(lv_obj_t *parent, int32_t diameter, int32_t width,
                                        int32_t start_deg, int32_t end_deg, uint32_t color) {
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_set_size(arc, diameter, diameter);
    lv_obj_center(arc);
    lv_obj_remove_flag(arc, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_set_style_pad_all(arc, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(arc, 0, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);       // hide the full-ring track
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);        // hide the knob
    lv_obj_set_style_arc_width(arc, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(color), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, false, LV_PART_INDICATOR);      // flat caps (Python arc)
    lv_arc_set_angles(arc, start_deg, end_deg);                       // fixed span; rotation animates it
    return arc;
}

void loading_spinner_screen(void *ctx_json) {
    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_optional_screen_json_ctx(json_str, cfg);  // ctx optional; every field optional

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(BACKGROUND_COLOR), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr, 0, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    const int32_t screen_h = lv_display_get_vertical_resolution(NULL);

    // Centered Bitcoin logo (baked asset, one per profile — orange disc + white ₿).
    const lv_image_dsc_t *logo = btc_logo_for_active_profile();
    const int32_t logo_size = (int32_t)logo->header.w;   // square
    lv_obj_t *logo_img = lv_image_create(scr);
    lv_image_set_src(logo_img, logo);
    lv_obj_center(logo_img);

    // Orbit geometry (ports LoadingScreenThread): the comet rides a ring one
    // orbit_gap outside the logo, drawn COMPONENT_PADDING thick — matching Python's
    // bounding_box = logo ± orbit_gap and arc width = COMPONENT_PADDING.
    const int32_t orbit_gap   = 2 * COMPONENT_PADDING;
    const int32_t arc_width   = COMPONENT_PADDING;
    const int32_t orbit_diam  = logo_size + 2 * orbit_gap;
    const int32_t arc_sweep   = 45;                       // Python arc_sweep
    const uint32_t arc_bright = (uint32_t)BITCOIN_ORANGE; // Python arc_color "#ff9416"
    const uint32_t arc_dim    = 0x80490b;                 // Python arc_trailing_color

    // Trailing (dim) arc first so the bright head draws on top, then the bright head.
    // The dim segment sits immediately BEHIND the bright one so the pair reads as a
    // single comet (bright head [0,45], dim tail [-45,0]); a shared rotation spins them.
    lv_obj_t *dim_arc    = loading_make_comet_arc(scr, orbit_diam, arc_width,
                                                  360 - arc_sweep, 360, arc_dim);
    lv_obj_t *bright_arc = loading_make_comet_arc(scr, orbit_diam, arc_width,
                                                  0, arc_sweep, arc_bright);

    // Optional status text, centered above the spinner (Python positions it in the
    // band between the screen top and the top of the orbit).
    if (cfg.contains("text") && cfg["text"].is_string()) {
        std::string text = cfg["text"].get<std::string>();
        if (!text.empty()) {
            lv_obj_t *label = lv_label_create(scr);
            lv_label_set_text(label, text.c_str());
            lv_obj_set_style_text_font(label, &TOP_NAV_TITLE_FONT, LV_PART_MAIN);
            lv_obj_set_style_text_color(label, lv_color_hex(BODY_FONT_COLOR), LV_PART_MAIN);
            lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
            // Python: screen_y = (canvas_h - orbit_bottom) / 2, i.e. centered in the
            // gap above the orbit. orbit_bottom = (canvas_h + logo)/2 + orbit_gap.
            const int32_t orbit_bottom = (screen_h + logo_size) / 2 + orbit_gap;
            lv_obj_align(label, LV_ALIGN_TOP_MID, 0, (screen_h - orbit_bottom) / 2);
        }
    }

    // Self-driven spin (see loading_spin_timer_cb): a clamped wall-clock integrator, so
    // it runs smoothly at healthy fps, slows under extreme load instead of jumping, and
    // recovers. Not gated on static render — the generator advances no ticks before the
    // still PNG, so that frame is a deterministic 0° (bright [0,45] + dim tail); the GIF
    // path advances 70 ms/frame, each unclamped (< 60° step), for a seamless 7-frame loop.
    loading_spin_ctx_t *spin = new loading_spin_ctx_t{ bright_arc, dim_arc, nullptr, lv_tick_get(), 0 };
    spin->timer = lv_timer_create(loading_spin_timer_cb, LOADING_SPIN_TICK_MS, spin);
    lv_obj_add_event_cb(scr, loading_cleanup_cb, LV_EVENT_DELETE, spin);

    load_screen_and_cleanup_previous(scr);
}
