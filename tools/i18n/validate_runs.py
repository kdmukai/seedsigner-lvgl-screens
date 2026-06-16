#!/usr/bin/env python3
"""
validate_runs.py — end-to-end validation of a PRODUCTION glyph-run pack.

Proves that the runs.json + subset .ttf a complex-script pack ships actually
render correctly through the real on-device glyph-id path — not just that the
offline shaping is internally consistent. For each selected string it:

  1. reads the pre-shaped glyph run straight from lang-packs/<loc>/runs.json and
     the subset lang-packs/<loc>/<loc>.ttf the pack ships,
  2. renders a libraqm ORACLE (FreeType + HarfBuzz) of the same string against the
     same subset — an INDEPENDENT rasterizer,
  3. drives the existing `screenshot_gen --shape-spike` harness (tiny_ttf/stb,
     by glyph-id — the device path) over the run, and
  4. compares the two with the spike's ink-IoU structural metric.

This is the "rasterize-all release-validation gate" the plan calls for, run on the
shipped artifacts. It reuses the (committed) spike harness as the device-render
engine and its IoU comparator; if the spike is ever retired, fold those two
helpers in here.

Usage:
    .venv/bin/python tools/i18n/validate_runs.py --locale hi [--locale th ...]
    .venv/bin/python tools/i18n/validate_runs.py --all-locales
Options:
    --msgids-all   validate every single-line plain run (default: the test-screen
                   set below — main menu + status screens)
"""

import argparse
import json
import os
import shutil
import struct
import subprocess
import sys

from PIL import Image, ImageDraw, ImageFont, features

# Reuse the spike's structural IoU comparator (committed). These are the only
# imports from throwaway code; fold them in here if the spike is removed.
from spike_compare import IOU_PASS, crop_to_ink, load_gray, score

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

# Canvas geometry — MUST match shape_spike.cpp's meta.json defaults so the device
# render and the oracle land at the same pixels (white-on-black, baseline anchor).
CANVAS_W, CANVAS_H = 800, 240
BASELINE_Y, MARGIN_X, PX = 160, 24, 48
FG, BG = (255, 255, 255), (0, 0, 0)

# Default validation set: the strings on the screens this work targets (main menu
# + large-icon status). Keyed by English msgid; present in every catalog. Picked
# because they exercise reorder/conjunct (hi), mark stacking (th) and the Nastaliq
# cascade (ur) over real UI text rather than cherry-picked words.
TEST_MSGIDS = [
    "Home", "Scan", "Seeds", "Tools", "Settings",
    "Backup Verified", "Success!",
    "All mnemonic backup words were successfully verified!", "OK",
    "Caution", "SeedQR is your private key!",
    "Never photograph or scan it into a device that connects to the internet.",
    "I understand",
]


def write_runs_bin(path, lines):
    """Emit the spike "SSR1" run table that shape_spike.cpp reads (see
    spike_shape.py for the format)."""
    with open(path, "wb") as f:
        f.write(b"SSR1")
        f.write(struct.pack("<I", len(lines)))
        for ln in lines:
            for field in ("name", "font_file", "text"):
                b = ln[field].encode("utf-8")
                f.write(struct.pack("<H", len(b))); f.write(b)
            dir_flag = 1 if ln["direction"] == "rtl" else 0
            f.write(struct.pack("<HHBB", ln["upem"], PX, dir_flag, 0))
            f.write(struct.pack("<I", len(ln["glyphs"])))
            for g in ln["glyphs"]:
                f.write(struct.pack("<Iiiii I", g["gid"],
                                    g["x_advance"], g["y_advance"],
                                    g["x_offset"], g["y_offset"], g.get("cluster", 0)))


def render_oracle(ttf_path, text, direction, language, out_png):
    """libraqm reference render (the independent oracle)."""
    img = Image.new("RGB", (CANVAS_W, CANVAS_H), BG)
    draw = ImageDraw.Draw(img)
    fnt = ImageFont.truetype(ttf_path, PX, layout_engine=ImageFont.Layout.RAQM)
    draw.text((MARGIN_X, BASELINE_Y), text, font=fnt, fill=FG,
              anchor="ls", direction=direction, language=language)
    img.save(out_png)


def select_runs(runs_doc, want_all):
    """Pick single-line plain runs to validate (the spike harness renders one
    glyph run per line). Multi-line/segmented runs are out of scope for this
    pixel oracle and skipped with a note."""
    by_id = {r["msgid"]: r for r in runs_doc["runs"] if r["kind"] == "plain"}
    ids = sorted(by_id) if want_all else [m for m in TEST_MSGIDS if m in by_id]
    out, skipped = [], []
    for m in ids:
        r = by_id[m]
        if len(r["lines"]) != 1:
            skipped.append(m)
            continue
        out.append(r)
    return out, skipped


def validate_locale(locale, font_dir, want_all):
    pack = os.path.join(font_dir, locale)
    runs_doc = json.load(open(os.path.join(pack, "runs.json"), encoding="utf-8"))
    subset = os.path.join(pack, runs_doc["font"])
    direction, language, upem = runs_doc["direction"], locale, runs_doc["upem"]

    selected, skipped = select_runs(runs_doc, want_all)
    if not selected:
        print(f"[{locale}] no validatable runs found", file=sys.stderr)
        return False

    out_dir = os.path.join(os.path.dirname(__file__), "validate_out", locale)
    os.makedirs(out_dir, exist_ok=True)
    font_file = f"{locale}.ttf"
    shutil.copyfile(subset, os.path.join(out_dir, font_file))

    # meta.json so shape_spike.cpp uses the same canvas as the oracle.
    json.dump({"px": PX, "canvas_w": CANVAS_W, "canvas_h": CANVAS_H,
               "baseline_y": BASELINE_Y, "margin_x": MARGIN_X, "fg": list(FG), "bg": list(BG)},
              open(os.path.join(out_dir, "meta.json"), "w"))

    lines = []
    for i, r in enumerate(selected):
        name = f"r{i:03d}"
        render_oracle(subset, r["text"], direction, language,
                      os.path.join(out_dir, f"spike_ref_{name}.png"))
        lines.append({"name": name, "font_file": font_file, "text": r["text"],
                      "upem": upem, "direction": direction, "glyphs": r["lines"][0]["glyphs"]})
    write_runs_bin(os.path.join(out_dir, "spike_runs.bin"), lines)

    # Device render through the proven glyph-id path.
    gen = os.path.join(REPO_ROOT, "tools/apps/screenshot_generator/build/screenshot_gen")
    subprocess.run([gen, "--shape-spike", out_dir], check=True,
                   stdout=subprocess.DEVNULL)

    print(f"\n=== {locale} ({len(lines)} strings){' [skipped multi-line: ' + ','.join(skipped) + ']' if skipped else ''} ===")
    print(f"{'msgid':<48} {'IoU':>6}  result")
    print("-" * 66)
    all_pass = True
    for r, ln in zip(selected, lines):
        ref = crop_to_ink(load_gray(os.path.join(out_dir, f"spike_ref_{ln['name']}.png")))
        dev = crop_to_ink(load_gray(os.path.join(out_dir, f"spike_dev_{ln['name']}.png")))
        iou, _mad, _shift = score(ref, dev)
        ok = iou >= IOU_PASS
        all_pass &= ok
        label = (r["msgid"][:45] + "...") if len(r["msgid"]) > 45 else r["msgid"]
        print(f"{label:<48} {iou:>6.3f}  {'PASS' if ok else 'FAIL'}")
    return all_pass


def main():
    if not features.check("raqm"):
        print("ERROR: Pillow built without libraqm; cannot render the oracle.", file=sys.stderr)
        return 1
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--locale", action="append", default=[], help="locale to validate (repeatable)")
    ap.add_argument("--all-locales", action="store_true", help="validate every shaping pack present")
    ap.add_argument("--font-dir", default=os.path.join(REPO_ROOT, "lang-packs"))
    ap.add_argument("--msgids-all", action="store_true", help="validate all single-line plain runs")
    args = ap.parse_args()

    locales = args.locale
    if args.all_locales:
        locales = sorted(d for d in os.listdir(args.font_dir)
                         if os.path.exists(os.path.join(args.font_dir, d, "runs.json")))
    if not locales:
        print("No locales selected (use --locale or --all-locales).", file=sys.stderr)
        return 1

    overall = True
    for loc in locales:
        overall &= validate_locale(loc, args.font_dir, args.msgids_all)
    print("\n" + ("ALL PASS" if overall else "SOME FAILED"))
    return 0 if overall else 1


if __name__ == "__main__":
    sys.exit(main())
