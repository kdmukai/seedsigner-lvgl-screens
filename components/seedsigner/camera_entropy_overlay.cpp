#include "camera_entropy_overlay.h"

#include "components.h"      // back_button(), button()
#include "gui_constants.h"   // colors, scaled layout macros, active_profile()
#include "input_profile.h"   // input_profile_get_mode()
#include "navigation.h"      // attach_keypad_indevs_to_group, nav_aux_key_index
#include "seedsigner.h"      // SEEDSIGNER_RET_BACK_BUTTON

#include "lvgl.h"

// ---------------------------------------------------------------------------
// Image-entropy capture overlay renderer (LVGL widgets — ESP Path A + Pi Zero).
// See camera_entropy_overlay.h for the phase/mode contract. Geometry mirrors the
// Python PIL entropy screens (tools_screens.py) at the square's scale, using the
// profile-scaled layout macros so it holds across 240/320/480/800.
// ---------------------------------------------------------------------------

// Host emit seam (same weak symbol components.cpp uses): the shutter + accept controls
// report a selection the host reads via poll_for_result as "button_selected". The back
// button (back_button()) already emits SEEDSIGNER_RET_BACK_BUTTON → "topnav_back". The
// index is ignored by the entropy host loop (it keys off the phase), so 0 is fine.
extern "C" __attribute__((weak)) void seedsigner_lvgl_on_button_selected(uint32_t index, const char *label);

struct camera_entropy_overlay {
    input_mode_t           mode;
    camera_entropy_phase_t phase;

    // Full-display frozen final frame, shown only in CONFIRM (both modes). Empty +
    // hidden until the host pushes a frame via camera_entropy_overlay_set_confirm_image().
    lv_obj_t *confirm_image;

    // Touch controls (NULL in hardware mode).
    lv_obj_t *back_btn;        // top-left gutter: PREVIEW=cancel, CONFIRM=reshoot
    lv_obj_t *capture_ctrl;    // PREVIEW capture: shutter circle OR standard bottom button
    lv_obj_t *accept_btn;      // CONFIRM accept

    // Hardware / transient text — each is a shadow+body label pair (NULL in touch mode
    // except the capturing pair, which is built in both modes).
    lv_obj_t *preview_shadow,  *preview_lbl;
    lv_obj_t *confirm_shadow,  *confirm_lbl;
    lv_obj_t *capturing_shadow, *capturing_lbl;

    // Hardware keypad sink (NULL in touch mode). Carries the CURRENT phase in its
    // user_data so the key handler reads it off the widget rather than this handle —
    // callers may free the handle right after create() while the widgets live on.
    lv_obj_t *keypad_sink;
};

// --- shared helpers ---------------------------------------------------------

// Opaque-black rectangle over a gutter strip flanking the preview square (mirrors
// camera_preview_overlay). No-op for empty strips (fill-landscape adds nothing).
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

// Bottom-center instruction text over the square (shadow underneath + body on top), as
// the hardware branch of camera_preview_overlay / Python's stroke_fill text.
static void make_bottom_text(lv_obj_t *parent, const char *text, uint32_t body_color,
                             int32_t SX, int32_t SY, int32_t SW, int32_t SH,
                             lv_obj_t **out_shadow, lv_obj_t **out_body) {
    const int32_t EP = EDGE_PADDING;
    const int     px = active_profile().px_multiplier;
    const int32_t shadow_off = (2 * px) / 100 > 0 ? (2 * px) / 100 : 1;
    const int32_t text_w = SW - 2 * EP;

    for (int pass = 0; pass < 2; ++pass) {
        lv_obj_t *lbl = lv_label_create(parent);
        lv_label_set_text(lbl, text);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl, text_w);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_text_font(lbl, &BUTTON_FONT, LV_PART_MAIN);
        lv_obj_set_style_text_color(
            lbl, lv_color_hex(pass == 0 ? 0x000000 : body_color), LV_PART_MAIN);
        int32_t off = (pass == 0) ? shadow_off : 0;
        lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, SX + EP + off, 0);
        lv_obj_update_layout(lbl);
        int32_t lh = lv_obj_get_height(lbl);
        lv_obj_set_y(lbl, SY + SH - EP - lh + off);
        if (pass == 0) *out_shadow = lbl; else *out_body = lbl;
    }
}

static void show(lv_obj_t *o, bool visible) {
    if (!o) return;
    if (visible) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else         lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

// The confirm frame's descriptor plus the deallocator that matches how its pixels were
// allocated. The pixel buffer may come from the LVGL heap (the copying setter) or from
// caller-owned memory such as PSRAM (the _owned setter), and those cannot be freed the
// same way — so the two always travel together. `dsc` is first, so a pointer to this
// struct is also a valid lv_image_dsc_t* for lv_image_set_src().
struct confirm_frame {
    lv_image_dsc_t dsc;
    void (*free_fn)(void *);   // NULL => lv_free
};

static void confirm_frame_release(confirm_frame *f) {
    if (!f) return;
    void *pixels = (void *)f->dsc.data;
    if (pixels) {
        if (f->free_fn) f->free_fn(pixels);
        else            lv_free(pixels);
    }
    lv_free(f);
}

// Free the confirm-image frame (pixel buffer + its heap dsc) when the canvas/image
// widget is deleted. The frame is owned by the WIDGET, not the overlay handle
// (tooling destroys the handle immediately after build), so it must be released on
// the widget's LV_EVENT_DELETE. The frame pointer lives in the widget's user_data.
static void confirm_image_deleted_cb(lv_event_t *e) {
    lv_obj_t *img = (lv_obj_t *)lv_event_get_target(e);
    confirm_frame_release((confirm_frame *)lv_obj_get_user_data(img));
}

// Shutter / accept clicks → host "button_selected" (index ignored by the entropy loop).
static void action_clicked_cb(lv_event_t *e) {
    const char *label = (const char *)lv_event_get_user_data(e);
    seedsigner_lvgl_on_button_selected(0, label);
}

// --- Hardware keypad input --------------------------------------------------
// In hardware mode this overlay draws only instruction TEXT — every control above is
// touch-only — so without a group of its own the keypad indev has NONE (the outgoing
// screen's group was deleted along with it) and LVGL discards every key before it
// reaches a widget. Entropy is then not merely unexitable but entirely inert: no
// capture, no accept, no cancel. The sink + group + attach_keypad_indevs_to_group()
// shape matches every other keypad-driven screen, and the attach latches a key held
// across the screen swap so a carried-over press can't fire a capture on arrival.
// Duplicated with camera_preview_overlay, differing only in the key map.
// [slated for extraction: camera_overlay_common, consolidation ledger §12]

// The group is owned by `parent` (freed on its LV_EVENT_DELETE), NOT by the overlay
// handle — see the keypad_sink field comment.
static void keypad_group_deleted_cb(lv_event_t *e) {
    lv_group_t *group = (lv_group_t *)lv_event_get_user_data(e);
    if (group) lv_group_del(group);
}

// The phase the key handler dispatches on, stored on the sink widget itself so it
// stays valid for the widget's whole life. Kept in sync by ..._set_phase().
static void keypad_sink_set_phase(lv_obj_t *sink, camera_entropy_phase_t phase) {
    if (sink) lv_obj_set_user_data(sink, (void *)(uintptr_t)phase);
}

// Build the focusable 1x1 transparent sink that receives LV_EVENT_KEY, put it in a
// fresh group, and hand the keypad indevs over. Returns the sink (the caller hangs the
// phase on its user_data).
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

// Phase-dependent key map, mirroring the Python PIL screens exactly:
//   PREVIEW   (ToolsImageEntropyLivePreviewScreen) LEFT = back, ANYCLICK = capture.
//   CONFIRM   (ToolsImageEntropyFinalImageScreen)  LEFT = reshoot, RIGHT or
//             ANYCLICK = accept.
// ANYCLICK is Python's KEYS__ANYCLICK = [KEY_PRESS, KEY1, KEY2, KEY3]; KEY1-3 go
// through the shared nav_aux_key_index() recognizer so aux-key handling stays
// byte-equivalent across the corpus. Reshoot rides the same back event as cancel —
// the host tells them apart by the phase it is in, per the header's event contract.
static void entropy_keypad_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_KEY) return;

    lv_obj_t *sink = (lv_obj_t *)lv_event_get_target(e);
    const camera_entropy_phase_t phase =
        (camera_entropy_phase_t)(uintptr_t)lv_obj_get_user_data(sink);

    const uint32_t key = lv_event_get_key(e);
    const bool anyclick = (key == LV_KEY_ENTER) || (nav_aux_key_index(key) != 0);

    switch (phase) {
        case CAMERA_ENTROPY_PHASE_PREVIEW:
            if (key == LV_KEY_LEFT) {
                seedsigner_lvgl_on_button_selected(SEEDSIGNER_RET_BACK_BUTTON, "back");
            } else if (anyclick) {
                seedsigner_lvgl_on_button_selected(0, "capture");
            }
            break;

        case CAMERA_ENTROPY_PHASE_CONFIRM:
            if (key == LV_KEY_LEFT) {
                seedsigner_lvgl_on_button_selected(SEEDSIGNER_RET_BACK_BUTTON, "back");
            } else if (key == LV_KEY_RIGHT || anyclick) {
                seedsigner_lvgl_on_button_selected(0, "accept");
            }
            break;

        case CAMERA_ENTROPY_PHASE_CAPTURING:
            // Transient: the frame is mid-freeze/latch. Swallow keys so a second
            // click can't post an accept against the phase that hasn't loaded yet.
            break;
    }
}

// Camera "shutter" — a white circle (concentric ring+disc, or a single solid disc) with
// an optional camera glyph centered on it. The disc is opaque white so the icon reads at
// consistent contrast over any live-camera content behind it.
static lv_obj_t *build_shutter(lv_obj_t *parent, int32_t cx, int32_t bottom_y,
                               const char *icon_glyph, bool solid) {
    const int32_t BH = BUTTON_HEIGHT;
    int32_t D    = 2 * BH;                    // outer diameter (scales with the profile)
    int32_t ring = D / 12 > 2 ? D / 12 : 2;   // ring thickness

    lv_obj_t *outer = lv_obj_create(parent);
    lv_obj_remove_flag(outer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(outer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(outer, D, D);
    lv_obj_set_pos(outer, cx - D / 2, bottom_y - D);
    lv_obj_set_style_radius(outer, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_pad_all(outer, 0, LV_PART_MAIN);

    if (solid) {
        // Single solid white disc.
        lv_obj_set_style_border_width(outer, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_color(outer, lv_color_hex(BODY_FONT_COLOR), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(outer, LV_OPA_COVER, LV_PART_MAIN);
    } else {
        // White ring, transparent gap, white inner disc.
        lv_obj_set_style_bg_opa(outer, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(outer, ring, LV_PART_MAIN);
        lv_obj_set_style_border_color(outer, lv_color_hex(BODY_FONT_COLOR), LV_PART_MAIN);
        lv_obj_set_style_border_opa(outer, LV_OPA_COVER, LV_PART_MAIN);

        int32_t inner_d = D - 4 * ring;
        lv_obj_t *inner = lv_obj_create(outer);
        lv_obj_remove_flag(inner, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
        lv_obj_set_size(inner, inner_d, inner_d);
        lv_obj_center(inner);
        lv_obj_set_style_radius(inner, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_pad_all(inner, 0, LV_PART_MAIN);
        lv_obj_set_style_border_width(inner, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_color(inner, lv_color_hex(BODY_FONT_COLOR), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(inner, LV_OPA_COVER, LV_PART_MAIN);
    }

    // Camera glyph centered on the shutter (dark, on the white fill). A label is not
    // clickable, so the click still lands on `outer`.
    if (icon_glyph && icon_glyph[0]) {
        lv_obj_t *icon = lv_label_create(outer);
        lv_label_set_text(icon, icon_glyph);
        lv_obj_set_style_text_font(icon, &ICON_FONT__SEEDSIGNER, LV_PART_MAIN);
        lv_obj_set_style_text_color(icon, lv_color_hex((uint32_t)BUTTON_BACKGROUND_COLOR), LV_PART_MAIN);
        lv_obj_center(icon);
    }

    // (The caller wires the LV_EVENT_CLICKED → "capture" emit, uniformly for every
    // capture-control style.)
    return outer;
}

camera_entropy_overlay_t *camera_entropy_overlay_create(lv_obj_t *parent,
                                                        const camera_entropy_overlay_spec_t *spec) {
    if (!parent || !spec) {
        return NULL;
    }

    camera_entropy_overlay_t *o =
        (camera_entropy_overlay_t *)lv_malloc(sizeof(camera_entropy_overlay_t));
    lv_memzero(o, sizeof(*o));
    o->mode  = input_profile_get_mode();
    o->phase = spec->phase;

    const int32_t SX = spec->square_x, SY = spec->square_y;
    const int32_t SW = spec->square_w, SH = spec->square_h;
    const int32_t EP = EDGE_PADDING, BH = BUTTON_HEIGHT;

    // --- Gutter blanking (same rationale as camera_preview_overlay) -----------
    const int32_t PW = lv_display_get_horizontal_resolution(NULL);
    const int32_t PH = lv_display_get_vertical_resolution(NULL);
    add_gutter_fill(parent, 0, 0, SX, PH);                     // left
    add_gutter_fill(parent, SX + SW, 0, PW - (SX + SW), PH);   // right
    add_gutter_fill(parent, SX, 0, SW, SY);                    // top
    add_gutter_fill(parent, SX, SY + SH, SW, PH - (SY + SH));  // bottom

    // Full-display frozen-frame surface for the CONFIRM phase. Created HERE — after
    // the gutter fills, before the transient text + affordances below — so it renders
    // ABOVE the black gutters (a confirm frame fills the whole display, gutters
    // included) but BELOW the accept / back controls. Because the gutter rects are
    // never removed, hiding this image on reshoot instantly restores the black gutters
    // + the live square with no explicit clear. Empty + hidden until the host pushes a
    // frame; positioned/sized when the frame arrives.
    o->confirm_image = lv_image_create(parent);
    lv_obj_set_pos(o->confirm_image, 0, 0);
    lv_obj_add_flag(o->confirm_image, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(o->confirm_image, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_add_event_cb(o->confirm_image, confirm_image_deleted_cb, LV_EVENT_DELETE, NULL);

    // Capturing transient text — bottom-center, accent color, in BOTH modes.
    if (spec->capturing_text && spec->capturing_text[0]) {
        make_bottom_text(parent, spec->capturing_text, (uint32_t)ACCENT_COLOR,
                         SX, SY, SW, SH, &o->capturing_shadow, &o->capturing_lbl);
    }

    if (o->mode == INPUT_MODE_TOUCH) {
        // Back button in the top-left gutter, persistent across PREVIEW/CONFIRM (its
        // MEANING changes by phase: cancel vs reshoot — the host loop interprets it).
        o->back_btn = back_button(parent, LV_ALIGN_TOP_LEFT, EP, EP);

        // The camera glyph is intrinsic to the shutter (a symbol, not translatable text):
        // default to the FontAwesome camera when the host doesn't override capture_icon,
        // so callers (e.g. the builder's camera_entropy) needn't know the glyph.
        const char *cap_icon = (spec->capture_icon && spec->capture_icon[0])
                                   ? spec->capture_icon
                                   : FontAwesomeIconConstants::CAMERA;

        // PREVIEW capture control. BUTTON: a standard bottom button (icon + label), same
        // footprint as the Accept button. RING/SOLID: a shutter circle bottom-center.
        if (spec->capture_style == CAMERA_ENTROPY_CAPTURE_BUTTON) {
            // button_ex sizes itself to its parent's content width AT CREATION and lays
            // out the icon+label for that width (a later set_width would NOT re-center the
            // icon layout), so build it inside a transparent holder already sized to the
            // final in-square footprint: same width/position as the Accept button.
            lv_obj_t *holder = lv_obj_create(parent);
            lv_obj_remove_flag(holder, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
            lv_obj_set_size(holder, SW - 2 * EP, BH);
            lv_obj_set_pos(holder, SX + EP, SY + SH - EP - BH);
            lv_obj_set_style_pad_all(holder, 0, LV_PART_MAIN);
            lv_obj_set_style_border_width(holder, 0, LV_PART_MAIN);
            lv_obj_set_style_radius(holder, 0, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(holder, LV_OPA_TRANSP, LV_PART_MAIN);
            // Force the holder's coords before button_ex reads its content width to size
            // itself (a fresh object's coords aren't final until a layout pass).
            lv_obj_update_layout(holder);

            button_opts_t opts = {};
            opts.text             = spec->capture_label;   // localized, e.g. "Capture"
            opts.icon             = cap_icon;              // camera glyph (defaulted above)
            opts.is_text_centered = true;
            opts.icon_color       = SEEDSIGNER_ICON_COLOR_DEFAULT;
            opts.label_color      = SEEDSIGNER_ICON_COLOR_DEFAULT;
            o->capture_ctrl = button_ex(holder, &opts);    // fills the holder (top-mid, content width)
        } else {
            const int32_t cx       = SX + SW / 2;
            const int32_t bottom_y = SY + SH - EP;
            bool solid = (spec->capture_style == CAMERA_ENTROPY_CAPTURE_SOLID);
            o->capture_ctrl = build_shutter(parent, cx, bottom_y, cap_icon, solid);
        }
        lv_obj_add_event_cb(o->capture_ctrl, action_clicked_cb, LV_EVENT_CLICKED, (void *)"capture");

        // CONFIRM: stylized full-width Accept button along the square's bottom.
        o->accept_btn = button(parent, spec->accept_label ? spec->accept_label : "", NULL);
        lv_obj_set_width(o->accept_btn, SW - 2 * EP);
        lv_obj_set_pos(o->accept_btn, SX + EP, SY + SH - EP - BH);
        lv_obj_add_event_cb(o->accept_btn, action_clicked_cb, LV_EVENT_CLICKED, (void *)"accept");
    } else {
        // Hardware / joystick: bottom-center instruction text, mode-exclusive per phase.
        if (spec->preview_instructions && spec->preview_instructions[0]) {
            make_bottom_text(parent, spec->preview_instructions, (uint32_t)BODY_FONT_COLOR,
                             SX, SY, SW, SH, &o->preview_shadow, &o->preview_lbl);
        }
        if (spec->confirm_instructions && spec->confirm_instructions[0]) {
            make_bottom_text(parent, spec->confirm_instructions, (uint32_t)BODY_FONT_COLOR,
                             SX, SY, SW, SH, &o->confirm_shadow, &o->confirm_lbl);
        }

        // Joystick keys ARE the controls in this mode (there are no on-screen ones),
        // so they are wired whether or not the host supplied instruction text.
        o->keypad_sink = attach_keypad_sink(parent, entropy_keypad_cb);
    }

    camera_entropy_overlay_set_phase(o, o->phase);
    return o;
}

void camera_entropy_overlay_set_phase(camera_entropy_overlay_t *o, camera_entropy_phase_t phase) {
    if (!o) return;
    o->phase = phase;

    // Keep the key handler's dispatch phase in step with the visuals (no-op in touch
    // mode, where there is no sink). Called from create()'s tail too, so the sink is
    // seeded with the initial phase.
    keypad_sink_set_phase(o->keypad_sink, phase);

    const bool preview   = (phase == CAMERA_ENTROPY_PHASE_PREVIEW);
    const bool capturing = (phase == CAMERA_ENTROPY_PHASE_CAPTURING);
    const bool confirm   = (phase == CAMERA_ENTROPY_PHASE_CONFIRM);

    // Frozen final frame shows only in CONFIRM (both modes); hiding it on reshoot
    // reveals the black gutters + live square underneath.
    show(o->confirm_image, confirm);

    // Transient capturing text shows only during CAPTURING (both modes).
    show(o->capturing_shadow, capturing);
    show(o->capturing_lbl,    capturing);

    if (o->mode == INPUT_MODE_TOUCH) {
        // Back button hidden during the brief capturing transient, else visible
        // (cancel in PREVIEW, reshoot in CONFIRM).
        show(o->back_btn,     !capturing);
        show(o->capture_ctrl,  preview);
        show(o->accept_btn,    confirm);
    } else {
        show(o->preview_shadow, preview);
        show(o->preview_lbl,    preview);
        show(o->confirm_shadow, confirm);
        show(o->confirm_lbl,    confirm);
    }
}

void camera_entropy_overlay_set_confirm_image(camera_entropy_overlay_t *o,
                                              const void *rgb565, int32_t w, int32_t h) {
    if (!o || !o->confirm_image || !rgb565 || w <= 0 || h <= 0) return;

    // Copy into an LVGL-heap buffer so the caller may reuse or free its own immediately,
    // then hand that copy to the owned path (lv_free is the matching deallocator). NB a
    // full-display frame will not fit a typical LVGL pool — see the header warning; such
    // callers should use ..._set_confirm_image_owned() directly.
    size_t bytes = (size_t)w * h * 2;   // RGB565, 2 bytes/pixel
    uint8_t *pixels = (uint8_t *)lv_malloc(bytes);
    if (!pixels) return;
    lv_memcpy(pixels, rgb565, bytes);
    camera_entropy_overlay_set_confirm_image_owned(o, pixels, w, h, NULL);
}

void camera_entropy_overlay_set_confirm_image_owned(camera_entropy_overlay_t *o,
                                                    void *rgb565, int32_t w, int32_t h,
                                                    void (*free_fn)(void *)) {
    // Own the buffer from here on, including on every rejection path — the contract is
    // that the caller has handed it over, so bailing out must release rather than leak.
    if (!rgb565) return;
    if (!o || !o->confirm_image || w <= 0 || h <= 0) {
        if (free_fn) free_fn(rgb565);
        else         lv_free(rgb565);
        return;
    }

    size_t bytes = (size_t)w * h * 2;   // RGB565, 2 bytes/pixel

    // Descriptor + deallocator, tied to the WIDGET (freed on LV_EVENT_DELETE, or
    // replaced just below on a re-capture) — tooling destroys the handle right after
    // build, so widget-scoped ownership is what keeps the frame alive.
    confirm_frame *f = (confirm_frame *)lv_malloc(sizeof(confirm_frame));
    if (!f) {
        if (free_fn) free_fn(rgb565);
        else         lv_free(rgb565);
        return;
    }
    lv_memzero(f, sizeof(*f));
    f->free_fn          = free_fn;
    f->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    f->dsc.header.cf    = LV_COLOR_FORMAT_RGB565;
    f->dsc.header.w     = (uint32_t)w;
    f->dsc.header.h     = (uint32_t)h;
    f->dsc.header.stride = (uint32_t)(w * 2);
    f->dsc.data_size    = (uint32_t)bytes;
    f->dsc.data         = (const uint8_t *)rgb565;

    // Release any prior frame (a re-capture after reshoot) before adopting this one.
    confirm_frame_release((confirm_frame *)lv_obj_get_user_data(o->confirm_image));
    lv_obj_set_user_data(o->confirm_image, f);

    lv_image_set_src(o->confirm_image, &f->dsc);

    // Center on the display: a full-display frame lands at (0,0); a smaller one is
    // centered over the black gutters.
    int32_t PW = lv_display_get_horizontal_resolution(NULL);
    int32_t PH = lv_display_get_vertical_resolution(NULL);
    lv_obj_set_pos(o->confirm_image, (PW - w) / 2, (PH - h) / 2);
}

void camera_entropy_overlay_destroy(camera_entropy_overlay_t *o) {
    if (!o) return;
    lv_free(o);
}
