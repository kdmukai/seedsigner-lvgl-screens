/*
 * shape_spike — THROWAWAY on-device half of the offline-HarfBuzz shaping spike.
 * See shape_spike.h and docs/complex-script-shaping-spike-plan.md.
 */

#include "shape_spike.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

#include <png.h>
#include <nlohmann/json.hpp>

#include "lvgl.h"
#include "src/misc/lv_text_ap.h"   // presentation-form mapper (the current path)

#include "stb_glyph_metrics.h"

using json = nlohmann::json;

// One subset .ttf serves every glyph; a generous cache avoids re-rasterizing.
#define SPIKE_TTF_CACHE 512

// ---------------------------------------------------------------------------
// Run-table model (mirrors the RUN FORMAT documented in spike_shape.py).
// ---------------------------------------------------------------------------
struct SpikeGlyph {
    uint32_t gid;
    int32_t  x_advance, y_advance, x_offset, y_offset;
    uint32_t cluster;
};

struct SpikeLine {
    std::string name;
    std::string font_file;
    std::string text;        // utf-8 source (for the presentation-form path)
    int  upem = 1000;
    int  px = 48;
    uint8_t direction = 0;   // 0 = LTR, 1 = RTL
    std::vector<SpikeGlyph> glyphs;
};

// ---------------------------------------------------------------------------
// Little helpers.
// ---------------------------------------------------------------------------
static bool read_file(const std::string &path, std::vector<uint8_t> &out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return true;
}

// Bounds-checked little-endian cursor over the run blob.
struct Reader {
    const uint8_t *p;
    size_t n, i = 0;
    bool ok = true;
    Reader(const uint8_t *p_, size_t n_) : p(p_), n(n_) {}
    bool need(size_t k) { if (i + k > n) { ok = false; return false; } return true; }
    uint8_t  u8()  { if (!need(1)) return 0; return p[i++]; }
    uint16_t u16() { if (!need(2)) return 0; uint16_t v = p[i] | (p[i + 1] << 8); i += 2; return v; }
    uint32_t u32() { if (!need(4)) return 0; uint32_t v = (uint32_t)p[i] | ((uint32_t)p[i+1]<<8) | ((uint32_t)p[i+2]<<16) | ((uint32_t)p[i+3]<<24); i += 4; return v; }
    int32_t  i32() { return (int32_t)u32(); }
    std::string str(uint16_t len) {
        if (!need(len)) return std::string();
        std::string s((const char *)(p + i), len);
        i += len;
        return s;
    }
};

static bool parse_runs(const std::vector<uint8_t> &blob, std::vector<SpikeLine> &lines) {
    Reader r(blob.data(), blob.size());
    if (r.str(4) != "SSR1") { fprintf(stderr, "shape_spike: bad magic\n"); return false; }
    uint32_t line_count = r.u32();
    for (uint32_t li = 0; li < line_count && r.ok; ++li) {
        SpikeLine ln;
        ln.name      = r.str(r.u16());
        ln.font_file = r.str(r.u16());
        ln.text      = r.str(r.u16());
        ln.upem      = r.u16();
        ln.px        = r.u16();
        ln.direction = r.u8();
        (void)r.u8();  // reserved
        uint32_t gc = r.u32();
        ln.glyphs.reserve(gc);
        for (uint32_t gi = 0; gi < gc && r.ok; ++gi) {
            SpikeGlyph g;
            g.gid       = r.u32();
            g.x_advance = r.i32();
            g.y_advance = r.i32();
            g.x_offset  = r.i32();
            g.y_offset  = r.i32();
            g.cluster   = r.u32();
            ln.glyphs.push_back(g);
        }
        lines.push_back(std::move(ln));
    }
    if (!r.ok) { fprintf(stderr, "shape_spike: truncated run table\n"); return false; }
    return true;
}

// White-source-over compositing of an A8 coverage mask onto an RGB24 canvas.
// (White-on-black ⇒ each channel equals the coverage; "over" handles the
// overlap of stacked marks / the Nastaliq cascade correctly.)
static void blit_a8_white(std::vector<uint8_t> &rgb, int W, int H,
                          const uint8_t *a8, int w, int h, int stride, int gx, int gy) {
    for (int sy = 0; sy < h; ++sy) {
        int dy = gy + sy;
        if (dy < 0 || dy >= H) continue;
        for (int sx = 0; sx < w; ++sx) {
            int dx = gx + sx;
            if (dx < 0 || dx >= W) continue;
            uint8_t a = a8[(size_t)sy * stride + sx];
            if (!a) continue;
            size_t di = ((size_t)dy * W + dx) * 3;
            for (int c = 0; c < 3; ++c) {
                int d = rgb[di + c];
                rgb[di + c] = (uint8_t)(a + d * (255 - a) / 255);
            }
        }
    }
}

static void decode_utf8(const char *s, std::vector<uint32_t> &out) {
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        uint32_t cp; int n;
        if (*p < 0x80)      { cp = *p;        n = 1; }
        else if (*p < 0xE0) { cp = *p & 0x1F; n = 2; }
        else if (*p < 0xF0) { cp = *p & 0x0F; n = 3; }
        else                { cp = *p & 0x07; n = 4; }
        for (int k = 1; k < n; ++k) {
            if ((p[k] & 0xC0) != 0x80) { n = k; cp = 0; break; }
            cp = (cp << 6) | (p[k] & 0x3F);
        }
        if (cp) out.push_back(cp);
        p += n;
    }
}

static int write_png_rgb24(const std::string &path, const std::vector<uint8_t> &rgb, int W, int H) {
    FILE *fp = fopen(path.c_str(), "wb");
    if (!fp) return -1;
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) { fclose(fp); return -1; }
    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_write_struct(&png, NULL); fclose(fp); return -1; }
    if (setjmp(png_jmpbuf(png))) { png_destroy_write_struct(&png, &info); fclose(fp); return -1; }
    png_init_io(png, fp);
    png_set_IHDR(png, info, (png_uint_32)W, (png_uint_32)H, 8, PNG_COLOR_TYPE_RGB,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    std::vector<png_bytep> rows((size_t)H);
    for (int y = 0; y < H; ++y) rows[y] = (png_bytep)(rgb.data() + (size_t)y * W * 3);
    png_write_image(png, rows.data());
    png_write_end(png, NULL);
    png_destroy_write_struct(&png, &info);
    fclose(fp);
    return 0;
}

// ---------------------------------------------------------------------------
// Render path A — the NEW path: drive the existing tiny_ttf rasterizer by
// glyph-id and place each glyph by the pre-shaped run's GPOS offsets/advances.
// ---------------------------------------------------------------------------
static void render_glyph_run(const SpikeLine &ln, lv_font_t *font, stb_metrics_t *sm,
                             std::vector<uint8_t> &rgb, int W, int H, int baseline, int margin) {
    const float scale = stb_metrics_scale(sm, (float)ln.px);
    long long cx = 0, cy = 0;  // pen cursor, font design units

    for (const SpikeGlyph &g : ln.glyphs) {
        // The shim: a glyph dsc the draw seam consumes verbatim — gid + A8, no
        // codepoint lookup. tiny_ttf rasterizes/caches keyed on (gid, size).
        lv_font_glyph_dsc_t gd;
        memset(&gd, 0, sizeof(gd));
        gd.resolved_font = font;
        gd.gid.index = g.gid;
        gd.format = LV_FONT_GLYPH_FORMAT_A8;

        const lv_draw_buf_t *db = (const lv_draw_buf_t *)lv_font_get_glyph_bitmap(&gd, NULL);
        if (db && db->data && db->header.w > 0 && db->header.h > 0) {
            // Bounding box (baseline-relative, y-down) from the metrics-only stb
            // re-init; the same stb+scale tiny_ttf rasterized with, so the box
            // lines up with the cached bitmap.
            int ix0, iy0, ix1, iy1;
            stb_metrics_glyph_box(sm, (int)g.gid, scale, &ix0, &iy0, &ix1, &iy1);

            long ox = lround((double)(cx + g.x_offset) * scale);
            long oy = lround((double)(cy + g.y_offset) * scale);  // y-up positive
            int gx = margin + (int)ox + ix0;
            int gy = baseline + iy0 - (int)oy;

            blit_a8_white(rgb, W, H, db->data, db->header.w, db->header.h,
                          db->header.stride, gx, gy);
        }
        lv_font_glyph_release_draw_data(&gd);

        cx += g.x_advance;
        cy += g.y_advance;
    }
}

// ---------------------------------------------------------------------------
// Render path B — the CURRENT path: map base letters to Arabic presentation
// forms (LVGL's lv_text_ap), then draw those codepoints left-to-right through
// the normal tiny_ttf codepoint lookup. No GPOS, so Nastaliq collapses to flat
// Naskh (the negative control); fa still joins correctly (the regression check).
// ---------------------------------------------------------------------------
static void render_presentation(const std::string &text, bool rtl, lv_font_t *font,
                                std::vector<uint8_t> &rgb, int W, int H, int baseline, int margin) {
    uint32_t need = lv_text_ap_calc_bytes_count(text.c_str());
    std::vector<char> buf(need + 1, 0);
    lv_text_ap_proc(text.c_str(), buf.data());

    std::vector<uint32_t> cps;
    decode_utf8(buf.data(), cps);
    // The run path already emits visual order; here we reproduce LVGL's bidi for
    // a pure-RTL string by reversing the logical presentation-form sequence.
    if (rtl) std::reverse(cps.begin(), cps.end());

    long pen = margin;
    for (uint32_t cp : cps) {
        lv_font_glyph_dsc_t gd;
        memset(&gd, 0, sizeof(gd));
        if (!lv_font_get_glyph_dsc(font, &gd, cp, 0)) continue;
        if (gd.box_w > 0 && gd.box_h > 0) {
            const lv_draw_buf_t *db = (const lv_draw_buf_t *)lv_font_get_glyph_bitmap(&gd, NULL);
            if (db && db->data) {
                int gx = (int)pen + gd.ofs_x;
                int gy = baseline - gd.box_h - gd.ofs_y;
                blit_a8_white(rgb, W, H, db->data, db->header.w, db->header.h,
                              db->header.stride, gx, gy);
            }
            lv_font_glyph_release_draw_data(&gd);
        }
        pen += gd.adv_w;
    }
}

// ---------------------------------------------------------------------------
// Orchestrator.
// ---------------------------------------------------------------------------
int run_shape_spike(const char *spike_dir) {
    std::string dir = spike_dir;

    // Canvas geometry — read from meta.json so it cannot drift from the
    // reference renders spike_shape.py produced.
    std::vector<uint8_t> meta_bytes;
    if (!read_file(dir + "/meta.json", meta_bytes)) {
        fprintf(stderr, "shape_spike: missing %s/meta.json (run spike_shape.py first)\n", spike_dir);
        return 1;
    }
    json meta = json::parse(meta_bytes.begin(), meta_bytes.end(), nullptr, false);
    if (meta.is_discarded()) { fprintf(stderr, "shape_spike: bad meta.json\n"); return 1; }
    const int W = meta.value("canvas_w", 800);
    const int H = meta.value("canvas_h", 240);
    const int baseline = meta.value("baseline_y", 160);
    const int margin = meta.value("margin_x", 24);

    std::vector<uint8_t> blob;
    if (!read_file(dir + "/spike_runs.bin", blob)) {
        fprintf(stderr, "shape_spike: missing %s/spike_runs.bin\n", spike_dir);
        return 1;
    }
    std::vector<SpikeLine> lines;
    if (!parse_runs(blob, lines)) return 1;

    // The current path's font: Noto Sans Arabic (Naskh), the family today's
    // presentation-form path would use for any Arabic-script locale. Loaded once
    // from the vendored assets (repo root = spike_dir/../../..).
    std::string assets = dir + "/../../../components/seedsigner/assets";
    std::vector<uint8_t> naskh_bytes;
    bool have_naskh = read_file(assets + "/NotoSansAR-Regular.ttf", naskh_bytes);

    int rc = 0;
    for (const SpikeLine &ln : lines) {
        std::vector<uint8_t> font_bytes;
        if (!read_file(dir + "/" + ln.font_file, font_bytes)) {
            fprintf(stderr, "[%s] missing subset font %s\n", ln.name.c_str(), ln.font_file.c_str());
            rc = 1;
            continue;
        }

        lv_font_t *font = lv_tiny_ttf_create_data_ex(font_bytes.data(), font_bytes.size(),
                                                     ln.px, LV_FONT_KERNING_NONE, SPIKE_TTF_CACHE);
        stb_metrics_t *sm = stb_metrics_create(font_bytes.data(), font_bytes.size());
        if (!font || !sm) {
            fprintf(stderr, "[%s] font/metrics init failed\n", ln.name.c_str());
            if (font) lv_tiny_ttf_destroy(font);
            if (sm) stb_metrics_destroy(sm);
            rc = 1;
            continue;
        }

        // --- NEW path: pre-shaped glyph run ---
        std::vector<uint8_t> rgb((size_t)W * H * 3, 0);
        render_glyph_run(ln, font, sm, rgb, W, H, baseline, margin);
        std::string dev_png = dir + "/spike_dev_" + ln.name + ".png";
        write_png_rgb24(dev_png, rgb, W, H);
        printf("[%s] %2zu glyphs -> spike_dev_%s.png\n", ln.name.c_str(), ln.glyphs.size(), ln.name.c_str());

        stb_metrics_destroy(sm);
        lv_tiny_ttf_destroy(font);

        // --- CURRENT path (RTL lines only): presentation forms over Naskh ---
        if (ln.direction == 1 && have_naskh) {
            lv_font_t *naskh = lv_tiny_ttf_create_data_ex(naskh_bytes.data(), naskh_bytes.size(),
                                                          ln.px, LV_FONT_KERNING_NONE, SPIKE_TTF_CACHE);
            if (naskh) {
                std::vector<uint8_t> rgb_old((size_t)W * H * 3, 0);
                render_presentation(ln.text, /*rtl=*/true, naskh, rgb_old, W, H, baseline, margin);
                std::string old_png = dir + "/spike_old_" + ln.name + ".png";
                write_png_rgb24(old_png, rgb_old, W, H);
                printf("[%s] presentation-form path -> spike_old_%s.png\n", ln.name.c_str(), ln.name.c_str());
                lv_tiny_ttf_destroy(naskh);
            }
        }
    }
    return rc;
}
