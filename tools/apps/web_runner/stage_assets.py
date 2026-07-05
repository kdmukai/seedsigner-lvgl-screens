#!/usr/bin/env python3
"""Stage the web_runner runtime assets next to the built bundle.

The browser playground fetches fonts + scenarios at runtime, so a translation /
font / scenario edit is a file swap with no WASM rebuild. This copies, for each
supported locale:
  - scenario catalog -> <dest>/assets/scenarios/<locale>.json   (raw; the page
    does the RFC 7396 variation merge-patch client-side)
  - font pack        -> <dest>/assets/lang-packs/<locale>/*.ttf  (script/CJK only)
and writes <dest>/assets/scenarios/locales.json, the ordered locale index
(code + display name) the picker is built from.

Font packs are gitignored / regenerable. They must exist under lang-packs/
before staging; pass --regen-packs to (re)build them first via build_fontpacks
(needs --gen-bin + fontTools, and, for the CJK corpus packs, the translations
checkout build_fontpacks defaults to).

Usage:
  stage_assets.py --dest tools/apps/web_runner/build-wasm
  stage_assets.py --dest site/play --regen-packs --gen-bin <screenshot_gen>
"""

import argparse
import json
import os
import shutil
import subprocess
import sys

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))

# The supported-locale set is the single shared source of truth (also used by the
# multi-language screenshot gallery). Latin-baseline locales need no pack (the
# compiled-in OpenSans Western floor covers Latin-1 + Latin Extended-A); the
# PACK_LOCALES ship a fetched .ttf pack (Greek/Cyrillic/Vietnamese subsets, CJK
# corpus). fa/hi/th are intentionally absent: no packs yet (Phase 2).
def _label(e):
    # English readable name first, native name/script second (e.g. "Spanish
    # (Español)"); collapses to just the English name when they match.
    return e["english"] if e["native"] == e["english"] else f'{e["english"]} ({e["native"]})'


with open(os.path.join(REPO_ROOT, "tools/i18n/supported_locales.json"), encoding="utf-8") as _f:
    _supported = json.load(_f)
LOCALES = [(e["code"], _label(e)) for e in _supported["locales"]]
PACK_LOCALES = list(_supported["pack_locales"])


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dest", required=True, help="build/serve dir to stage assets/ under")
    ap.add_argument("--regen-packs", action="store_true",
                    help="rebuild the font packs via build_fontpacks before copying")
    ap.add_argument("--gen-bin",
                    default=os.path.join(REPO_ROOT, "tools/apps/screenshot_generator/build/screenshot_gen"),
                    help="screenshot_gen binary (for --regen-packs)")
    args = ap.parse_args()

    dest = os.path.abspath(args.dest)
    scen_dir = os.path.join(dest, "assets", "scenarios")
    packs_dir = os.path.join(dest, "assets", "lang-packs")
    os.makedirs(scen_dir, exist_ok=True)
    os.makedirs(packs_dir, exist_ok=True)

    if args.regen_packs:
        cmd = [sys.executable, os.path.join(REPO_ROOT, "tools/i18n/build_fontpacks.py"),
               "--gen-bin", args.gen_bin]
        for loc in PACK_LOCALES:
            cmd += ["--locale", loc]
        subprocess.run(cmd, check=True)

    # Scenario catalogs (raw localized JSON — the page expands variations itself).
    src_scen = os.path.join(REPO_ROOT, "tools/scenarios/localized")
    for code, _name in LOCALES:
        src = os.path.join(src_scen, f"{code}.json")
        if not os.path.exists(src):
            sys.exit(f"stage_assets: missing scenario catalog {src}")
        shutil.copyfile(src, os.path.join(scen_dir, f"{code}.json"))

    # Font packs (only the script/CJK locales need one).
    src_packs = os.path.join(REPO_ROOT, "lang-packs")
    for code in PACK_LOCALES:
        src = os.path.join(src_packs, code)
        if not os.path.isdir(src):
            sys.exit(f"stage_assets: missing font pack {src} "
                     f"(run tools/i18n/build_fontpacks.py or pass --regen-packs)")
        out = os.path.join(packs_dir, code)
        os.makedirs(out, exist_ok=True)
        for fn in sorted(os.listdir(src)):
            # The browser fetches exactly the files ss_pack_files lists: the role
            # .ttf(s) plus runs.bin for complex-script (shaping) packs. (runs.json is
            # the repo-side debug/oracle mirror; manifest.json stays repo-side too —
            # the WASM build reads the manifest from the render layer.) endonym_<h>.bin
            # are the pre-rendered language names the locale picker fetches per row.
            if (fn.endswith(".ttf") or fn == "runs.bin"
                    or fn.startswith("endonym_")):
                shutil.copyfile(os.path.join(src, fn), os.path.join(out, fn))

    # Locale index the picker is built from.
    with open(os.path.join(scen_dir, "locales.json"), "w", encoding="utf-8") as f:
        json.dump([{"code": c, "name": n} for c, n in LOCALES], f,
                  ensure_ascii=False, indent=2)
        f.write("\n")

    print(f"stage_assets: staged {len(LOCALES)} locales "
          f"({len(PACK_LOCALES)} with packs) -> {scen_dir} + {packs_dir}")


if __name__ == "__main__":
    main()
