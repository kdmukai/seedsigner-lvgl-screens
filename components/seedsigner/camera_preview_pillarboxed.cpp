#include "camera_preview_pillarboxed.h"

#include "components.h"      // back_button()
#include "gui_constants.h"   // colors, scaled layout macros, fonts, SeedSignerIconConstants
#include "lvgl.h"

#include <string>

// ---------------------------------------------------------------------------
// Pillarboxed camera-preview chrome renderer (portable LVGL). See the header for the
// portrait-authoring / landscape-mount rationale. All coordinates below are PORTRAIT
// (the panel's native frame); the inline comments give the landscape-mounted reading
// via portrait (x,y) -> landscape (lx = PH - y, ly = x).
// ---------------------------------------------------------------------------

// ===========================================================================
// Pre-rotated text (portable canvas -> 90 deg CCW crop -> lv_image)
// ===========================================================================
// A label drawn normally in portrait reads sideways once the panel is mounted landscape.
// We render the text into a hidden RGB565 canvas (opaque black bg — it sits on a black
// pillar, so no alpha blend needed), crop to the ink bbox, rotate the crop 90 deg CCW,
// and show the result as an lv_image. The mount's 90 deg CW then nets to upright. The
// rotate is a pure transpose (lossless) over a tiny bbox, run only on set_progress()
// (a few times/sec, never per camera frame), so its cost is negligible. Buffers come
// from lv_malloc so this stays board-agnostic (renders on desktop tooling too).
//
// Lifetime: a rot_text is HEAP-allocated and OWNED BY ITS WIDGET — an LV_EVENT_DELETE
// callback on the image frees the struct + buffers when the parent tree is reaped. The
// image descriptor lives inside it, so the widget's source stays valid regardless of when
// the owning overlay handle is destroyed (the desktop tools free the handle immediately
// but keep the widgets to render — see camera_preview_pillarboxed_destroy).
struct rot_text {
    lv_obj_t      *canvas;    // hidden source canvas (horizontal text)
    lv_obj_t      *img;       // visible rotated image
    lv_image_dsc_t dsc;
    uint16_t      *src_buf;
    uint16_t      *rot_buf;
    int32_t        cw;        // canvas width  (px)
    int32_t        ch;        // canvas height (px)
    int32_t        center_x;  // portrait point the rotated result is centered on
    int32_t        center_y;
};

static void rot_text_delete_cb(lv_event_t *e) {
    rot_text *rt = (rot_text *)lv_event_get_user_data(e);
    lv_free(rt->src_buf);
    lv_free(rt->rot_buf);
    lv_free(rt);
}

// Tight bbox of non-black (non-zero) pixels in an RGB565 buffer. Returns false if empty.
static bool rot_text_bbox(const uint16_t *p, int32_t w, int32_t h,
                          int32_t *ox, int32_t *oy, int32_t *ow, int32_t *oh) {
    int32_t min_x = w, min_y = h, max_x = -1, max_y = -1;
    for (int32_t y = 0; y < h; y++) {
        for (int32_t x = 0; x < w; x++) {
            if (p[y * w + x] != 0) {
                if (x < min_x) min_x = x;
                if (x > max_x) max_x = x;
                if (y < min_y) min_y = y;
                if (y > max_y) max_y = y;
            }
        }
    }
    if (max_x < 0) return false;
    *ox = min_x; *oy = min_y; *ow = max_x - min_x + 1; *oh = max_y - min_y + 1;
    return true;
}

// Build a widget-owned rot_text sized to hold `sample` (the widest string it will show,
// e.g. "100%") at `font`, centered on portrait (center_x, center_y). NULL on alloc fail.
static rot_text *rot_text_create(lv_obj_t *parent, const char *sample, const lv_font_t *font,
                                 int32_t center_x, int32_t center_y) {
    rot_text *rt = (rot_text *)lv_malloc(sizeof(rot_text));
    if (!rt) return NULL;
    lv_memzero(rt, sizeof(*rt));

    lv_point_t sz = {0, 0};
    lv_text_get_size(&sz, sample, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    rt->cw = sz.x + 8;                 // small margin so glyph edges never clip
    rt->ch = sz.y + 8;
    rt->center_x = center_x;
    rt->center_y = center_y;

    size_t px = (size_t)rt->cw * rt->ch;
    rt->src_buf = (uint16_t *)lv_malloc(px * sizeof(uint16_t));
    rt->rot_buf = (uint16_t *)lv_malloc(px * sizeof(uint16_t));  // swapped dims, same count
    if (!rt->src_buf || !rt->rot_buf) {
        lv_free(rt->src_buf);
        lv_free(rt->rot_buf);
        lv_free(rt);
        return NULL;
    }
    lv_memzero(rt->src_buf, px * sizeof(uint16_t));
    lv_memzero(rt->rot_buf, px * sizeof(uint16_t));

    rt->canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(rt->canvas, rt->src_buf, rt->cw, rt->ch, LV_COLOR_FORMAT_RGB565);
    lv_obj_add_flag(rt->canvas, LV_OBJ_FLAG_HIDDEN);

    rt->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    rt->dsc.header.w = rt->ch;         // rotated: dims swapped
    rt->dsc.header.h = rt->cw;
    rt->dsc.data_size = px * sizeof(uint16_t);
    rt->dsc.data = (const uint8_t *)rt->rot_buf;

    rt->img = lv_image_create(parent);
    lv_image_set_src(rt->img, &rt->dsc);
    lv_obj_add_event_cb(rt->img, rot_text_delete_cb, LV_EVENT_DELETE, rt);  // widget owns rt
    return rt;
}

// Render `text` at `color`/`font`, crop, rotate 90 deg CCW, recenter on (center_x,center_y).
static void rot_text_set(rot_text *rt, const char *text, lv_color_t color,
                         const lv_font_t *font) {
    if (!rt) return;

    lv_canvas_fill_bg(rt->canvas, lv_color_black(), LV_OPA_COVER);
    lv_layer_t layer;
    lv_canvas_init_layer(rt->canvas, &layer);
    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.color = color;
    dsc.font = font;
    dsc.text = text;
    lv_area_t coords = {4, 4, rt->cw - 4, rt->ch - 4};
    lv_draw_label(&layer, &dsc, &coords);
    lv_canvas_finish_layer(rt->canvas, &layer);

    int32_t cx, cy, cw, ch;
    if (!rot_text_bbox(rt->src_buf, rt->cw, rt->ch, &cx, &cy, &cw, &ch)) {
        lv_obj_add_flag(rt->img, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    // 90 deg CCW of the cropped region: dst is ch (wide) x cw (tall).
    for (int32_t y = 0; y < ch; y++) {
        for (int32_t x = 0; x < cw; x++) {
            rt->rot_buf[(cw - 1 - x) * ch + y] = rt->src_buf[(cy + y) * rt->cw + (cx + x)];
        }
    }

    rt->dsc.header.w = ch;
    rt->dsc.header.h = cw;
    rt->dsc.data_size = (size_t)ch * cw * sizeof(uint16_t);
    lv_image_set_src(rt->img, &rt->dsc);   // re-point: dims changed
    lv_obj_set_pos(rt->img, rt->center_x - ch / 2, rt->center_y - cw / 2);
    lv_obj_remove_flag(rt->img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(rt->img);
}

// ===========================================================================
// Pillarboxed chrome
// ===========================================================================

struct camera_preview_pillarboxed {
    lv_obj_t *back_btn;        // CHEVRON_DOWN back button, bottom pillar (landscape top-left)
    lv_obj_t *bar_container;   // wide very-dark-gray container (the "progress zone")
    lv_obj_t *bar_track;       // narrow track (inactive gray) inside the container
    lv_obj_t *bar_fill;        // narrow fill (green), grows landscape-bottom -> top
    lv_obj_t *dot;             // most-recent-frame status dot (center of the left pillar)
    rot_text *pct;             // pre-rotated "NN%" readout (widget-owned; may be NULL)

    int32_t   fill_lo;         // portrait x: landscape TOP end of the track (empty)
    int32_t   fill_hi;         // portrait x: landscape BOTTOM end of the track (fill anchor)
    bool      scanning;
    bool      meter_shown;     // latched true once an ANIMATED (intermediate) frame is seen —
                               // the meter (container/track/fill/percent) stays hidden until
                               // then, and never shows for a static QR (straight to 100%).
};

static void style_rect(lv_obj_t *o, uint32_t color, lv_opa_t opa, int32_t radius) {
    lv_obj_remove_flag(o, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(o, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(o, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(o, radius, LV_PART_MAIN);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, opa, LV_PART_MAIN);
}

// Three always-visible dot states, so "no frame yet" reads as a distinct state rather than
// a blank gap: EMPTY ring (no/undecoded frame), solid GREEN (new part), solid GRAY (repeat
// part). The outline ring is permanent (set at creation), so the empty state is a hollow
// ring; the filled states cover it. Never hides the dot (scanning visibility is set_scanning's
// job).
static void apply_dot_status(camera_preview_pillarboxed *o, camera_overlay_frame_status_t s) {
    switch (s) {
        case CAMERA_OVERLAY_FRAME_ADDED:     // new part -> solid green
            lv_obj_set_style_bg_color(o->dot, lv_color_hex((uint32_t)SUCCESS_COLOR), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(o->dot, LV_OPA_COVER, LV_PART_MAIN);
            break;
        case CAMERA_OVERLAY_FRAME_REPEATED:  // repeat part -> solid gray
            lv_obj_set_style_bg_color(o->dot, lv_color_hex((uint32_t)INACTIVE_COLOR), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(o->dot, LV_OPA_COVER, LV_PART_MAIN);
            break;
        default:                             // NONE / MISS -> empty ring
            lv_obj_set_style_bg_opa(o->dot, LV_OPA_TRANSP, LV_PART_MAIN);
            break;
    }
}

// The meter = container + track + fill + percent, shown/hidden together. It stays hidden
// until an animated frame is seen (see set_progress); the dot + back button are independent
// and always visible while scanning.
static void pb_meter_set_hidden(camera_preview_pillarboxed *o, bool hidden) {
    lv_obj_t *parts[3] = { o->bar_container, o->bar_track, o->bar_fill };
    for (int i = 0; i < 3; i++) {
        if (hidden) lv_obj_add_flag(parts[i], LV_OBJ_FLAG_HIDDEN);
        else        lv_obj_remove_flag(parts[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (o->pct) {
        if (hidden) lv_obj_add_flag(o->pct->img, LV_OBJ_FLAG_HIDDEN);
        else        lv_obj_remove_flag(o->pct->img, LV_OBJ_FLAG_HIDDEN);
    }
}

// Grow the fill from the landscape-bottom anchor toward the top; repaint the percent. Snap
// (no glide): a gliding vertical fill would repaint the rotated percent every frame.
static void pb_update_meter(camera_preview_pillarboxed *o, int percent) {
    int32_t bar_len = o->fill_hi - o->fill_lo;
    int32_t w = (int32_t)((int64_t)bar_len * percent / 100);
    lv_obj_set_width(o->bar_fill, w);
    lv_obj_set_x(o->bar_fill, o->fill_hi - w);
    std::string text = std::to_string(percent) + "%";
    rot_text_set(o->pct, text.c_str(), lv_color_hex((uint32_t)BODY_FONT_COLOR), &BUTTON_FONT);
}

camera_preview_pillarboxed_t *camera_preview_pillarboxed_create(
    lv_obj_t *parent, const camera_preview_pillarboxed_spec_t *spec) {
    if (!parent || !spec) return NULL;

    camera_preview_pillarboxed_t *o =
        (camera_preview_pillarboxed_t *)lv_malloc(sizeof(camera_preview_pillarboxed_t));
    if (!o) return NULL;
    lv_memzero(o, sizeof(*o));

    const int32_t SX = spec->square_x, SY = spec->square_y;
    const int32_t SW = spec->square_w, SH = spec->square_h;
    const int32_t EP = EDGE_PADDING;
    const int     px = active_profile().px_multiplier;
    const int32_t PH = lv_display_get_vertical_resolution(NULL);   // portrait long axis

    // --- Pillar strips (black) -------------------------------------------
    // The camera fills the square; the strips above (-> landscape RIGHT pillar) and below
    // (-> landscape LEFT pillar) are static black. Painted first so they sit behind the
    // chrome. Each strip self-covers so stale content never shows through.
    struct { int32_t x, y, w, h; } strips[2] = {
        { SX, 0,        SW, SY },                 // top strip  -> landscape right pillar
        { SX, SY + SH,  SW, PH - (SY + SH) },     // bottom strip -> landscape left pillar
    };
    for (int i = 0; i < 2; i++) {
        if (strips[i].h <= 0) continue;
        lv_obj_t *bg = lv_obj_create(parent);
        lv_obj_set_size(bg, strips[i].w, strips[i].h);
        lv_obj_set_pos(bg, strips[i].x, strips[i].y);
        style_rect(bg, 0x000000, LV_OPA_COVER, 0);
    }

    // --- Percent readout (pre-rotated, landscape TOP of the pillar) ------
    // A SEPARATE element (never inside the bar container, so decimal precision can't force
    // the container wider). Built + rendered FIRST so the bar can start a precise
    // COMPONENT_PADDING below the percent's actual INK: rot_text crops to the ink bbox, so
    // the font line height overshoots the visible glyph and would leave the crop's slack as
    // extra gap. Centered across the pillar (portrait y = SY/2); ink top anchored ~EP from
    // the top frame border. set_progress (end of create) re-renders at the corrected center.
    o->pct = rot_text_create(parent, "100%", &BUTTON_FONT, EP, SY / 2);
    rot_text_set(o->pct, "100%", lv_color_hex((uint32_t)BODY_FONT_COLOR), &BUTTON_FONT);
    int32_t pct_h = 0;
    if (o->pct) {
        lv_obj_update_layout(o->pct->img);
        pct_h = lv_obj_get_width(o->pct->img);   // ink portrait-X (landscape-vertical) extent
        o->pct->center_x = EP + pct_h / 2;        // anchor ink top at EP
    }
    const int32_t pct_bottom = EP + pct_h;

    // --- Vertical progress bar (top strip = landscape right pillar) -------
    // The landscape overlay's proportions, rotated 90 deg: a WIDE very-dark-gray container
    // (the progress "zone") holds a NARROW track (lighter gray) + fill (green). Long axis =
    // portrait X (landscape vertical); the fill is anchored at the high-x end (landscape
    // bottom) and grows toward low-x (landscape top). All centered on the pillar's short
    // axis (portrait Y ~ SY/2). Container starts COMPONENT_PADDING below the percent ink.
    const int32_t cont_lo    = pct_bottom + COMPONENT_PADDING;
    const int32_t cont_hi    = SX + SW - EP;                // landscape bottom end (EP from the frame)
    const int32_t cont_thick = BUTTON_HEIGHT;               // portrait Y (landscape width) — wide
    const int32_t cont_y     = SY / 2 - cont_thick / 2;

    o->bar_container = lv_obj_create(parent);
    lv_obj_set_size(o->bar_container, cont_hi - cont_lo, cont_thick);
    lv_obj_set_pos(o->bar_container, cont_lo, cont_y);
    style_rect(o->bar_container, 0x1a1a1a, LV_OPA_COVER, BUTTON_RADIUS);  // rounded rect, like the landscape container

    const int32_t inset      = COMPONENT_PADDING;           // track inset from the container ends
    const int32_t trk_thick  = LIST_ITEM_PADDING;           // portrait Y — narrow slice
    const int32_t trk_y      = SY / 2 - trk_thick / 2;
    const int32_t trk_radius = trk_thick / 2;
    o->fill_lo = cont_lo + inset;                           // landscape top end of the track
    o->fill_hi = cont_hi - inset;                           // landscape bottom (fill anchor)

    o->bar_track = lv_obj_create(parent);
    lv_obj_set_size(o->bar_track, o->fill_hi - o->fill_lo, trk_thick);
    lv_obj_set_pos(o->bar_track, o->fill_lo, trk_y);
    style_rect(o->bar_track, (uint32_t)INACTIVE_COLOR, LV_OPA_COVER, trk_radius);

    o->bar_fill = lv_obj_create(parent);
    lv_obj_set_size(o->bar_fill, 0, trk_thick);
    lv_obj_set_pos(o->bar_fill, o->fill_hi, trk_y);         // zero-width at the anchor
    style_rect(o->bar_fill, (uint32_t)GREEN_INDICATOR_COLOR, LV_OPA_COVER, trk_radius);

    // --- Status dot (center of the bottom strip = middle of the landscape LEFT pillar,
    // where the eye rests) -----------------------------------------------
    int32_t dot = (14 * px) / 100;
    if (dot < 2) dot = 2;
    int32_t dot_cx = SX + SW / 2;              // portrait: horizontal center of the strip
    int32_t dot_cy = (SY + SH + PH) / 2;       // portrait: vertical center of the bottom strip
    o->dot = lv_obj_create(parent);
    lv_obj_set_size(o->dot, dot, dot);
    lv_obj_set_pos(o->dot, dot_cx - dot / 2, dot_cy - dot / 2);
    // Permanent outline ring + a transparent fill: the EMPTY state (no frame) reads as a
    // hollow ring; apply_dot_status fills it green/gray on decode. Starts empty.
    style_rect(o->dot, (uint32_t)INACTIVE_COLOR, LV_OPA_TRANSP, dot / 2);
    int32_t dot_border = px / 100;   // hairline ring (~2px at the 480 profile) — light + elegant
    if (dot_border < 1) dot_border = 1;
    lv_obj_set_style_border_width(o->dot, dot_border, LV_PART_MAIN);
    lv_obj_set_style_border_color(o->dot, lv_color_hex((uint32_t)INACTIVE_COLOR), LV_PART_MAIN);
    lv_obj_set_style_border_opa(o->dot, LV_OPA_COVER, LV_PART_MAIN);

    // --- Back button (bottom strip = landscape LEFT pillar) --------------
    // EP from the portrait bottom + left edges = EP from the landscape top-left corner.
    // CHEVRON_DOWN reads as "left" once mounted (no rotation needed).
    o->back_btn = back_button(parent, LV_ALIGN_BOTTOM_LEFT, EP, -EP,
                              SeedSignerIconConstants::CHEVRON_DOWN);

    // --- Initial state ---------------------------------------------------
    // Dot + back button show while scanning; the METER stays hidden until an animated frame
    // arrives (set_progress). A spec with an initial progress > 0 (the desktop tool rendering a
    // mid/complete state) reveals it up front; the device starts at 0 and reveals at runtime.
    o->meter_shown = false;
    camera_preview_pillarboxed_set_scanning(o, spec->scanning_active);
    apply_dot_status(o, spec->frame_status);
    if (spec->scanning_active && spec->progress_percent > 0) {
        o->meter_shown = true;
        pb_meter_set_hidden(o, false);
        pb_update_meter(o, spec->progress_percent > 100 ? 100 : spec->progress_percent);
    }
    return o;
}

void camera_preview_pillarboxed_set_scanning(camera_preview_pillarboxed_t *o, bool active) {
    if (!o) return;
    o->scanning = active;
    if (active) {
        lv_obj_remove_flag(o->dot, LV_OBJ_FLAG_HIDDEN);  // dot shows throughout (empty ring first)
        pb_meter_set_hidden(o, !o->meter_shown);         // meter follows its own reveal latch
    } else {
        lv_obj_add_flag(o->dot, LV_OBJ_FLAG_HIDDEN);
        pb_meter_set_hidden(o, true);
    }
}

void camera_preview_pillarboxed_set_progress(camera_preview_pillarboxed_t *o,
                                             int percent,
                                             camera_overlay_frame_status_t frame_status) {
    if (!o) return;
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;

    // The dot always tracks the latest frame; the meter is gated on animation.
    apply_dot_status(o, frame_status);

    // Reveal the meter the first time we see an intermediate part (0 < percent < 100) — the
    // signature of an ANIMATED QR. A static QR decodes in one frame and reports 100% straight
    // away, so it never trips this and the meter never appears. The latch stays set once
    // revealed, so the meter remains up (and fills) through the final 100% of an animated scan.
    if (!o->meter_shown && percent > 0 && percent < 100) {
        o->meter_shown = true;
        pb_meter_set_hidden(o, false);
    }
    if (o->meter_shown) {
        pb_update_meter(o, percent);
    }
}

void camera_preview_pillarboxed_destroy(camera_preview_pillarboxed_t *o) {
    if (!o) return;
    // Free ONLY the handle. The widgets (incl. the rotated-percent image + its buffers,
    // freed by the image's LV_EVENT_DELETE cb) belong to the parent tree and outlive this
    // — the desktop tools destroy the handle immediately but keep the widgets to render.
    lv_free(o);
}
