#include "camera_preview_overlay.h"

#include "components.h"      // back_button()
#include "gui_constants.h"   // colors, scaled layout macros, active_profile()
#include "input_profile.h"   // input_profile_get_mode()

#include "lvgl.h"

#include <string>

// ---------------------------------------------------------------------------
// Camera live-preview overlay renderer (LVGL widgets — ESP Path A + Pi Zero).
// See camera_preview_overlay.h for the layering/orchestration rationale. Geometry
// mirrors Python ScanScreen (scan_screens.py) at the square's scale, using the
// profile-scaled layout macros so it holds across 240/320/480.
// ---------------------------------------------------------------------------

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
};

// Width (px) of the literal "100%" at the button font — the right-hand gutter the
// progress track must leave for the percent label (Python pre-measures the same).
static int32_t percent_text_width() {
    lv_point_t size = {0, 0};
    lv_text_get_size(&size, "100%", &BUTTON_FONT, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
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

void camera_preview_overlay_set_progress(camera_preview_overlay_t *o,
                                         int percent,
                                         camera_overlay_frame_status_t frame_status) {
    if (!o) return;

    // A progress event implies we are scanning.
    camera_preview_overlay_set_scanning(o, true);

    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;

    int32_t fill_w = (int32_t)((int64_t)percent * o->track_width / 100);
    lv_obj_set_width(o->fill, fill_w);

    std::string text = std::to_string(percent) + "%";
    lv_label_set_text(o->percent_label, text.c_str());

    apply_dot_status(o, frame_status);
}

void camera_preview_overlay_destroy(camera_preview_overlay_t *o) {
    if (o) lv_free(o);
}
