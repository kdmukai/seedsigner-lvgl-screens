#include "glyph_runs.h"
#include "font_registry.h"

#include "lvgl.h"
#include "src/misc/lv_text_ap.h"   // Arabic/Persian presentation-form mapper
#include "src/widgets/label/lv_label_private.h"  // label->offset / ->text_size (no public getter)
#include "src/misc/lv_area_private.h"             // lv_area_intersect (clip the scrolled mask)

#include "stb_glyph_metrics.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// Parsed run-table model (filled from the compact lang-packs/<loc>/runs.bin blob
// — see the SSRB format in tools/i18n/runs_bin.py, which the BinReader below
// mirrors, and docs/knowledge/complex-script-run-pipeline.md).
// ---------------------------------------------------------------------------
namespace {

struct RunGlyph {
    uint32_t gid;
    int32_t  x_advance, y_advance, x_offset, y_offset;  // font design units
};
using RunLine = std::vector<RunGlyph>;

// One logical (\n-split) line: its visual-order glyphs plus the glyph indices at
// which the device may wrap it (break BEFORE that glyph). The breaks are computed
// offline by ICU's dictionary-aware line breaker — real WORD boundaries even for
// no-space scripts (Thai/…) — so the device just greedy-fits over them.
struct RunVLine {
    RunLine               glyphs;
    std::vector<uint32_t> breaks;  // sorted, strictly interior glyph indices
};

struct RunEntry {
    std::vector<RunVLine> lines;  // one per \n-split line (a run is shaped per line)
};

// One ordered part of a SEGMENTED {}-template entry: either a shaped literal run,
// or a runtime value HOLE. A finished label holds the value-filled string (not the
// template), so the device matches the label against the literals in order,
// extracts the inserted values, and renders the literals shaped (`glyphs`) with the
// values drawn between them. See docs/knowledge/complex-script-run-pipeline.md §4
// and shape_inventory.classify (the offline `segmented` classification).
struct SegPart {
    bool        is_hole = false;
    std::string lit;     // literal text (used to anchor the match); empty for a hole
    RunLine     glyphs;  // shaped glyphs of the literal; empty for a hole
};
struct SegEntry {
    std::vector<SegPart> parts;  // template order: lit/hole alternation
};

struct RunTable {
    int  upem = 1000;
    bool rtl  = false;
    // Keyed by the TRANSLATED string (msgstr) — that is what a finished label
    // holds. Multiple msgids can share one msgstr; the shaped result is identical,
    // so last-wins is harmless.
    std::unordered_map<std::string, RunEntry> by_text;
    // Segmented {}-templates, matched against value-filled labels by their literal
    // anchors (a small list — ~26 for hi — scanned only when by_text misses).
    std::vector<SegEntry> segmented;
};

// ---------------------------------------------------------------------------
// Bounds-checked little-endian reader over the runs.bin (SSRB) blob. Every read
// validates remaining bytes and trips `ok=false` on overrun; the parser bails on
// the first failure. The signing device must never walk past a truncated or
// malformed pack, so reads fail closed (returning 0/empty) rather than reading
// out of bounds. Mirrors the byte layout documented in tools/i18n/runs_bin.py.
// ---------------------------------------------------------------------------
struct BinReader {
    const uint8_t* p;
    const uint8_t* end;
    bool ok = true;

    BinReader(const uint8_t* data, size_t len) : p(data), end(data + len) {}

    bool need(size_t n) {
        if (!ok || (size_t)(end - p) < n) { ok = false; return false; }
        return true;
    }
    uint8_t  u8()  { if (!need(1)) return 0; return *p++; }
    uint16_t u16() { if (!need(2)) return 0; uint16_t v = (uint16_t)(p[0] | (p[1] << 8)); p += 2; return v; }
    uint32_t u32() { if (!need(4)) return 0;
                     uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8)
                                | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
                     p += 4; return v; }
    int16_t  i16() { return (int16_t)u16(); }

    // A u16-length-prefixed UTF-8 string (no NUL). Empty on overrun.
    std::string str() {
        uint16_t n = u16();
        if (!need(n)) return std::string();
        std::string s((const char*)p, n);
        p += n;
        return s;
    }

    // A glyph run: u16 count + one 8-byte record each. y_advance is not carried
    // (always 0 for these horizontal scripts) and is set to 0.
    void glyphs(RunLine& out) {
        uint16_t n = u16();
        out.reserve(out.size() + n);
        for (uint16_t i = 0; i < n && ok; ++i) {
            RunGlyph g;
            g.gid       = u16();
            g.x_advance = i16();
            g.x_offset  = i16();
            g.y_offset  = i16();
            g.y_advance = 0;
            if (!ok) break;
            out.push_back(g);
        }
    }
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
// Greedy line-wrap a shaped run line into visual lines that fit `width_px`,
// cutting only at the offline-computed `breaks` — ICU dictionary WORD boundaries
// (real words even for the no-space scripts; spaces elsewhere). Greedy: fill each
// line to the last break that still fits. A trailing SPACE glyph at the cut is
// trimmed so it doesn't hang past the edge (ICU folds a word's following space
// into that word, so the space sits just before the next break). A segment with
// no break that overflows is left to run off the edge (e.g. one unbreakable word).
//
// All the linguistic intelligence is in `breaks`; this stays a dumb fit. The run
// is visual order and a break is a non-joining boundary, so glyphs split without
// re-shaping. width_px <= 0 (e.g. RTL) or no breaks => the line is returned whole.
//
// NOTE LTR-only: a visual-order RTL run would need right-anchored breaking to put
// the first-read words on the top line, so callers pass width_px = 0 for RTL.
std::vector<RunLine> wrap_line(const RunLine& g, const std::vector<uint32_t>& breaks,
                               int width_px, float scale, uint32_t space_gid) {
    std::vector<RunLine> out;
    if (width_px <= 0 || g.size() < 2 || breaks.empty()) { out.push_back(g); return out; }

    size_t start = 0;
    while (start < g.size()) {
        double acc = 0;
        size_t cut = start;   // last break index that fits, > start (start = no break yet)
        size_t i = start;
        for (; i < g.size(); ++i) {
            if (i > start && std::binary_search(breaks.begin(), breaks.end(), (uint32_t)i))
                cut = i;
            acc += (double)g[i].x_advance * scale;
            if (acc > width_px && cut > start) break;  // overflowed and have somewhere to cut
        }
        if (i >= g.size()) { out.emplace_back(g.begin() + start, g.end()); break; }

        size_t end = cut;
        while (end > start && space_gid && g[end - 1].gid == space_gid) --end;  // trim trailing space
        out.emplace_back(g.begin() + start, g.begin() + end);
        start = cut;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Balanced wrap (shaped path). Narrow a label's wrap column to the SMALLEST width
// that still produces the same number of visual lines — floored at half the full
// width — so greedy wrapping fills the lines evenly and a lone trailing word is
// pulled up. Width-only: the line count (and therefore the baked mask height) is
// unchanged. Cost is trivial: a binary search of a handful of pure-arithmetic
// passes over the already-shaped advances + offline ICU break marks (no
// rasterization, no re-shaping), once per label bake.
//
// Applied to EVERY wrapped shaped label (in bake_run / bake_segmented below). The
// only multi-line wrapped text in the app today is body copy, so in practice this
// only balances body text; single-line labels (titles, buttons) have one line and
// are left untouched. NOTE: if a future screen wraps shaped text that should NOT
// be balanced, gate this per-label (e.g. an opt-in flag threaded from the caller)
// rather than skipping it here.
//
// `measure(width, &nlines, &max_line_w)` reports, for a trial width, the visual
// line count and the widest resulting line; a width is acceptable only if it keeps
// the line count AND no line exceeds it (no word forced to overflow the column).
template <typename MeasureFn>
static int balanced_wrap_width(int full_width, MeasureFn measure) {
    if (full_width <= 1) return full_width;
    size_t n0 = 0; int maxw0 = 0;
    measure(full_width, &n0, &maxw0);
    if (n0 < 2) return full_width;   // single line: nothing to balance

    int lo = full_width / 2, hi = full_width, best = full_width;
    if (lo < 1) lo = 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        size_t n = 0; int mw = 0;
        measure(mid, &n, &mw);
        if (n <= n0 && mw <= mid) { best = mid; hi = mid - 1; }  // same lines, fits: narrower
        else                      { lo = mid + 1; }              // extra line / overflow: wider
    }
    return best;
}

// Measure helper for the plain glyph-run path: greedy-wrap every logical
// (\n-split) line to `width_px` and report the total visual line count + widest
// line. Iterating entry.lines keeps intentional newlines intact.
static void measure_wrapped_runs(const RunEntry& entry, int width_px, float scale,
                                 uint32_t space_gid, size_t* nlines, int* max_line_w) {
    size_t n = 0; int maxw = 0;
    for (const RunVLine& logical : entry.lines) {
        std::vector<RunLine> w = wrap_line(logical.glyphs, logical.breaks, width_px, scale, space_gid);
        for (const RunLine& vl : w) {
            long long adv = 0;
            for (const RunGlyph& g : vl) adv += g.x_advance;
            int lw = (int)lround((double)adv * scale);
            if (lw > maxw) maxw = lw;
            ++n;
        }
    }
    if (nlines)     *nlines = n;
    if (max_line_w) *max_line_w = maxw;
}

// ---------------------------------------------------------------------------
// Bake a parsed run into an A8 alpha mask sized to the font's line box (plus a
// generous margin to catch left bearings and cursive overhang). Resolution is
// taken from the label's own font, so one run table serves every PX_MULTIPLIER.
// `wrap_width` (px, 0 = no wrap) word-wraps long lines to fit the label.
// Returns a heap LabelRun on success (caller owns), or nullptr.
// ---------------------------------------------------------------------------
LabelRun* bake_run(const RunEntry& entry, const lv_font_t* font, int px, int upem,
                   lv_text_align_t align, bool rtl, int wrap_width) {
    size_t len = 0;
    const uint8_t* bytes = seedsigner_registered_font_bytes(font, &len);
    stb_metrics_t* sm = metrics_for(bytes, len);
    if (!sm) return nullptr;

    const float scale = stb_metrics_scale(sm, (float)px);
    (void)upem;  // scale already folds upem (== px / units_per_em)

    const int line_height = lv_font_get_line_height(font);
    const int ascent      = line_height - font->base_line;  // baseline within line box
    const int margin      = line_height;                    // bearing/overhang slack

    // Wrap each logical (\n-split) line to the label width at its offline break
    // marks, producing the visual lines actually laid out below. SPACE glyph id
    // from the same subset stb (to trim a trailing space at a cut).
    const uint32_t space_gid = (uint32_t)stb_metrics_glyph_index(sm, ' ');

    // Balanced wrap: even out the lines by shrinking the column (see
    // balanced_wrap_width). Width-only, keeps the line count, preserves the
    // \n-split logical lines. wrap_width <= 0 (RTL / no-wrap) is left untouched.
    if (wrap_width > 0) {
        wrap_width = balanced_wrap_width(wrap_width, [&](int w, size_t* n, int* mw) {
            measure_wrapped_runs(entry, w, scale, space_gid, n, mw);
        });
    }

    std::vector<RunLine> vlines;
    for (const RunVLine& logical : entry.lines) {
        std::vector<RunLine> w = wrap_line(logical.glyphs, logical.breaks, wrap_width, scale, space_gid);
        for (RunLine& vl : w) vlines.push_back(std::move(vl));
    }

    const size_t nlines = vlines.size();
    if (nlines == 0) return nullptr;

    // Pass 1 — per-line typographic width (sum of advances) and the block width.
    std::vector<int> line_w(nlines, 0);
    int layout_w = 0;
    for (size_t li = 0; li < nlines; ++li) {
        long long adv = 0;
        for (const RunGlyph& g : vlines[li]) adv += g.x_advance;
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
        for (const RunGlyph& g : vlines[li]) {
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

// ---------------------------------------------------------------------------
// Segmented {}-template runs (value insertion).
//
// A finished label holds the value-FILLED string (e.g. `सीड शब्द #5`), not the
// template (`सीड शब्द #{}`). We match the label against a template's literal
// anchors, extract the inserted values, and bake one mask: literals drawn SHAPED
// (their offline glyph runs), each hole value drawn between them. A value can be
// an integer/ASCII string OR a translated text snippet, so a value is first looked
// up in the plain run table (shaped — handles a complex-script snippet that is its
// own translated msgid) and only falls back to the codepoint path otherwise.
// LTR only (the literal/value order is left-to-right) — segmented matching is
// skipped for RTL locales.
// ---------------------------------------------------------------------------

// Decode one UTF-8 codepoint at byte `i` (advanced past it). 0 at end; U+FFFD on a
// malformed byte (advanced by one to make progress). Self-contained so the render
// layer needs no LVGL text-encoding internals; our scripts are all BMP.
uint32_t utf8_next(const std::string& s, size_t& i) {
    if (i >= s.size()) return 0;
    unsigned char c = (unsigned char)s[i];
    uint32_t cp; int n;
    if      (c < 0x80)        { cp = c;        n = 1; }
    else if ((c >> 5) == 0x6) { cp = c & 0x1F; n = 2; }
    else if ((c >> 4) == 0xE) { cp = c & 0x0F; n = 3; }
    else if ((c >> 3) == 0x1E){ cp = c & 0x07; n = 4; }
    else { i += 1; return 0xFFFD; }
    if (i + (size_t)n > s.size()) { i = s.size(); return 0xFFFD; }
    for (int k = 1; k < n; ++k) {
        unsigned char cc = (unsigned char)s[i + k];
        if ((cc & 0xC0) != 0x80) { i += 1; return 0xFFFD; }
        cp = (cp << 6) | (cc & 0x3F);
    }
    i += n;
    return cp;
}

// One laid-out glyph of a matched segmented string, in a single flat sequence that
// mixes shaped literal/snippet glyphs (drawn by gid via stb) and codepoint value
// glyphs (drawn via the font's fallback chain). Flattening to a uniform list lets
// the same greedy word-wrap as the plain path run across literals AND values.
struct FlatGlyph {
    bool   is_cp;       // true: codepoint glyph (dsc path); false: shaped run glyph (gid)
    uint32_t code;      // gid (run) or unicode codepoint (cp)
    float  x_off_px;    // shaped GPOS x offset (run); 0 (cp uses dsc.ofs_x)
    float  y_off_px;    // shaped GPOS y offset (run); 0 (cp uses dsc.ofs_y)
    float  advance_px;  // pen advance
    bool   is_space;    // a breaking space (trimmed at a wrap cut)
    bool   can_break;   // a line may break BEFORE this glyph
};

// A line may break BEFORE this glyph iff the previous laid-out glyph was a space —
// the same word-boundary rule the plain path applies (here computed on device,
// since segmented literals carry no offline break marks). Devanagari/Latin/Arabic
// body text is space-separated, so this wraps it correctly; the no-space scripts
// (Thai/…) almost never appear as segmented templates, and would simply not wrap.
inline bool break_before_here(const std::vector<FlatGlyph>& flat) {
    return !flat.empty() && flat.back().is_space;
}

// Append a shaped run (a literal segment or a shaped snippet value) to `flat`.
void flat_append_run(std::vector<FlatGlyph>& flat, const RunLine& glyphs,
                     float scale, uint32_t space_gid) {
    for (const RunGlyph& g : glyphs) {
        FlatGlyph f;
        f.is_cp      = false;
        f.code       = g.gid;
        f.x_off_px   = (float)((double)g.x_offset * scale);
        f.y_off_px   = (float)((double)g.y_offset * scale);
        f.advance_px = (float)((double)g.x_advance * scale);
        f.is_space   = (space_gid && g.gid == space_gid);
        f.can_break  = break_before_here(flat);
        flat.push_back(f);
    }
}

// Append a codepoint value (integer / ASCII / untranslated snippet) to `flat`,
// resolving each codepoint through the font's fallback chain (ASCII drops to the
// baked OpenSans floor). Break opportunity after an ASCII space.
void flat_append_cp(std::vector<FlatGlyph>& flat, const std::string& v,
                    const lv_font_t* font) {
    size_t i = 0;
    for (uint32_t cp = utf8_next(v, i); cp; cp = utf8_next(v, i)) {
        lv_font_glyph_dsc_t d;
        memset(&d, 0, sizeof(d));
        if (!lv_font_get_glyph_dsc(font, &d, cp, 0)) continue;  // absent: no glyph/advance
        FlatGlyph f;
        f.is_cp      = true;
        f.code       = cp;
        f.x_off_px   = 0;
        f.y_off_px   = 0;
        f.advance_px = (float)d.adv_w;
        f.is_space   = (cp == 0x20);
        f.can_break  = break_before_here(flat);
        flat.push_back(f);
    }
}

// Flatten a matched segmented entry into one glyph sequence: shaped literals, and
// each hole value shaped (if it is itself a translated run — handles a complex-
// script snippet that is its own msgid) or else on the codepoint path.
std::vector<FlatGlyph> flatten_segmented(const SegEntry& entry,
                                         const std::vector<std::string>& values,
                                         const lv_font_t* font, float scale,
                                         uint32_t space_gid) {
    std::vector<FlatGlyph> flat;
    size_t hole = 0;
    for (const SegPart& p : entry.parts) {
        if (!p.is_hole) {
            flat_append_run(flat, p.glyphs, scale, space_gid);
            continue;
        }
        const std::string& v = (hole < values.size()) ? values[hole] : std::string();
        ++hole;
        // ap_form is a no-op for LTR (the only direction segmented runs match in).
        auto it = g_table.by_text.find(ap_form(v));
        if (it != g_table.by_text.end() && !it->second.lines.empty()) {
            for (const RunVLine& ln : it->second.lines)
                flat_append_run(flat, ln.glyphs, scale, space_gid);
        } else {
            flat_append_cp(flat, v, font);
        }
    }
    return flat;
}

// Draw one flat glyph at pen `x` (px, left of the glyph origin) on `baseline`.
void draw_flat_glyph(lv_draw_buf_t* mask, int mask_w, int mask_h, int stride,
                     const FlatGlyph& f, double x, int baseline,
                     const lv_font_t* font, stb_metrics_t* sm, float scale) {
    if (!f.is_cp) {
        lv_font_glyph_dsc_t gd;
        memset(&gd, 0, sizeof(gd));
        gd.resolved_font = (lv_font_t*)font;
        gd.gid.index = f.code;
        gd.format = LV_FONT_GLYPH_FORMAT_A8;
        const lv_draw_buf_t* db = (const lv_draw_buf_t*)lv_font_get_glyph_bitmap(&gd, NULL);
        if (db && db->data && db->header.w > 0 && db->header.h > 0) {
            int ix0, iy0, ix1, iy1;
            stb_metrics_glyph_box(sm, (int)f.code, scale, &ix0, &iy0, &ix1, &iy1);
            int gx = (int)lround(x + (double)f.x_off_px) + ix0;
            int gy = baseline + iy0 - (int)lround((double)f.y_off_px);  // HB y-up -> y-down
            blit_a8((uint8_t*)mask->data, mask_w, mask_h, stride,
                    (const uint8_t*)db->data, db->header.w, db->header.h,
                    db->header.stride, gx, gy);
        }
        lv_font_glyph_release_draw_data(&gd);
        return;
    }
    lv_font_glyph_dsc_t d;
    memset(&d, 0, sizeof(d));
    if (!lv_font_get_glyph_dsc(font, &d, f.code, 0)) return;
    const lv_draw_buf_t* db = (const lv_draw_buf_t*)lv_font_get_glyph_bitmap(&d, NULL);
    if (db && db->data && d.box_w > 0 && d.box_h > 0) {
        int gx = (int)lround(x) + d.ofs_x;
        int gy = baseline - d.ofs_y - ((int)d.box_h - 1);  // dsc box -> top row
        blit_a8((uint8_t*)mask->data, mask_w, mask_h, stride,
                (const uint8_t*)db->data, db->header.w, db->header.h,
                db->header.stride, gx, gy);
    }
    lv_font_glyph_release_draw_data(&d);
}

// Greedy-wrap a flat glyph sequence into [start,end) line ranges fitting `width_px`,
// cutting only where `can_break` and trimming a trailing space at the cut. Mirrors
// wrap_line (the plain path). width_px <= 0 => a single line. Trimmed spaces between
// the cut and the line end are dropped from both lines (they vanish at the break).
std::vector<std::pair<size_t, size_t>> wrap_flat(const std::vector<FlatGlyph>& flat,
                                                 int width_px) {
    std::vector<std::pair<size_t, size_t>> lines;
    const size_t n = flat.size();
    if (width_px <= 0 || n == 0) { lines.push_back({0, n}); return lines; }

    size_t start = 0;
    while (start < n) {
        double acc = 0;
        size_t cut = start;   // last break index that fits, > start (start = none yet)
        size_t i = start;
        for (; i < n; ++i) {
            if (i > start && flat[i].can_break) cut = i;
            acc += flat[i].advance_px;
            if (acc > width_px && cut > start) break;  // overflowed and have a cut
        }
        if (i >= n) { lines.push_back({start, n}); break; }

        size_t end = cut;
        while (end > start && flat[end - 1].is_space) --end;  // trim trailing space
        lines.push_back({start, end});
        start = cut;
    }
    return lines;
}

// Measure helper for the segmented path: greedy-wrap the flat glyph sequence to
// `width_px` and report the line count + widest line (advances are already px).
static void measure_wrapped_flat(const std::vector<FlatGlyph>& flat, int width_px,
                                 size_t* nlines, int* max_line_w) {
    std::vector<std::pair<size_t, size_t>> lines = wrap_flat(flat, width_px);
    int maxw = 0;
    for (const auto& ln : lines) {
        double adv = 0;
        for (size_t i = ln.first; i < ln.second; ++i) adv += flat[i].advance_px;
        int lw = (int)lround(adv);
        if (lw > maxw) maxw = lw;
    }
    if (nlines)     *nlines = lines.size();
    if (max_line_w) *max_line_w = maxw;
}

// Match `text` (the value-filled label) against a template's literal anchors. On
// success fills `values` (one per hole, in order) and returns true. Leftmost-greedy:
// each literal is found at/after the previous cut. Byte-level matching is valid —
// UTF-8 is self-synchronising and the literals start/end on codepoint boundaries.
bool match_segmented(const SegEntry& seg, const std::string& text,
                     std::vector<std::string>& values) {
    values.clear();
    size_t c = 0;                          // cursor into text (bytes)
    size_t hole_start = std::string::npos; // start of an open (pending) hole value
    for (const SegPart& p : seg.parts) {
        if (p.is_hole) {
            if (hole_start != std::string::npos) return false;  // adjacent holes: ambiguous
            hole_start = c;
            continue;
        }
        if (hole_start == std::string::npos) {
            // Leading literal (or after a consumed literal): must sit exactly at c.
            if (c > text.size() || text.compare(c, p.lit.size(), p.lit) != 0) return false;
            c += p.lit.size();
        } else {
            // Literal closes the open hole: the value is everything up to it.
            size_t found = text.find(p.lit, c);
            if (found == std::string::npos) return false;
            values.push_back(text.substr(hole_start, found - hole_start));
            hole_start = std::string::npos;
            c = found + p.lit.size();
        }
    }
    if (hole_start != std::string::npos) {
        values.push_back(text.substr(hole_start));  // trailing hole runs to the end
    } else if (c != text.size()) {
        return false;  // a trailing literal must consume the whole string
    }
    return true;
}

// Find the first segmented template the label matches, flatten it, word-wrap to the
// label width, and bake the (multi-line) mask — reusing the LabelRun the plain path
// produces. Returns nullptr if no template matches (label falls back to the
// codepoint path, as before). `wrap_width` <= 0 keeps it single line.
LabelRun* bake_segmented(const char* label_text, const lv_font_t* font, int px,
                         lv_text_align_t align, int wrap_width) {
    size_t len = 0;
    const uint8_t* bytes = seedsigner_registered_font_bytes(font, &len);
    stb_metrics_t* sm = metrics_for(bytes, len);
    if (!sm) return nullptr;
    const float scale = stb_metrics_scale(sm, (float)px);

    const std::string text(label_text);
    std::vector<std::string> values;
    const SegEntry* match = nullptr;
    for (const SegEntry& seg : g_table.segmented) {
        if (match_segmented(seg, text, values)) { match = &seg; break; }
    }
    if (!match) return nullptr;

    const uint32_t space_gid = (uint32_t)stb_metrics_glyph_index(sm, ' ');
    std::vector<FlatGlyph> flat = flatten_segmented(*match, values, font, scale, space_gid);
    if (flat.empty()) return nullptr;

    // Balanced wrap: shrink the column to even out the lines (see
    // balanced_wrap_width); width-only, keeps the line count. Mirrors bake_run.
    if (wrap_width > 0) {
        wrap_width = balanced_wrap_width(wrap_width, [&](int w, size_t* n, int* mw) {
            measure_wrapped_flat(flat, w, n, mw);
        });
    }

    // Word-wrap to the label width at the break opportunities (after spaces / part
    // edges) — long body text (e.g. the change-address warning) wraps like the
    // plain path; short titles/labels stay single line.
    std::vector<std::pair<size_t, size_t>> lines = wrap_flat(flat, wrap_width);
    const size_t nlines = lines.size();
    if (nlines == 0) return nullptr;

    const int line_height = lv_font_get_line_height(font);
    const int ascent      = line_height - font->base_line;
    const int margin      = line_height;  // bearing/overhang slack (matches bake_run)

    // Per-line widths (sum of advances) and the block width.
    std::vector<double> line_w(nlines, 0);
    double layout_wd = 0;
    for (size_t li = 0; li < nlines; ++li) {
        double w = 0;
        for (size_t i = lines[li].first; i < lines[li].second; ++i) w += flat[i].advance_px;
        line_w[li] = w;
        if (w > layout_wd) layout_wd = w;
    }
    const int layout_w = (int)lround(layout_wd);
    if (layout_w <= 0) return nullptr;

    const int mask_w = layout_w + 2 * margin;
    const int mask_h = (int)nlines * line_height + 2 * margin;
    lv_draw_buf_t* mask = lv_draw_buf_create(mask_w, mask_h, LV_COLOR_FORMAT_A8, 0);
    if (!mask) return nullptr;
    const int stride = mask->header.stride;
    memset(mask->data, 0, (size_t)stride * mask_h);

    // Paint each line, intra-aligned within layout_w (the block is aligned to the
    // label box later, at draw time).
    for (size_t li = 0; li < nlines; ++li) {
        double line_off;
        switch (align) {
            case LV_TEXT_ALIGN_CENTER: line_off = (layout_wd - line_w[li]) / 2; break;
            case LV_TEXT_ALIGN_RIGHT:  line_off = (layout_wd - line_w[li]);     break;
            default:                   line_off = 0; break;  // LEFT / AUTO-LTR
        }
        const double pen_x0  = margin + line_off;
        const int    baseline = margin + (int)li * line_height + ascent;
        double pen = pen_x0;
        for (size_t i = lines[li].first; i < lines[li].second; ++i) {
            draw_flat_glyph(mask, mask_w, mask_h, stride, flat[i], pen, baseline,
                            font, sm, scale);
            pen += flat[i].advance_px;
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

    lv_text_align_t align     = lv_obj_get_style_text_align(label, LV_PART_MAIN);
    const lv_label_long_mode_t long_mode = lv_label_get_long_mode(label);
    const bool overflows      = run->layout_w > content_w;

    // The scroll/marquee machinery (offset honoring, scroll-mode start-justify, the
    // circular wrap copy, and the content-box clip) is LTR-only for now. RTL (ur)
    // is deferred — its SCROLL_CIRCULAR title seeds offset.x at a large NEGATIVE
    // start (-(size.x+gap), the BIDI circular start), so honoring it would push the
    // run off-screen-left; and its button labels were never clipped. Keeping RTL on
    // the legacy center/right path leaves ur byte-identical (see plan Item 2c, which
    // owns the RTL start-justify + scroll framing, and project memory: ur untouched).
    const bool ltr     = !g_table.rtl;
    const bool scrolls = ltr && (long_mode == LV_LABEL_LONG_SCROLL ||
                                 long_mode == LV_LABEL_LONG_SCROLL_CIRCULAR);

    // A single-line label whose run overflows must show its START edge, not
    // center-clip to its middle — the shaped counterpart of
    // apply_button_label_layout()'s subset-path start-justify. The start edge is
    // left for LTR, right for RTL. It applies to button labels (LONG_CLIP, both
    // directions — legacy A2 behavior) and to the LTR scrollable modes (the top-nav
    // title, and any future scrolling headline), where it mirrors LVGL's own
    // draw_main forcing CENTER/RIGHT to the base-dir start on overflow (Task 0).
    // LONG_DOT (the status headline today) and LONG_WRAP body text are intentionally
    // excluded: DOT center-clips an overflowing run, and an unwrapped RTL body line
    // (ur wrapping deferred) keeps its centered rendering. Labels that fit are
    // unchanged; an explicit LEFT/RIGHT align is always honored as-is.
    const bool start_justify =
        overflows && (long_mode == LV_LABEL_LONG_CLIP || scrolls);
    if (align == LV_TEXT_ALIGN_CENTER && start_justify) {
        align = g_table.rtl ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT;
    }

    int text_x;
    switch (align) {
        case LV_TEXT_ALIGN_CENTER: text_x = cc.x1 + (content_w - run->layout_w) / 2; break;
        case LV_TEXT_ALIGN_RIGHT:  text_x = cc.x1 + (content_w - run->layout_w);     break;
        default:                   text_x = cc.x1; break;  // LEFT / AUTO-LTR
    }

    // Honor the label's scroll offset (LTR only — see above). LVGL animates
    // label->offset.x for the SCROLL / SCROLL_CIRCULAR marquee (and offset.y for
    // vertical scroll) and the codepoint draw path translates the text by it — but
    // the glyph-run mask is drawn from a separate event (DRAW_MAIN_END) and
    // previously ignored it, so a shaped marquee moved nothing. Adding the offset
    // lets LTR shaped labels ride LVGL's existing scroll machinery (Task 0). It is 0
    // for the fitting / static case, so non-scrolling labels are byte-identical.
    const lv_point_t offset = ltr ? ((lv_label_t*)label)->offset : lv_point_t{0, 0};

    // Mask pen origin (col/row == margin) maps to (text_x, baseline); since line 0
    // baseline sits at margin+ascent inside the mask and LVGL would put it at
    // content_top+ascent, mask row 0 -> content_top - margin (margins cancel ascent).
    lv_area_t area;
    area.x1 = text_x  - run->margin + offset.x;
    area.y1 = cc.y1   - run->margin + offset.y;
    area.x2 = area.x1 + run->mask->header.w - 1;
    area.y2 = area.y1 + run->mask->header.h - 1;

    lv_draw_image_dsc_t img;
    lv_draw_image_dsc_init(&img);
    img.src         = run->mask;                                       // A8 alpha mask
    img.recolor     = lv_obj_get_style_text_color(label, LV_PART_MAIN);// live text color
    img.recolor_opa = LV_OPA_COVER;                                    // tint A8 mask fully
    img.opa         = LV_OPA_COVER;  // coverage comes from the mask, not the (suppressed) text_opa

    // When an LTR run overflows a clip/scroll mode, clip the mask to the content box
    // so the scrolled-out tail (and the wrap-around copy below) never bleed past the
    // label's edges — matching LVGL's own SCROLL/CLIP clip. The fitting case (and
    // all RTL) skips this so glyphs with side/vertical overshoot keep painting into
    // the label's extended draw area exactly as before (byte-identical).
    lv_area_t clip_ori = layer->_clip_area;
    const bool clip_to_content = ltr && start_justify;
    if (clip_to_content) {
        lv_area_t clipped;
        if (!lv_area_intersect(&clipped, &cc, &clip_ori)) return;  // nothing visible
        layer->_clip_area = clipped;
    }

    lv_draw_image(layer, &img, &area);

    // SCROLL_CIRCULAR draws a second copy one period ahead so the marquee loops
    // seamlessly: as the first copy scrolls off the start edge, the second enters
    // from the far edge. LVGL's offset animation travels one period =
    // text_size.x + WAIT_CHAR_COUNT spaces (codepoint metrics), so the copy is
    // placed exactly that far along to stay in lock-step with the animation. NOTE:
    // the shaped run width (run->layout_w) can differ from the codepoint text_size
    // that drives the animation, so the visible inter-copy gap is the period minus
    // the run width rather than a fixed N-space gap; perfecting shaped marquee
    // geometry is tracked in Items 2b/2c.
    if (scrolls && long_mode == LV_LABEL_LONG_SCROLL_CIRCULAR && overflows) {
        const lv_font_t* font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
        const int32_t period = ((lv_label_t*)label)->text_size.x +
                               lv_font_get_glyph_width(font, ' ', ' ') * LV_LABEL_WAIT_CHAR_COUNT;
        area.x1 += period;
        area.x2 += period;
        lv_draw_image(layer, &img, &area);
    }

    if (clip_to_content) layer->_clip_area = clip_ori;
}

void glyph_run_delete_cb(lv_event_t* e) {
    free_label_run((LabelRun*)lv_event_get_user_data(e));
}

// Return the LabelRun attached to `obj` (the user_data of its glyph_run_draw_cb
// event), or nullptr if none. Used both to skip double-attach when a screen baked
// its runs early, and to expose the run's drawn height to the layout.
LabelRun* find_label_run(lv_obj_t* obj) {
    uint32_t n = lv_obj_get_event_count(obj);
    for (uint32_t i = 0; i < n; ++i) {
        lv_event_dsc_t* d = lv_obj_get_event_dsc(obj, i);
        if (d && lv_event_dsc_get_cb(d) == glyph_run_draw_cb) {
            return (LabelRun*)lv_event_dsc_get_user_data(d);
        }
    }
    return nullptr;
}

// --- Recursively attach runs to matching labels. ----------------------------
void attach_runs(lv_obj_t* obj) {
    // User text-entry stays on the codepoint path (ASCII; never shaped).
    if (lv_obj_check_type(obj, &lv_textarea_class)) return;

    // Already baked (a screen front-loaded its runs so it could measure them) —
    // don't double-attach; just recurse to children below.
    if (lv_obj_check_type(obj, &lv_label_class) && !find_label_run(obj)) {
        // Only labels drawn with a registered shaping-locale script font are
        // candidates — this cleanly skips icon / keyboard / pure-ASCII labels.
        const lv_font_t* font = lv_obj_get_style_text_font(obj, LV_PART_MAIN);
        int px = seedsigner_registered_font_px(font);
        if (px > 0) {
            const char* text = lv_label_get_text(obj);
            if (text && *text) {
                LabelRun* run = nullptr;
                lv_text_align_t align = lv_obj_get_style_text_align(obj, LV_PART_MAIN);
                // Word-wrap ONLY labels that actually wrap their codepoint text —
                // i.e. LV_LABEL_LONG_WRAP (the multi-line body/warning text). The
                // single-line labels (the top_nav title + button labels use
                // SCROLL/CLIP) must stay on one line: wrapping a run to a
                // single-line label's content width splits it across lines the
                // label can't show, so only line 0 survives — e.g. a Thai title
                // "1 อินพุต" whose run overflows the narrow title region collapses
                // to just its ASCII prefix "1". RTL is never wrapped (a visual-order
                // run needs right-anchored breaking). wrap_width 0 => single line.
                const bool wraps =
                    (lv_label_get_long_mode(obj) == LV_LABEL_LONG_WRAP) && !g_table.rtl;
                const int wrap_width = wraps ? lv_obj_get_content_width(obj) : 0;

                auto it = g_table.by_text.find(text);
                if (it != g_table.by_text.end()) {
                    run = bake_run(it->second, font, px, g_table.upem,
                                   align, g_table.rtl, wrap_width);
                } else if (!g_table.rtl && !g_table.segmented.empty()) {
                    // No whole-string match: the label may be a value-filled
                    // {}-template. Match it against the segmented anchors and bake
                    // shaped literals + inserted values. LTR only.
                    run = bake_segmented(text, font, px, align, wrap_width);
                }
                if (run) {
                    // Suppress the (wrong) codepoint text; draw the run instead.
                    lv_obj_set_style_text_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN);
                    lv_obj_add_event_cb(obj, glyph_run_draw_cb, LV_EVENT_DRAW_MAIN_END, run);
                    lv_obj_add_event_cb(obj, glyph_run_delete_cb, LV_EVENT_DELETE, run);
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
bool seedsigner_set_glyph_runs(const char* runs_blob, size_t len) {
    seedsigner_clear_glyph_runs();
    if (!runs_blob || len == 0) return true;  // cleared

    BinReader r((const uint8_t*)runs_blob, len);

    // Header: magic "SSRB", version, upem, direction (see tools/i18n/runs_bin.py).
    const uint8_t m0 = r.u8(), m1 = r.u8(), m2 = r.u8(), m3 = r.u8();
    if (!r.ok || m0 != 'S' || m1 != 'S' || m2 != 'R' || m3 != 'B') {
        fprintf(stderr, "seedsigner_set_glyph_runs: bad magic (not runs.bin)\n");
        seedsigner_clear_glyph_runs();
        return false;
    }
    const uint16_t version = r.u16();
    if (version != 1) {
        fprintf(stderr, "seedsigner_set_glyph_runs: unsupported runs.bin version %u\n", version);
        seedsigner_clear_glyph_runs();
        return false;
    }
    g_table.upem = r.u16();
    const uint8_t direction = r.u8();
    r.u8();  // reserved
    g_table.rtl = (direction == 1);
    const uint32_t run_count = r.u32();

    for (uint32_t i = 0; i < run_count && r.ok; ++i) {
        const uint8_t kind = r.u8();

        if (kind == 0) {  // plain: a whole-string (multi-line) run, keyed by text
            const std::string text = r.str();
            const uint16_t line_count = r.u16();
            RunEntry entry;
            entry.lines.reserve(line_count);
            for (uint16_t li = 0; li < line_count && r.ok; ++li) {
                RunVLine vline;
                r.glyphs(vline.glyphs);
                const uint16_t break_count = r.u16();
                vline.breaks.reserve(break_count);
                for (uint16_t bi = 0; bi < break_count && r.ok; ++bi)
                    vline.breaks.push_back(r.u16());
                entry.lines.push_back(std::move(vline));
            }
            // Key by the presentation-form transform so RTL labels (stored as
            // presentation forms by LVGL) match; a no-op for non-Arabic scripts.
            if (r.ok) g_table.by_text[ap_form(text)] = std::move(entry);

        } else if (kind == 1) {  // segmented: ordered literal runs + hole markers
            const uint16_t part_count = r.u16();
            SegEntry seg;
            seg.parts.reserve(part_count);
            for (uint16_t pi = 0; pi < part_count && r.ok; ++pi) {
                SegPart part;
                if (r.u8()) {            // is_hole
                    part.is_hole = true;
                } else {
                    part.lit = r.str();
                    r.glyphs(part.glyphs);
                }
                seg.parts.push_back(std::move(part));
            }
            if (r.ok && !seg.parts.empty()) g_table.segmented.push_back(std::move(seg));

        } else {
            fprintf(stderr, "seedsigner_set_glyph_runs: bad run kind %u\n", kind);
            r.ok = false;
        }
    }

    if (!r.ok) {
        fprintf(stderr, "seedsigner_set_glyph_runs: truncated/invalid runs.bin\n");
        seedsigner_clear_glyph_runs();
        return false;
    }

    g_have_table = true;
    return true;
}

void seedsigner_clear_glyph_runs() {
    g_table.by_text.clear();
    g_table.segmented.clear();
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

int32_t seedsigner_label_run_drawn_height(struct _lv_obj_t* label) {
    if (!label) return -1;
    LabelRun* run = find_label_run((lv_obj_t*)label);
    if (!run || !run->mask) return -1;
    // mask height == nlines*line_height + 2*margin (see bake_run); the block the
    // run actually paints, from the label's content top, is nlines*line_height.
    return (int32_t)run->mask->header.h - 2 * run->margin;
}

int seedsigner_label_run_overflows(struct _lv_obj_t* label) {
    if (!label) return -1;
    LabelRun* run = find_label_run((lv_obj_t*)label);
    if (!run) return -1;
    // Same test as glyph_run_draw_cb: the typographic block width vs the label's
    // content box. run->layout_w is the shaped width (presentation forms / conjuncts
    // already accounted for), which the codepoint measure can't reproduce.
    int32_t content_w = lv_obj_get_content_width((lv_obj_t*)label);
    return run->layout_w > content_w ? 1 : 0;
}
