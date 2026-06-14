#!/usr/bin/env python3
"""
One-shot font-pack builder for SeedSigner i18n.

The per-locale font pack is two derived artifacts that travel together:
  1. a subset .ttf (+ manifest.json)  -- steps/build_lang_font.py        -> tools/fontpack/out/<loc>/
  2. localized scenarios              -- steps/gen_localized_scenarios.py -> tools/scenarios/<loc>.json

This is the single entry point that produces both, so consumers (CI, the desktop
tools, a human) run ONE command instead of remembering the two-step order and
keeping their --locale sets in sync. The step scripts live in steps/ and remain
runnable on their own; this only orchestrates them.

The locale set is derived ONCE from the render layer's canonical manifest
(`screenshot_gen --dump-locales`) -- the same single source of truth the C layer
owns -- so there is no second hardcoded list to drift. --locale restricts it.

  Fonts are built for each manifest locale (English needs none -- it is fully
  covered by the baked OpenSans floor). Scenarios are localized for English too
  (identity passthrough), so every locale the gallery shows has a scenarios file.

Prerequisites: a built screenshot_gen (for --dump-locales) and fontTools
(`pip install fonttools`). Signing / pack bundling is left to the platform layer.
"""

import argparse
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))


def manifest_locales(gen_bin):
    """The locales the render layer declares it needs additional fonts for."""
    dump = subprocess.run([gen_bin, "--dump-locales"],
                          check=True, capture_output=True, text=True)
    manifest = json.loads(dump.stdout)
    names = []
    for profile in manifest.values():
        for loc in profile.get("locales", []):
            if loc["locale"] not in names:
                names.append(loc["locale"])
    return names


def run(script, extra_args):
    """Invoke one of the step scripts (in steps/) with the current interpreter."""
    cmd = [sys.executable, os.path.join(HERE, "steps", script)] + extra_args
    print(f"\n=== {script} {' '.join(extra_args)} ===", flush=True)
    subprocess.run(cmd, check=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--gen-bin",
                    default=os.path.join(REPO_ROOT, "tools/screenshot_generator/build/screenshot_gen"),
                    help="screenshot_gen binary (provides --dump-locales)")
    ap.add_argument("--translations-dir",
                    default=os.path.join(REPO_ROOT, "tools/seedsigner-translations"),
                    help="seedsigner-translations checkout (.po catalogs)")
    ap.add_argument("--assets-dir",
                    default=os.path.join(REPO_ROOT, "components/seedsigner/assets"),
                    help="vendored source TTFs (OpenSans + Noto)")
    ap.add_argument("--font-out-dir",
                    default=os.path.join(REPO_ROOT, "tools/fontpack/out"),
                    help="output root for <locale>/<locale>.ttf")
    ap.add_argument("--scenarios",
                    default=os.path.join(REPO_ROOT, "tools/scenarios.json"),
                    help="English source scenarios to localize")
    ap.add_argument("--scenarios-out-dir",
                    default=os.path.join(REPO_ROOT, "tools/scenarios"),
                    help="output root for <locale>.json")
    ap.add_argument("--locale", action="append",
                    help="only this locale (repeatable); default = all in the manifest")
    args = ap.parse_args()

    # One source of truth for the locale set: what the render layer declares.
    font_locales = args.locale or manifest_locales(args.gen_bin)
    if not font_locales:
        print("No locales need additional fonts (manifest is empty).", file=sys.stderr)
        # Still localize English scenarios so the gallery has its baseline.

    # Step 1: subset one .ttf per locale that needs a script font.
    font_args = ["--gen-bin", args.gen_bin,
                 "--translations-dir", args.translations_dir,
                 "--assets-dir", args.assets_dir,
                 "--out-dir", args.font_out_dir]
    for loc in font_locales:
        font_args += ["--locale", loc]
    if font_locales:
        run("build_lang_font.py", font_args)

    # Step 2: localize scenarios for English + every font locale, so each locale
    # the gallery renders has a matching scenarios file (English = passthrough).
    scenario_locales = ["en"] + [loc for loc in font_locales if loc != "en"]
    scenario_args = ["--scenarios", args.scenarios,
                     "--translations-dir", args.translations_dir,
                     "--out-dir", args.scenarios_out_dir]
    for loc in scenario_locales:
        scenario_args += ["--locale", loc]
    run("gen_localized_scenarios.py", scenario_args)

    print(f"\nfont packs built for: {', '.join(font_locales) or '(none)'}")
    print(f"scenarios localized for: {', '.join(scenario_locales)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
