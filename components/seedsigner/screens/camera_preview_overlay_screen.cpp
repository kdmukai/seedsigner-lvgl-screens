// camera_preview_overlay_screen
//
// Python provenance: no direct screen class — desktop-tooling host for the
// camera_preview_overlay module, whose overlay UI ports ScanScreen (scan_screens.py).
//
// On device the overlay (camera_preview_overlay.{h,cpp}) is composited over LIVE
// camera pixels the camera adapter owns, and the host pushes state through
// set_scanning()/set_progress(). This wrapper lets the screenshot generator and the
// runners exercise the SAME overlay spec WITHOUT a camera: it synthesizes a
// placeholder square "preview" background and renders the overlay onto it from a
// static, JSON-described state. Every real affordance (status bar, instruction text,
// gutter back button) belongs to the overlay module — this file owns only the
// synthetic background and the one-shot spec, and it neither navigates nor pushes
// live updates.
//
// Chrome-free tier: no top-nav scaffold — a bare root screen holds the placeholder
// preview panel and the module's overlay; the mandatory load_screen_and_cleanup_previous
// tail still applies. The sibling camera_entropy_overlay_screen wraps the entropy
// overlay the same way, and the two share the synthetic-preview-background builder
// verbatim (kept byte-exact pending the camera_overlay_common extraction).
//
// Lifecycle: Tier 1 (stateless) — no statics, no timers, no heap ctx. The overlay
// handle is created and immediately destroyed; destroy() frees only the handle struct,
// leaving the widgets parented on the screen tree. The screen opts out of the screensaver
// (apply_screensaver_policy, default_allow=false), which both keeps the saver off the live
// preview and — via the flag-driven teardown reset in load_screen_and_cleanup_previous —
// gives the successor screen a full screensaver window.
//
// The back affordance is chosen by the overlay module from input_profile_get_mode(),
// which the tools set per resolution (240 = hardware -> on-preview instruction text;
// larger = touch -> gutter back button).
//
// cfg — the ctx itself is optional (parse_optional_screen_json_ctx: NULL/empty ctx
// yields an empty config; any present ctx gets the same strict validation as every
// other screen). All keys are optional:
//   instructions_text  (string, optional)   full hardware-mode bottom line, already
//            composed + localized by the host (e.g. "< back  |  Scan a QR code");
//            NULL/empty -> no instruction text. The overlay ignores it in touch mode.
//   scanning           (bool,   default false)  show the status bar instead of the
//            back affordance.
//   progress           (int 0..100, default 0)  animated-QR decode percent; any value
//            > 0 forces scanning on.
//   frame_status       (int 0..3, default 0)  most-recent-frame decode status dot
//            (0 none / 1 added / 2 repeated / 3 miss).
//   total_segments     (int, default 0)  >0 switches the progress track to a segmented
//            per-frame view (BBQR/Specter indexed cycle); the percent is derived from the
//            decoded mask. 0 keeps the continuous fill (UR/fountain/unknown-total).
//   decoded_count      (int, default 0)  segmented only: light the first K frames
//            (compact authoring for dense/degenerate cases).
//   decoded_range      (object {start,count}, optional)  segmented only: light a
//            contiguous run at an offset, so the chunk doesn't hug the left edge.
//   decoded_indices    (int[], optional)  segmented only: light these explicit 0-based
//            frames; unions with decoded_count. Out-of-order aware (e.g. [3] = middle-first).
//   just_decoded_index (int, default -1)  0-based currently-read piece; drawn as the
//            highlighted "current" cell (e.g. the piece a repeated frame is re-reading).
//   fill_landscape     (bool, default: short display dim <= 240)  preview geometry.
//            Higher-res DSI panels (short dim > 240) default false = a center-cut
//            SQUARE with static side gutters; the Pi Zero (<= 240) defaults true =
//            fill the display. Set either way to override the per-resolution default.
//   square             (object {x,y,w,h}, optional)  explicit preview-square rect;
//            overrides the fill_landscape-derived rect in whole or per-field.

#include "screen_scaffold.h"          // parse_optional_screen_json_ctx, load_screen_and_cleanup_previous
#include "seedsigner.h"               // camera_preview_overlay_screen entry-point declaration
#include "camera_preview_overlay.h"   // camera_preview_overlay_spec_t, camera_preview_overlay_create/_destroy, camera_overlay_frame_status_t

#include "lvgl.h"                      // bare-root + placeholder lv_obj build, per-object style setters, lv_display_get_*_resolution, lv_memzero

#include <nlohmann/json.hpp>          // json (optional cfg read)

#include <string>                     // std::string (instructions_text)
#include <vector>                     // std::vector (decoded-frame mask)

using json = nlohmann::json;


void camera_preview_overlay_screen(void *ctx_json) {
    // --- Config ---

    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_optional_screen_json_ctx(json_str, cfg);  // boot/overlay tier: NULL/empty ctx -> empty config

    // --- Bare-root build ---

    // Bare root screen (chrome-free: no top-nav scaffold), solid black background,
    // nothing scrolls. [slated for extraction: create_bare_root_screen() chrome-free
    // helper, extraction ledger #11]
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr, 0, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // --- Body ---

    // 1. Preview geometry + synthetic placeholder background. This block is duplicated
    //    verbatim with camera_entropy_overlay_screen and kept byte-exact for a clean
    //    future lift. [slated for extraction: camera_overlay_common synthetic-preview
    //    builder, consolidation ledger §12]
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

    // 2. Overlay spec assembled from the static cfg state. instructions_text is optional
    //    content (NULL/empty -> no line), matching the module's documented contract; the
    //    remaining fields are structural (geometry + status), read with cfg.value()
    //    defaults. A non-zero progress implies the scanning bar (module parity).
    std::string instructions_text;
    if (cfg.contains("instructions_text") && cfg["instructions_text"].is_string()) {
        instructions_text = cfg["instructions_text"].get<std::string>();
    }

    camera_preview_overlay_spec_t spec;
    lv_memzero(&spec, sizeof(spec));
    spec.instructions_text  = instructions_text.empty() ? nullptr : instructions_text.c_str();
    spec.square_x = sx; spec.square_y = sy; spec.square_w = sw; spec.square_h = sh;
    spec.scanning_active    = cfg.value("scanning", false);
    spec.progress_percent   = cfg.value("progress", 0);
    spec.frame_status       = (camera_overlay_frame_status_t)cfg.value("frame_status", 0);
    if (spec.progress_percent > 0) spec.scanning_active = true;

    // 3. Segmented-progress state (BBQR/Specter indexed cycles). total_segments > 0
    //    switches the track to per-frame cells; the decoded mask is authored two ways
    //    (unioned): decoded_count lights the first K frames (compact for dense/degenerate
    //    demos), and decoded_indices lights explicit 0-based frames (supports out-of-order,
    //    e.g. a middle-first scan = decoded_indices:[mid]). The mask outlives create().
    std::vector<uint8_t> decoded_mask;
    int total_segments = cfg.value("total_segments", 0);
    if (total_segments > 0) {
        decoded_mask.assign((size_t)total_segments, 0);

        int decoded_count = cfg.value("decoded_count", 0);
        for (int i = 0; i < decoded_count && i < total_segments; ++i) decoded_mask[i] = 1;

        // A contiguous run [start, start+count) at an arbitrary offset — for demos where
        // the lit chunk should clearly NOT hug the left edge (so it doesn't read as a
        // left-to-right meter). Unions with the others.
        if (cfg.contains("decoded_range") && cfg["decoded_range"].is_object()) {
            const auto &r = cfg["decoded_range"];
            int start = r.value("start", 0);
            int count = r.value("count", 0);
            for (int i = start; i < start + count && i < total_segments; ++i) {
                if (i >= 0) decoded_mask[(size_t)i] = 1;
            }
        }

        if (cfg.contains("decoded_indices") && cfg["decoded_indices"].is_array()) {
            for (const auto &v : cfg["decoded_indices"]) {
                int idx = v.is_number_integer() ? v.get<int>() : -1;
                if (idx >= 0 && idx < total_segments) decoded_mask[(size_t)idx] = 1;
            }
        }
        spec.scanning_active = true;  // segments imply scanning
    }
    spec.total_segments     = total_segments;
    spec.decoded            = decoded_mask.empty() ? nullptr : decoded_mask.data();
    spec.just_decoded_index = cfg.value("just_decoded_index", -1);

    // Build onto the screen (gutter button in the gutter; in-square elements over the
    // preview). Tooling renders one static state, so we free the update handle right
    // away — destroy() releases only the handle struct; the widgets stay in the tree.
    // On device the host instead retains the handle to push set_scanning/set_progress.
    camera_preview_overlay_t *overlay = camera_preview_overlay_create(scr, &spec);
    camera_preview_overlay_destroy(overlay);

    // --- Load ---

    // Screensaver policy (default_allow=false): a scan runs with NO user input while the user
    // lines up the QR, so the saver must never cover it. The flag suppresses the saver while
    // this screen shows; load_screen_and_cleanup_previous wires the teardown idle-clock reset
    // off the flag, so the successor screen (e.g. Select Signer) isn't covered by the stale
    // idle clock the instant the host dismisses the scan.
    apply_screensaver_policy(scr, cfg, /*default_allow=*/false);

    load_screen_and_cleanup_previous(scr);
}
