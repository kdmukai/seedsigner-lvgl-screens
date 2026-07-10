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
// camera_preview_overlay_screen — tooling host for the camera live-preview overlay
// ---------------------------------------------------------------------------
// On device the overlay (camera_preview_overlay.{h,cpp}) is composited over LIVE
// camera pixels the camera adapter owns; the host pushes state via set_scanning()/
// set_progress(). This entry point lets the screenshot generator + runners exercise
// the SAME spec without a camera by synthesizing a placeholder square "preview"
// background and rendering the overlay onto it with a static, JSON-described state.
// The back affordance follows input_profile_get_mode(), which the tools set per
// resolution (240 = hardware → instruction text; larger = touch → gutter button).
//
// JSON config (all optional):
//   instructions_text : full hardware-mode bottom line (host composes "< back | ...")
//   scanning          : bool — show the status bar instead of the back affordance
//   progress          : 0..100 — animated-QR percent (implies scanning)
//   frame_status      : 0 none / 1 added / 2 repeated / 3 miss — status dot
//   fill_landscape    : bool — preview geometry. Default depends on resolution: the
//                       higher-res DSI panels (short dim > 240) default to a center-cut
//                       SQUARE with static side gutters (false); the Pi Zero (<= 240)
//                       fills the display (true). Set true on a DSI panel to opt into
//                       full landscape width; set false on the Pi Zero to force a square.
//   square            : {x,y,w,h} — explicit preview-square rect (overrides the above)
void camera_preview_overlay_screen(void *ctx_json) {
    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_optional_screen_json_ctx(json_str, cfg);  // validates shape; ctx + every field optional

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr, 0, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    int32_t screen_w = lv_display_get_horizontal_resolution(NULL);
    int32_t screen_h = lv_display_get_vertical_resolution(NULL);

    // Preview geometry, defaulting per resolution:
    //   - Higher-resolution DSI panels (short dimension > 240 — the 480x320 / 800x480
    //     touch displays) DEFAULT to a landscape center-cut SQUARE, leaving static side
    //     gutters along the long axis. Those panels have per-frame update limits along
    //     their long axis, so minimizing the redrawn span there (camera writes only the
    //     square; the gutters never refresh) is the win — hence not opt-in.
    //   - The Pi Zero (<= 240 short dimension, incl. 320x240) FILLS the display, matching
    //     Python ScanScreen (render_rect defaults to the whole canvas; 240x240 square ==
    //     full anyway).
    // cfg["fill_landscape"] overrides the per-resolution default in EITHER direction
    // (true on a DSI panel opts into full landscape width; false on the Pi Zero forces
    // the square); cfg["square"] sets an explicit rect.
    int32_t short_dim = screen_w < screen_h ? screen_w : screen_h;
    bool fill_landscape = cfg.value("fill_landscape", short_dim <= 240);

    int32_t sx = 0, sy = 0, sw = screen_w, sh = screen_h;
    if (!fill_landscape) {
        // Center-cut square of the short dimension; the long axis keeps static gutters.
        sx = (screen_w - short_dim) / 2;
        sy = (screen_h - short_dim) / 2;
        sw = short_dim; sh = short_dim;
    }

    // Explicit rect overrides either default.
    if (cfg.contains("square") && cfg["square"].is_object()) {
        const auto &sq = cfg["square"];
        sx = sq.value("x", sx);
        sy = sq.value("y", sy);
        sw = sq.value("w", sw);
        sh = sq.value("h", sh);
    }

    // Placeholder "camera preview" fill so the overlay stands alone in tooling. A flat
    // mid-gray stands in for live pixels; the surrounding gutters stay black.
    lv_obj_t *preview = lv_obj_create(scr);
    lv_obj_set_size(preview, sw, sh);
    lv_obj_set_pos(preview, sx, sy);
    lv_obj_set_style_radius(preview, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(preview, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(preview, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(preview, lv_color_hex(0x3a3a3a), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(preview, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(preview, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

    std::string instr;
    if (cfg.contains("instructions_text") && cfg["instructions_text"].is_string()) {
        instr = cfg["instructions_text"].get<std::string>();
    }

    camera_preview_overlay_spec_t spec;
    lv_memzero(&spec, sizeof(spec));
    spec.instructions_text  = instr.empty() ? nullptr : instr.c_str();
    spec.square_x = sx; spec.square_y = sy; spec.square_w = sw; spec.square_h = sh;
    spec.scanning_active    = cfg.value("scanning", false);
    spec.progress_percent   = cfg.value("progress", 0);
    spec.frame_status       = (camera_overlay_frame_status_t)cfg.value("frame_status", 0);
    if (spec.progress_percent > 0) spec.scanning_active = true;

    // Build onto the screen (gutter button in the gutter; in-square elements over the
    // preview). Tooling renders one static state, so we free the update handle right
    // away — destroy() releases only the handle struct; the widgets stay in the tree.
    // On device the host instead retains the handle to push set_scanning/set_progress.
    camera_preview_overlay_t *overlay = camera_preview_overlay_create(scr, &spec);
    camera_preview_overlay_destroy(overlay);

    load_screen_and_cleanup_previous(scr);
}
