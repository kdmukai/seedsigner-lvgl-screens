#include "locale_picker.h"
#include "components.h"     // button_text_label, label_subset_text_width
#include "gui_constants.h"  // COMPONENT_PADDING (gap between English name and native)

#include "lvgl.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

// ---------------------------------------------------------------------------
// The endonym image provider (set by the host before building the picker). Same
// seam/signature as ss_load_locale's provider: the host's existing filesystem /
// SD / staging provider serves "endonym_<h>.bin" exactly as it serves ".ttf".
// ---------------------------------------------------------------------------
static ss_pack_provider_t g_provider = nullptr;
static void*              g_provider_user = nullptr;

void locale_picker_set_image_provider(ss_pack_provider_t provider, void* user) {
    g_provider = provider;
    g_provider_user = user;
}

namespace {

// Parse a self-describing "SSA8" A8-alpha blob (see tools/i18n/render_endonym.py)
// into an LVGL A8 draw buffer. FAILS CLOSED — returns nullptr on any short read,
// bad magic/version, or a payload smaller than width*height — so a truncated or
// half-copied pack yields no image rather than an out-of-bounds read. The row
// then keeps its live text; discovery never crashes on a bad pack.
lv_draw_buf_t* parse_ssa8(const uint8_t* b, size_t len) {
    if (!b || len < 12) return nullptr;
    if (b[0] != 'S' || b[1] != 'S' || b[2] != 'A' || b[3] != '8') return nullptr;
    if (b[4] != 1) return nullptr;                       // version
    const int w = (int)(b[6] | (b[7] << 8));
    const int h = (int)(b[8] | (b[9] << 8));
    if (w <= 0 || h <= 0) return nullptr;
    if (len < (size_t)12 + (size_t)w * (size_t)h) return nullptr;

    lv_draw_buf_t* buf = lv_draw_buf_create(w, h, LV_COLOR_FORMAT_A8, 0);
    if (!buf) return nullptr;

    // The blob is tightly packed (stride == width); the draw buf may pad rows to
    // its own alignment, so copy row by row into the buffer's stride.
    const uint8_t* src = b + 12;
    uint8_t* dst = (uint8_t*)buf->data;
    const int stride = buf->header.stride;
    for (int row = 0; row < h; ++row) {
        memcpy(dst + (size_t)row * stride, src + (size_t)row * w, (size_t)w);
    }
    return buf;
}

// Paint the native-script endonym image on the row's single line, immediately
// AFTER the row's English-name text (the row reads "English  नेटिव"), recolored to
// the row's LIVE text color (the same A8-mask-with-recolor mechanism
// glyph_run_draw_cb uses) so it inverts with the focus highlight. Clipped to the
// button so an over-wide row can't bleed past its edge.
void endonym_draw_cb(lv_event_t* e) {
    lv_draw_buf_t* buf   = (lv_draw_buf_t*)lv_event_get_user_data(e);
    lv_obj_t*      btn   = lv_event_get_target_obj(e);
    lv_layer_t*    layer = lv_event_get_layer(e);
    if (!buf || !btn || !layer) return;

    lv_area_t bc;
    lv_obj_get_content_coords(btn, &bc);
    const int img_w = buf->header.w;
    const int img_h = buf->header.h;

    // Start where the English-name text ends: the label's content left plus its
    // measured text width plus a small gap. Take the row's live text color from the
    // label — button_set_active flips it on focus, so the image tracks. Fall back to
    // the button box if the label is somehow absent.
    lv_obj_t* lbl = button_text_label(btn);
    int x = bc.x1;
    lv_color_t color = lv_obj_get_style_text_color(btn, LV_PART_MAIN);
    if (lbl) {
        lv_area_t lc;
        lv_obj_get_content_coords(lbl, &lc);
        const lv_font_t* font = lv_obj_get_style_text_font(lbl, LV_PART_MAIN);
        const int32_t text_w = label_subset_text_width(lbl, font);
        x = lc.x1 + text_w + COMPONENT_PADDING;
        color = lv_obj_get_style_text_color(lbl, LV_PART_MAIN);
    }
    const int y = bc.y1 + (lv_area_get_height(&bc) - img_h) / 2;

    lv_area_t area;
    area.x1 = x;
    area.y1 = y;
    area.x2 = x + img_w - 1;
    area.y2 = y + img_h - 1;

    lv_draw_image_dsc_t img;
    lv_draw_image_dsc_init(&img);
    img.src         = buf;              // A8 alpha coverage
    img.recolor     = color;           // tint to the row's live (focus-tracking) color
    img.recolor_opa = LV_OPA_COVER;
    img.opa         = LV_OPA_COVER;     // coverage comes from the A8 alpha, not opa

    // Clip HORIZONTALLY to the button content box so a wide row (long English name
    // + native) can't paint over the scrollbar / neighbouring columns. Keep the
    // original VERTICAL clip so a tall glyph's bottom anti-aliased row is never cut
    // (e.g. the base strokes of 한국어 / 日本語) — the image is a hair taller than the
    // content box and box-clipping it would shave the last row.
    lv_area_t clip_ori = layer->_clip_area;
    lv_area_t clipped = clip_ori;
    if (clipped.x1 < bc.x1) clipped.x1 = bc.x1;
    if (clipped.x2 > bc.x2) clipped.x2 = bc.x2;
    if (clipped.x1 > clipped.x2) return;  // nothing visible horizontally
    layer->_clip_area = clipped;
    lv_draw_image(layer, &img, &area);
    layer->_clip_area = clip_ori;
}

void endonym_delete_cb(lv_event_t* e) {
    lv_draw_buf_t* buf = (lv_draw_buf_t*)lv_event_get_user_data(e);
    if (buf) lv_draw_buf_destroy(buf);
}

}  // namespace

bool locale_picker_attach_endonym(lv_obj_t* btn, const char* locale,
                                  const char* image_file) {
    if (!btn || !locale || !image_file || !g_provider) return false;

    lv_obj_t* lbl = button_text_label(btn);
    if (!lbl) return false;

    const uint8_t* bytes = nullptr;
    size_t len = 0;
    if (!g_provider(locale, image_file, &bytes, &len, g_provider_user) || !bytes || len == 0) {
        return false;
    }
    lv_draw_buf_t* buf = parse_ssa8(bytes, len);  // copies; provider bytes may now go stale
    if (!buf) return false;

    // The label keeps its live English-name text; the native-script image is drawn
    // right after it (endonym_draw_cb reads the label's text width + live color).
    lv_obj_add_event_cb(btn, endonym_draw_cb, LV_EVENT_DRAW_MAIN_END, buf);
    lv_obj_add_event_cb(btn, endonym_delete_cb, LV_EVENT_DELETE, buf);
    return true;
}
