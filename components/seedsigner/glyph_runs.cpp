#include "glyph_runs.h"
#include "font_registry.h"

#include "lvgl.h"
#include "src/misc/lv_text_ap.h"   // Arabic/Persian presentation-form mapper

#include "stb_glyph_metrics.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Parsed run-table model (mirrors lang-packs/<loc>/runs.json — see
// tools/i18n/build_fontpacks.py and docs/knowledge/complex-script-run-pipeline.md).
// ---------------------------------------------------------------------------
namespace {

struct RunGlyph {
    uint32_t gid;
    int32_t  x_advance, y_advance, x_offset, y_offset;  // font design units
};
using RunLine = std::vector<RunGlyph>;

struct RunEntry {
    std::vector<RunLine> lines;  // one per \n-split line (a run is shaped per line)
};

struct RunTable {
    int  upem = 1000;
    bool rtl  = false;
    // Keyed by the TRANSLATED string (msgstr) — that is what a finished label
    // holds. Multiple msgids can share one msgstr; the shaped result is identical,
    // so last-wins is harmless.
    std::unordered_map<std::string, RunEntry> by_text;
};

// Active table + a metrics handle over the subset bytes it was shaped against.
RunTable      g_table;
bool          g_have_table = false;

// Apply LVGL's Arabic/Persian presentation-form transform — the SAME one
// lv_label_set_text applies under LV_USE_ARABIC_PERSIAN_CHARS, so a finished
// Arabic label stores presentation forms (U+FExx), not the logical base letters
// the offline run table is keyed by. We key the table through this transform so
// the device matches what the label actually holds. It is a no-op for scripts
// with no Arabic-range codepoints (Devanagari/Thai/Latin), so one keying path
// serves every locale.
std::string ap_form(const std::string& s) {
    uint32_t n = lv_text_ap_calc_bytes_count(s.c_str());
    std::string out(n, '\0');
    lv_text_ap_proc(s.c_str(), &out[0]);
    if (!out.empty() && out.back() == '\0') out.pop_back();  // proc NUL-terminates
    return out;
}

// One stb metrics handle per resident subset buffer (all roles of a shaping
// locale share the same .ttf, so one handle serves every size). Rebuilt when the
// underlying bytes change; torn down on clear.
stb_metrics_t* g_metrics       = nullptr;
const uint8_t* g_metrics_bytes = nullptr;

stb_metrics_t* metrics_for(const uint8_t* bytes, size_t len) {
    if (!bytes) return nullptr;
    if (g_metrics && g_metrics_bytes == bytes) return g_metrics;
    if (g_metrics) { stb_metrics_destroy(g_metrics); g_metrics = nullptr; }
    g_metrics = stb_metrics_create(bytes, len);
    g_metrics_bytes = g_metrics ? bytes : nullptr;
    return g_metrics;
}

// Per-label state: the baked A8 alpha mask of the whole run block plus where the
// pen origin / typographic block sits inside it, so the draw event can place and
// align the mask against the label's content box. Owned by the label (freed on
// LV_EVENT_DELETE).
struct LabelRun {
    lv_draw_buf_t* mask = nullptr;  // A8 coverage of the full (multi-line) block
    int32_t margin      = 0;        // pen origin inset inside the mask (x and y)
    int32_t layout_w    = 0;        // typographic block width, px (for alignment)
    int32_t ascent      = 0;        // baseline offset of line 0 within the line box
    int32_t line_height = 0;        // px line advance, == lv_font_get_line_height
};

void free_label_run(LabelRun* run) {
    if (!run) return;
    if (run->mask) lv_draw_buf_destroy(run->mask);
    delete run;
}

// --- A8 "source-over" composite of one glyph mask into the run mask. ---------
// White-on-transparent coverage: each destination byte is the alpha; "over"
// makes stacked marks / the Nastaliq cascade accumulate correctly.
void blit_a8(uint8_t* dst, int dst_w, int dst_h, int dst_stride,
             const uint8_t* src, int src_w, int src_h, int src_stride,
             int gx, int gy) {
    for (int sy = 0; sy < src_h; ++sy) {
        int dy = gy + sy;
        if (dy < 0 || dy >= dst_h) continue;
        for (int sx = 0; sx < src_w; ++sx) {
            int dx = gx + sx;
            if (dx < 0 || dx >= dst_w) continue;
            uint8_t a = src[(size_t)sy * src_stride + sx];
            if (!a) continue;
            uint8_t& d = dst[(size_t)dy * dst_stride + dx];
            d = (uint8_t)(a + d * (255 - a) / 255);
        }
    }
}

// ---------------------------------------------------------------------------
// Bake a parsed run into an A8 alpha mask sized to the font's line box (plus a
// generous margin to catch left bearings and cursive overhang). Resolution is
// taken from the label's own font, so one run table serves every PX_MULTIPLIER.
// Returns a heap LabelRun on success (caller owns), or nullptr.
// ---------------------------------------------------------------------------
LabelRun* bake_run(const RunEntry& entry, const lv_font_t* font, int px, int upem,
                   lv_text_align_t align, bool rtl) {
    size_t len = 0;
    const uint8_t* bytes = seedsigner_registered_font_bytes(font, &len);
    stb_metrics_t* sm = metrics_for(bytes, len);
    if (!sm) return nullptr;

    const float scale = stb_metrics_scale(sm, (float)px);
    (void)upem;  // scale already folds upem (== px / units_per_em)

    const int line_height = lv_font_get_line_height(font);
    const int ascent      = line_height - font->base_line;  // baseline within line box
    const int margin      = line_height;                    // bearing/overhang slack

    const size_t nlines = entry.lines.size();
    if (nlines == 0) return nullptr;

    // Pass 1 — per-line typographic width (sum of advances) and the block width.
    std::vector<int> line_w(nlines, 0);
    int layout_w = 0;
    for (size_t li = 0; li < nlines; ++li) {
        long long adv = 0;
        for (const RunGlyph& g : entry.lines[li]) adv += g.x_advance;
        line_w[li] = (int)lround((double)adv * scale);
        if (line_w[li] > layout_w) layout_w = line_w[li];
    }

    const int mask_w = layout_w + 2 * margin;
    const int mask_h = (int)nlines * line_height + 2 * margin;
    if (mask_w <= 0 || mask_h <= 0) return nullptr;

    lv_draw_buf_t* mask = lv_draw_buf_create(mask_w, mask_h, LV_COLOR_FORMAT_A8, 0);
    if (!mask) return nullptr;
    const int stride = mask->header.stride;
    memset(mask->data, 0, (size_t)stride * mask_h);

    // Pass 2 — place each glyph by its pre-shaped GPOS offset/advance. Each line
    // is intra-aligned within layout_w (so multi-line center/right is correct);
    // the block as a whole is aligned to the label box later, at draw time.
    for (size_t li = 0; li < nlines; ++li) {
        int line_off;
        switch (align) {
            case LV_TEXT_ALIGN_CENTER: line_off = (layout_w - line_w[li]) / 2; break;
            case LV_TEXT_ALIGN_RIGHT:  line_off = (layout_w - line_w[li]);     break;
            case LV_TEXT_ALIGN_AUTO:   line_off = rtl ? (layout_w - line_w[li]) : 0; break;
            default:                   line_off = 0; break;  // LEFT
        }
        const int pen_x0   = margin + line_off;
        const int baseline = margin + (int)li * line_height + ascent;

        long long cx = 0, cy = 0;  // pen cursor in font design units
        for (const RunGlyph& g : entry.lines[li]) {
            lv_font_glyph_dsc_t gd;
            memset(&gd, 0, sizeof(gd));
            gd.resolved_font = (lv_font_t*)font;
            gd.gid.index = g.gid;
            gd.format = LV_FONT_GLYPH_FORMAT_A8;

            const lv_draw_buf_t* db = (const lv_draw_buf_t*)lv_font_get_glyph_bitmap(&gd, NULL);
            if (db && db->data && db->header.w > 0 && db->header.h > 0) {
                int ix0, iy0, ix1, iy1;
                stb_metrics_glyph_box(sm, (int)g.gid, scale, &ix0, &iy0, &ix1, &iy1);

                long ox = lround((double)(cx + g.x_offset) * scale);
                long oy = lround((double)(cy + g.y_offset) * scale);  // y-up positive
                int gx = pen_x0  + (int)ox + ix0;
                int gy = baseline + iy0 - (int)oy;  // HB y-up -> screen y-down

                blit_a8((uint8_t*)mask->data, mask_w, mask_h, stride,
                        (const uint8_t*)db->data, db->header.w, db->header.h,
                        db->header.stride, gx, gy);
            }
            lv_font_glyph_release_draw_data(&gd);  // cache entry: release immediately

            cx += g.x_advance;
            cy += g.y_advance;
        }
    }

    LabelRun* run = new LabelRun();
    run->mask        = mask;
    run->margin      = margin;
    run->layout_w    = layout_w;
    run->ascent      = ascent;
    run->line_height = line_height;
    return run;
}

// --- Draw event: paint the baked mask over the label's box, recolored to the
// label's LIVE text color (so focus highlighting still works). ----------------
void glyph_run_draw_cb(lv_event_t* e) {
    LabelRun* run = (LabelRun*)lv_event_get_user_data(e);
    lv_obj_t*  label = lv_event_get_target_obj(e);
    lv_layer_t* layer = lv_event_get_layer(e);
    if (!run || !run->mask || !layer) return;

    lv_area_t cc;
    lv_obj_get_content_coords(label, &cc);
    const int content_w = lv_area_get_width(&cc);

    lv_text_align_t align = lv_obj_get_style_text_align(label, LV_PART_MAIN);
    int text_x;
    switch (align) {
        case LV_TEXT_ALIGN_CENTER: text_x = cc.x1 + (content_w - run->layout_w) / 2; break;
        case LV_TEXT_ALIGN_RIGHT:  text_x = cc.x1 + (content_w - run->layout_w);     break;
        default:                   text_x = cc.x1; break;  // LEFT / AUTO-LTR
    }

    // Mask pen origin (col/row == margin) maps to (text_x, baseline); since line 0
    // baseline sits at margin+ascent inside the mask and LVGL would put it at
    // content_top+ascent, mask row 0 -> content_top - margin (margins cancel ascent).
    lv_area_t area;
    area.x1 = text_x  - run->margin;
    area.y1 = cc.y1   - run->margin;
    area.x2 = area.x1 + run->mask->header.w - 1;
    area.y2 = area.y1 + run->mask->header.h - 1;

    lv_draw_image_dsc_t img;
    lv_draw_image_dsc_init(&img);
    img.src         = run->mask;                                       // A8 alpha mask
    img.recolor     = lv_obj_get_style_text_color(label, LV_PART_MAIN);// live text color
    img.recolor_opa = LV_OPA_COVER;                                    // tint A8 mask fully
    img.opa         = LV_OPA_COVER;  // coverage comes from the mask, not the (suppressed) text_opa
    lv_draw_image(layer, &img, &area);
}

void glyph_run_delete_cb(lv_event_t* e) {
    free_label_run((LabelRun*)lv_event_get_user_data(e));
}

// --- Recursively attach runs to matching labels. ----------------------------
void attach_runs(lv_obj_t* obj) {
    // User text-entry stays on the codepoint path (ASCII; never shaped).
    if (lv_obj_check_type(obj, &lv_textarea_class)) return;

    if (lv_obj_check_type(obj, &lv_label_class)) {
        // Only labels drawn with a registered shaping-locale script font are
        // candidates — this cleanly skips icon / keyboard / pure-ASCII labels.
        const lv_font_t* font = lv_obj_get_style_text_font(obj, LV_PART_MAIN);
        int px = seedsigner_registered_font_px(font);
        if (px > 0) {
            const char* text = lv_label_get_text(obj);
            if (text && *text) {
                auto it = g_table.by_text.find(text);
                if (it != g_table.by_text.end()) {
                    lv_text_align_t align = lv_obj_get_style_text_align(obj, LV_PART_MAIN);
                    LabelRun* run = bake_run(it->second, font, px, g_table.upem,
                                             align, g_table.rtl);
                    if (run) {
                        // Suppress the (wrong) codepoint text; draw the run instead.
                        lv_obj_set_style_text_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN);
                        lv_obj_add_event_cb(obj, glyph_run_draw_cb, LV_EVENT_DRAW_MAIN_END, run);
                        lv_obj_add_event_cb(obj, glyph_run_delete_cb, LV_EVENT_DELETE, run);
                    }
                }
            }
        }
    }

    uint32_t n = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < n; ++i) attach_runs(lv_obj_get_child(obj, i));
}

}  // namespace

// ---------------------------------------------------------------------------
// Public seams.
// ---------------------------------------------------------------------------
bool seedsigner_set_glyph_runs(const char* runs_json, size_t len) {
    seedsigner_clear_glyph_runs();
    if (!runs_json || len == 0) return true;  // cleared

    json doc = json::parse(runs_json, runs_json + len, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded() || !doc.is_object()) {
        fprintf(stderr, "seedsigner_set_glyph_runs: bad JSON\n");
        return false;
    }

    g_table.upem = doc.value("upem", 1000);
    g_table.rtl  = (doc.value("direction", std::string("ltr")) == "rtl");

    if (!doc.contains("runs") || !doc["runs"].is_array()) {
        fprintf(stderr, "seedsigner_set_glyph_runs: no runs array\n");
        return false;
    }

    auto read_line = [](const json& jline, RunLine& out) {
        for (const json& jg : jline) {
            RunGlyph g;
            g.gid       = jg.value("gid", 0u);
            g.x_advance = jg.value("x_advance", 0);
            g.y_advance = jg.value("y_advance", 0);
            g.x_offset  = jg.value("x_offset", 0);
            g.y_offset  = jg.value("y_offset", 0);
            out.push_back(g);
        }
    };

    for (const json& e : doc["runs"]) {
        // v1 renders the "plain" kind (whole-string, multi-line). "segmented"
        // entries carry a {}-template; matching them needs device-side template
        // matching against the value-filled label text — a tracked follow-up, so
        // they are parsed-skipped here (the label falls back to codepoint text).
        if (e.value("kind", std::string()) != "plain") continue;
        if (!e.contains("text") || !e.contains("lines") || !e["lines"].is_array()) continue;

        RunEntry entry;
        for (const json& jline : e["lines"]) {
            RunLine line;
            read_line(jline, line);
            entry.lines.push_back(std::move(line));
        }
        // Key by the presentation-form transform so RTL labels (stored as
        // presentation forms by LVGL) match; a no-op for non-Arabic scripts.
        g_table.by_text[ap_form(e["text"].get<std::string>())] = std::move(entry);
    }

    g_have_table = true;
    return true;
}

void seedsigner_clear_glyph_runs() {
    g_table.by_text.clear();
    g_table.upem = 1000;
    g_table.rtl = false;
    g_have_table = false;
    if (g_metrics) { stb_metrics_destroy(g_metrics); g_metrics = nullptr; }
    g_metrics_bytes = nullptr;
}

void apply_glyph_runs_to_labels(struct _lv_obj_t* screen) {
    if (!g_have_table || !seedsigner_locale_uses_glyph_runs() || !screen) return;
    attach_runs((lv_obj_t*)screen);
}
