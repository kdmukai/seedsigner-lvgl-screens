// loading_spinner_screen
//
// Python provenance: LoadingScreenThread (screen.py)
//
// The animated "processing" spinner, shown while the host CPU runs a long,
// blocking task (seed generation, PSBT signing, ...). Python spins a background
// THREAD that repaints a comet arc around the Bitcoin logo while the main thread
// works. Here the host thread is busy inside the task and cannot drive frames, so
// the animation is SELF-DRIVEN by an lv_timer: as long as the platform keeps
// ticking lv_timer_handler (a dedicated display task on both the ESP32 and Pi
// Zero backends), the comet keeps rotating with zero host involvement.
//
// The screen takes no input and returns no result — it is a pure visual. The
// host shows it, does its work, then dismisses it by loading the next screen.
//
// Chrome-free tier: no top-nav scaffold — a bare root screen holds the logo, the
// two comet arcs, and the optional status line; the mandatory
// load_screen_and_cleanup_previous tail still applies.
//
// Lifecycle (stateful, Tier 2): the spin driver is one heap ctx owning one
// lv_timer. Unlike an lv_anim, an lv_timer is NOT owned by any widget and is NOT
// freed when its objects are deleted — teardown is explicit:
// loading_spinner_cleanup_cb is registered on the screen root for
// LV_EVENT_DELETE, so when the next screen's load_screen_and_cleanup_previous
// deletes this screen, the callback deletes the timer and frees the ctx.
//
// Layout (ports LoadingScreenThread): a centered Bitcoin logo (the baked
// btc_logo_* asset — orange disc + white tilted ₿) with a two-tone "comet"
// orbiting it: a bright 45° leading arc (#ff9416) trailed by a dim 45° arc
// (#80490b), the two rotating together. Optional status text sits centered in
// the band above the spinner.
//
// cfg — the ctx itself is optional (parse_optional_screen_json_ctx: NULL/empty
// ctx yields an empty config and the bare spinner; any present ctx gets the same
// strict validation as every other screen):
//   text  (string, optional)   localized short status line shown above the
//          spinner, supplied by the host view layer (e.g. "Loading...");
//          omitted or empty -> no label.

#include "screen_scaffold.h"  // parse_optional_screen_json_ctx, load_screen_and_cleanup_previous
#include "seedsigner.h"       // loading_spinner_screen decl
#include "gui_constants.h"    // btc_logo_for_active_profile, COMPONENT_PADDING, TOP_NAV_TITLE_FONT, BACKGROUND_COLOR, BITCOIN_ORANGE, BODY_FONT_COLOR

#include "lvgl.h"             // lv_arc / lv_image / lv_label / lv_timer + per-object style setters

#include <nlohmann/json.hpp>  // json (optional cfg read)

#include <string>             // std::string

using json = nlohmann::json;


// ---------------------------------------------------------------------------
// Spin driver
// ---------------------------------------------------------------------------

// Comet spin tuning. The timer advances by REAL elapsed time (so the rate is
// consistent), but CLAMPS the per-frame step: a plain wall-clock lv_anim would, on a
// starved frame, skip straight to the time-correct angle (a visible jump); clamping
// caps how far one frame can move, so under extreme load the spin instead visibly
// SLOWS DOWN — and smoothly speeds back up as frames recover — a natural "still
// working" signal, never a leap. Rotation is accumulated in millidegrees for
// smoothness at high frame rates.
static const uint32_t LOADING_SPINNER_SPIN_PERIOD_MS     = 490;    // one revolution at healthy fps (~2 rev/s)
static const uint32_t LOADING_SPINNER_SPIN_TICK_MS       = 33;     // update cadence (~30 fps)
static const int32_t  LOADING_SPINNER_SPIN_MAX_STEP_MDEG = 60000;  // clamp: at most 60° advance per frame

namespace {

// Spin-driver context — allocated with `new` in the entry point and released with
// `delete` in loading_spinner_cleanup_cb (the timer is deleted there too: an
// lv_timer is not auto-freed with its objects).
struct loading_spinner_spin_ctx_t {
    lv_obj_t   *bright_arc;
    lv_obj_t   *dim_arc;
    lv_timer_t *timer;
    uint32_t    last_tick;       // lv_tick at the previous update
    int32_t     rotation_mdeg;   // accumulated rotation, millidegrees [0, 360000)
};

// Per-tick spin advance: the clamped wall-clock integrator described at the
// tuning constants above.
void loading_spinner_spin_timer_cb(lv_timer_t *timer) {
    loading_spinner_spin_ctx_t *ctx = (loading_spinner_spin_ctx_t *)lv_timer_get_user_data(timer);
    if (!ctx || !lv_obj_is_valid(ctx->bright_arc)) return;

    uint32_t now        = lv_tick_get();
    uint32_t elapsed_ms = now - ctx->last_tick;   // ms since last update (uint wrap-safe)
    ctx->last_tick = now;

    // Wall-clock advance, clamped so a long (starved) frame slows the spin rather than
    // skipping ahead. The clamp sits just above the generator's 70 ms GIF frame step, so
    // the captured GIF stays the healthy-rate loop while only real on-device stalls slow it.
    int32_t advance_mdeg = (int32_t)((uint64_t)elapsed_ms * 360000u / LOADING_SPINNER_SPIN_PERIOD_MS);
    if (advance_mdeg > LOADING_SPINNER_SPIN_MAX_STEP_MDEG) advance_mdeg = LOADING_SPINNER_SPIN_MAX_STEP_MDEG;
    ctx->rotation_mdeg = (ctx->rotation_mdeg + advance_mdeg) % 360000;

    int32_t rotation_deg = ctx->rotation_mdeg / 1000;
    lv_arc_set_rotation(ctx->bright_arc, rotation_deg);
    if (lv_obj_is_valid(ctx->dim_arc)) lv_arc_set_rotation(ctx->dim_arc, rotation_deg);
}

// LV_EVENT_DELETE teardown on the screen root: reset the idle clock, stop the
// (not widget-owned) spin timer, then free the ctx.
void loading_spinner_cleanup_cb(lv_event_t *e) {
    // The spinner is dismissed by the host loading the NEXT screen (this LV_EVENT_DELETE
    // fires from that screen's load_screen_and_cleanup_previous). Unlike an ordinary screen
    // swap, no user input drove it: a long host task (large-PSBT parse/sign, xpub calc) can
    // outlast the screensaver timeout with zero input, so LVGL's idle clock
    // (lv_display_get_inactive_time) is stale and the overlay manager would fire the
    // screensaver over the freshly-rendered result. Count the dismissal as activity so the
    // successor screen gets a full idle window. Same primitive the overlay manager uses when
    // it wakes from the screensaver. It runs in the same context as the rest of this teardown
    // (lv_timer_delete / delete ctx, driven by the successor's load_screen_and_cleanup_previous),
    // so it needs no synchronization that path doesn't already hold on either backend.
    // Platform-agnostic — one fix for ESP32 and Pi Zero.
    lv_display_trigger_activity(NULL);

    loading_spinner_spin_ctx_t *ctx = (loading_spinner_spin_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;
    if (ctx->timer) lv_timer_delete(ctx->timer);
    delete ctx;
}

// Build one comet segment as a bare indicator-only arc (no track, no knob, not
// interactive) spanning [start,end] degrees, in `color`, at the shared orbit radius.
lv_obj_t *loading_spinner_make_comet_arc(lv_obj_t *parent, int32_t diameter, int32_t width,
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

}  // namespace


void loading_spinner_screen(void *ctx_json) {
    // --- Config ---

    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_optional_screen_json_ctx(json_str, cfg);  // boot/overlay tier: NULL/empty ctx -> empty config

    // --- Bare-root build ---

    // 1. Bare root screen (chrome-free: no top-nav scaffold), solid background,
    //    nothing scrolls.
    lv_obj_t *screen_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen_root, lv_color_hex(BACKGROUND_COLOR), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen_root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen_root, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(screen_root, 0, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(screen_root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(screen_root, LV_OBJ_FLAG_SCROLLABLE);

    const int32_t screen_height = lv_display_get_vertical_resolution(NULL);

    // 2. Centered Bitcoin logo (baked asset, one per profile — orange disc + white ₿).
    const lv_image_dsc_t *logo = btc_logo_for_active_profile();
    const int32_t logo_size = (int32_t)logo->header.w;   // square
    lv_obj_t *logo_image = lv_image_create(screen_root);
    lv_image_set_src(logo_image, logo);
    lv_obj_center(logo_image);

    // 3. Orbit geometry (ports LoadingScreenThread): the comet rides a ring one
    //    orbit_gap outside the logo, drawn COMPONENT_PADDING thick — matching Python's
    //    bounding_box = logo ± orbit_gap and arc width = COMPONENT_PADDING.
    const int32_t orbit_gap      = 2 * COMPONENT_PADDING;
    const int32_t arc_width      = COMPONENT_PADDING;
    const int32_t orbit_diameter = logo_size + 2 * orbit_gap;
    const int32_t arc_sweep      = 45;                       // Python arc_sweep
    const uint32_t arc_bright    = (uint32_t)BITCOIN_ORANGE; // Python arc_color "#ff9416"
    const uint32_t arc_dim       = 0x80490b;                 // Python arc_trailing_color

    //    Trailing (dim) arc first so the bright head draws on top, then the bright head.
    //    The dim segment sits immediately BEHIND the bright one so the pair reads as a
    //    single comet (bright head [0,45], dim tail [-45,0]); a shared rotation spins them.
    lv_obj_t *dim_arc    = loading_spinner_make_comet_arc(screen_root, orbit_diameter, arc_width,
                                                          360 - arc_sweep, 360, arc_dim);
    lv_obj_t *bright_arc = loading_spinner_make_comet_arc(screen_root, orbit_diameter, arc_width,
                                                          0, arc_sweep, arc_bright);

    // 4. Optional status text, centered above the spinner (Python positions it in the
    //    band between the screen top and the top of the orbit).
    if (cfg.contains("text") && cfg["text"].is_string()) {
        std::string text = cfg["text"].get<std::string>();
        if (!text.empty()) {
            lv_obj_t *label = lv_label_create(screen_root);
            lv_label_set_text(label, text.c_str());
            lv_obj_set_style_text_font(label, &TOP_NAV_TITLE_FONT, LV_PART_MAIN);
            lv_obj_set_style_text_color(label, lv_color_hex(BODY_FONT_COLOR), LV_PART_MAIN);
            lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
            // Python: screen_y = (canvas_h - orbit_bottom) / 2, i.e. centered in the
            // gap above the orbit. orbit_bottom = (canvas_h + logo)/2 + orbit_gap.
            const int32_t orbit_bottom = (screen_height + logo_size) / 2 + orbit_gap;
            lv_obj_align(label, LV_ALIGN_TOP_MID, 0, (screen_height - orbit_bottom) / 2);
        }
    }

    // --- Animation setup ---

    // Self-driven spin (see loading_spinner_spin_timer_cb): a clamped wall-clock
    // integrator, so it runs smoothly at healthy fps, slows under extreme load instead
    // of jumping, and recovers. Not gated on static render — the generator advances no
    // ticks before the still PNG, so that frame is a deterministic 0° (bright [0,45] +
    // dim tail); the GIF path advances 70 ms/frame, each unclamped (< 60° step), for a
    // seamless 7-frame loop. The DELETE callback on the root tears the timer + ctx down
    // when the host's next screen replaces this one.
    loading_spinner_spin_ctx_t *ctx = new loading_spinner_spin_ctx_t{ bright_arc, dim_arc, nullptr, lv_tick_get(), 0 };
    ctx->timer = lv_timer_create(loading_spinner_spin_timer_cb, LOADING_SPINNER_SPIN_TICK_MS, ctx);
    lv_obj_add_event_cb(screen_root, loading_spinner_cleanup_cb, LV_EVENT_DELETE, ctx);

    // --- Load ---

    load_screen_and_cleanup_previous(screen_root);
}
