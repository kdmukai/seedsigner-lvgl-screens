// opening_splash_screen
//
// Python provenance: OpeningSplashScreen (screen.py)
//
// The boot splash: a centered SeedSigner logo fades/slides into place, the
// version label reveals beneath it, and — when partner logos are enabled — a
// bottom "With support from:" + HRF band reveals a beat later. The screen owns no
// buttons; it emits SEEDSIGNER_RET_SPLASH_COMPLETE via
// seedsigner_lvgl_on_button_selected once the timed sequence finishes (or on the
// first touch/key when dismissible), the same async completion pattern as
// screensaver_screen.
//
// Two non-animated render paths short-circuit before any timer/ctx is created:
//   - boot_logo_only: just the centered logo (previews the firmware's C-boot logo
//     position), no version / band / animation / completion.
//   - static render (screenshot generator, seedsigner_lvgl_is_static_render): the
//     FINAL frame composed at full opacity, since a single still cannot animate.
//
// MicroPython handoff: the firmware holds its C-boot logo on the display through
// boot and passes logo_already_shown=true, so the splash shows the logo SOLID from
// frame 0 (no fade) and animates only the version + band — a seamless boot->splash
// transition.
//
// Layout: chrome-free tier (no top-nav scaffold) — a bare root screen with absolute
// positioning. Y offsets are the Python 240px-reference constants (e.g. the -56 logo
// raise) scaled by active_profile().px_multiplier; the version anchor reclaims
// text_top_leading so the visible glyphs land where PIL puts them. The partner band
// lives in a transparent full-screen container so the whole band reveals as a unit.
// Documented deviation from Python: the logo always ENTERS centered and then slides
// UP into its raised slot (Python fades it in already-raised) — one consistent motion
// across both platforms that also makes the MCU boot->splash handoff seamless.
//
// Content policy (spec §5): version and the reveal flags default, so a NULL/empty
// ctx still yields a valid logo-only splash (the MicroPython boot handoff, before
// any host view layer exists). But sponsor_text is localized CONTENT: it is
// REQUIRED from the host whenever the partner band actually renders (partner logos
// enabled and not boot_logo_only), so no English caption is ever baked in. version
// stays a data default (empty -> no label). The structural defaults live in ONE
// place, the merge-patch defaults object below.
//
// Lifecycle (stateful, Tier 2): one heap ctx owns the reveal lv_timer, the optional
// keypad-sink lv_group, and the phase state; opening_splash_cleanup_handler on the
// screen root (LV_EVENT_DELETE) tears all three down. This file is the §6 rule-6
// exemplar for TIMER-NULL-BEFORE-HOST-CALLBACK re-entrancy safety:
// opening_splash_emit_complete deletes AND nulls the timer BEFORE invoking the host
// completion callback, so a synchronous host screen-swap (which re-enters screen
// construction and deletes this screen) cannot double-free the timer.
//
// cfg — the ctx is optional (parse_optional_screen_json_ctx: NULL/empty ctx yields
// the all-defaults English splash; any present ctx is merge-patched over the
// defaults):
//   version             (string, default "")                   version text under the logo; empty -> no label
//   show_partner_logos  (bool,   default false)                 reveal the bottom HRF partner band
//   sponsor_text        (string, required WHEN the band renders — show_partner_logos && !boot_logo_only)  band caption above the HRF logo
//   logo_already_shown  (bool,   default false)                 MicroPython handoff: logo already on screen -> no fade
//   boot_logo_only      (bool,   default false)                 preview only the centered logo, no reveal/animation
//   dismissible         (bool,   default true)                  first touch/key completes the splash early
//   durations           (object, optional)                      reveal timing overrides; each sub-key optional:
//     fade_in_ms        (uint,   default 1200)                  logo fade-in
//     slide_in_ms       (uint,   default 600)                   logo raise (only when a band is shown)
//     hold_logo_ms      (uint,   default 1000)                  hold after the version appears, before the band
//     hold_final_ms     (uint,   default 2000)                  hold on the final frame before completing

#include "screen_scaffold.h"  // parse_optional_screen_json_ctx, load_screen_and_cleanup_previous
#include "seedsigner.h"       // opening_splash_screen decl, SEEDSIGNER_RET_SPLASH_COMPLETE, seedsigner_lvgl_on_button_selected, seedsigner_lvgl_is_static_render, text_top_leading
#include "gui_constants.h"    // active_profile, seedsigner_logo_for_active_profile, hrf_logo_for_active_profile, COMPONENT_PADDING, TOP_NAV_TITLE_FONT, BODY_FONT, ACCENT_COLOR
#include "input_profile.h"    // input_profile_get_mode, INPUT_MODE_HARDWARE

#include "lvgl.h"             // lv_obj / lv_image / lv_label / lv_anim / lv_timer / lv_group / lv_indev + style setters, lv_malloc / lv_memzero / lv_free

#include <nlohmann/json.hpp>  // json (cfg defaults, merge_patch, reads)

#include <stdexcept>          // std::runtime_error (required sponsor_text)
#include <string>             // std::string

using json = nlohmann::json;


// ---------------------------------------------------------------------------
// Lifecycle + phase machine  (Tier 2: one heap ctx, LV_EVENT_DELETE teardown)
// ---------------------------------------------------------------------------

namespace {

// Timed reveal sequence: the timer advances through these phases on wall-clock
// elapsed time, toggling LV_OBJ_FLAG_HIDDEN to bring elements in.
enum opening_splash_phase_t {
    OPENING_SPLASH_PHASE_INTRO = 0,  // logo fading/sliding in; version + band hidden
    OPENING_SPLASH_PHASE_LOGO,       // logo settled, version visible, holding
    OPENING_SPLASH_PHASE_PARTNER,    // partner band revealed, holding
    OPENING_SPLASH_PHASE_DONE,
};

// Pure-POD context: allocated with lv_malloc + lv_memzero and released with
// lv_free (no C++ members that need constructors/destructors).
struct opening_splash_ctx_t {
    lv_obj_t             *screen;
    lv_obj_t             *version_label;  // revealed once the logo intro completes (NULL if no version)
    lv_obj_t             *partner_band;   // transparent full-screen container, NULL if no partner
    lv_timer_t           *timer;
    lv_group_t           *group;
    opening_splash_phase_t phase;
    bool                  dismissible;
    bool                  show_partner;
    bool                  emitted;
    uint32_t              phase_start;    // lv_tick at phase entry
    uint32_t              intro_ms;       // logo fade/slide-in duration (0 = nothing to animate)
    uint32_t              hold_logo_ms;
    uint32_t              hold_final_ms;
};

// Emit the one-shot completion result. Guarded by ctx->emitted so it fires exactly
// once. Rule-6 re-entrancy safety: delete AND null the timer BEFORE the host
// callback, so a synchronous host screen-swap inside the callback (which deletes
// this screen and runs the cleanup handler) cannot double-delete the timer.
void opening_splash_emit_complete(opening_splash_ctx_t *ctx) {
    if (ctx->emitted) return;
    ctx->emitted = true;
    ctx->phase = OPENING_SPLASH_PHASE_DONE;
    if (ctx->timer) { lv_timer_del(ctx->timer); ctx->timer = NULL; }
    seedsigner_lvgl_on_button_selected(SEEDSIGNER_RET_SPLASH_COMPLETE, "splash_complete");
}

// lv_anim exec cb: the logo's fade-in opacity.
void opening_splash_logo_opa_cb(void *obj, int32_t v) {
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, LV_PART_MAIN);
}

// lv_anim exec cb: the logo's vertical position (center-relative offset). Used for
// the MicroPython partner handoff — the held boot logo is centered, so slide it up
// to the partner-band offset rather than letting it jump.
void opening_splash_logo_y_cb(void *obj, int32_t v) {
    lv_obj_align((lv_obj_t *)obj, LV_ALIGN_CENTER, 0, v);
}

void opening_splash_timer_cb(lv_timer_t *timer) {
    opening_splash_ctx_t *ctx = (opening_splash_ctx_t *)lv_timer_get_user_data(timer);

    // Early dismiss on touch: poll pointer devices directly (like the screensaver).
    if (ctx->dismissible) {
        lv_indev_t *indev = NULL;
        while ((indev = lv_indev_get_next(indev)) != NULL) {
            if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER &&
                lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED) {
                opening_splash_emit_complete(ctx);
                return;
            }
        }
    }

    uint32_t elapsed = lv_tick_get() - ctx->phase_start;

    switch (ctx->phase) {
    case OPENING_SPLASH_PHASE_INTRO:
        // Logo fading and/or sliding into position. Once it settles, bring in the
        // version text — Python draws the version only after the logo fade.
        if (elapsed >= ctx->intro_ms) {
            if (ctx->version_label) lv_obj_remove_flag(ctx->version_label, LV_OBJ_FLAG_HIDDEN);
            ctx->phase = OPENING_SPLASH_PHASE_LOGO;
            ctx->phase_start = lv_tick_get();
        }
        break;
    case OPENING_SPLASH_PHASE_LOGO: {
        // Version visible; hold a beat, then reveal the partner band (if any).
        uint32_t hold = ctx->show_partner ? ctx->hold_logo_ms : ctx->hold_final_ms;
        if (elapsed >= hold) {
            if (ctx->show_partner && ctx->partner_band) {
                lv_obj_remove_flag(ctx->partner_band, LV_OBJ_FLAG_HIDDEN);
                ctx->phase = OPENING_SPLASH_PHASE_PARTNER;
                ctx->phase_start = lv_tick_get();
            } else {
                opening_splash_emit_complete(ctx);
            }
        }
        break;
    }
    case OPENING_SPLASH_PHASE_PARTNER:
        if (elapsed >= ctx->hold_final_ms) opening_splash_emit_complete(ctx);
        break;
    case OPENING_SPLASH_PHASE_DONE:
    default:
        break;
    }
}

// LV_EVENT_KEY on the keypad sink: any key completes the splash (hardware input).
void opening_splash_key_handler(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_KEY) return;
    opening_splash_ctx_t *ctx = (opening_splash_ctx_t *)lv_event_get_user_data(e);
    if (ctx && ctx->dismissible) opening_splash_emit_complete(ctx);
}

// LV_EVENT_DELETE teardown on the screen root: the timer and group are NOT
// widget-owned (an lv_timer/lv_group is not freed when its objects are deleted), so
// delete them explicitly, then free the ctx.
void opening_splash_cleanup_handler(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;
    opening_splash_ctx_t *ctx = (opening_splash_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;
    if (ctx->timer) { lv_timer_del(ctx->timer); ctx->timer = NULL; }
    if (ctx->group) { lv_group_del(ctx->group); ctx->group = NULL; }
    lv_free(ctx);
}

}  // namespace


void opening_splash_screen(void *ctx_json) {
    // --- Config ---

    // Defaults reproduce the English opening splash so a NULL/empty ctx still
    // renders (boot-tier contract, spec §5). A caller overrides only the keys it
    // cares about via RFC 7396 merge-patch, like main_menu_screen. version and the
    // flags default here (SINGLE source of truth — the scalar reads below take the
    // merged values); sponsor_text is NOT defaulted — it is localized content, read
    // and required conditionally below.
    json cfg = {
        {"version", ""},
        {"show_partner_logos", false},
        {"logo_already_shown", false},
        {"boot_logo_only", false},
        {"dismissible", true},
    };
    // The shared optional parse turns a missing context into an empty (normalized)
    // object, so the defaults above survive untouched.
    const char *json_str = (const char *)ctx_json;
    json incoming;
    parse_optional_screen_json_ctx(json_str, incoming);
    cfg.merge_patch(incoming);

    // merge-patch guarantees every default key is present, so read them directly.
    const std::string version      = cfg["version"].get<std::string>();
    const bool show_partner        = cfg["show_partner_logos"].get<bool>();
    const bool logo_already_shown  = cfg["logo_already_shown"].get<bool>();
    const bool boot_logo_only      = cfg["boot_logo_only"].get<bool>();
    const bool dismissible         = cfg["dismissible"].get<bool>();

    // sponsor_text is localized CONTENT shown only on the partner band; require it
    // from the host ONLY when that band actually renders, so a boot_logo_only /
    // no-partners splash needn't supply it (an English literal baked here would ship
    // untranslated).
    std::string sponsor_text;
    if (show_partner && !boot_logo_only) {
        if (!cfg.contains("sponsor_text") || !cfg["sponsor_text"].is_string() ||
            cfg["sponsor_text"].get<std::string>().empty()) {
            throw std::runtime_error("opening_splash_screen: sponsor_text is required when partner logos are shown");
        }
        sponsor_text = cfg["sponsor_text"].get<std::string>();
    }

    // Reveal timing (optional overrides via a nested durations object).
    uint32_t fade_in_ms = 1200, slide_in_ms = 600, hold_logo_ms = 1000, hold_final_ms = 2000;
    if (cfg.contains("durations") && cfg["durations"].is_object()) {
        const auto &durations = cfg["durations"];
        fade_in_ms    = durations.value("fade_in_ms", fade_in_ms);
        slide_in_ms   = durations.value("slide_in_ms", slide_in_ms);
        hold_logo_ms  = durations.value("hold_logo_ms", hold_logo_ms);
        hold_final_ms = durations.value("hold_final_ms", hold_final_ms);
    }

    // --- Bare-root build ---

    // Bare root screen (chrome-free: no top-nav scaffold), black background,
    // nothing scrolls.
    lv_obj_t *screen_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen_root, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen_root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(screen_root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(screen_root, LV_OBJ_FLAG_SCROLLABLE);

    const int32_t screen_h = lv_display_get_vertical_resolution(NULL);
    const int32_t screen_w = lv_display_get_horizontal_resolution(NULL);
    const int32_t px_mult  = active_profile().px_multiplier;

    // 1. Centered logo, shifted up to make room for the partner band when shown.
    //    (-56 is the Python offset at the 240px reference; scale it by px_multiplier.)
    const lv_image_dsc_t *logo = seedsigner_logo_for_active_profile();
    const int32_t logo_w = (int32_t)logo->header.w;
    const int32_t logo_h = (int32_t)logo->header.h;
    const int32_t logo_offset_y = (show_partner && !boot_logo_only) ? -(56 * px_mult / 100) : 0;

    lv_obj_t *logo_img = lv_image_create(screen_root);
    lv_image_set_src(logo_img, logo);
    lv_obj_set_size(logo_img, logo_w, logo_h);
    lv_obj_align(logo_img, LV_ALIGN_CENTER, 0, logo_offset_y);

    // Boot-logo preview: only the centered logo (matches the firmware C-boot
    // logo's position). No version, partner band, animation, or completion.
    if (boot_logo_only) {
        load_screen_and_cleanup_previous(screen_root);
        return;
    }

    // 2. Version label (accent color), COMPONENT_PADDING below the logo's bottom.
    //    Mirrors Python: version_y = canvas_h/2 + logo_h/2 + logo_offset_y + CP, drawn
    //    top-anchored; subtract text_top_leading so the visible text lands like PIL.
    lv_obj_t *version_label = NULL;
    if (!version.empty()) {
        version_label = lv_label_create(screen_root);
        lv_label_set_text(version_label, version.c_str());
        lv_obj_set_style_text_font(version_label, &TOP_NAV_TITLE_FONT, LV_PART_MAIN);
        lv_obj_set_style_text_color(version_label, lv_color_hex(ACCENT_COLOR), LV_PART_MAIN);
        const int32_t version_top = screen_h / 2 + logo_h / 2 + logo_offset_y + COMPONENT_PADDING;
        const int32_t lead = text_top_leading(&TOP_NAV_TITLE_FONT, version.c_str());
        lv_obj_align(version_label, LV_ALIGN_TOP_MID, 0, version_top - lead);
    }

    // 3. Partner band: "With support from:" + HRF logo, pinned to the bottom.
    //    Built in a transparent full-screen container so the whole band reveals as a
    //    unit. Layout from the bottom up (mirrors Python): CP bottom margin, HRF logo,
    //    CP/2 gap, sponsor text.
    lv_obj_t *partner_band = NULL;
    if (show_partner) {
        const lv_image_dsc_t *hrf = hrf_logo_for_active_profile();
        const int32_t hrf_h = (int32_t)hrf->header.h;

        partner_band = lv_obj_create(screen_root);
        lv_obj_remove_style_all(partner_band);
        lv_obj_set_size(partner_band, screen_w, screen_h);
        lv_obj_set_pos(partner_band, 0, 0);
        lv_obj_remove_flag(partner_band, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

        lv_obj_t *hrf_img = lv_image_create(partner_band);
        lv_image_set_src(hrf_img, hrf);
        lv_obj_set_size(hrf_img, (int32_t)hrf->header.w, hrf_h);
        const int32_t hrf_y = screen_h - COMPONENT_PADDING - hrf_h;
        lv_obj_align(hrf_img, LV_ALIGN_TOP_MID, 0, hrf_y);

        lv_obj_t *sponsor = lv_label_create(partner_band);
        lv_label_set_text(sponsor, sponsor_text.c_str());
        lv_obj_set_style_text_font(sponsor, &BODY_FONT, LV_PART_MAIN);
        lv_obj_set_style_text_color(sponsor, lv_color_hex(0xcccccc), LV_PART_MAIN);
        const int32_t line_h = lv_font_get_line_height(&BODY_FONT);
        lv_obj_align(sponsor, LV_ALIGN_TOP_MID, 0, hrf_y - (COMPONENT_PADDING / 2) - line_h);
    }

    // Static render (screenshot generator): the final frame at full opacity, no
    // animation or timing — exactly what a single still should capture.
    if (seedsigner_lvgl_is_static_render()) {
        load_screen_and_cleanup_previous(screen_root);
        return;
    }

    // --- Live entrance ---

    // Unified "center, then up": the logo always enters CENTERED (CPython fades it in
    // there; MicroPython's held C-boot logo is already there), then, when a partner
    // band is shown, it slides UP into its raised slot. The version and partner band
    // stay hidden until the logo settles — Python brings the version in only after
    // the logo finishes, and the band follows a beat later. (This diverges from
    // Python, which fades the logo in already-raised: the slide gives both platforms
    // one consistent motion and makes the boot->splash handoff seamless on the MCU.)
    if (version_label) lv_obj_add_flag(version_label, LV_OBJ_FLAG_HIDDEN);
    if (partner_band)  lv_obj_add_flag(partner_band,  LV_OBJ_FLAG_HIDDEN);

    lv_obj_align(logo_img, LV_ALIGN_CENTER, 0, 0);  // enter centered (where the boot logo sits)

    const uint32_t fade_ms  = logo_already_shown ? 0u : fade_in_ms;     // no fade if already on screen
    const uint32_t slide_ms = (logo_offset_y != 0) ? slide_in_ms : 0u;  // slide only when a band raises it

    if (fade_ms > 0) {
        lv_obj_set_style_opa(logo_img, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_anim_t fade;
        lv_anim_init(&fade);
        lv_anim_set_var(&fade, logo_img);
        lv_anim_set_exec_cb(&fade, opening_splash_logo_opa_cb);
        lv_anim_set_values(&fade, LV_OPA_TRANSP, LV_OPA_COVER);
        lv_anim_set_duration(&fade, fade_ms);
        lv_anim_set_path_cb(&fade, lv_anim_path_ease_out);
        lv_anim_start(&fade);
    }
    if (slide_ms > 0) {
        lv_anim_t slide;
        lv_anim_init(&slide);
        lv_anim_set_var(&slide, logo_img);
        lv_anim_set_exec_cb(&slide, opening_splash_logo_y_cb);
        lv_anim_set_values(&slide, 0, logo_offset_y);
        lv_anim_set_duration(&slide, slide_ms);
        lv_anim_set_delay(&slide, fade_ms);  // slide begins once the fade (if any) finishes
        lv_anim_set_path_cb(&slide, lv_anim_path_ease_in_out);
        lv_anim_start(&slide);
    }

    // --- Lifecycle ---

    opening_splash_ctx_t *ctx = (opening_splash_ctx_t *)lv_malloc(sizeof(opening_splash_ctx_t));
    lv_memzero(ctx, sizeof(*ctx));
    ctx->screen        = screen_root;
    ctx->version_label = version_label;
    ctx->partner_band  = partner_band;
    ctx->phase         = OPENING_SPLASH_PHASE_INTRO;
    ctx->dismissible   = dismissible;
    ctx->show_partner  = show_partner;
    ctx->intro_ms      = fade_ms + slide_ms;
    ctx->hold_logo_ms  = hold_logo_ms;
    ctx->hold_final_ms = hold_final_ms;
    ctx->phase_start   = lv_tick_get();
    ctx->timer = lv_timer_create(opening_splash_timer_cb, 50, ctx);

    // Keypad sink: any key press dismisses (hardware input mode), like the screensaver.
    if (dismissible && input_profile_get_mode() == INPUT_MODE_HARDWARE) {
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
        lv_obj_add_event_cb(sink, opening_splash_key_handler, LV_EVENT_KEY, ctx);

        lv_indev_t *indev = NULL;
        while ((indev = lv_indev_get_next(indev)) != NULL) {
            if (lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD ||
                lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER) {
                lv_indev_set_group(indev, ctx->group);
            }
        }
    }

    lv_obj_add_event_cb(screen_root, opening_splash_cleanup_handler, LV_EVENT_DELETE, ctx);

    // --- Load ---

    load_screen_and_cleanup_previous(screen_root);
}
