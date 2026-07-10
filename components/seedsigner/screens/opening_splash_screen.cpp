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
// opening_splash_screen — opening splash (parity with Python OpeningSplashScreen)
// ---------------------------------------------------------------------------
// Like every screen here it BUILDS-and-RETURNS; the timed reveal is driven by an
// lv_anim (logo fade) + an lv_timer (hold/reveal sequence), and completion is
// emitted via seedsigner_lvgl_on_button_selected(SEEDSIGNER_RET_SPLASH_COMPLETE)
// — the screensaver_screen() async pattern. The screenshot generator renders a
// single static frame, so the FINAL frame is composed at full opacity up front
// and the fade/timer/hidden-band are gated behind !seedsigner_lvgl_is_static_render().
//
// MicroPython passes logo_already_shown=true: the firmware holds the C-boot logo
// on the display through boot, so the splash shows the logo SOLID from frame 0
// (no fade) and animates only the version + partner band — a seamless handoff.

typedef enum {
    SPLASH_PHASE_INTRO = 0,  // logo fading/sliding in; version + band hidden
    SPLASH_PHASE_LOGO,       // logo settled, version visible, holding
    SPLASH_PHASE_PARTNER,    // partner band revealed, holding
    SPLASH_PHASE_DONE,
} splash_phase_t;

typedef struct {
    lv_obj_t      *screen;
    lv_obj_t      *version_label;  // revealed once the logo intro completes (NULL if no version)
    lv_obj_t      *partner_band;   // transparent full-screen container, NULL if no partner
    lv_timer_t    *timer;
    lv_group_t    *group;
    splash_phase_t phase;
    bool           dismissible;
    bool           show_partner;
    bool           emitted;
    uint32_t       phase_start;    // lv_tick at phase entry
    uint32_t       intro_ms;       // logo fade/slide-in duration (0 = nothing to animate)
    uint32_t       hold_logo_ms;
    uint32_t       hold_final_ms;
} splash_ctx_t;

static void splash_emit_complete(splash_ctx_t *ctx) {
    if (ctx->emitted) return;
    ctx->emitted = true;
    ctx->phase = SPLASH_PHASE_DONE;
    if (ctx->timer) { lv_timer_del(ctx->timer); ctx->timer = NULL; }
    seedsigner_lvgl_on_button_selected(SEEDSIGNER_RET_SPLASH_COMPLETE, "splash_complete");
}

static void splash_logo_opa_cb(void *obj, int32_t v) {
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, LV_PART_MAIN);
}

// Animate the logo's vertical position (center-relative offset). Used for the
// MicroPython partner handoff: the held boot logo is centered, so slide it up to
// the partner-band offset rather than letting it jump.
static void splash_logo_y_cb(void *obj, int32_t v) {
    lv_obj_align((lv_obj_t *)obj, LV_ALIGN_CENTER, 0, v);
}

static void splash_timer_cb(lv_timer_t *timer) {
    splash_ctx_t *ctx = (splash_ctx_t *)lv_timer_get_user_data(timer);

    // Early dismiss on touch: poll pointer devices directly (like the screensaver).
    if (ctx->dismissible) {
        lv_indev_t *indev = NULL;
        while ((indev = lv_indev_get_next(indev)) != NULL) {
            if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER &&
                lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED) {
                splash_emit_complete(ctx);
                return;
            }
        }
    }

    uint32_t elapsed = lv_tick_get() - ctx->phase_start;

    switch (ctx->phase) {
    case SPLASH_PHASE_INTRO:
        // Logo fading and/or sliding into position. Once it settles, bring in the
        // version text — Python draws the version only after the logo fade.
        if (elapsed >= ctx->intro_ms) {
            if (ctx->version_label) lv_obj_remove_flag(ctx->version_label, LV_OBJ_FLAG_HIDDEN);
            ctx->phase = SPLASH_PHASE_LOGO;
            ctx->phase_start = lv_tick_get();
        }
        break;
    case SPLASH_PHASE_LOGO: {
        // Version visible; hold a beat, then reveal the partner band (if any).
        uint32_t hold = ctx->show_partner ? ctx->hold_logo_ms : ctx->hold_final_ms;
        if (elapsed >= hold) {
            if (ctx->show_partner && ctx->partner_band) {
                lv_obj_remove_flag(ctx->partner_band, LV_OBJ_FLAG_HIDDEN);
                ctx->phase = SPLASH_PHASE_PARTNER;
                ctx->phase_start = lv_tick_get();
            } else {
                splash_emit_complete(ctx);
            }
        }
        break;
    }
    case SPLASH_PHASE_PARTNER:
        if (elapsed >= ctx->hold_final_ms) splash_emit_complete(ctx);
        break;
    case SPLASH_PHASE_DONE:
    default:
        break;
    }
}

static void splash_key_handler(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_KEY) return;
    splash_ctx_t *ctx = (splash_ctx_t *)lv_event_get_user_data(e);
    if (ctx && ctx->dismissible) splash_emit_complete(ctx);
}

static void splash_cleanup_handler(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;
    splash_ctx_t *ctx = (splash_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;
    if (ctx->timer) { lv_timer_del(ctx->timer); ctx->timer = NULL; }
    if (ctx->group) { lv_group_del(ctx->group); ctx->group = NULL; }
    lv_free(ctx);
}

void opening_splash_screen(void *ctx_json) {
    // Defaults reproduce the English opening splash; a caller overrides only the
    // keys it cares about (RFC 7396 merge-patch), like main_menu_screen.
    json cfg = {
        {"version", ""},
        {"show_partner_logos", false},
        {"sponsor_text", "With support from:"},
        {"logo_already_shown", false},
        {"boot_logo_only", false},
        {"dismissible", true},
    };
    // The shared optional parse turns a missing context into an empty
    // (normalized) object, so the defaults above survive untouched.
    const char *json_str = (const char *)ctx_json;
    json incoming;
    parse_optional_screen_json_ctx(json_str, incoming);
    cfg.merge_patch(incoming);

    const std::string version      = cfg.value("version", std::string(""));
    const std::string sponsor_text = cfg.value("sponsor_text", std::string("With support from:"));
    const bool show_partner        = cfg.value("show_partner_logos", false);
    const bool logo_already_shown  = cfg.value("logo_already_shown", false);
    const bool boot_logo_only      = cfg.value("boot_logo_only", false);
    const bool dismissible         = cfg.value("dismissible", true);

    uint32_t fade_in_ms = 1200, slide_in_ms = 600, hold_logo_ms = 1000, hold_final_ms = 2000;
    if (cfg.contains("durations") && cfg["durations"].is_object()) {
        const auto &d = cfg["durations"];
        fade_in_ms    = d.value("fade_in_ms", fade_in_ms);
        slide_in_ms   = d.value("slide_in_ms", slide_in_ms);
        hold_logo_ms  = d.value("hold_logo_ms", hold_logo_ms);
        hold_final_ms = d.value("hold_final_ms", hold_final_ms);
    }

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    const int32_t screen_h = lv_display_get_vertical_resolution(NULL);
    const int32_t screen_w = lv_display_get_horizontal_resolution(NULL);
    const int32_t px_mult  = active_profile().px_multiplier;

    // Centered logo, shifted up to make room for the partner band when shown.
    // (-56 is the Python offset at the 240px reference; scale it by px_multiplier.)
    const lv_image_dsc_t *logo = seedsigner_logo_for_active_profile();
    const int32_t logo_w = (int32_t)logo->header.w;
    const int32_t logo_h = (int32_t)logo->header.h;
    const int32_t logo_offset_y = (show_partner && !boot_logo_only) ? -(56 * px_mult / 100) : 0;

    lv_obj_t *logo_img = lv_image_create(scr);
    lv_image_set_src(logo_img, logo);
    lv_obj_set_size(logo_img, logo_w, logo_h);
    lv_obj_align(logo_img, LV_ALIGN_CENTER, 0, logo_offset_y);

    // Boot-logo preview: only the centered logo (matches the firmware C-boot
    // logo's position). No version, partner band, animation, or completion.
    if (boot_logo_only) {
        load_screen_and_cleanup_previous(scr);
        return;
    }

    // Version label (accent color), COMPONENT_PADDING below the logo's bottom.
    // Mirrors Python: version_y = canvas_h/2 + logo_h/2 + logo_offset_y + CP, drawn
    // top-anchored; subtract text_top_leading so the visible text lands like PIL.
    lv_obj_t *version_label = NULL;
    if (!version.empty()) {
        version_label = lv_label_create(scr);
        lv_label_set_text(version_label, version.c_str());
        lv_obj_set_style_text_font(version_label, &TOP_NAV_TITLE_FONT, LV_PART_MAIN);
        lv_obj_set_style_text_color(version_label, lv_color_hex(ACCENT_COLOR), LV_PART_MAIN);
        const int32_t version_top = screen_h / 2 + logo_h / 2 + logo_offset_y + COMPONENT_PADDING;
        const int32_t lead = text_top_leading(&TOP_NAV_TITLE_FONT, version.c_str());
        lv_obj_align(version_label, LV_ALIGN_TOP_MID, 0, version_top - lead);
    }

    // Partner band: "With support from:" + HRF logo, pinned to the bottom.
    // Built in a transparent full-screen container so the whole band reveals as a
    // unit. Layout from the bottom up (mirrors Python): CP bottom margin, HRF logo,
    // CP/2 gap, sponsor text.
    lv_obj_t *partner_band = NULL;
    if (show_partner) {
        const lv_image_dsc_t *hrf = hrf_logo_for_active_profile();
        const int32_t hrf_h = (int32_t)hrf->header.h;

        partner_band = lv_obj_create(scr);
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
        load_screen_and_cleanup_previous(scr);
        return;
    }

    // Live entrance — unified "center, then up": the logo always enters CENTERED
    // (CPython fades it in there; MicroPython's held C-boot logo is already there),
    // then, when a partner band is shown, it slides UP into its raised slot. The
    // version and partner band stay hidden until the logo settles — Python brings
    // the version in only after the logo finishes, and the band follows a beat
    // later. (This diverges from Python, which fades the logo in already-raised:
    // the slide gives both platforms one consistent motion and makes the
    // boot->splash handoff seamless on the MCU.)
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
        lv_anim_set_exec_cb(&fade, splash_logo_opa_cb);
        lv_anim_set_values(&fade, LV_OPA_TRANSP, LV_OPA_COVER);
        lv_anim_set_duration(&fade, fade_ms);
        lv_anim_set_path_cb(&fade, lv_anim_path_ease_out);
        lv_anim_start(&fade);
    }
    if (slide_ms > 0) {
        lv_anim_t slide;
        lv_anim_init(&slide);
        lv_anim_set_var(&slide, logo_img);
        lv_anim_set_exec_cb(&slide, splash_logo_y_cb);
        lv_anim_set_values(&slide, 0, logo_offset_y);
        lv_anim_set_duration(&slide, slide_ms);
        lv_anim_set_delay(&slide, fade_ms);  // slide begins once the fade (if any) finishes
        lv_anim_set_path_cb(&slide, lv_anim_path_ease_in_out);
        lv_anim_start(&slide);
    }

    splash_ctx_t *ctx = (splash_ctx_t *)lv_malloc(sizeof(splash_ctx_t));
    lv_memzero(ctx, sizeof(*ctx));
    ctx->screen        = scr;
    ctx->version_label = version_label;
    ctx->partner_band  = partner_band;
    ctx->phase         = SPLASH_PHASE_INTRO;
    ctx->dismissible   = dismissible;
    ctx->show_partner  = show_partner;
    ctx->intro_ms      = fade_ms + slide_ms;
    ctx->hold_logo_ms  = hold_logo_ms;
    ctx->hold_final_ms = hold_final_ms;
    ctx->phase_start   = lv_tick_get();
    ctx->timer = lv_timer_create(splash_timer_cb, 50, ctx);

    // Keypad sink: any key press dismisses (hardware input mode), like the screensaver.
    if (dismissible && input_profile_get_mode() == INPUT_MODE_HARDWARE) {
        lv_obj_t *sink = lv_obj_create(scr);
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
        lv_obj_add_event_cb(sink, splash_key_handler, LV_EVENT_KEY, ctx);

        lv_indev_t *indev = NULL;
        while ((indev = lv_indev_get_next(indev)) != NULL) {
            if (lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD ||
                lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER) {
                lv_indev_set_group(indev, ctx->group);
            }
        }
    }

    lv_obj_add_event_cb(scr, splash_cleanup_handler, LV_EVENT_DELETE, ctx);
    load_screen_and_cleanup_previous(scr);
}
