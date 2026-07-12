// camera_entropy_overlay_screen
//
// Python provenance: no Python screen class — this is a desktop-tooling host. The
// overlay it renders (camera_entropy_overlay.{h,cpp}) ports the image-entropy
// capture screens ToolsImageEntropyLivePreviewScreen / ToolsImageEntropyFinalImageScreen
// (tools_screens.py). On device the host owns the live camera surface and pushes
// phase changes, so no single Python screen class maps to this entry point.
//
// Synthesizes a placeholder gray "camera preview" square and renders the entropy
// overlay onto it in ONE static, JSON-described phase (preview / capturing /
// confirm), so the screenshot generator and interactive runner exercise the same
// overlay spec without a camera. Takes no input and returns no result here — the
// affordances (back button, shutter, accept button, instruction text) are built by
// the overlay module and, on device, wire to the host via SEEDSIGNER_RET_BACK_BUTTON
// / seedsigner_lvgl_on_button_selected.
//
// Chrome-free tier (spec §8): no top-nav scaffold — a bare root screen holds the
// synthetic preview panel and the overlay; the mandatory load_screen_and_cleanup_previous
// tail still applies.
//
// Lifecycle: Tier 1 (stateless) — no statics, no timers, no heap ctx. The overlay
// handle is created and immediately destroyed; destroy() frees only the handle struct,
// leaving the widgets parented on the screen tree. The one teardown callback
// (reset_idle_clock_on_teardown) carries no state — it just resets the idle clock on
// LV_EVENT_DELETE so the successor screen gets a full screensaver window.
//
// Layout notes: the synthetic-preview-background block (bare root + per-resolution
// center-cut geometry + gray placeholder panel) is duplicated near-verbatim in the
// twin camera_preview_overlay_screen. It is kept byte-exact here and slated for
// extraction into a shared synthetic-preview-background builder at the rollout
// consolidation decision (spec §12); the two copies deliberately stay identical
// until then. The back affordance and controls follow input_profile_get_mode()
// (read inside the overlay): 240px hardware panels get bottom-center instruction
// text; larger touch panels get on-screen buttons.
//
// cfg — the ctx itself is optional (parse_optional_screen_json_ctx: a NULL/empty ctx
// yields an empty config; any present ctx gets the same strict validation as every
// other screen). EVERY key is optional; on device the strings arrive host-provided
// and already localized (a literal baked here would be English-only by construction):
//   phase                (string, default "preview")   "preview" | "capturing" | "confirm"; unknown -> "preview".
//   preview_instructions (string, optional)   hardware-mode PREVIEW bottom line (e.g. "< back | click a button"); empty -> omitted.
//   confirm_instructions (string, optional)   hardware-mode CONFIRM bottom line (e.g. "< reshoot | accept >"); empty -> omitted.
//   capturing_text       (string, optional)   accent-color transient line during the CAPTURING phase; empty -> omitted.
//   capture_icon         (string, optional)   glyph forwarded to the touch-mode shutter control; empty -> nullptr.
//   capture_style        (string, default "ring")   touch-mode PREVIEW capture control: "ring" | "solid" shutter, or "button"; unknown -> "ring".
//   capture_label        (string, optional)   touch-mode capture BUTTON text (capture_style "button" only); empty -> omitted.
//   accept_label         (string, optional)   touch-mode CONFIRM Accept button text; empty -> omitted.
//   fill_landscape       (bool, default: short dim <= 240)   preview geometry: true fills the display, false center-cuts a square with side gutters.
//   square               (object {x,y,w,h}, optional)   explicit preview-square rect, overriding the per-resolution default.

#include "screen_scaffold.h"        // parse_optional_screen_json_ctx, load_screen_and_cleanup_previous
#include "seedsigner.h"             // camera_entropy_overlay_screen declaration
#include "camera_entropy_overlay.h" // camera_entropy_overlay_create/destroy + spec / phase / capture-style types
#include "image_entropy.h"          // image_entropy_process (crop-to-fill + luminance stretch), for the tooling CONFIRM frame

#include "lvgl.h"                   // lv_obj / lv_display / lv_color / lv_memzero + per-object style setters

#include <nlohmann/json.hpp>        // json (optional cfg reads)

#include <string>                   // std::string

using json = nlohmann::json;


// Desktop tooling only: there is no camera, so synthesize a deliberately LOW-CONTRAST
// test frame and run it through the REAL image_entropy_process + set_confirm_image path,
// exercising crop-to-fill, the luminance stretch, and the CONFIRM render in one shot. The
// frame is a 4:3 source (unlike any display profile, so the crop-to-fill shows), a
// horizontal brightness ramp compressed to ~[50,205] (so the 2% stretch has room to
// expand), tinted in three vertical colour bands by an equal +40 (so a correct UNIFORM
// stretch preserves their hue — a per-channel stretch would skew them).
static void tooling_push_synthetic_confirm_frame(camera_entropy_overlay_t *overlay,
                                                  int32_t screen_w, int32_t screen_h) {
    const int32_t SRC_W = 320, SRC_H = 240;

    uint8_t  *src = (uint8_t  *)lv_malloc((size_t)SRC_W * SRC_H * 3);
    uint16_t *dst = (uint16_t *)lv_malloc((size_t)screen_w * screen_h * 2);
    if (!src || !dst) {
        if (src) lv_free(src);
        if (dst) lv_free(dst);
        return;
    }

    for (int32_t y = 0; y < SRC_H; ++y) {
        for (int32_t x = 0; x < SRC_W; ++x) {
            int base = 50 + (x * 155) / (SRC_W - 1);   // low-contrast horizontal ramp
            int r = base, g = base, b = base;
            if      (y <     SRC_H / 3) r = base + 40;  // warm band
            else if (y < 2 * SRC_H / 3) g = base + 40;  // green band
            else                        b = base + 40;  // cool band
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (b > 255) b = 255;

            uint8_t *p = src + ((size_t)y * SRC_W + x) * 3;
            p[0] = (uint8_t)r; p[1] = (uint8_t)g; p[2] = (uint8_t)b;
        }
    }

    image_entropy_process(src, SRC_W, SRC_H, IMAGE_ENTROPY_PIXFMT_RGB888,
                          dst, screen_w, screen_h);
    camera_entropy_overlay_set_confirm_image(overlay, dst, screen_w, screen_h);

    lv_free(src);
    lv_free(dst);
}


void camera_entropy_overlay_screen(void *ctx_json) {
    // --- Config ---

    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_optional_screen_json_ctx(json_str, cfg);  // boot/overlay tier: NULL/empty ctx -> empty config; every field optional

    // --- Bare-root build ---

    // Synthetic camera-preview background — duplicated near-verbatim in
    // camera_preview_overlay_screen and kept byte-exact with it; slated for
    // extraction into a shared builder at the rollout consolidation decision (§12).

    // 1. Bare root screen (chrome-free: no top-nav scaffold), solid black, nothing scrolls.
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr, 0, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    int32_t screen_w = lv_display_get_horizontal_resolution(NULL);
    int32_t screen_h = lv_display_get_vertical_resolution(NULL);

    // 2. Preview geometry, defaulting per resolution (identical policy to
    //    camera_preview_overlay_screen): the higher-resolution DSI touch panels
    //    (short dimension > 240) default to a landscape center-cut SQUARE with static
    //    side gutters — those panels have per-frame update limits along their long
    //    axis, so confining the live camera writes to the square (the gutters never
    //    refresh) is the win, hence not opt-in. The 240px Pi Zero (short dim <= 240)
    //    FILLS the display. cfg["fill_landscape"] overrides the default in either
    //    direction; cfg["square"] sets an explicit rect.
    int32_t short_dim = screen_w < screen_h ? screen_w : screen_h;
    bool fill_landscape = cfg.value("fill_landscape", short_dim <= 240);

    int32_t sx = 0, sy = 0, sw = screen_w, sh = screen_h;
    if (!fill_landscape) {
        sx = (screen_w - short_dim) / 2;
        sy = (screen_h - short_dim) / 2;
        sw = short_dim; sh = short_dim;
    }
    if (cfg.contains("square") && cfg["square"].is_object()) {
        const auto &sq = cfg["square"];
        sx = sq.value("x", sx);
        sy = sq.value("y", sy);
        sw = sq.value("w", sw);
        sh = sq.value("h", sh);
    }

    // 3. Placeholder "camera preview" fill (flat mid-gray stands in for live pixels);
    //    the surrounding gutters stay black.
    lv_obj_t *preview = lv_obj_create(scr);
    lv_obj_set_size(preview, sw, sh);
    lv_obj_set_pos(preview, sx, sy);
    lv_obj_set_style_radius(preview, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(preview, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(preview, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(preview, lv_color_hex(0x3a3a3a), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(preview, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(preview, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

    // --- Body ---

    // 4. Initial phase selector (structural, not user-visible text): an unrecognized
    //    string falls through to the PREVIEW default.
    camera_entropy_phase_t phase = CAMERA_ENTROPY_PHASE_PREVIEW;
    if (cfg.contains("phase") && cfg["phase"].is_string()) {
        std::string phase_name = cfg["phase"].get<std::string>();
        if (phase_name == "capturing")    phase = CAMERA_ENTROPY_PHASE_CAPTURING;
        else if (phase_name == "confirm") phase = CAMERA_ENTROPY_PHASE_CONFIRM;
    }

    // 5. Optional host-localized overlay strings. Each is genuinely phase-dependent
    //    (e.g. confirm_instructions is unused in the preview phase), so an absent key
    //    is not an error: it maps to nullptr and the overlay omits that element.
    std::string preview_instructions, confirm_instructions, capturing_text,
                capture_icon, capture_label, accept_label;
    auto read_optional_string = [&](const char *key, std::string &destination) {
        if (cfg.contains(key) && cfg[key].is_string()) destination = cfg[key].get<std::string>();
    };
    read_optional_string("preview_instructions", preview_instructions);
    read_optional_string("confirm_instructions", confirm_instructions);
    read_optional_string("capturing_text",       capturing_text);
    read_optional_string("capture_icon",         capture_icon);
    read_optional_string("capture_label",        capture_label);
    read_optional_string("accept_label",         accept_label);

    // 6. Assemble the declarative overlay spec and build it onto the root. Tooling
    //    renders one static phase, so the handle is freed immediately after build —
    //    destroy() releases only the handle struct while the widgets stay parented on
    //    the tree; on device the host instead retains the handle to push
    //    camera_entropy_overlay_set_phase().
    camera_entropy_overlay_spec_t spec;
    lv_memzero(&spec, sizeof(spec));
    spec.square_x = sx; spec.square_y = sy; spec.square_w = sw; spec.square_h = sh;
    spec.preview_instructions = preview_instructions.empty() ? nullptr : preview_instructions.c_str();
    spec.confirm_instructions = confirm_instructions.empty() ? nullptr : confirm_instructions.c_str();
    spec.capturing_text       = capturing_text.empty()       ? nullptr : capturing_text.c_str();
    spec.capture_icon         = capture_icon.empty()         ? nullptr : capture_icon.c_str();

    // Touch-mode capture-control style selector (structural): unknown -> RING shutter.
    camera_entropy_capture_style_t capture_style = CAMERA_ENTROPY_CAPTURE_RING;
    if (cfg.contains("capture_style") && cfg["capture_style"].is_string()) {
        std::string capture_style_name = cfg["capture_style"].get<std::string>();
        if (capture_style_name == "solid")       capture_style = CAMERA_ENTROPY_CAPTURE_SOLID;
        else if (capture_style_name == "button") capture_style = CAMERA_ENTROPY_CAPTURE_BUTTON;
    }
    spec.capture_style        = capture_style;
    spec.capture_label        = capture_label.empty() ? nullptr : capture_label.c_str();
    spec.accept_label         = accept_label.empty()  ? nullptr : accept_label.c_str();
    spec.phase = phase;

    camera_entropy_overlay_t *overlay = camera_entropy_overlay_create(scr, &spec);

    // On device the host pushes the processed final frame in the CONFIRM phase; in
    // tooling there is no camera, so feed a synthetic frame through the same path so the
    // full-display confirm render (crop-to-fill + luminance stretch) is visible + gateable.
    if (phase == CAMERA_ENTROPY_PHASE_CONFIRM) {
        tooling_push_synthetic_confirm_frame(overlay, screen_w, screen_h);
    }

    camera_entropy_overlay_destroy(overlay);

    // --- Load ---

    // Count this screen's teardown as activity: the entropy live-preview runs with NO user
    // input while the user aims the camera, so LVGL's idle clock goes stale. Without this,
    // when the host dismisses it by loading the next screen, the overlay dispatcher would fire
    // the screensaver over the freshly-rendered successor instead of showing it — the same fix
    // the loading spinner applies (PR #69).
    reset_idle_clock_on_teardown(scr);

    load_screen_and_cleanup_previous(scr);
}
