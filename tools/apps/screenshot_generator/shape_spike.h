/*
 * shape_spike — THROWAWAY on-device half of the offline-HarfBuzz shaping spike.
 *
 * Loads the artifacts spike_shape.py emits into <spike_dir> (spike_runs.bin,
 * meta.json, spike_<name>.ttf subset fonts), then for each pre-shaped glyph run:
 *   - rasterizes every glyph BY GLYPH-ID through the EXISTING tiny_ttf engine
 *     (lv_font_get_glyph_bitmap — the zero-submodule-edit seam),
 *   - recovers each glyph's bounding box from a metrics-only stb re-init,
 *   - places it by the run's GPOS offsets / advances and blits into a canvas,
 *   - writes spike_dev_<name>.png next to the libraqm reference.
 *
 * For the RTL lines it ALSO renders the CURRENT presentation-form path (LVGL's
 * lv_text_ap over Noto Sans Arabic, codepoint-driven, no GPOS) to
 * spike_old_<name>.png — the fa regression target and the Urdu negative control.
 *
 * Returns 0 on success. lv_init() must already have been called.
 */
#ifndef SHAPE_SPIKE_H
#define SHAPE_SPIKE_H

int run_shape_spike(const char *spike_dir);

#endif /* SHAPE_SPIKE_H */
