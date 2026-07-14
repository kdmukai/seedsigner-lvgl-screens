// camera_preview_pillarboxed_screen
//
// Python provenance: no direct screen class — desktop-tooling host for the
// camera_preview_pillarboxed module (the native-portrait-mounted-landscape sibling of
// camera_preview_overlay). On device the module's chrome sits in the pillar strips beside
// a LIVE camera square the camera adapter direct-blits, and the host pushes state through
// set_scanning()/set_progress(). This wrapper exercises the SAME module WITHOUT a camera:
// it synthesizes a placeholder square "preview" and renders the chrome from a static,
// JSON-described state.
//
// Orientation: the module authors in PORTRAIT (the panel's native frame). The tools
// simulate the physical landscape mount via the scenario's tool-only "render_as_landscape"
// flag (render at swapped/portrait dims, present the capture rotated to landscape) — that
// belongs to the tool, not here; this file just builds the portrait layout for whatever
// (portrait) resolution the tool set. So the placeholder square is the display's SHORT
// dimension, centered, leaving the top/bottom pillar strips the chrome occupies.
//
// Chrome-free tier: no top-nav scaffold; a bare black root holds the placeholder square
// and the module's chrome. Lifecycle: Tier 1 (stateless) — the module handle is created
// and immediately destroyed (destroy frees only the handle; the widgets, incl. the
// pre-rotated percent image and its buffers, stay in the tree and free themselves on
// parent teardown).
//
// cfg — all keys optional (parse_optional_screen_json_ctx: NULL/empty ctx -> defaults):
//   scanning      (bool, default true)   show the status bar + dot (this screen IS the
//            scan chrome, so it defaults on, unlike the landscape overlay).
//   progress      (int 0..100, default 0)  animated-QR decode percent.
//   frame_status  (int 0..3, default 0)  most-recent-frame status dot
//            (0 none / 1 added=green / 2 repeated=gray / 3 miss=hidden).
//   square        (object {x,y,w,h}, optional)  explicit preview-square rect override.

#include "screen_scaffold.h"             // parse_optional_screen_json_ctx, load_screen_and_cleanup_previous
#include "seedsigner.h"                  // camera_preview_pillarboxed_screen entry-point declaration
#include "camera_preview_pillarboxed.h"  // spec + create/destroy + camera_overlay_frame_status_t

#include "lvgl.h"                         // bare-root + placeholder build, lv_display_get_*_resolution, lv_memzero

#include <nlohmann/json.hpp>

using json = nlohmann::json;


void camera_preview_pillarboxed_screen(void *ctx_json) {
    // --- Config ---
    const char *json_str = (const char *)ctx_json;
    json cfg;
    parse_optional_screen_json_ctx(json_str, cfg);

    // --- Bare-root build (chrome-free, solid black) ---
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr, 0, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // --- Preview geometry: centered square = short display dim (leaves the pillar strips) ---
    int32_t screen_w = lv_display_get_horizontal_resolution(NULL);
    int32_t screen_h = lv_display_get_vertical_resolution(NULL);
    int32_t short_dim = screen_w < screen_h ? screen_w : screen_h;

    int32_t sx = (screen_w - short_dim) / 2;
    int32_t sy = (screen_h - short_dim) / 2;
    int32_t sw = short_dim, sh = short_dim;

    if (cfg.contains("square") && cfg["square"].is_object()) {
        const auto &sq = cfg["square"];
        sx = sq.value("x", sx);
        sy = sq.value("y", sy);
        sw = sq.value("w", sw);
        sh = sq.value("h", sh);
    }

    // Placeholder "camera preview" fill (flat mid-gray) so the chrome stands alone; the
    // pillar strips stay black (the module paints them).
    lv_obj_t *preview = lv_obj_create(scr);
    lv_obj_set_size(preview, sw, sh);
    lv_obj_set_pos(preview, sx, sy);
    lv_obj_set_style_radius(preview, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(preview, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(preview, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(preview, lv_color_hex(0x3a3a3a), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(preview, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(preview, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

    // --- Chrome spec from the static cfg state ---
    camera_preview_pillarboxed_spec_t spec;
    lv_memzero(&spec, sizeof(spec));
    spec.square_x = sx; spec.square_y = sy; spec.square_w = sw; spec.square_h = sh;
    spec.scanning_active  = cfg.value("scanning", true);
    spec.progress_percent = cfg.value("progress", 0);
    spec.frame_status     = (camera_overlay_frame_status_t)cfg.value("frame_status", 0);

    camera_preview_pillarboxed_t *chrome = camera_preview_pillarboxed_create(scr, &spec);
    camera_preview_pillarboxed_destroy(chrome);

    // --- Load ---
    load_screen_and_cleanup_previous(scr);
}
