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
//   1. Aspect-FIT with a CAPPED letterbox — scale the source to fit inside the
//      display at its native aspect ratio and centre it, black-filling the
//      leftover strip (letterbox when the display is relatively taller,
//      pillarbox when it is relatively wider). Nothing is ever stretched.
//
//      Bars are capped at 20% of the destination axis; past that the source is
//      centre-cropped instead, trading SOME letterbox for SOME crop rather than
//      all of either. Panels whose aspect suits the frame never hit the cap and
//      show it whole: a 3:2 still is complete on the 800x480 P4 (10% bars) but
//      would have blacked out a third of the 240x240 Pi Zero without the cap.
//
//      This deliberately differs from the app's PIL resize_image_to_fill, which
//      crops to fill. Fill is invisible on a square panel, but on the 800x480
//      P4 4.3" it silently discarded 20% (4:3 sensor) to 40% (square frame) of
//      the frame height -- the user cannot review what they cannot see.
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

// Aspect-fit + luminance stretch `src` (src_w x src_h, `fmt`) into the caller-
// allocated RGB565 `dst` (dst_w x dst_h, i.e. dst_w*dst_h uint16_t). Writes the
// FULL dst buffer -- the fitted image plus its black bars -- so the result can be
// blitted straight over the live preview. No allocation, no LVGL. Safe no-op if
// any dimension is <= 0 or a pointer is NULL.
void image_entropy_process(const void *src, int32_t src_w, int32_t src_h,
                           image_entropy_pixfmt_t fmt,
                           uint16_t *dst, int32_t dst_w, int32_t dst_h);

#ifdef __cplusplus
}
#endif

#endif // SEEDSIGNER_IMAGE_ENTROPY_H
