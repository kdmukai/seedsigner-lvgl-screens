// io_test_screen
//
// LVGL port of Python's IOTestScreen (settings_screens.py:55). The hardware self-test:
// a D-pad pictogram (joystick up/down/left/right + center click) on the left, KEY1/KEY2/
// KEY3 controls on the right, over a live camera square, with a "Capturing image…" band
// that appears while a still is grabbed.
//
// ── Input is SELF-OWNED (the point of a self-test) ──
// Unlike the passive live screens (qr_display, seed_address_verification), this screen
// READS THE KEYPAD ITSELF and lights up whichever physical control the user actuates —
// exactly what Python's _run does with hw_inputs.wait_for(). It installs its own LVGL
// group + keypad sink and, on each key, flashes the matching control:
//   LV_KEY_UP/DOWN/LEFT/RIGHT -> the D-pad arrows; LV_KEY_ENTER -> the center click;
//   '1'/'2'/'3' (KEY1/2/3, per navigation.cpp is_aux_key) -> the right-side buttons.
// KEY1/2/3 are ALSO forwarded to the host (seedsigner_lvgl_on_aux_key) so it can grab a
// frame / clear / exit. There is NO back button (Python show_back_button = False).
//
// ── Camera is still the board adapter's (two-clock model, see camera_preview_overlay.h) ──
// Camera pixels never flow through this screen; the adapter blits the live/captured frame
// and this screen is the chrome overlay. The host reflects its async single-frame grab via
// io_test_set_capture_state() (shows the "Capturing…" band, toggles the KEY2 "Clear" label).
//
// The KEY1/2/3 buttons reuse the shared kb_side_button / kb_flash_side_button (the same
// right-side panel the passphrase keyboard uses) so they align with the physical keys and
// stay on-screen instead of running off the right edge.
//
// cfg:
//   top_nav.title    (str) — default "I/O Test".
//   capturing_text   (str) — the transient band text, default "Capturing image…".
//   clear_label      (str) — KEY2 label once a still exists, default "Clear".
//   camera_glyph     (str) — KEY1 icon glyph, default FontAwesome camera.
//   exit_label       (str) — KEY3 label, default "Exit".

#include "screen_scaffold.h"   // parse/scaffold/load helpers
#include "seedsigner.h"        // io_test_capture_state_t, is_static_render
#include "keyboard_core.h"     // kb_side_button, kb_flash_side_button
#include "gui_constants.h"     // fonts, colors, sizes, padding, icon glyphs, active_profile
#include "input_profile.h"     // input_profile_set_mode (force hardware input)
#include "lvgl.h"

#include <nlohmann/json.hpp>
#include <string>

using json = nlohmann::json;

// Weak host hook (defined in navigation.cpp): forward KEY1/2/3 so the host can act.
extern "C" void seedsigner_lvgl_on_aux_key(const char *key_name);

namespace {

// The flashable controls, indexed into io_test_ctx::controls.
enum {
    IOC_UP = 0, IOC_DOWN, IOC_LEFT, IOC_RIGHT, IOC_CLICK,
    IOC_KEY1, IOC_KEY2, IOC_KEY3,
    IOC_COUNT
};

// Live-screen state, heap-allocated + registered as g_io for the host capture API; freed
// on the screen's LV_EVENT_DELETE (identity-guarded, like nav_ctx / g_addr_verify).
struct io_test_ctx {
    lv_obj_t   *controls[IOC_COUNT] = {};  // the widget to flash per control
    lv_obj_t   *capturing_band      = nullptr;
    lv_obj_t   *key2_label          = nullptr;  // toggles "" <-> clear_label
    lv_group_t *group               = nullptr;  // owned; deleted on cleanup
    std::string clear_label;
};

io_test_ctx *g_io = nullptr;   // the single active io_test_screen, or nullptr

// ── D-pad control: an accent-outlined rounded rect that inverts (accent fill, dark
// content) while PRESSED, so kb_flash_side_button drives its press-flash too. ──
lv_obj_t *make_dpad_box(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h) {
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_flag(box, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, w, h);
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

lv_obj_t *make_dpad_arrow(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h,
                          const char *glyph) {
    lv_obj_t *box = make_dpad_box(parent, x, y, w, h);
    lv_obj_t *ic = lv_label_create(box);
    lv_label_set_text(ic, glyph);
    lv_obj_set_style_text_font(ic, &ICON_FONT__SEEDSIGNER, LV_PART_MAIN);
    lv_obj_center(ic);
    return box;
}

lv_obj_t *make_dpad_center(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h) {
    lv_obj_t *box = make_dpad_box(parent, x, y, w, h);
    // The joystick-click indicator: a small ring (Python's FontAwesome CIRCLE isn't baked
    // into the LVGL icon font, so it's drawn as an lv_obj). Stays light on the accent flash.
    int32_t d = ICON_INLINE_FONT_SIZE - 6;
    if (d < 6) d = 6;
    lv_obj_t *dot = lv_obj_create(box);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(dot, d, d);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dot, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_color(dot, lv_color_hex(BUTTON_FONT_COLOR), LV_PART_MAIN);
    lv_obj_set_style_border_width(dot, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(dot, 0, LV_PART_MAIN);
    lv_obj_center(dot);
    return box;
}

// Map a keypad code to a control index; -1 if none.
int control_for_key(uint32_t key) {
    switch (key) {
        case LV_KEY_UP:    return IOC_UP;
        case LV_KEY_DOWN:  return IOC_DOWN;
        case LV_KEY_LEFT:  return IOC_LEFT;
        case LV_KEY_RIGHT: return IOC_RIGHT;
        case LV_KEY_ENTER: return IOC_CLICK;
        case (uint32_t)'1': return IOC_KEY1;
        case (uint32_t)'2': return IOC_KEY2;
        case (uint32_t)'3': return IOC_KEY3;
        default: return -1;
    }
}

void io_test_key_handler(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_KEY) return;
    io_test_ctx *ctx = (io_test_ctx *)lv_event_get_user_data(e);
    uint32_t key = lv_event_get_key(e);

    int idx = control_for_key(key);
    if (idx < 0) return;

    kb_flash_side_button(ctx->controls[idx]);   // brief accent flash (matches Python)

    // Forward the aux keys so the host can grab a frame (KEY1) / clear (KEY2) / exit (KEY3).
    if      (idx == IOC_KEY1) seedsigner_lvgl_on_aux_key("KEY1");
    else if (idx == IOC_KEY2) seedsigner_lvgl_on_aux_key("KEY2");
    else if (idx == IOC_KEY3) seedsigner_lvgl_on_aux_key("KEY3");
}

void io_test_cleanup_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;
    io_test_ctx *ctx = (io_test_ctx *)lv_event_get_user_data(e);
    if (ctx->group) lv_group_del(ctx->group);   // clears the indev's group pointer (LVGL)
    if (g_io == ctx) g_io = nullptr;             // don't null a newer screen built before teardown
    delete ctx;
}

}  // namespace

// ── Host-push API (see seedsigner.h) ──
extern "C" void io_test_set_capture_state(io_test_capture_state_t state) {
    if (!g_io) return;
    if (g_io->capturing_band) {
        if (state == IO_TEST_CAPTURE_CAPTURING) lv_obj_remove_flag(g_io->capturing_band, LV_OBJ_FLAG_HIDDEN);
        else                                    lv_obj_add_flag(g_io->capturing_band, LV_OBJ_FLAG_HIDDEN);
    }
    if (g_io->key2_label) {
        lv_label_set_text(g_io->key2_label,
                          state == IO_TEST_CAPTURE_CAPTURED ? g_io->clear_label.c_str() : "");
    }
}

void io_test_screen(void *ctx_json) {
    json cfg;
    parse_screen_json_ctx((const char *)ctx_json, cfg);

    // Titled, no back/power button (Python show_back_button = False; power inherited False).
    if (!cfg.contains("top_nav") || !cfg["top_nav"].is_object()) cfg["top_nav"] = json::object();
    if (!cfg["top_nav"].contains("title")) cfg["top_nav"]["title"] = "I/O Test";
    cfg["top_nav"]["show_back_button"]  = false;
    cfg["top_nav"]["show_power_button"] = false;

    // This screen exists to test the physical joystick + keys, so it only makes sense
    // with hardware input. Force hardware mode so the keypad delivers the joystick / KEY
    // events (the runner's keypad drops keys in touch mode). The screen is gated off on
    // touch-only builds by the app, so this never fights a genuine touch device.
    input_profile_set_mode(INPUT_MODE_HARDWARE);

    std::string capturing_text = cfg.value("capturing_text", std::string("Capturing image\xE2\x80\xA6"));
    std::string clear_label    = cfg.value("clear_label",    std::string("Clear"));
    std::string exit_label     = cfg.value("exit_label",     std::string("Exit"));
    std::string camera_glyph   = cfg.value("camera_glyph",   std::string(FontAwesomeIconConstants::CAMERA));

    // No button_list -> scaffold Mode 1 (body == upper_body). We place the pictogram
    // into `body` at body-local coordinates (as the passphrase side panel does).
    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, false);
    lv_obj_t *body = screen.body;
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    io_test_ctx *ctx = new io_test_ctx();
    ctx->clear_label = clear_label;

    const int32_t W        = lv_display_get_horizontal_resolution(NULL);
    const int32_t H        = lv_display_get_vertical_resolution(NULL);
    const int32_t screen_h = lv_obj_get_height(screen.screen);
    const int32_t content_w = lv_obj_get_content_width(body);

    // ── D-pad pictogram (left), centered vertically on the full canvas (Python) ──
    const int32_t ibw = BUTTON_HEIGHT + 2;                          // input_button_width
    const int32_t ibh = ibw + 2;                                   // input_button_height
    const int32_t cx  = ibw + COMPONENT_PADDING;                   // center-column x (body-local; LEFT sits at x=0)
    const int32_t cy  = (screen_h - ibh) / 2 - TOP_NAV_HEIGHT;     // center button top (body-local)

    ctx->controls[IOC_CLICK] = make_dpad_center(body, cx, cy, ibw, ibh);
    ctx->controls[IOC_UP]    = make_dpad_arrow(body, cx, cy - ibh - COMPONENT_PADDING, ibw, ibh, SeedSignerIconConstants::CHEVRON_UP);
    ctx->controls[IOC_DOWN]  = make_dpad_arrow(body, cx, cy + ibh + COMPONENT_PADDING, ibw, ibh, SeedSignerIconConstants::CHEVRON_DOWN);
    ctx->controls[IOC_LEFT]  = make_dpad_arrow(body, cx - ibw - COMPONENT_PADDING, cy, ibw, ibh, SeedSignerIconConstants::CHEVRON_LEFT);
    ctx->controls[IOC_RIGHT] = make_dpad_arrow(body, cx + ibw + COMPONENT_PADDING, cy, ibw, ibh, SeedSignerIconConstants::CHEVRON_RIGHT);

    // ── KEY1 / KEY2 / KEY3 (right), reusing the shared side panel so they align with the
    // physical keys and stay on-screen (same layout as seed_add_passphrase_screen): a 56px
    // strip that overshoots the right edge by COMPONENT_PADDING, KEY2 centered on the
    // canvas, KEY1/KEY3 offset by 3*COMPONENT_PADDING + BUTTON_HEIGHT. ──
    const int32_t panel_w = 56 * active_profile().px_multiplier / 100;
    const int32_t btn_h   = BUTTON_HEIGHT;
    const int32_t px      = content_w + EDGE_PADDING + COMPONENT_PADDING - panel_w;
    const int32_t clipped = COMPONENT_PADDING;
    const int32_t center_y = (screen_h - btn_h) / 2 - TOP_NAV_HEIGHT;
    const int32_t spacing  = 3 * COMPONENT_PADDING + btn_h;

    ctx->controls[IOC_KEY1] = kb_side_button(body, px, center_y - spacing, panel_w, btn_h,
                                             camera_glyph.c_str(), &ICON_FONT__SEEDSIGNER,
                                             BUTTON_FONT_COLOR, clipped, NULL);

    // KEY2 starts blank (nothing to clear yet); the screenshot generator shows the label so
    // translators can review it (Python: `if not is_screenshot_generator: text = " "`).
    const char *key2_initial = seedsigner_lvgl_is_static_render() ? clear_label.c_str() : "";
    ctx->controls[IOC_KEY2] = kb_side_button(body, px, center_y, panel_w, btn_h,
                                             key2_initial, &BUTTON_FONT, BUTTON_FONT_COLOR,
                                             clipped, &ctx->key2_label);

    ctx->controls[IOC_KEY3] = kb_side_button(body, px, center_y + spacing, panel_w, btn_h,
                                             exit_label.c_str(), &BUTTON_FONT, BUTTON_FONT_COLOR,
                                             clipped, NULL);

    // ── "Capturing image…" band (hidden until CAPTURING) ──
    const int32_t band_h = (int32_t)lv_font_get_line_height(&TOP_NAV_TITLE_FONT) + 2 * COMPONENT_PADDING;
    lv_obj_t *band = lv_obj_create(screen.screen);
    lv_obj_remove_flag(band, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(band, W, band_h);
    lv_obj_set_pos(band, 0, (H - band_h) / 2);
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

    // ── Own the keypad: a 1x1 transparent sink is the sole group member; its LV_EVENT_KEY
    // handler flashes controls. (Mirrors nav_bind's group/sink wiring; this screen does its
    // own input, not focus navigation.) Set the indev group BEFORE loading (so the outgoing
    // screen's lv_group_del doesn't touch our indev). ──
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
    lv_indev_t *indev = NULL;
    while ((indev = lv_indev_get_next(indev)) != NULL) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD ||
            lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER) {
            lv_indev_set_group(indev, ctx->group);
        }
    }

    // Register as the live target + free on delete (identity-guarded).
    g_io = ctx;
    lv_obj_add_event_cb(screen.screen, io_test_cleanup_cb, LV_EVENT_DELETE, ctx);

    load_screen_and_cleanup_previous(screen.screen);
}
