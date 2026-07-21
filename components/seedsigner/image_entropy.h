#ifndef SEEDSIGNER_IMAGE_ENTROPY_H
#define SEEDSIGNER_IMAGE_ENTROPY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Image-entropy display processing — portable, LVGL-free pixel math
// ---------------------------------------------------------------------------
// Turns a raw camera frame into the RGB565 buffer shown in the image-entropy
// CONFIRM phase (camera_entropy_overlay_set_confirm_image). Two steps, in order:
//
//   1. Aspect-FIT — scale the source to fit inside the display at its native
//      aspect ratio and centre it, black-filling the leftover strip (letterbox
//      when the display is relatively taller, pillarbox when it is relatively
//      wider). Nothing is stretched and, deliberately, nothing is cropped: a
//      screen whose only job is to confirm the capture must show it WHOLE. Every
//      ESP32 panel is landscape and is fed a SQUARE still, so the frame lands as
//      a centred square with pillar bars (160 px each side on the 800x480 P4 4.3",
//      80 px on the 480x320 P4 3.5"); the square 240x240 Pi Zero fits it exactly.
//
//      This deliberately differs from the app's PIL resize_image_to_fill, which
//      crops to fill. Fill is invisible on a square panel, but on the 800x480
//      P4 4.3" it would silently discard ~40% of a square frame's width -- the
//      user cannot review what they cannot see.
//
//      Resampling is a BOX FILTER: each destination pixel averages the source
//      pixels its footprint covers, degenerating to nearest-neighbour when
//      magnifying. This is what makes a high-resolution still worth capturing --
//      point-sampling a 720x480 still onto a 240-wide panel would keep 1 pixel
//      in 9 and alias badly enough to look worse than a preview frame.
//
//   2. Luminance uniform contrast stretch — one luminance histogram, a 2% black-
//      /white-point clip, one LUT applied identically to R, G and B. Levels
//      shadows and highlights without the per-channel colour casts that PIL's
//      autocontrast(cutoff=2) produced (one channel stretched far harder than the
//      others). Because the map is a single shared affine, hue is preserved.
//      Sampled and applied over the image rect ONLY — the bars are padding, and
//      letting them into the histogram would spend the whole 2% shadow clip on
//      pure black and neutralise the black point.
//
// Together the bars and the contrast stretch are also deliberate SIGNAL: they
// make the frozen review frame read as clearly apart from the live preview.
//
// This is a DISPLAY copy only: it never touches the entropy chain or the raw
// latched frame handed to the app (see docs/image-entropy-lvgl-native-contract.md).
// Deliberately free of LVGL so it can be unit-/pixel-gated on its own.

// Supported source pixel formats (the destination is always RGB565).
typedef enum {
    IMAGE_ENTROPY_PIXFMT_RGB565 = 0,  // 16-bit native-endian, R[15:11] G[10:5] B[4:0]
    IMAGE_ENTROPY_PIXFMT_RGB888 = 1,  // 3 bytes/pixel, R, G, B order
} image_entropy_pixfmt_t;

// Post-resize processing variant. The resize itself is always the box filter
// (fast, allocation-free); this selects whether a sharpen pass follows it.
typedef enum {
    // Box downscale only. The DEFAULT: what image_entropy_process() uses, and what
    // the Pi Zero and the desktop tooling stay on (small panels; already good).
    IMAGE_ENTROPY_FILTER_BOX = 0,
    // Box downscale + a mild luminance unsharp mask. A cheap edge-acuity boost (a
    // light per-pixel sharpen over the fitted rect, integer math, ~single-digit ms
    // for a full panel), so the frozen review frame reads as crisper than the live
    // preview without the multi-second cost of a wide bicubic reconstruction. The
    // ESP32-P4 opts into this for its one-shot confirm image. It enhances EXISTING
    // edges rather than recovering detail (there is none to recover on the P4 --
    // see the entropy still notes) and, like any sharpen, lifts high-frequency
    // NOISE as well as edges, so it is a "pop," not a fidelity gain.
    IMAGE_ENTROPY_FILTER_BOX_SHARPEN = 1,
} image_entropy_filter_t;

// Aspect-fit + luminance stretch `src` (src_w x src_h, `fmt`) into the caller-
// allocated RGB565 `dst` (dst_w x dst_h, i.e. dst_w*dst_h uint16_t). `filter`
// selects an optional trailing sharpen (see above). Writes the FULL dst buffer --
// the fitted image plus its black bars -- so the result can be blitted straight
// over the live preview. Allocation-free (both variants) and no LVGL. Safe no-op
// if any dimension is <= 0 or a pointer is NULL.
void image_entropy_process_filtered(const void *src, int32_t src_w, int32_t src_h,
                                    image_entropy_pixfmt_t fmt,
                                    uint16_t *dst, int32_t dst_w, int32_t dst_h,
                                    image_entropy_filter_t filter);

// Convenience wrapper: image_entropy_process_filtered() with the BOX filter. This
// is the stable, unchanged entry point -- existing callers keep the box behavior.
void image_entropy_process(const void *src, int32_t src_w, int32_t src_h,
                           image_entropy_pixfmt_t fmt,
                           uint16_t *dst, int32_t dst_w, int32_t dst_h);

#ifdef __cplusplus
}
#endif

#endif // SEEDSIGNER_IMAGE_ENTROPY_H
