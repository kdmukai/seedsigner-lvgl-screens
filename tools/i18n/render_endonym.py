#!/usr/bin/env python3
"""
Offline endonym rasterizer for the SeedSigner language picker.

The language-selection screen must show every onboard language's name in its OWN
native script (日本語, हिन्दी, Русский, فارسی, …) on one screen — but the native
LVGL path cannot keep 7-11 script fonts resident at once (it blows the ESP32's
small glyph-cache pool). So for every language that ships a font pack we PRE-RENDER
its endonym here, offline, into a small A8 alpha image; the picker blits that image
(tinted to the row's text color) with ZERO runtime fonts. Latin-script endonyms
covered by the baked floor stay live text and are NOT rendered here.

Shaping (Devanagari conjuncts, Thai stacking, Nastaliq/Arabic joining + RTL) is
resolved here by PIL's libraqm layout engine — the same shaper the pack-shaping
validation oracle uses — so the device does zero shaping for the picker.

Output: one `endonym_<height>.bin` per distinct display-profile height, each a
tight-cropped A8 alpha bitmap in the self-describing "SSA8" container below. The
C picker (components/seedsigner/locale_picker.*) parses it into an lv_image_dsc_t.

Standalone use (one language, ad-hoc sizes):
  python3 render_endonym.py --text "हिन्दी" \
      --font ../../components/seedsigner/assets/NotoSansDevanagari-Regular.ttf \
      --out /tmp/hi --size 240:20 --size 320:26 --size 480:40
build_fontpacks.py imports render_endonym_images() and supplies per-profile sizes
read from the render layer's own manifest (screenshot_gen --dump-locales).
"""

import argparse
import os
import struct

from PIL import Image, ImageDraw, ImageFont

# ---------------------------------------------------------------------------
# "SSA8" A8-alpha container (little-endian). Kept self-describing so the blob the
# host hands the picker through the pack provider carries its own dimensions —
# no external metadata needed to draw it. Mirror-parsed in C (locale_picker.cpp).
#
#   off  size  field
#   0    4     magic  "SSA8"
#   4    1     version = 1
#   5    1     bpp     = 8   (A8; informational — the format IS A8)
#   6    2     width   (u16)
#   8    2     height  (u16)
#   10   2     reserved = 0
#   12   w*h   A8 alpha bytes, row-major, stride = width (1 byte/px)
# ---------------------------------------------------------------------------
A8_MAGIC = b"SSA8"
A8_VERSION = 1
A8_HEADER_LEN = 12


def _pack_a8(img_l):
    """Serialize a PIL 'L' (8-bit grayscale = coverage/alpha) image into an SSA8
    blob. The L value IS the alpha the picker tints; stride is tight (= width)."""
    w, h = img_l.size
    if not (0 < w <= 0xFFFF and 0 < h <= 0xFFFF):
        raise ValueError(f"endonym image {w}x{h} out of range for u16 header")
    header = A8_MAGIC + bytes([A8_VERSION, 8]) + struct.pack("<HHH", w, h, 0)
    return header + img_l.tobytes()  # 'L' tobytes() is row-major, 1 byte/px


def render_endonym_bitmap(text, font_path, px, *, tight_pad=1):
    """Render `text` at `px` using libraqm layout (complex-script shaping + bidi)
    to a tight-cropped PIL 'L' image (ink only). Returns None if the string has no
    ink (e.g. empty). Raises if the font can't be loaded."""
    font = ImageFont.truetype(font_path, px, layout_engine=ImageFont.Layout.RAQM)

    # Ink bbox from the shaped layout (getbbox uses the font's RAQM engine).
    left, top, right, bottom = font.getbbox(text)
    w = max(1, right - left)
    h = max(1, bottom - top)

    # Draw onto a padded canvas offset so the ink starts inside it, then crop to
    # the actual non-zero pixels — robust to marks/overhangs the bbox may clip.
    canvas = Image.new("L", (w + 2 * tight_pad, h + 2 * tight_pad), 0)
    draw = ImageDraw.Draw(canvas)
    draw.text((tight_pad - left, tight_pad - top), text, fill=255, font=font)

    bbox = canvas.getbbox()
    if bbox is None:
        return None
    return canvas.crop(bbox)


def render_endonym_images(text, font_path, sizes, out_dir):
    """Render `text` at each (height_key, px) in `sizes` -> out_dir/endonym_<key>.bin.
    `sizes` is a list of (str key, int px). Returns a dict
      { key: {"file": "endonym_<key>.bin", "w": W, "h": H, "bytes": N}, ... }
    describing each emitted blob (for the pack manifest)."""
    os.makedirs(out_dir, exist_ok=True)
    result = {}
    for key, px in sizes:
        img = render_endonym_bitmap(text, font_path, px)
        if img is None:
            continue
        blob = _pack_a8(img)
        fname = f"endonym_{key}.bin"
        with open(os.path.join(out_dir, fname), "wb") as f:
            f.write(blob)
        w, h = img.size
        result[key] = {"file": fname, "w": w, "h": h, "bytes": len(blob)}
    return result


def _parse_size(spec):
    """'240:20' -> ('240', 20)."""
    key, _, px = spec.partition(":")
    if not px:
        raise argparse.ArgumentTypeError(f"--size expects KEY:PX, got {spec!r}")
    return (key, int(px))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--text", required=True, help="endonym string (native script)")
    ap.add_argument("--font", required=True, help="source TTF (full, not the subset)")
    ap.add_argument("--out", required=True, help="output dir for endonym_<key>.bin")
    ap.add_argument("--size", action="append", type=_parse_size, required=True,
                    metavar="KEY:PX", help="profile key + px, repeatable (e.g. 240:20)")
    args = ap.parse_args()

    meta = render_endonym_images(args.text, args.font, args.size, args.out)
    for key, m in meta.items():
        print(f"[endonym] {key}: {m['file']}  {m['w']}x{m['h']}  ({m['bytes']} bytes)")
    return 0 if meta else 1


if __name__ == "__main__":
    raise SystemExit(main())
