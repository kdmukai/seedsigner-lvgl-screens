#include "image_entropy.h"

#include <stddef.h>   // NULL, size_t
#include <stdint.h>
#include <string.h>   // memset (letter-/pillarbox fill)

// ---------------------------------------------------------------------------
// Image-entropy display processing (see image_entropy.h for the contract).
//
// After the fit/resize into `dst`, two passes over the fitted image rect: one
// to build a luminance histogram, one to apply the stretch LUT. A few ms for a
// full-display frame, sub-1 KB of tables. No floats, no allocation, no LVGL.
// ---------------------------------------------------------------------------

namespace {

// Most of the destination axis (as a percentage) that may go to letter-/pillarbox
// bars before we start centre-cropping the source instead. See step 1.
//
// 20 is the knee where the two panels we ship land on opposite sides:
//   * 800x480 (P4 4.3"), 3:2 still -> 10% bars: under the cap, shown WHOLE. A 4:3
//     still lands at exactly 20%, also whole. Widescreen panels suit widescreen
//     stills, so the cap simply never binds there -- which is the intent.
//   * 240x240 (Pi Zero), 3:2 still -> pure fit would bar 33% of the panel. The cap
//     binds: 20% bars, and ~17% of the frame width centre-cropped. Neither
//     extreme, which is the compromise a square panel needs.
// Raising this favours completeness on square panels; lowering it favours filling
// them. It is one number precisely so the trade-off stays reviewable.
constexpr int32_t kMaxBarPercent = 20;

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

    // --- Step 1: aspect-fit with a capped letterbox -----------------------

    // Two goals pull against each other:
    //   * Show the WHOLE frame. The user is reviewing what they just captured;
    //     hiding part of it defeats the screen.
    //   * Don't drown a small panel in black. On the 240x240 Pi Zero a 3:2 still
    //     letterboxes a THIRD of the screen away, which reads as a bug.
    // So: fit the frame, but cap the bars at kMaxBarPercent of the destination
    // axis. When the cap binds, scale past the pure fit and crop the overflow --
    // yielding SOME letterbox and SOME crop rather than all of either. Panels
    // whose aspect already suits the frame never reach the cap and show it whole.
    //
    // The surviving bars are deliberate signal, not just leftovers: together with
    // the contrast stretch (step 2) they make the frozen review frame read as
    // clearly apart from the live preview it replaced.
    //
    // Aspect ratios compare by cross-multiplication to stay in integer math:
    //   dst_w/dst_h  vs  src_w/src_h   ->   dst_w*src_h  vs  src_w*dst_h
    //
    // Produces two rects: the source CROP (what we sample) and the destination
    // CONTENT rect (where it lands). Pure fit is just crop == whole source.
    int64_t crop_x = 0, crop_y = 0, crop_w = src_w, crop_h = src_h;
    int32_t content_w = dst_w, content_h = dst_h;

    const int64_t dst_by_src = (int64_t)dst_w * src_h;
    const int64_t src_by_dst = (int64_t)src_w * dst_h;

    if (dst_by_src > src_by_dst) {
        // Destination relatively wider than the source -> pillarbox (side bars).
        content_w = (int32_t)((int64_t)src_w * dst_h / src_h);

        const int32_t max_bars = dst_w * kMaxBarPercent / 100;
        if (dst_w - content_w > max_bars) {
            // Cap binds: widen the image to leave only max_bars of side bar. The
            // scale is now content_w/src_w, so only dst_h/scale source rows still
            // fit the panel height -- centre-crop the source vertically to those.
            content_w = dst_w - max_bars;
            crop_h    = (int64_t)dst_h * src_w / content_w;
            if (crop_h > src_h) crop_h = src_h;
            crop_y    = (src_h - crop_h) / 2;
        }
    } else if (dst_by_src < src_by_dst) {
        // Destination relatively taller than the source -> letterbox (top/bottom).
        content_h = (int32_t)((int64_t)src_h * dst_w / src_w);

        const int32_t max_bars = dst_h * kMaxBarPercent / 100;
        if (dst_h - content_h > max_bars) {
            // Cap binds: the mirror of the branch above, axes swapped.
            content_h = dst_h - max_bars;
            crop_w    = (int64_t)dst_w * src_h / content_h;
            if (crop_w > src_w) crop_w = src_w;
            crop_x    = (src_w - crop_w) / 2;
        }
    }
    // else: identical aspect ratio -> content == destination, no bars, no crop.

    // Extreme aspect mismatches can round a dimension to zero; keep it drawable.
    if (content_w < 1) content_w = 1;
    if (content_h < 1) content_h = 1;

    const int32_t off_x = (dst_w - content_w) / 2;
    const int32_t off_y = (dst_h - content_h) / 2;

    // Black out the whole destination first, so the letter-/pillarbox strips are
    // opaque black over whatever the live preview last left underneath. RGB565
    // 0x0000 is black, so a plain zero fill does it.
    if (off_x > 0 || off_y > 0) {
        memset(dst, 0, (size_t)dst_w * dst_h * sizeof(uint16_t));
    }

    // Box-filter resample of the crop rect into the content rect: each
    // destination pixel averages the source pixels its footprint covers.
    //
    // The averaging is what makes a high-resolution still worth capturing. Point
    // sampling a 720x480 still down to a 240-wide panel would keep 1 pixel in 9
    // and throw the rest away -- aliasing edges and dropping detail badly enough
    // that the extra capture resolution could look WORSE than a preview frame.
    //
    // One path serves both directions: when magnifying, a footprint rounds down
    // to a single source pixel and the average degenerates to exactly the
    // nearest-neighbour sample, so upscales are unchanged. Cost is one pass over
    // the source, a few ms even for a full-resolution still on a Pi Zero.
    for (int32_t y = 0; y < content_h; ++y) {
        // Source row band feeding this destination row.
        int64_t sy0 = crop_y + (int64_t)y       * crop_h / content_h;
        int64_t sy1 = crop_y + (int64_t)(y + 1) * crop_h / content_h;
        if (sy1 <= sy0)             sy1 = sy0 + 1;          // magnifying: one row
        if (sy1 > crop_y + crop_h)  sy1 = crop_y + crop_h;  // clamp to the crop

        uint16_t *dst_row = dst + (size_t)(off_y + y) * dst_w + off_x;

        for (int32_t x = 0; x < content_w; ++x) {
            // Source column band feeding this destination pixel.
            int64_t sx0 = crop_x + (int64_t)x       * crop_w / content_w;
            int64_t sx1 = crop_x + (int64_t)(x + 1) * crop_w / content_w;
            if (sx1 <= sx0)             sx1 = sx0 + 1;
            if (sx1 > crop_x + crop_w)  sx1 = crop_x + crop_w;

            // Sum the footprint. A band is at most a few hundred pixels even for
            // a wild downscale, so 8-bit channels cannot overflow 32 bits.
            uint32_t sum_r = 0, sum_g = 0, sum_b = 0, count = 0;
            for (int64_t sy = sy0; sy < sy1; ++sy) {
                for (int64_t sx = sx0; sx < sx1; ++sx) {
                    uint8_t r, g, b;
                    read_src_pixel(src, fmt, (size_t)(sy * src_w + sx), &r, &g, &b);
                    sum_r += r; sum_g += g; sum_b += b;
                    ++count;
                }
            }

            // count >= 1 always: both bands were forced non-empty above.
            dst_row[x] = rgb565_pack((uint8_t)(sum_r / count),
                                     (uint8_t)(sum_g / count),
                                     (uint8_t)(sum_b / count));
        }
    }

    // --- Step 2a: luminance histogram of the resized frame ----------------

    // Only the content rect is sampled: the black bars are presentation padding,
    // not scene content, and counting them would hand the entire 2% shadow clip
    // to pure black and effectively disable the black point.
    const int32_t total = content_w * content_h;
    uint32_t histogram[256] = {0};
    for (int32_t y = 0; y < content_h; ++y) {
        const uint16_t *dst_row = dst + (size_t)(off_y + y) * dst_w + off_x;
        for (int32_t x = 0; x < content_w; ++x) {
            uint8_t r, g, b;
            rgb565_unpack(dst_row[x], &r, &g, &b);
            histogram[luma(r, g, b)]++;
        }
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
    // Again over the content rect only. lut[0] is always 0, so the bars would
    // survive a full-buffer pass anyway -- but skipping them keeps the bars
    // provably untouched and saves the work.

    for (int32_t y = 0; y < content_h; ++y) {
        uint16_t *dst_row = dst + (size_t)(off_y + y) * dst_w + off_x;
        for (int32_t x = 0; x < content_w; ++x) {
            uint8_t r, g, b;
            rgb565_unpack(dst_row[x], &r, &g, &b);
            dst_row[x] = rgb565_pack(lut[r], lut[g], lut[b]);
        }
    }
}
