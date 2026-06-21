#ifndef GLYPH_RUNS_H
#define GLYPH_RUNS_H

// ---------------------------------------------------------------------------
// Complex-script glyph-run render layer (Devanagari / Thai / Nastaliq / …)
// ---------------------------------------------------------------------------
//
// Complex scripts can't be rendered correctly by mapping codepoints to glyphs
// one-to-one: they reorder, form conjuncts (glyphs with no codepoint), and
// position marks with GPOS offsets. We solve this OFFLINE — HarfBuzz shapes each
// translated string at build time into a pre-shaped glyph RUN
// (glyph_id, x offset/advance, in font design units, visual order) shipped as the
// compact binary `lang-packs/<loc>/runs.bin` (SSRB format, see tools/i18n/runs_bin.py)
// next to the subset `<loc>.ttf`. See
// docs/knowledge/complex-script-run-pipeline.md and
// docs/knowledge/offline-harfbuzz-shaping-spike-findings.md.
//
// This module is the on-device half. The host pushes the active locale's run
// table in (seedsigner_set_glyph_runs); after a screen is built, the screen
// layer calls apply_glyph_runs_to_labels(), which — for every label whose text
// matches a run-table entry — suppresses the label's (wrong) codepoint text and
// paints the pre-shaped run instead. Glyphs are rasterized BY GLYPH-ID through
// the same tiny_ttf engine the rest of the UI uses (lv_font_get_glyph_bitmap),
// baked once into an A8 alpha mask, and drawn via lv_draw_image with recolor =
// the label's live text color (so focus highlighting still works). No LVGL
// submodule patch; reuses LVGL's clip/blend.
//
// Coexistence: only labels drawn with a registered shaping-locale script font are
// considered (see font_registry.h), so Latin/icon/ASCII labels are untouched.

#include <cstddef>
#include <cstdint>

struct _lv_obj_t;

// Install (or replace) the active locale's pre-shaped run table from runs.bin
// (SSRB) bytes. Pass nullptr/0 to clear. Returns true on parse success (false on a
// bad/truncated blob, leaving no table). Call alongside seedsigner_set_locale();
// the table is keyed by translated msgstr because that is what a finished label
// holds (not the English msgid the offline table is built from — the host supplies
// the already-resolved blob).
bool seedsigner_set_glyph_runs(const char* runs_blob, size_t len);

// Drop the active run table and release the metrics handle. Call before
// switching locale/profile (sibling to seedsigner_clear_registered_fonts).
void seedsigner_clear_glyph_runs();

// Walk a finished screen tree and replace matched labels' rendering with their
// pre-shaped glyph runs. No-op unless the active locale uses shaping
// (seedsigner_locale_uses_glyph_runs) and a run table is loaded. Hooked into the
// single global screen-load post-pass (load_screen_and_cleanup_previous),
// sibling to apply_rtl_text_to_labels(). Idempotent: a label that already carries
// a run is skipped, so a screen may call it early (to measure a baked run) and the
// global post-pass then re-runs it harmlessly.
void apply_glyph_runs_to_labels(struct _lv_obj_t* screen);

// Drawn vertical extent (px) of the glyph run attached to `label`, measured from
// the label's content-box top: nlines * line_height (== the baked mask height minus
// its top+bottom bearing margins) — the height the run actually paints. This is
// TALLER than the label widget's own box, which LVGL sizes from the codepoint text
// at the tighter tight_line_space advance, NOT the run's full line_height. A layout
// that vertically centers a shaped body must use this value rather than
// lv_obj_get_coords(label).bottom. Returns -1 when the label has no attached run (a
// plain codepoint label, or a non-shaping locale), so callers fall back cleanly to
// the label box. The run must already be attached (apply_glyph_runs_to_labels).
int32_t seedsigner_label_run_drawn_height(struct _lv_obj_t* label);

// Whether the glyph run attached to `label` is wider than the label's content box —
// i.e. an overflowing shaped line that clips and would scroll/start-justify. This is
// the same comparison glyph_run_draw_cb makes (run->layout_w > content_w), exposed so
// a caller (the touch long-press-to-scroll gesture) can decide whether a shaped label
// actually overflows without re-measuring the codepoint text (which mis-counts the
// on-screen presentation forms / conjuncts). Returns 1 if it overflows, 0 if it fits,
// and -1 when no run is attached (a plain codepoint label, or a non-shaping locale) —
// callers then measure the codepoint text themselves.
int seedsigner_label_run_overflows(struct _lv_obj_t* label);

#endif // GLYPH_RUNS_H
