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
//   1. Aspect-crop-to-FILL — resize the source frame to the full display,
//      cropping to the display's aspect ratio (the native equivalent of the
//      app's resize_image_to_fill: fill the whole panel, crop the overflow;
//      never letterbox). Nearest-neighbour sampling.
//
//   2. Luminance uniform contrast stretch — one luminance histogram, a 2% black-
//      /white-point clip, one LUT applied identically to R, G and B. Levels
//      shadows and highlights without the per-channel colour casts that PIL's
//      autocontrast(cutoff=2) produced (one channel stretched far harder than the
//      others). Because the map is a single shared affine, hue is preserved.
//
// This is a DISPLAY copy only: it never touches the entropy chain or the raw
// latched frame handed to the app (see docs/image-entropy-lvgl-native-contract.md).
// Deliberately free of LVGL so it can be unit-/pixel-gated on its own.

// Supported source pixel formats (the destination is always RGB565).
typedef enum {
    IMAGE_ENTROPY_PIXFMT_RGB565 = 0,  // 16-bit native-endian, R[15:11] G[10:5] B[4:0]
    IMAGE_ENTROPY_PIXFMT_RGB888 = 1,  // 3 bytes/pixel, R, G, B order
} image_entropy_pixfmt_t;

// Crop-to-fill + luminance stretch `src` (src_w x src_h, `fmt`) into the caller-
// allocated RGB565 `dst` (dst_w x dst_h, i.e. dst_w*dst_h uint16_t). No allocation,
// no LVGL. Safe no-op if any dimension is <= 0 or a pointer is NULL.
void image_entropy_process(const void *src, int32_t src_w, int32_t src_h,
                           image_entropy_pixfmt_t fmt,
                           uint16_t *dst, int32_t dst_w, int32_t dst_h);

#ifdef __cplusplus
}
#endif

#endif // SEEDSIGNER_IMAGE_ENTROPY_H
