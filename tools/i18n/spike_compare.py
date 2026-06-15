#!/usr/bin/env python3
"""
THROWAWAY spike validator — diff the on-device glyph-run renders against the
libraqm reference oracles (see docs/complex-script-shaping-spike-plan.md, Step 3).

For each case it loads spike_ref_<name>.png (FreeType+HarfBuzz oracle) and
spike_dev_<name>.png (tiny_ttf/stb by-glyph-id), crops both to their ink
bounding box, and — because stb and FreeType hint/round sub-pixels slightly
differently, and the left anchor can sit a pixel off — searches a tiny ±3px
shift for the best alignment. It then reports:

  * ink IoU  — intersection-over-union of the thresholded ink masks. This is the
    STRUCTURAL metric: same glyphs, same visual order, marks stacked and the
    Nastaliq cascade at the right offsets all push IoU toward 1.0; a missing
    mark / wrong glyph / flat-vs-cascade render tanks it.
  * mean |Δ| over the union bbox — the AA-sensitivity sanity check.

PASS = ink IoU >= IOU_PASS (structural match within AA tolerance). A side-by-side
spike_cmp_<name>.png (reference | device | overlay) is written for the eyeball.

Usage:  .venv/bin/python tools/i18n/spike_compare.py [spike_out_dir]
"""

import os
import sys

import numpy as np
from PIL import Image

NAMES = ["hi", "ur", "th", "fa", "insertion"]
INK = 128          # coverage >= INK counts as ink
SHIFT = 3          # max ± alignment search (px) — both renders land at the same
                   # canvas position, so this only absorbs sub-pixel rounding
IOU_PASS = 0.80    # structural-match threshold


def load_gray(path):
    return np.asarray(Image.open(path).convert("L"), dtype=np.int16)


def ink_bbox(a):
    ys, xs = np.where(a >= INK)
    if len(xs) == 0:
        return None
    return xs.min(), ys.min(), xs.max() + 1, ys.max() + 1


def crop_to_ink(a):
    bb = ink_bbox(a)
    if bb is None:
        return np.zeros((1, 1), dtype=np.int16)
    x0, y0, x1, y1 = bb
    return a[y0:y1, x0:x1]


def score(ref, dev):
    """Best (IoU, mean|Δ|, shift) over a small alignment search. Both images are
    cropped to their ink bbox, so (0,0) means perfectly co-located; the search
    only absorbs sub-pixel rasterizer rounding."""
    h = max(ref.shape[0], dev.shape[0]) + 2 * SHIFT
    w = max(ref.shape[1], dev.shape[1]) + 2 * SHIFT
    R = np.zeros((h, w), dtype=np.int16)
    R[SHIFT:SHIFT + ref.shape[0], SHIFT:SHIFT + ref.shape[1]] = ref
    Rmask = R >= INK
    best = (-1.0, 1e9, (0, 0))
    for dy in range(-SHIFT, SHIFT + 1):
        for dx in range(-SHIFT, SHIFT + 1):
            D = np.zeros((h, w), dtype=np.int16)
            ys, xs = SHIFT + dy, SHIFT + dx
            D[ys:ys + dev.shape[0], xs:xs + dev.shape[1]] = dev
            Dmask = D >= INK
            inter = np.logical_and(Rmask, Dmask).sum()
            union = np.logical_or(Rmask, Dmask).sum()
            iou = inter / union if union else 1.0
            if iou > best[0]:
                mad = float(np.abs(R - D).mean())
                best = (iou, mad, (dx, dy))
    return best


def composite(ref, dev, shift, path):
    """reference | device | overlay(ref=green, dev=magenta) — eyeball aid."""
    dx, dy = shift
    h = max(ref.shape[0], dev.shape[0]) + 2 * SHIFT
    w = max(ref.shape[1], dev.shape[1]) + 2 * SHIFT
    R = np.zeros((h, w), dtype=np.int16)
    R[SHIFT:SHIFT + ref.shape[0], SHIFT:SHIFT + ref.shape[1]] = ref
    D = np.zeros((h, w), dtype=np.int16)
    D[SHIFT + dy:SHIFT + dy + dev.shape[0], SHIFT + dx:SHIFT + dx + dev.shape[1]] = dev
    overlay = np.zeros((h, w, 3), dtype=np.uint8)
    overlay[..., 1] = R.astype(np.uint8)   # reference -> green
    overlay[..., 0] = D.astype(np.uint8)   # device    -> red+blue = magenta
    overlay[..., 2] = D.astype(np.uint8)
    gap = np.zeros((h, 8), dtype=np.uint8)
    strip = np.concatenate([R.astype(np.uint8), gap,
                            D.astype(np.uint8), gap], axis=1)
    strip_rgb = np.stack([strip, strip, strip], axis=-1)
    out = np.concatenate([strip_rgb, overlay], axis=1)
    Image.fromarray(out, "RGB").save(path)


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "spike_out")
    print(f"{'case':<10} {'ink IoU':>8} {'mean|Δ|':>8} {'shift':>8}   result")
    print("-" * 52)
    all_pass = True
    for name in NAMES:
        ref_p = os.path.join(d, f"spike_ref_{name}.png")
        dev_p = os.path.join(d, f"spike_dev_{name}.png")
        if not (os.path.exists(ref_p) and os.path.exists(dev_p)):
            print(f"{name:<10} (missing PNG)")
            all_pass = False
            continue
        ref = crop_to_ink(load_gray(ref_p))
        dev = crop_to_ink(load_gray(dev_p))
        iou, mad, shift = score(ref, dev)
        composite(ref, dev, shift, os.path.join(d, f"spike_cmp_{name}.png"))
        ok = iou >= IOU_PASS
        all_pass &= ok
        print(f"{name:<10} {iou:>8.3f} {mad:>8.2f} {str(shift):>8}   "
              f"{'PASS' if ok else 'FAIL'}")
    print("-" * 52)
    print("ALL PASS" if all_pass else "SOME FAILED")
    return 0 if all_pass else 1


if __name__ == "__main__":
    sys.exit(main())
