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
// camera_entropy_overlay_screen — tooling host for the image-entropy overlay
// ---------------------------------------------------------------------------
// Sibling of camera_preview_overlay_screen, for camera_entropy_overlay: synthesizes a
// placeholder square "preview" and renders the entropy overlay onto it in a static,
// JSON-described PHASE, so the screenshot generator + runners exercise the SAME spec
// without a camera. The back affordance/controls follow input_profile_get_mode(), which
// the tools set per resolution (240 = hardware → text; larger = touch → buttons).
//
// JSON config (all optional; on device the strings are host-provided + localized):
//   phase                : "preview" (default) | "capturing" | "confirm"
//   preview_instructions : hardware-mode PREVIEW bottom line (e.g. "< back | click a button")
//   confirm_instructions : hardware-mode CONFIRM bottom line (e.g. "< reshoot | accept >")
//   capturing_text       : accent-color transient line (e.g. "Capturing image…")
//   capture_label        : touch-mode label above the shutter
//   accept_label         : touch-mode Accept button text
//   fill_landscape       : bool — preview geometry (default: short dim <= 240 fills)
//   square               : {x,y,w,h} — explicit preview-square rect
void camera_entropy_overlay_screen(void *ctx_json) {
    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_optional_screen_json_ctx(json_str, cfg);  // ctx optional; every field optional

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr, 0, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    int32_t screen_w = lv_display_get_horizontal_resolution(NULL);
    int32_t screen_h = lv_display_get_vertical_resolution(NULL);

    // Same per-resolution geometry default as camera_preview_overlay_screen: DSI panels
    // (short dim > 240) center-cut a SQUARE with static gutters; the Pi Zero fills.
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

    // Placeholder "camera preview" fill (flat mid-gray); gutters stay black.
    lv_obj_t *preview = lv_obj_create(scr);
    lv_obj_set_size(preview, sw, sh);
    lv_obj_set_pos(preview, sx, sy);
    lv_obj_set_style_radius(preview, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(preview, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(preview, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(preview, lv_color_hex(0x3a3a3a), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(preview, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(preview, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

    camera_entropy_phase_t phase = CAMERA_ENTROPY_PHASE_PREVIEW;
    if (cfg.contains("phase") && cfg["phase"].is_string()) {
        std::string p = cfg["phase"].get<std::string>();
        if (p == "capturing")    phase = CAMERA_ENTROPY_PHASE_CAPTURING;
        else if (p == "confirm") phase = CAMERA_ENTROPY_PHASE_CONFIRM;
    }

    std::string preview_instr, confirm_instr, capturing_text, capture_label, accept_label;
    auto get_str = [&](const char *k, std::string &dst) {
        if (cfg.contains(k) && cfg[k].is_string()) dst = cfg[k].get<std::string>();
    };
    std::string capture_icon;
    get_str("preview_instructions", preview_instr);
    get_str("confirm_instructions", confirm_instr);
    get_str("capturing_text",       capturing_text);
    get_str("capture_icon",         capture_icon);
    get_str("capture_label",        capture_label);
    get_str("accept_label",         accept_label);

    camera_entropy_overlay_spec_t spec;
    lv_memzero(&spec, sizeof(spec));
    spec.square_x = sx; spec.square_y = sy; spec.square_w = sw; spec.square_h = sh;
    spec.preview_instructions = preview_instr.empty()  ? nullptr : preview_instr.c_str();
    spec.confirm_instructions = confirm_instr.empty()  ? nullptr : confirm_instr.c_str();
    spec.capturing_text       = capturing_text.empty() ? nullptr : capturing_text.c_str();
    spec.capture_icon         = capture_icon.empty()   ? nullptr : capture_icon.c_str();
    camera_entropy_capture_style_t capture_style = CAMERA_ENTROPY_CAPTURE_RING;
    if (cfg.contains("capture_style") && cfg["capture_style"].is_string()) {
        std::string cs = cfg["capture_style"].get<std::string>();
        if (cs == "solid")       capture_style = CAMERA_ENTROPY_CAPTURE_SOLID;
        else if (cs == "button") capture_style = CAMERA_ENTROPY_CAPTURE_BUTTON;
    }
    spec.capture_style        = capture_style;
    spec.capture_label        = capture_label.empty()  ? nullptr : capture_label.c_str();
    spec.accept_label         = accept_label.empty()   ? nullptr : accept_label.c_str();
    spec.phase = phase;

    // Tooling renders one static state, so free the handle right away — destroy()
    // releases only the handle struct; the widgets stay in the tree.
    camera_entropy_overlay_t *overlay = camera_entropy_overlay_create(scr, &spec);
    camera_entropy_overlay_destroy(overlay);

    load_screen_and_cleanup_previous(scr);
}
