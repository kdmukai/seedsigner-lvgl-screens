/*
 * stb_glyph_metrics — THROWAWAY spike helper.
 *
 * The on-device half of the offline-HarfBuzz shaping spike rasterizes glyphs
 * BY GLYPH-ID through the existing tiny_ttf engine (lv_font_get_glyph_bitmap,
 * keyed on gid). That public seam returns the A8 bitmap and its width/height,
 * but NOT the glyph's bounding-box offset from the pen origin — which we need to
 * place the bitmap. tiny_ttf only exposes that offset through its codepoint
 * lookup (ttf_get_glyph_dsc_cb -> stbtt_FindGlyphIndex), the one path a
 * pre-shaped run must bypass.
 *
 * Per the spike plan's chosen "zero-submodule-edit" route, we recover the offset
 * by re-initialising our OWN stbtt_fontinfo over the same already-resident font
 * buffer and asking stb for the glyph's bitmap box by glyph-id directly. Because
 * it is the same stb, the same font bytes, and the same EM->px scale that
 * tiny_ttf uses internally, the box matches the rasterized bitmap exactly.
 *
 * This proves the production shim needs no patch to the LVGL submodule: a small
 * metrics-only stb re-init in the render layer is sufficient. C linkage so the
 * stb implementation lives in one plain C translation unit.
 */
#ifndef STB_GLYPH_METRICS_H
#define STB_GLYPH_METRICS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle wrapping an stbtt_fontinfo. `data` MUST outlive the handle
 * (stb references it for lazy glyph reads — same contract as tiny_ttf). */
typedef struct stb_metrics stb_metrics_t;

/* Init over a font buffer. Returns NULL on failure. */
stb_metrics_t *stb_metrics_create(const unsigned char *data, size_t len);
void           stb_metrics_destroy(stb_metrics_t *m);

/* EM->pixel scale for a given pixel size — identical to the value tiny_ttf
 * computes via lv_tiny_ttf_set_size(), so boxes line up with the cache. */
float stb_metrics_scale(const stb_metrics_t *m, float pixel_size);

/* Pixel bounding box of glyph `gid` at `scale`, baseline-relative, y-down
 * (stb convention): top-left = (ix0, iy0). */
void stb_metrics_glyph_box(const stb_metrics_t *m, int gid, float scale,
                           int *ix0, int *iy0, int *ix1, int *iy1);

#ifdef __cplusplus
}
#endif

#endif /* STB_GLYPH_METRICS_H */
