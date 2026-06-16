/*
 * stb_glyph_metrics — glyph-id bounding boxes for the glyph-run render path.
 * See stb_glyph_metrics.h.
 *
 * Hosts a SECOND, file-local stb_truetype instance (STBTT_STATIC keeps every
 * symbol internal to this translation unit, so it never clashes with the copy
 * compiled inside the LVGL submodule's lv_tiny_ttf.c). We mirror that file's stb
 * setup exactly — same headers, same macros — so the math is byte-identical and
 * a box recovered here lines up with the bitmap tiny_ttf rasterized for the same
 * glyph-id at the same px.
 */

#include "stb_glyph_metrics.h"

#include <stdlib.h>

/* Match lv_tiny_ttf.c's stb configuration. STBRP is pulled in because the
 * truetype IMPLEMENTATION compiles the (unused) font-packing API that
 * references stbrp types; STATIC keeps both copies private to their TU. */
#define STB_RECT_PACK_IMPLEMENTATION
#define STBRP_STATIC
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "src/libs/tiny_ttf/stb_rect_pack.h"
#include "src/libs/tiny_ttf/stb_truetype_htcw.h"

struct stb_metrics {
    stbtt_fontinfo info;
};

stb_metrics_t *stb_metrics_create(const unsigned char *data, size_t len)
{
    (void)len;
    if (!data) return NULL;
    stb_metrics_t *m = (stb_metrics_t *)calloc(1, sizeof(*m));
    if (!m) return NULL;
    if (stbtt_InitFont(&m->info, data, stbtt_GetFontOffsetForIndex(data, 0)) == 0) {
        free(m);
        return NULL;
    }
    return m;
}

void stb_metrics_destroy(stb_metrics_t *m)
{
    free(m);
}

float stb_metrics_scale(const stb_metrics_t *m, float pixel_size)
{
    /* Same call tiny_ttf uses in lv_tiny_ttf_set_size(). */
    return stbtt_ScaleForMappingEmToPixels(&m->info, pixel_size);
}

void stb_metrics_glyph_box(const stb_metrics_t *m, int gid, float scale,
                           int *ix0, int *iy0, int *ix1, int *iy1)
{
    stbtt_GetGlyphBitmapBox(&m->info, gid, scale, scale, ix0, iy0, ix1, iy1);
}

int stb_metrics_glyph_index(const stb_metrics_t *m, int codepoint)
{
    return stbtt_FindGlyphIndex(&m->info, codepoint);
}
