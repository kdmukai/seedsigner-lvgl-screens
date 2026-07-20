#include "camera_preview_overlay.h"

#include "components.h"      // back_button()
#include "gui_constants.h"   // colors, scaled layout macros, active_profile()
#include "input_profile.h"   // input_profile_get_mode()
#include "navigation.h"      // attach_keypad_indevs_to_group (held-key-safe input handoff)
#include "seedsigner.h"      // SEEDSIGNER_RET_BACK_BUTTON

#include "lvgl.h"

#include <string>

// ---------------------------------------------------------------------------
// Camera live-preview overlay renderer (LVGL widgets — ESP Path A + Pi Zero).
// See camera_preview_overlay.h for the layering/orchestration rationale. Geometry
// mirrors Python ScanScreen (scan_screens.py) at the square's scale, using the
// profile-scaled layout macros so it holds across 240/320/480.
// ---------------------------------------------------------------------------

// Host emit seam (same weak symbol components.cpp uses): in hardware mode the
// joystick back key posts the SAME event the touch back button does, so the host
// reads one signal per mode. See the keypad block below.
extern "C" __attribute__((weak)) void seedsigner_lvgl_on_button_selected(uint32_t index, const char *label);

struct camera_preview_overlay {
    input_mode_t mode;

    // Back affordance (state: not scanning). Exactly one is non-NULL per mode.
    lv_obj_t *back_btn;        // touch: button in the top-left gutter (persists)
    lv_obj_t *instr_shadow;    // hardware: instruction text drop shadow
    lv_obj_t *instr_label;     // hardware: instruction text

    // Status bar (state: scanning), all positioned relative to the square.
    lv_obj_t *status_bar;      // semi-transparent rounded container
    lv_obj_t *track;           // progress track (inactive gray)
    lv_obj_t *fill;            // progress fill (green)
    lv_obj_t *percent_label;   // "NN%"
    lv_obj_t *dot;             // most-recent-frame status dot

    int32_t   track_width;     // px width of the full track (for fill scaling)
    int32_t   fill_thickness;  // px thickness of track/fill (for radius)
    int32_t   displayed_percent;  // current on-screen % (animated); drives glide + snap
};

// Duration of the completion fill-to-100% glide (the ONLY animated transition;
// intermediate advances snap — see set_progress for why). The consumer's
// completion hold (~550ms) is sized to outlast this so the glide plays before
// the camera tears down.
static const uint32_t OVERLAY_PROGRESS_ANIM_MS = 300;

// Width (px) of the literal "100%" at the button font — the right-hand gutter the
// progress track must leave for the percent label (Python pre-measures the same).
static int32_t percent_text_width() {
    lv_point_t size = {0, 0};
    lv_text_get_size(&size, "100%", &BUTTON_FONT, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

// Paint one opaque black rectangle at (x,y,w,h) in the parent's coordinate space.
// Used to blank a gutter strip flanking the preview square. No-op for empty strips
// so the fill-landscape case (square == full display, no gutters) adds nothing.
static void add_gutter_fill(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h) {
    if (w <= 0 || h <= 0) return;
    lv_obj_t *rect = lv_obj_create(parent);
    lv_obj_set_size(rect, w, h);
    lv_obj_set_pos(rect, x, y);
    lv_obj_set_style_radius(rect, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(rect, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(rect, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(rect, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(rect, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(rect, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
}

// Round-rect helper shared by the status-bar container, track, and fill.
static void style_rounded(lv_obj_t *obj, uint32_t color, lv_opa_t opa, int32_t radius) {
    lv_obj_remove_flag(obj, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, radius, LV_PART_MAIN);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, opa, LV_PART_MAIN);
}

// Color the status dot for a frame status; hide it for MISS/NONE. Used at build time
// and on every set_progress() call.
static void apply_dot_status(camera_preview_overlay_t *o, camera_overlay_frame_status_t s) {
    uint32_t color = 0;
    bool visible = true;
    switch (s) {
        case CAMERA_OVERLAY_FRAME_ADDED:    color = (uint32_t)SUCCESS_COLOR;  break;
        case CAMERA_OVERLAY_FRAME_REPEATED: color = (uint32_t)INACTIVE_COLOR; break;
        default:                            visible = false;                  break;  // MISS / NONE
    }
    if (visible) {
        lv_obj_set_style_bg_color(o->dot, lv_color_hex(color), LV_PART_MAIN);
        lv_obj_remove_flag(o->dot, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(o->dot, LV_OBJ_FLAG_HIDDEN);
    }
}

// --- Hardware keypad input --------------------------------------------------
// In hardware mode the overlay draws only instruction TEXT — no focusable widget —
// so without a group of its own the keypad indev has NONE (the outgoing screen's
// group was deleted along with it) and LVGL's keypad handler discards every key
// before it reaches a widget. The flow then cannot be backed out of at all. The
// invisible-sink + group + attach_keypad_indevs_to_group() shape below is the same
// one every other keypad-driven screen uses (seed_transcribe_zoomed_qr_screen,
// opening_splash_screen); the attach also latches a key held across the screen swap
// so a carried-over press can't cancel the scan the instant it opens.
// This block is duplicated with camera_entropy_overlay (differing only in the key
// map). [slated for extraction: camera_overlay_common, consolidation ledger §12]

// The group is owned by `parent` (freed on its LV_EVENT_DELETE), NOT by the overlay
// handle: callers may destroy the handle immediately after create() while the widget
// tree lives on (the desktop tooling does exactly that).
static void keypad_group_deleted_cb(lv_event_t *e) {
    lv_group_t *group = (lv_group_t *)lv_event_get_user_data(e);
    if (group) lv_group_del(group);
}

// Build the focusable 1x1 transparent sink that receives LV_EVENT_KEY, put it in a
// fresh group, and hand the keypad indevs over. Returns the sink so a caller can hang
// per-phase state on its user_data (the entropy overlay does; this one is stateless).
static lv_obj_t *attach_keypad_sink(lv_obj_t *parent, lv_event_cb_t key_cb) {
    lv_obj_t *sink = lv_obj_create(parent);
    lv_obj_set_size(sink, 1, 1);
    lv_obj_set_pos(sink, 0, 0);
    // Fully invisible: the sink is the FOCUSED object, and the theme paints an outline
    // on focus — over live camera pixels here, so zero outline/shadow as well as bg.
    lv_obj_set_style_opa(sink, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sink, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(sink, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(sink, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(sink, 0, LV_PART_MAIN);
    lv_obj_remove_flag(sink, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

    lv_group_t *group = lv_group_create();
    lv_group_add_obj(group, sink);
    lv_obj_add_event_cb(sink, key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(parent, keypad_group_deleted_cb, LV_EVENT_DELETE, group);

    attach_keypad_indevs_to_group(group);
    return sink;
}

// Python ScanScreen exits the live preview on KEY_LEFT or KEY_RIGHT
// (scan_screens.py); both post the same back event the touch back button posts, so
// the host dispatches one code regardless of input mode. Every other key is ignored,
// matching Python (the scan has no other affordance).
static void preview_keypad_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_KEY) return;
    const uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_LEFT || key == LV_KEY_RIGHT) {
        seedsigner_lvgl_on_button_selected(SEEDSIGNER_RET_BACK_BUTTON, "back");
    }
}

camera_preview_overlay_t *camera_preview_overlay_create(lv_obj_t *parent,
                                                        const camera_preview_overlay_spec_t *spec) {
    if (!parent || !spec) {
        return NULL;
    }

    camera_preview_overlay_t *o =
        (camera_preview_overlay_t *)lv_malloc(sizeof(camera_preview_overlay_t));
    lv_memzero(o, sizeof(*o));
    o->mode = input_profile_get_mode();

    const int32_t SX = spec->square_x, SY = spec->square_y;
    const int32_t SW = spec->square_w, SH = spec->square_h;
    const int32_t EP = EDGE_PADDING, CP = COMPONENT_PADDING, BH = BUTTON_HEIGHT;
    const int     px = active_profile().px_multiplier;

    // --- Gutter blanking --------------------------------------------------
    // The live camera preview fills only the square; the surrounding gutters are
    // static (never refreshed per frame). On device the overlay is composited onto a
    // parent that still holds whatever screen was up last (usually the main menu), so
    // that stale content shows through the strips flanking the square. Paint opaque
    // black over the parent region OUTSIDE the square — created first so these sit at
    // the bottom of the z-order (behind the back button / status bar) and, by only
    // covering the gutters, never touch the live square. When the preview fills the
    // display (Pi Zero, square == screen) every strip is empty and nothing is added.
    // Parent spans the full display (see header), so its extent is the display size.
    const int32_t PW = lv_display_get_horizontal_resolution(NULL);
    const int32_t PH = lv_display_get_vertical_resolution(NULL);
    add_gutter_fill(parent, 0, 0, SX, PH);                      // left  (full height)
    add_gutter_fill(parent, SX + SW, 0, PW - (SX + SW), PH);    // right (full height)
    add_gutter_fill(parent, SX, 0, SW, SY);                     // top   (above square)
    add_gutter_fill(parent, SX, SY + SH, SW, PH - (SY + SH));   // bottom (below square)

    // --- Back affordance --------------------------------------------------
    if (o->mode == INPUT_MODE_TOUCH) {
        // The shared back button, alone, in the parent's top-left gutter (outside the
        // square so it never overlaps the live preview). One definition for the top
        // nav and here (components.cpp back_button()).
        o->back_btn = back_button(parent, LV_ALIGN_TOP_LEFT, EP, EP);
    } else if (spec->instructions_text && spec->instructions_text[0]) {
        // Hardware/joystick: bottom-center instruction text over the square, with a
        // 1px-ish drop shadow for legibility (Python ScanScreen renders shadow then
        // text). Two labels: shadow underneath, body on top.
        const int32_t shadow_off = (2 * px) / 100 > 0 ? (2 * px) / 100 : 1;
        const int32_t text_w     = SW - 2 * EP;

        for (int pass = 0; pass < 2; ++pass) {
            lv_obj_t *lbl = lv_label_create(parent);
            lv_label_set_text(lbl, spec->instructions_text);
            lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
            lv_obj_set_width(lbl, text_w);
            lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
            lv_obj_set_style_text_font(lbl, &BUTTON_FONT, LV_PART_MAIN);
            lv_obj_set_style_text_color(
                lbl, lv_color_hex(pass == 0 ? 0x000000 : (uint32_t)BODY_FONT_COLOR), LV_PART_MAIN);

            // Bottom-aligned within the square, horizontally centered on it.
            int32_t off = (pass == 0) ? shadow_off : 0;
            lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, SX + EP + off, 0);
            lv_obj_update_layout(lbl);
            int32_t lh = lv_obj_get_height(lbl);
            lv_obj_set_y(lbl, SY + SH - EP - lh + off);

            if (pass == 0) o->instr_shadow = lbl; else o->instr_label = lbl;
        }
    }

    // Hardware/joystick keys. Wired independently of the instruction line above —
    // that text is only a hint, and LEFT/RIGHT must back out of the scan whether or
    // not the host supplied one.
    if (o->mode == INPUT_MODE_HARDWARE) {
        attach_keypad_sink(parent, preview_keypad_cb);
    }

    // --- Status bar (built hidden unless scanning) ------------------------
    // Semi-transparent rounded container along the bottom of the square, inset by
    // EDGE_PADDING on both sides (Python: width = square - 2*EP, height = BUTTON_HEIGHT).
    const int32_t bar_w = SW - 2 * EP;
    o->status_bar = lv_obj_create(parent);
    lv_obj_set_size(o->status_bar, bar_w, BH);
    lv_obj_set_pos(o->status_bar, SX + EP, SY + SH - EP - BH);
    style_rounded(o->status_bar, 0x000000, (lv_opa_t)191, BUTTON_RADIUS);  // 191/255 ≈ 75%

    // Progress track + fill: a thin pill, vertically centered in the bar. Track width
    // leaves room on the right for the percent label (Python: bar - 2*EP - w("100%") - EP/2).
    o->fill_thickness = LIST_ITEM_PADDING;                       // = scaled 4px
    o->track_width    = bar_w - 2 * EP - percent_text_width() - EP / 2;
    if (o->track_width < 0) o->track_width = 0;
    const int32_t track_y    = (BH - o->fill_thickness) / 2;
    const int32_t pill_radius = o->fill_thickness / 2;

    o->track = lv_obj_create(o->status_bar);
    lv_obj_set_size(o->track, o->track_width, o->fill_thickness);
    lv_obj_set_pos(o->track, EP, track_y);
    style_rounded(o->track, (uint32_t)INACTIVE_COLOR, LV_OPA_COVER, pill_radius);

    o->fill = lv_obj_create(o->status_bar);
    lv_obj_set_size(o->fill, 0, o->fill_thickness);
    lv_obj_set_pos(o->fill, EP, track_y);
    style_rounded(o->fill, (uint32_t)GREEN_INDICATOR_COLOR, LV_OPA_COVER, pill_radius);

    o->percent_label = lv_label_create(o->status_bar);
    lv_label_set_text(o->percent_label, "0%");
    lv_obj_set_style_text_font(o->percent_label, &BUTTON_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(o->percent_label, lv_color_hex((uint32_t)BODY_FONT_COLOR), LV_PART_MAIN);
    lv_obj_align(o->percent_label, LV_ALIGN_RIGHT_MID, -EP, 0);

    // Most-recent-frame status dot, just above the bar's top-right corner (Python).
    int32_t dot = (10 * px) / 100;
    if (dot < 1) dot = 1;
    o->dot = lv_obj_create(parent);
    lv_obj_set_size(o->dot, dot, dot);
    lv_obj_set_pos(o->dot, SX + SW - EP - dot, SY + SH - EP - BH - CP - dot);
    style_rounded(o->dot, (uint32_t)INACTIVE_COLOR, LV_OPA_COVER, dot / 2);
    lv_obj_set_style_border_width(o->dot, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(o->dot, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_opa(o->dot, LV_OPA_COVER, LV_PART_MAIN);

    // Apply the initial state.
    camera_preview_overlay_set_scanning(o, spec->scanning_active);
    if (spec->scanning_active) {
        camera_preview_overlay_set_progress(o, spec->progress_percent, spec->frame_status);
    }
    return o;
}

void camera_preview_overlay_set_scanning(camera_preview_overlay_t *o, bool active) {
    if (!o) return;

    // Status bar + dot follow the scanning state in BOTH modes.
    if (active) {
        lv_obj_remove_flag(o->status_bar, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(o->status_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(o->dot, LV_OBJ_FLAG_HIDDEN);  // re-shown by set_progress
    }

    // Hardware mode: the instruction text and the status bar are mutually exclusive
    // (Python replaces one with the other). Touch mode: the gutter back button persists.
    if (o->mode == INPUT_MODE_HARDWARE) {
        if (o->instr_label)  { active ? lv_obj_add_flag(o->instr_label,  LV_OBJ_FLAG_HIDDEN)
                                      : lv_obj_remove_flag(o->instr_label,  LV_OBJ_FLAG_HIDDEN); }
        if (o->instr_shadow) { active ? lv_obj_add_flag(o->instr_shadow, LV_OBJ_FLAG_HIDDEN)
                                      : lv_obj_remove_flag(o->instr_shadow, LV_OBJ_FLAG_HIDDEN); }
    }
}

// Paints the fill width + percent label from a single value; also the lv_anim
// exec callback (var = the overlay) so the bar and number glide together.
static void overlay_progress_paint(void *var, int32_t percent) {
    camera_preview_overlay_t *o = (camera_preview_overlay_t *)var;
    o->displayed_percent = percent;
    int32_t fill_w = (int32_t)((int64_t)percent * o->track_width / 100);
    lv_obj_set_width(o->fill, fill_w);
    std::string text = std::to_string((int)percent) + "%";
    lv_label_set_text(o->percent_label, text.c_str());
}

void camera_preview_overlay_set_progress(camera_preview_overlay_t *o,
                                         int percent,
                                         camera_overlay_frame_status_t frame_status) {
    if (!o) return;

    // A progress event implies we are scanning.
    camera_preview_overlay_set_scanning(o, true);

    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;

    // Glide ONLY the final fill to 100%; snap everything else.
    //
    // In FULL render mode a gliding fill re-renders the whole screen every refresh,
    // which starves the camera preview's frame-push for the LVGL lock and visibly
    // freezes the live preview. An instant set_width is a single render (same cost
    // as one camera frame), so intermediate advances must snap to keep the preview
    // smooth. The completion glide is exempt because it plays during the consumer's
    // completion hold, right before the camera tears down — a brief freeze there is
    // harmless and the fill-to-full is a nice confirmation flourish. Single-frame
    // reads (displayed==0) and any non-forward update also snap.
    lv_anim_del(o, overlay_progress_paint);  // cancel any in-flight glide first
    bool glide_completion = (o->displayed_percent != 0)
                            && (percent > o->displayed_percent)
                            && (percent >= 100);
    if (!glide_completion) {
        overlay_progress_paint(o, percent);
    } else {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, o);
        lv_anim_set_exec_cb(&a, overlay_progress_paint);
        lv_anim_set_values(&a, o->displayed_percent, percent);
        lv_anim_set_duration(&a, OVERLAY_PROGRESS_ANIM_MS);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
    }

    apply_dot_status(o, frame_status);
}

void camera_preview_overlay_destroy(camera_preview_overlay_t *o) {
    if (!o) return;
    // Cancel any in-flight progress glide so its exec callback can't fire on the
    // freed overlay after teardown (use-after-free).
    lv_anim_del(o, overlay_progress_paint);
    lv_free(o);
}
