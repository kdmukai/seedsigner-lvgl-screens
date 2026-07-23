// io_test_screen
//
// Python provenance: IOTestScreen (settings_screens.py)
//
// The hardware self-test: a D-pad pictogram (joystick up/down/left/right + center
// click) on the left, KEY1/KEY2/KEY3 controls on the right, over a live camera
// square, with a "Capturing image…" band that appears while a still is grabbed.
// The screen returns nothing through the navigation callback; the host reacts to
// the forwarded aux keys (KEY3 = exit) and tears the screen down itself.
//
// Input is SELF-OWNED (the point of a self-test): unlike the passive live screens
// (qr_display, seed_address_verification), this screen READS THE KEYPAD ITSELF and
// lights up whichever physical control the user actuates — exactly what Python's
// _run does with hw_inputs.wait_for(). It installs its own LVGL group + keypad
// sink (no bind_screen_navigation) and, on each key, flashes the matching control:
//   LV_KEY_UP/DOWN/LEFT/RIGHT -> the D-pad arrows; LV_KEY_ENTER -> the center
//   click; KEY1/2/3 (per nav_aux_key_index, navigation.h) -> the right-side buttons.
// KEY1/2/3 are ALSO forwarded to the host (seedsigner_lvgl_on_aux_key) so it can
// grab a frame / clear / exit. There is NO back button (Python: show_back_button =
// False). Entry forces the GLOBAL input profile to INPUT_MODE_HARDWARE (not
// restored on teardown; the app gates this screen off on touch-only builds).
//
// Camera: this screen OWNS a host-blitted RGB565 pixel plane (the owned-plane model
// of seedsigner-raspi-lvgl's camera_preview.cpp), NOT a board adapter behind it. The
// plane is a CENTERED SQUARE lv_image whose side = the display's SHORT dimension, so it
// fills the full display height and is horizontally centered; its top strip tucks UNDER
// the (opaque) top_nav (a self-test only needs to prove the camera works, so cropping
// the top few rows behind the nav is fine). It is z-ordered UNDER the chrome and starts
// HIDDEN. A centered square is deliberate: the ESP dev boards mount the camera rotated
// 90deg from our landscape screen, so the sensor yields a square/portrait frame in
// landscape — the host CENTER-CROPS it to a square and fills the plane edge-to-edge (no
// stretch, no letterbox). On the square Pi display the square == the whole screen, i.e.
// full-bleed like Python's paste; on wider (touch) displays the square is PILLARBOXED —
// the left/right gutters fall outside the plane and show the standard screen background
// (the body is made transparent so the root BACKGROUND_COLOR reads through the gutters).
// Python's IOTestScreen does a single-frame grab on KEY1 (no live feed); we mirror that
// flow:
//   1. KEY1 -> host: io_test_set_capture_state(CAPTURING) shows the "Capturing…" band.
//   2. host grabs a frame, center-crops to a square -> io_test_blit_camera(rgb565)
//      memcpy's it into the plane's stable buffer + unhides the plane;
//      io_test_get_camera_plane_dims() tells the host the exact square side to produce.
//   3. host: io_test_set_capture_state(CAPTURED) hides the band + sets KEY2 = "Clear".
//   4. KEY2 -> host: io_test_set_capture_state(IDLE) re-hides the plane (Python "Clear").
// The plane stays hidden until the host blits, so the screen is inert (identical to the
// pre-plane chrome) on any platform whose host hasn't wired the blit yet.
//
// The KEY1/2/3 buttons are ACCENT-outlined rounded-rect boxes (io_test_make_key_box,
// same accent border + pressed-invert as the D-pad boxes) sized to Python's own
// key_button formulas (text_width("Clear") + 2*COMPONENT_PADDING + EDGE_PADDING wide;
// icon_height + 1.5*COMPONENT_PADDING tall) and placed at Python's canvas coordinates
// (screen_x = canvas_width - key_button_width + EDGE_PADDING, overshooting the right
// edge by EDGE_PADDING) — that placement IS the physical-key alignment, so they're
// parented to the screen root at canvas coords (not the shared kb_side_panel strip).
//
// Layout deviation from Python: the capturing band's height derives from the
// title-font line height + 2*COMPONENT_PADDING (Python sizes its message box
// ICON_LARGE_BUTTON_SIZE + 2*COMPONENT_PADDING tall); the text is centered in the
// band either way.
//
// Lifecycle (stateful, Tier 2): one heap ctx registered as g_io_test for the
// host-push capture API; io_test_cleanup_cb on the screen root's LV_EVENT_DELETE
// deletes the owned input group, clears g_io_test under an identity guard, and
// frees the ctx.
//
// cfg:
//   top_nav.title    (string, required)   localized screen title (Python:
//            _("I/O Test")); read by the scaffold.
//   capturing_text   (string, required)   localized transient band text
//            (Python: _("Capturing image...")).
//   clear_label      (string, required)   localized KEY2 label shown once a
//            still exists (Python: _("Clear")).
//   exit_label       (string, required)   localized KEY3 label (Python: _("Exit")).
//   camera_glyph     (string, default FontAwesomeIconConstants::CAMERA)  KEY1
//            icon glyph — an icon-font codepoint, not localized text (structural).
//   top_nav.show_back_button   forced false (Python: show_back_button = False);
//            a host-supplied value is ignored.
//   top_nav.show_power_button  forced false (Python BaseTopNavScreen default
//            False); a host-supplied value is ignored.
//   allow_screensaver          (bool, default true)  per-screen screensaver policy
//            (normalized by parse_screen_json_ctx, stamped by the scaffold).

#include "screen_scaffold.h"   // parse_screen_json_ctx, create_top_nav_screen_scaffold, load_screen_and_cleanup_previous
#include "seedsigner.h"        // io_test_capture_state_t, io_test_set_capture_state decl, seedsigner_lvgl_on_aux_key, seedsigner_lvgl_is_static_render
#include "gui_constants.h"     // BUTTON_HEIGHT, COMPONENT_PADDING, TOP_NAV_HEIGHT, button/accent colors, BUTTON_FONT, TOP_NAV_TITLE_FONT, ICON_FONT__SEEDSIGNER, icon glyph constants
#include "input_profile.h"     // input_profile_set_mode, INPUT_MODE_HARDWARE (force hardware input)
#include "keyboard_core.h"     // kb_flash_side_button (momentary press-flash on a non-clickable box)
#include "navigation.h"        // nav_aux_key_index (shared KEY1/2/3 recognizer)
#include "screen_helpers.h"    // ensure_top_nav_structure, require_top_nav_title

#include "lvgl.h"              // lv_obj / lv_label / lv_group / lv_indev + per-object style setters

#include <nlohmann/json.hpp>   // json (cfg reads + structural-default writes)

#include <cstring>             // memcpy (host frame -> pixel-plane buffer)
#include <stdexcept>           // std::runtime_error (required-field validation)
#include <string>              // std::string
#include <vector>             // std::vector (owned RGB565 pixel-plane buffer)

using json = nlohmann::json;

namespace {

// The flashable controls, indexed into io_test_ctx::controls.
enum {
    IO_TEST_CONTROL_UP = 0, IO_TEST_CONTROL_DOWN, IO_TEST_CONTROL_LEFT,
    IO_TEST_CONTROL_RIGHT,  IO_TEST_CONTROL_CLICK,
    IO_TEST_CONTROL_KEY1,   IO_TEST_CONTROL_KEY2,  IO_TEST_CONTROL_KEY3,
    IO_TEST_CONTROL_COUNT
};

// Live-screen state, registered as g_io_test for the host capture API; freed on
// the screen's LV_EVENT_DELETE (identity-guarded, like qr_display's ctx).
// Allocated with `new` / released with `delete` in io_test_cleanup_cb — the ctx
// carries a C++ std::string member (lv_malloc would skip its ctor/dtor).
struct io_test_ctx {
    lv_obj_t   *controls[IO_TEST_CONTROL_COUNT] = {};  // the widget to flash per control
    lv_obj_t   *capturing_band = nullptr;
    lv_obj_t   *key2_label     = nullptr;  // toggles "" <-> clear_label
    lv_group_t *group          = nullptr;  // owned; deleted on cleanup
    std::string clear_label;

    // Host-owned camera pixel-plane (mirrors camera_preview.cpp's owned sink): a
    // centered-square RGB565 lv_image the host blits a captured still into. `camera_buf`
    // backs `camera_dsc.data` and is sized ONCE (never resized) so the pointer LVGL
    // caches stays stable for the session. Starts HIDDEN; shown on blit, re-hidden on
    // IO_TEST_CAPTURE_IDLE ("Clear").
    lv_obj_t            *camera_plane = nullptr;
    lv_image_dsc_t       camera_dsc   = {};
    std::vector<uint8_t> camera_buf;
};

io_test_ctx *g_io_test = nullptr;   // the single active io_test_screen, or nullptr

// D-pad control: an accent-outlined rounded rect that inverts (accent fill, dark
// content) while PRESSED, so kb_flash_side_button drives its press-flash too.
lv_obj_t *io_test_make_dpad_box(lv_obj_t *parent, int32_t x, int32_t y,
                                int32_t width, int32_t height) {
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_flag(box, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, width, height);
    lv_obj_set_style_radius(box, BUTTON_RADIUS, LV_PART_MAIN);
    lv_obj_set_style_border_color(box, lv_color_hex(ACCENT_COLOR), LV_PART_MAIN);
    lv_obj_set_style_border_width(box, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(box, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(box, lv_color_hex(BUTTON_BACKGROUND_COLOR), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(box, lv_color_hex(ACCENT_COLOR), LV_PART_MAIN | LV_STATE_PRESSED);
    // Text color set on the box (not the label) so the PRESSED selector applies and the
    // glyph label inherits it — the same trick kb_side_button uses.
    lv_obj_set_style_text_color(box, lv_color_hex(BUTTON_FONT_COLOR), LV_PART_MAIN);
    lv_obj_set_style_text_color(box, lv_color_hex(BUTTON_SELECTED_FONT_COLOR), LV_PART_MAIN | LV_STATE_PRESSED);
    return box;
}

lv_obj_t *io_test_make_dpad_arrow(lv_obj_t *parent, int32_t x, int32_t y,
                                  int32_t width, int32_t height, const char *glyph) {
    lv_obj_t *box = io_test_make_dpad_box(parent, x, y, width, height);
    lv_obj_t *glyph_label = lv_label_create(box);
    lv_label_set_text(glyph_label, glyph);
    lv_obj_set_style_text_font(glyph_label, &ICON_FONT__SEEDSIGNER, LV_PART_MAIN);
    lv_obj_center(glyph_label);
    return box;
}

lv_obj_t *io_test_make_dpad_center(lv_obj_t *parent, int32_t x, int32_t y,
                                   int32_t width, int32_t height) {
    lv_obj_t *box = io_test_make_dpad_box(parent, x, y, width, height);
    // The joystick-click indicator: a SOLID disc (Python uses FontAwesome CIRCLE, a filled
    // dot — not a ring). It's drawn as an lv_obj because CIRCLE isn't baked into the LVGL
    // icon font. Filled in the button font color, no border; matches Python's IconButton dot.
    int32_t dot_diameter = ICON_INLINE_FONT_SIZE - 6;
    if (dot_diameter < 6) dot_diameter = 6;
    lv_obj_t *dot = lv_obj_create(box);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(dot, dot_diameter, dot_diameter);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dot, lv_color_hex(BUTTON_FONT_COLOR), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(dot, 0, LV_PART_MAIN);
    lv_obj_center(dot);
    return box;
}

// A KEY1/KEY2/KEY3 control: the same accent-outlined, pressed-inverting box as the D-pad
// (io_test_make_dpad_box sets the ACCENT border + the MAIN/PRESSED text colors the label
// inherits), with one centered label — an icon glyph for KEY1, text for KEY2/KEY3. The
// caller sizes/positions it per Python's key_button formulas. `out_label` captures the
// label so KEY2 can toggle "" <-> clear_label later.
lv_obj_t *io_test_make_key_box(lv_obj_t *parent, int32_t x, int32_t y,
                               int32_t width, int32_t height, const char *text,
                               const lv_font_t *font, lv_obj_t **out_label) {
    lv_obj_t *box = io_test_make_dpad_box(parent, x, y, width, height);
    lv_obj_t *label = lv_label_create(box);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_center(label);
    if (out_label) *out_label = label;
    return box;
}

// Map a keypad code to a control index; -1 if none.
int io_test_control_for_key(uint32_t key) {
    switch (key) {
        case LV_KEY_UP:    return IO_TEST_CONTROL_UP;
        case LV_KEY_DOWN:  return IO_TEST_CONTROL_DOWN;
        case LV_KEY_LEFT:  return IO_TEST_CONTROL_LEFT;
        case LV_KEY_RIGHT: return IO_TEST_CONTROL_RIGHT;
        case LV_KEY_ENTER: return IO_TEST_CONTROL_CLICK;
        default: break;
    }

    // KEY1/KEY2/KEY3 via the shared recognizer (navigation.h): 1/2/3 map onto the
    // contiguous IO_TEST_CONTROL_KEY1..KEY3 control slots.
    int aux_index = nav_aux_key_index(key);
    if (aux_index != 0) return IO_TEST_CONTROL_KEY1 + (aux_index - 1);

    return -1;
}

void io_test_key_handler(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_KEY) return;
    io_test_ctx *ctx = (io_test_ctx *)lv_event_get_user_data(e);
    uint32_t key = lv_event_get_key(e);

    int control_index = io_test_control_for_key(key);
    if (control_index < 0) return;

    kb_flash_side_button(ctx->controls[control_index]);   // brief accent flash (matches Python)

    // Forward the aux keys so the host can grab a frame (KEY1) / clear (KEY2) / exit (KEY3).
    if      (control_index == IO_TEST_CONTROL_KEY1) seedsigner_lvgl_on_aux_key("KEY1");
    else if (control_index == IO_TEST_CONTROL_KEY2) seedsigner_lvgl_on_aux_key("KEY2");
    else if (control_index == IO_TEST_CONTROL_KEY3) seedsigner_lvgl_on_aux_key("KEY3");
}

// LV_EVENT_DELETE teardown on the screen root: delete the owned input group
// (LVGL's lv_group_delete clears any indev pointing at it), unregister the
// host-push target, then free the ctx.
void io_test_cleanup_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;
    io_test_ctx *ctx = (io_test_ctx *)lv_event_get_user_data(e);
    if (ctx->group) lv_group_del(ctx->group);       // clears the indev's group pointer (LVGL)
    if (g_io_test == ctx) g_io_test = nullptr;      // don't null a newer screen built before teardown
    delete ctx;
}

}  // namespace


// ---------------------------------------------------------------------------
// Host-push API (see seedsigner.h)
// ---------------------------------------------------------------------------

extern "C" void io_test_set_capture_state(io_test_capture_state_t state) {
    if (!g_io_test) return;
    if (g_io_test->capturing_band) {
        if (state == IO_TEST_CAPTURE_CAPTURING) lv_obj_remove_flag(g_io_test->capturing_band, LV_OBJ_FLAG_HIDDEN);
        else                                    lv_obj_add_flag(g_io_test->capturing_band, LV_OBJ_FLAG_HIDDEN);
    }
    if (g_io_test->key2_label) {
        lv_label_set_text(g_io_test->key2_label,
                          state == IO_TEST_CAPTURE_CAPTURED ? g_io_test->clear_label.c_str() : "");
    }
    // IDLE is the "Clear" (KEY2) / initial state: blank the captured still by re-hiding the
    // pixel plane. CAPTURING/CAPTURED leave it as-is (the host blits the frame in between).
    if (g_io_test->camera_plane && state == IO_TEST_CAPTURE_IDLE) {
        lv_obj_add_flag(g_io_test->camera_plane, LV_OBJ_FLAG_HIDDEN);
    }
}

// Report the exact pixel-plane dimensions the host must produce: a square whose side is
// the display's short dimension. The host center-crops its (rotated/non-square) camera
// frame to this square and blits it via io_test_blit_camera. Zero when no screen is live.
extern "C" void io_test_get_camera_plane_dims(int *width, int *height) {
    int side = 0;
    if (g_io_test && g_io_test->camera_plane) {
        side = (int)g_io_test->camera_dsc.header.w;   // square: w == h
    }
    if (width)  *width  = side;
    if (height) *height = side;
}

// Blit a host-captured RGB565 still into the pixel plane and reveal it. `rgb565` must be
// exactly side*side*2 bytes (see io_test_get_camera_plane_dims); a mismatch is a silent
// no-op, as is the no-active-screen case. Copies into the plane's stable buffer and
// invalidates so the next pump repaints. Mirrors camera_preview_blit_rgb565.
extern "C" void io_test_blit_camera(const uint8_t *rgb565, size_t nbytes) {
    if (!g_io_test || !g_io_test->camera_plane || !rgb565) return;
    if (g_io_test->camera_buf.empty() || nbytes != g_io_test->camera_buf.size()) return;
    memcpy(g_io_test->camera_buf.data(), rgb565, nbytes);
    lv_obj_remove_flag(g_io_test->camera_plane, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(g_io_test->camera_plane);
}


void io_test_screen(void *ctx_json) {
    // --- Config ---

    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    // Required fields: the band text + KEY2/KEY3 labels are user-visible CONTENT,
    // which always arrives localized from the host view layer (a string literal
    // baked here would be English-only by construction). One throw per field,
    // before the scaffold exists, so no throw path can leak LVGL objects.
    if (!cfg.contains("capturing_text") || !cfg["capturing_text"].is_string()) {
        throw std::runtime_error("io_test_screen: capturing_text is required and must be a string");
    }
    if (!cfg.contains("clear_label") || !cfg["clear_label"].is_string()) {
        throw std::runtime_error("io_test_screen: clear_label is required and must be a string");
    }
    if (!cfg.contains("exit_label") || !cfg["exit_label"].is_string()) {
        throw std::runtime_error("io_test_screen: exit_label is required and must be a string");
    }
    std::string capturing_text = cfg["capturing_text"].get<std::string>();
    std::string clear_label    = cfg["clear_label"].get<std::string>();
    std::string exit_label     = cfg["exit_label"].get<std::string>();

    // Structural default: the KEY1 icon glyph is an icon-font codepoint, not
    // localized text (Python: FontAwesomeIconConstants.CAMERA).
    std::string camera_glyph = cfg.value("camera_glyph", std::string(FontAwesomeIconConstants::CAMERA));

    // Structural chrome defaults + required localized title, then the forced
    // no-chrome contract: Python shows NO back button (show_back_button = False)
    // and no power button (BaseTopNavScreen default False).
    ensure_top_nav_structure(cfg, /*default_show_back_button=*/false,
                                  /*default_show_power_button=*/false);
    require_top_nav_title(cfg, "io_test_screen");

    cfg["top_nav"]["show_back_button"]  = false;   // forced, not defaulted — Python: show_back_button = False
    cfg["top_nav"]["show_power_button"] = false;   // forced, not defaulted — Python BaseTopNavScreen default False

    // This screen exists to test the physical joystick + keys, so it only makes sense
    // with hardware input. Force hardware mode so the keypad delivers the joystick / KEY
    // events (the runner's keypad drops keys in touch mode). The screen is gated off on
    // touch-only builds by the app, so this never fights a genuine touch device. The
    // prior mode is NOT restored on teardown.
    input_profile_set_mode(INPUT_MODE_HARDWARE);

    // --- Scaffold ---

    // No button_list -> scaffold Mode 1 (body == upper_body). We place the pictogram
    // into `body` at body-local coordinates (as the passphrase side panel does).
    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, /*scrollable=*/false);
    lv_obj_t *body = screen.body;
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    // The KEY1/2/3 boxes are root-parented and deliberately overshoot the right canvas edge
    // by EDGE_PADDING (Python's hardware buttons overshoot canvas_width). Disable scrolling
    // on the screen root so that overshoot simply CLIPS off-screen (Python's behavior) rather
    // than making the root horizontally scrollable — which would draw a bottom scrollbar.
    lv_obj_remove_flag(screen.screen, LV_OBJ_FLAG_SCROLLABLE);

    // Tier-2 heap ctx (new/delete: std::string member). All required cfg was
    // validated above, so no throw path remains past this allocation.
    io_test_ctx *ctx = new io_test_ctx();
    ctx->clear_label = clear_label;

    // Two coordinate spaces, on purpose: the full-bleed band, the KEY panel and the camera
    // plane (all parented to the screen root) use CANVAS / display coordinates, while the
    // D-pad sits inside `body` at body-local coordinates (the body's own left pad supplies
    // EDGE_PADDING, so the LEFT arrow lands at x=0).
    const int32_t display_width  = lv_display_get_horizontal_resolution(NULL);
    const int32_t display_height = lv_display_get_vertical_resolution(NULL);
    const int32_t screen_height  = lv_obj_get_height(screen.screen);

    // --- Camera pixel plane (behind the chrome) ---

    // A centered SQUARE RGB565 image the host blits a captured still into (see the file
    // banner). Side = the display's short dimension, so it fills the full height and
    // centers horizontally, its top tucked under the top_nav. Parented to the screen root
    // and sent to the BACKGROUND (index 0) so the top_nav, D-pad, keys and band all draw
    // over it; `body` is made transparent so the plane shows through the body region (and
    // the root BACKGROUND_COLOR shows through the pillarbox gutters on wide displays).
    // Starts HIDDEN — blank until the host blits (Python: no image until a KEY1 capture).
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, LV_PART_MAIN);

    const int32_t plane_side  = (display_width < display_height) ? display_width : display_height;
    const size_t  plane_bytes = (size_t)plane_side * (size_t)plane_side * 2;
    ctx->camera_buf.assign(plane_bytes, 0);
    ctx->camera_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    ctx->camera_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
    ctx->camera_dsc.header.w      = plane_side;
    ctx->camera_dsc.header.h      = plane_side;
    ctx->camera_dsc.header.stride = (uint32_t)plane_side * 2;
    ctx->camera_dsc.data_size     = (uint32_t)plane_bytes;
    ctx->camera_dsc.data          = ctx->camera_buf.data();   // stable for the session (buf never resized)

    lv_obj_t *camera_plane = lv_image_create(screen.screen);
    lv_obj_set_size(camera_plane, plane_side, plane_side);
    lv_obj_set_pos(camera_plane, (display_width - plane_side) / 2, 0);   // centered; top under top_nav
    lv_image_set_src(camera_plane, &ctx->camera_dsc);
    lv_obj_remove_flag(camera_plane, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_add_flag(camera_plane, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_to_index(camera_plane, 0);   // background: under every chrome element
    ctx->camera_plane = camera_plane;

    // --- Body ---

    // 1. D-pad pictogram (left), centered vertically on the full canvas (Python).
    //    Python's dpad_center_x = EDGE_PADDING + input_button_width + COMPONENT_PADDING
    //    is canvas-relative; body-local coordinates drop the EDGE_PADDING (the body's
    //    own left pad supplies it — the LEFT arrow sits at x=0), and the body sits
    //    TOP_NAV_HEIGHT below the canvas top, hence the subtraction in the y term.
    const int32_t input_button_width  = BUTTON_HEIGHT + 2;                  // Python input_button_width
    const int32_t input_button_height = input_button_width + 2;            // Python input_button_height
    const int32_t dpad_center_x = input_button_width + COMPONENT_PADDING;  // center-column x (body-local)
    const int32_t dpad_center_y = (screen_height - input_button_height) / 2 - TOP_NAV_HEIGHT;  // center button top (body-local)

    ctx->controls[IO_TEST_CONTROL_CLICK] = io_test_make_dpad_center(body, dpad_center_x, dpad_center_y, input_button_width, input_button_height);
    ctx->controls[IO_TEST_CONTROL_UP]    = io_test_make_dpad_arrow(body, dpad_center_x, dpad_center_y - input_button_height - COMPONENT_PADDING, input_button_width, input_button_height, SeedSignerIconConstants::CHEVRON_UP);
    ctx->controls[IO_TEST_CONTROL_DOWN]  = io_test_make_dpad_arrow(body, dpad_center_x, dpad_center_y + input_button_height + COMPONENT_PADDING, input_button_width, input_button_height, SeedSignerIconConstants::CHEVRON_DOWN);
    ctx->controls[IO_TEST_CONTROL_LEFT]  = io_test_make_dpad_arrow(body, dpad_center_x - input_button_width - COMPONENT_PADDING, dpad_center_y, input_button_width, input_button_height, SeedSignerIconConstants::CHEVRON_LEFT);
    ctx->controls[IO_TEST_CONTROL_RIGHT] = io_test_make_dpad_arrow(body, dpad_center_x + input_button_width + COMPONENT_PADDING, dpad_center_y, input_button_width, input_button_height, SeedSignerIconConstants::CHEVRON_RIGHT);

    // 2. KEY1 / KEY2 / KEY3 (right): ACCENT-outlined boxes sized to Python's own
    //    key_button formulas. Python measures the "Clear" label to fix ONE shared width
    //    for all three, sizes the height off the camera-icon height, centers KEY2 on the
    //    canvas and offsets KEY1/KEY3 by 3*COMPONENT_PADDING + height. screen_x overshoots
    //    the right canvas edge by EDGE_PADDING — that placement IS the physical-key
    //    alignment — so these are parented to the SCREEN ROOT at canvas coordinates (like
    //    the band) to escape the body's edge padding/clipping.
    lv_point_t clear_sz;
    lv_text_get_size(&clear_sz, clear_label.c_str(), &BUTTON_FONT, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const int32_t icon_height       = (int32_t)lv_font_get_line_height(&ICON_FONT__SEEDSIGNER);  // Python: Icon(CAMERA).height
    const int32_t key_button_width  = clear_sz.x + 2 * COMPONENT_PADDING + EDGE_PADDING;          // Python key_button_width
    const int32_t key_button_height = icon_height + (3 * COMPONENT_PADDING) / 2;                   // Python: icon.height + int(1.5*COMPONENT_PADDING)
    const int32_t key_x   = display_width - key_button_width + EDGE_PADDING;                       // overshoots the right edge by EDGE_PADDING
    const int32_t key2_y  = display_height / 2 - key_button_height / 2;                            // KEY2 centered on the canvas
    const int32_t key_gap = 3 * COMPONENT_PADDING + key_button_height;                             // KEY1/KEY3 offset from KEY2

    ctx->controls[IO_TEST_CONTROL_KEY1] = io_test_make_key_box(screen.screen, key_x, key2_y - key_gap,
                                                               key_button_width, key_button_height,
                                                               camera_glyph.c_str(), &ICON_FONT__SEEDSIGNER, NULL);

    // KEY2 starts blank (nothing to clear yet); the screenshot generator shows the label so
    // translators can review it (Python: `if not is_screenshot_generator: text = " "`).
    const char *key2_initial = seedsigner_lvgl_is_static_render() ? clear_label.c_str() : "";
    ctx->controls[IO_TEST_CONTROL_KEY2] = io_test_make_key_box(screen.screen, key_x, key2_y,
                                                               key_button_width, key_button_height,
                                                               key2_initial, &BUTTON_FONT, &ctx->key2_label);

    ctx->controls[IO_TEST_CONTROL_KEY3] = io_test_make_key_box(screen.screen, key_x, key2_y + key_gap,
                                                               key_button_width, key_button_height,
                                                               exit_label.c_str(), &BUTTON_FONT, NULL);

    // 3. "Capturing image…" band (hidden until the host pushes CAPTURING). Parented
    //    to the screen root — NOT the body — so the full-bleed band escapes the
    //    body's padding/clipping: Python draws it edge-to-edge across the canvas.
    const int32_t band_height = (int32_t)lv_font_get_line_height(&TOP_NAV_TITLE_FONT) + 2 * COMPONENT_PADDING;
    lv_obj_t *band = lv_obj_create(screen.screen);
    lv_obj_remove_flag(band, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(band, display_width, band_height);
    lv_obj_set_pos(band, 0, (display_height - band_height) / 2);
    lv_obj_set_style_radius(band, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(band, lv_color_hex(BACKGROUND_COLOR), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(band, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(band, lv_color_hex(ACCENT_COLOR), LV_PART_MAIN);
    lv_obj_set_style_border_width(band, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(band, 0, LV_PART_MAIN);
    lv_obj_t *band_label = lv_label_create(band);
    lv_label_set_text(band_label, capturing_text.c_str());
    lv_obj_set_style_text_font(band_label, &TOP_NAV_TITLE_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(band_label, lv_color_hex(BODY_FONT_COLOR), LV_PART_MAIN);
    lv_obj_center(band_label);
    lv_obj_add_flag(band, LV_OBJ_FLAG_HIDDEN);
    ctx->capturing_band = band;

    // --- Navigation + load ---

    // Self-owned input, not bind_screen_navigation: a 1x1 transparent sink on the
    // screen root (invisible — root parenting keeps it out of the body's layout) is
    // the sole group member; its LV_EVENT_KEY handler flashes controls. (Mirrors
    // nav_bind's group/sink wiring; this screen does its own input, not focus
    // navigation.) Set the indev group BEFORE loading (so the outgoing screen's
    // lv_group_del doesn't touch our indev). The sink/indev wiring is a local copy
    // of a repo-wide idiom, slated for extraction at the rollout decision.
    ctx->group = lv_group_create();
    lv_group_set_wrap(ctx->group, false);
    lv_obj_t *sink = lv_obj_create(screen.screen);
    lv_obj_set_size(sink, 1, 1);
    lv_obj_set_pos(sink, 0, 0);
    lv_obj_set_style_opa(sink, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sink, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(sink, 0, LV_PART_MAIN);
    lv_obj_remove_flag(sink, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_group_add_obj(ctx->group, sink);
    lv_obj_add_event_cb(sink, io_test_key_handler, LV_EVENT_KEY, ctx);

    // Take over input, latching held keys so a key still held from the row that
    // launched the I/O test isn't reported as the first fresh press to test.
    attach_keypad_indevs_to_group(ctx->group);

    // Register as the live host-push target + free on delete (identity-guarded);
    // set only now, after full construction, so io_test_set_capture_state can never
    // observe a half-built ctx.
    g_io_test = ctx;
    lv_obj_add_event_cb(screen.screen, io_test_cleanup_cb, LV_EVENT_DELETE, ctx);

    load_screen_and_cleanup_previous(screen.screen);
}
