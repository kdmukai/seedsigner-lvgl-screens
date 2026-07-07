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

Font packs + localized scenarios are gitignored and built from the
seedsigner-language-packs repo (not committed). They must already exist under lang-packs/
+ tools/scenarios/localized/ before staging — build them with
`scripts/ci/ci.sh build-fontpacks` + `scripts/ci/ci.sh gen-localized-scenarios` (or
REGEN_PACKS=1 build.sh). This just copies them; this repo does not own the pack builder.

Usage:
  stage_assets.py --dest tools/apps/web_runner/build-wasm
"""

import argparse
import json
import os
import shutil
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
    args = ap.parse_args()

    dest = os.path.abspath(args.dest)
    scen_dir = os.path.join(dest, "assets", "scenarios")
    packs_dir = os.path.join(dest, "assets", "lang-packs")
    os.makedirs(scen_dir, exist_ok=True)
    os.makedirs(packs_dir, exist_ok=True)

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
                     f"(build it with `scripts/ci/ci.sh build-fontpacks`)")
        out = os.path.join(packs_dir, code)
        os.makedirs(out, exist_ok=True)
        for fn in sorted(os.listdir(src)):
            # manifest.json: the browser fetches + registers it (ss_register_manifest)
            # so the render layer learns the locale's policy — screens bakes no locale
            # table. Then it fetches exactly the files ss_pack_files lists: the role
            # .ttf(s) plus runs.bin for complex-script (shaping) packs. (runs.json is the
            # repo-side debug/oracle mirror — not staged.) endonym_<h>.bin are the
            # pre-rendered language names the locale picker fetches per row.
            if (fn == "manifest.json" or fn.endswith(".ttf") or fn == "runs.bin"
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
