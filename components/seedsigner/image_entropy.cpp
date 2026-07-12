#include "image_entropy.h"

#include <stddef.h>   // NULL, size_t
#include <stdint.h>

// ---------------------------------------------------------------------------
// Image-entropy display processing (see image_entropy.h for the contract).
//
// After the crop/resize into `dst`, two passes over the destination buffer: one
// to build a luminance histogram, one to apply the stretch LUT. A few ms for a
// full-display frame, sub-1 KB of tables. No floats, no allocation, no LVGL.
// ---------------------------------------------------------------------------

namespace {

// --- RGB565 (R[15:11] G[10:5] B[4:0]) pack / unpack -------------------------
// Expand 5/6-bit channels to a full 8-bit range by replicating the high bits
// into the freed low bits (the standard 5->8 / 6->8 widening: 0x1F -> 0xFF).

inline void rgb565_unpack(uint16_t px, uint8_t *r, uint8_t *g, uint8_t *b) {
    uint8_t r5 = (px >> 11) & 0x1F;
    uint8_t g6 = (px >> 5)  & 0x3F;
    uint8_t b5 =  px        & 0x1F;
    *r = (uint8_t)((r5 << 3) | (r5 >> 2));
    *g = (uint8_t)((g6 << 2) | (g6 >> 4));
    *b = (uint8_t)((b5 << 3) | (b5 >> 2));
}

inline uint16_t rgb565_pack(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Read one source pixel (either supported format) as 8-bit R/G/B.
inline void read_src_pixel(const void *src, image_entropy_pixfmt_t fmt,
                           size_t index, uint8_t *r, uint8_t *g, uint8_t *b) {
    if (fmt == IMAGE_ENTROPY_PIXFMT_RGB888) {
        const uint8_t *p = (const uint8_t *)src + index * 3;
        *r = p[0]; *g = p[1]; *b = p[2];
    } else {
        rgb565_unpack(((const uint16_t *)src)[index], r, g, b);
    }
}

// Rec.601 luma; the weights sum to 256, so the >>8 keeps the result in 0..255.
inline uint8_t luma(uint8_t r, uint8_t g, uint8_t b) {
    return (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);
}

}  // namespace


void image_entropy_process(const void *src, int32_t src_w, int32_t src_h,
                           image_entropy_pixfmt_t fmt,
                           uint16_t *dst, int32_t dst_w, int32_t dst_h) {
    if (!src || !dst || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) {
        return;
    }

    // --- Step 1: aspect-crop-to-fill --------------------------------------

    // Pick the largest centered source rectangle whose aspect ratio matches the
    // destination, so the resize fills the whole display and only crops the
    // overflow axis (mirrors the app's resize_image_to_fill). Aspect ratios are
    // compared by cross-multiplication to stay in integer math:
    //   dst_w/dst_h  vs  src_w/src_h   ->   dst_w*src_h  vs  src_w*dst_h
    int64_t crop_x = 0, crop_y = 0, crop_w = src_w, crop_h = src_h;

    int64_t dst_by_src = (int64_t)dst_w * src_h;
    int64_t src_by_dst = (int64_t)src_w * dst_h;

    if (dst_by_src > src_by_dst) {
        // Destination is wider than the source: keep full width, crop height.
        crop_h = (int64_t)src_w * dst_h / dst_w;
        crop_y = (src_h - crop_h) / 2;
    } else if (dst_by_src < src_by_dst) {
        // Destination is taller than the source: keep full height, crop width.
        crop_w = (int64_t)src_h * dst_w / dst_h;
        crop_x = (src_w - crop_w) / 2;
    }
    // else: identical aspect ratio -> use the whole source frame.

    // Nearest-neighbour resize of the crop rectangle into dst (RGB565).
    for (int32_t y = 0; y < dst_h; ++y) {
        int64_t sy = crop_y + (int64_t)y * crop_h / dst_h;
        if (sy >= src_h) sy = src_h - 1;

        for (int32_t x = 0; x < dst_w; ++x) {
            int64_t sx = crop_x + (int64_t)x * crop_w / dst_w;
            if (sx >= src_w) sx = src_w - 1;

            uint8_t r, g, b;
            read_src_pixel(src, fmt, (size_t)(sy * src_w + sx), &r, &g, &b);
            dst[(size_t)y * dst_w + x] = rgb565_pack(r, g, b);
        }
    }

    // --- Step 2a: luminance histogram of the resized frame ----------------

    const int32_t total = dst_w * dst_h;
    uint32_t histogram[256] = {0};
    for (int32_t i = 0; i < total; ++i) {
        uint8_t r, g, b;
        rgb565_unpack(dst[i], &r, &g, &b);
        histogram[luma(r, g, b)]++;
    }

    // --- Step 2b: black / white points at a 2% clip on each end -----------
    // Walking in from each end until we have passed `cutoff` pixels makes the
    // stretch robust to a few hot/dead pixels (the "minor clipping").

    const int32_t cutoff = (total * 2) / 100;   // 2% of pixels
    int lo = 0, hi = 255;
    int32_t accumulated = 0;
    for (lo = 0; lo < 256; ++lo) {
        accumulated += histogram[lo];
        if (accumulated > cutoff) break;
    }
    accumulated = 0;
    for (hi = 255; hi > 0; --hi) {
        accumulated += histogram[hi];
        if (accumulated > cutoff) break;
    }

    // Degenerate (flat / near-flat) frame: nothing to stretch, leave dst as-is.
    if (hi <= lo) return;

    // --- Step 2c: build the shared stretch LUT ----------------------------
    // f(v) = clamp((v - lo) * 255 / (hi - lo), 0, 255), applied identically to
    // every channel, so channel differences scale by one factor -> hue kept.

    uint8_t lut[256];
    const int span = hi - lo;
    for (int v = 0; v < 256; ++v) {
        if (v <= lo)      lut[v] = 0;
        else if (v >= hi) lut[v] = 255;
        else              lut[v] = (uint8_t)(((v - lo) * 255) / span);
    }

    // --- Step 2d: apply the LUT in place ----------------------------------

    for (int32_t i = 0; i < total; ++i) {
        uint8_t r, g, b;
        rgb565_unpack(dst[i], &r, &g, &b);
        dst[i] = rgb565_pack(lut[r], lut[g], lut[b]);
    }
}
