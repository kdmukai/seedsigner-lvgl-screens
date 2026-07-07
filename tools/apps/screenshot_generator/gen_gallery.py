#!/usr/bin/env python3
"""Generate the multi-language screenshot gallery.

Runs the screenshot generator once per supported locale
(tools/i18n/supported_locales.json), each with that locale's translated scenarios
+ font pack, into a per-locale subdirectory of the output:

  <out>/<locale>/manifest.json + <out>/<locale>/img/<res>/*.png|gif
  <out>/locales.json   the picker index: [{code,name}] of the locales that rendered
  <out>/index.html     the multi-language viewer (copied from the source if needed)

The viewer (screenshots/index.html) reads locales.json, then <locale>/manifest.json
for the selected language.

Prerequisites (the caller ensures these exist): a built screenshot_gen, the
localized scenario catalogs (tools/i18n/gen_localized_scenarios.py), and the font
packs for the pack locales (built via the language-packs submodule:
`scripts/ci/ci.sh build-fontpacks`).

Usage:
  gen_gallery.py --out tools/apps/screenshot_generator/screenshots [--jobs N]
"""

import argparse
import concurrent.futures
import json
import os
import shutil
import subprocess
import sys

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
DEFAULT_GEN = os.path.join(REPO_ROOT, "tools/apps/screenshot_generator/build/screenshot_gen")
SRC_INDEX = os.path.join(REPO_ROOT, "tools/apps/screenshot_generator/screenshots/index.html")
SCENARIOS_DIR = os.path.join(REPO_ROOT, "tools/scenarios/localized")


def supported_locales():
    with open(os.path.join(REPO_ROOT, "tools/i18n/supported_locales.json"), encoding="utf-8") as f:
        return json.load(f)["locales"]


def label(e):
    # English readable name first, native name/script second (e.g. "Greek
    # (Ελληνικά)"); collapses to just the English name when they match.
    return e["english"] if e["native"] == e["english"] else f'{e["english"]} ({e["native"]})'


def render_one(code, gen_bin, out):
    """Render one locale's gallery into <out>/<code>/ (own manifest.json + img/)."""
    scenarios = os.path.join(SCENARIOS_DIR, f"{code}.json")
    cmd = [gen_bin, "--locale", code, "--scenarios-file", scenarios,
           "--out-dir", os.path.join(out, code)]
    r = subprocess.run(cmd, cwd=REPO_ROOT, capture_output=True, text=True)
    return code, r.returncode, (r.stdout or "") + (r.stderr or "")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True, help="gallery root (gets <locale>/ + locales.json + index.html)")
    ap.add_argument("--gen-bin", default=DEFAULT_GEN, help="screenshot_gen binary")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 1),
                    help="parallel locales (each generator is single-threaded)")
    args = ap.parse_args()

    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)
    locales = supported_locales()
    codes = [l["code"] for l in locales]

    print(f"gen_gallery: rendering {len(codes)} locales -> {out} (jobs={args.jobs})")
    failed = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futures = [ex.submit(render_one, c, args.gen_bin, out) for c in codes]
        for fut in concurrent.futures.as_completed(futures):
            code, rc, log = fut.result()
            print(f"  [{code}] {'ok' if rc == 0 else 'FAIL rc=' + str(rc)}", flush=True)
            if rc != 0:
                failed.append(code)
                sys.stderr.write(f"--- {code} ---\n{log[-2000:]}\n")

    # Picker index — only the locales that actually rendered.
    rendered = [l for l in locales if l["code"] not in failed]
    with open(os.path.join(out, "locales.json"), "w", encoding="utf-8") as f:
        json.dump([{"code": l["code"], "name": label(l)} for l in rendered],
                  f, ensure_ascii=False, indent=2)
        f.write("\n")

    # Multi-language viewer (skip if the output IS the source tree).
    dst_index = os.path.join(out, "index.html")
    if os.path.abspath(SRC_INDEX) != dst_index:
        shutil.copyfile(SRC_INDEX, dst_index)

    print(f"gen_gallery: {len(rendered)}/{len(codes)} locales ok"
          + (f" — FAILED: {failed}" if failed else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
