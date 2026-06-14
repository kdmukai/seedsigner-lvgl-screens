#!/usr/bin/env python3
"""
Offline font-pack builder for SeedSigner i18n (Tiny TTF engine).

Produces a production-ready per-locale font pack under the repo-root lang-packs/:
  lang-packs/<locale>/<locale>.ttf  +  manifest.json (sha256)

Per locale:
  1. Read the canonical font manifest straight from the render layer
     (`screenshot_gen --dump-locales`) -- the locale->{font,size,chain} table is
     owned in C; this tool never duplicates it. It only needs each locale's
     source font family from there, and the locale SET (the single source of
     truth; --locale restricts it).
  2. Extract the locale's glyph corpus from its translation catalog (.po),
     EXCLUDING ASCII (the baked-in OpenSans floor / fallback covers it).
  3. Subset the source TTF to that corpus -> one small .ttf per locale. Fonts are
     loaded at runtime via lv_tiny_ttf_create_data(), which rasterizes on demand,
     so a single subset .ttf serves every size and every resolution profile.

This is the single entry point for font packs; it does NOT localize scenarios
(that is a separate, test-only step: gen_localized_scenarios.py). Self-contained:
parses .po directly (no Babel), subsets via fontTools. Signing / pack bundling is
left to the platform layer.

Prerequisites: a built screenshot_gen (for --dump-locales) and fontTools
(`pip install fonttools`).
"""

import argparse
import hashlib
import json
import os
import subprocess
import sys

from po_catalog import corpus_chars

# This file lives at tools/i18n/; repo root is two levels up.
REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def source_ttf(source_family, assets_dir):
    """Source TTF for a locale's script. This repo owns all locale fonts
    (vendored in components/seedsigner/assets/). CJK Noto families ship a single
    Regular weight, which serves every text role. (Latin-script locales whose
    roles mix Regular/SemiBold are a later concern — Phase 1 is CJK only.)"""
    weight = "SemiBold" if source_family == "OpenSans" else "Regular"
    return os.path.join(assets_dir, f"{source_family}-{weight}.ttf")


def subset_ttf(src_ttf, codepoints, out_ttf, drop_layout):
    """Subset src_ttf to `codepoints` -> out_ttf using fontTools."""
    unicodes = ",".join(f"U+{ord(c):04X}" for c in codepoints)
    cmd = [
        sys.executable, "-m", "fontTools.subset", src_ttf,
        f"--unicodes={unicodes}",
        f"--output-file={out_ttf}",
        "--no-hinting", "--desubroutinize",
        # Keep notdef so out-of-corpus codepoints have a glyph slot.
        "--notdef-outline",
    ]
    if drop_layout:
        # CJK needs no shaping; dropping layout tables shrinks the file and the
        # parser surface. (Arabic/Thai in Phase 2 must KEEP these.)
        cmd.append("--drop-tables+=GSUB,GPOS,GDEF")
    subprocess.run(cmd, check=True)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def manifest_locales(gen_bin, only):
    """locale -> {source_family, chain} from the render layer's manifest,
    restricted to `only` if given (else every locale the manifest declares)."""
    dump = subprocess.run([gen_bin, "--dump-locales"],
                          check=True, capture_output=True, text=True)
    manifest = json.loads(dump.stdout)
    locales = {}
    for profile in manifest.values():
        for loc in profile.get("locales", []):
            name = loc["locale"]
            if only and name not in only:
                continue
            # px sizes are irrelevant to subsetting; one .ttf serves every size.
            locales.setdefault(name, {"source_family": loc["source_family"],
                                      "chain": loc["chain"]})
    return locales


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--gen-bin",
                    default=os.path.join(REPO_ROOT, "tools/apps/screenshot_generator/build/screenshot_gen"),
                    help="screenshot_gen binary (provides --dump-locales)")
    ap.add_argument("--translations-dir",
                    default=os.path.join(REPO_ROOT, "tools/i18n/seedsigner-translations"),
                    help="seedsigner-translations checkout (.po catalogs)")
    ap.add_argument("--assets-dir",
                    default=os.path.join(REPO_ROOT, "components/seedsigner/assets"),
                    help="vendored source TTFs (OpenSans + Noto), owned by this repo")
    ap.add_argument("--out-dir",
                    default=os.path.join(REPO_ROOT, "lang-packs"),
                    help="output root (repo-root lang-packs/) for <locale>/<locale>.ttf")
    ap.add_argument("--locale", action="append",
                    help="only build this locale (repeatable); default = all in manifest")
    args = ap.parse_args()

    locales = manifest_locales(args.gen_bin, args.locale)
    if not locales:
        print("No matching locales in manifest.", file=sys.stderr)
        return 1

    for name, entry in locales.items():
        po = os.path.join(args.translations_dir, "l10n", name, "LC_MESSAGES", "messages.po")
        if not os.path.exists(po):
            print(f"[{name}] SKIP: no catalog at {po}", file=sys.stderr)
            continue
        symbols = corpus_chars(po)
        if not symbols:
            print(f"[{name}] SKIP: empty non-ASCII corpus", file=sys.stderr)
            continue

        src = source_ttf(entry["source_family"], args.assets_dir)
        if not os.path.exists(src):
            print(f"[{name}] SKIP: source TTF not found: {src}", file=sys.stderr)
            continue

        loc_out = os.path.join(args.out_dir, name)
        os.makedirs(loc_out, exist_ok=True)
        out_ttf = os.path.join(loc_out, f"{name}.ttf")

        # Primary-script (CJK) locales need no shaping -> drop layout tables.
        drop_layout = (entry["chain"] == "primary")

        # ASCII is deliberately EXCLUDED from every subset (CJK included): the
        # baked-in OpenSans floor covers it. For a PRIMARY-chain (CJK) locale the
        # script font is the primary for the whole text element, so when it lacks
        # a codepoint LVGL's fallback chain must defer to OpenSans. That renders
        # embedded English technical terms at the NORMAL English size (OpenSans)
        # rather than the bumped CJK size — a deliberate divergence from the
        # single-font Python renderer, which has no fallback and draws embedded
        # English at the bumped size.
        #
        # This requires the lv_tiny_ttf fallback fix (third_party/patches/
        # lv_tiny_ttf-fallback-chain.patch): the stock no-cache path reports
        # absent glyphs as "found", which would render embedded English as blank
        # .notdef boxes instead of falling through. With the patch applied the
        # chain advances correctly and ASCII can stay out of the subset.
        subset_ttf(src, symbols, out_ttf, drop_layout)

        manifest_out = {
            "locale": name,
            "source_family": entry["source_family"],
            "chain": entry["chain"],
            "corpus_glyphs": len(symbols),
            "font": {
                "file": f"{name}.ttf",
                "source_ttf": os.path.relpath(src, REPO_ROOT),
                "bytes": os.path.getsize(out_ttf),
                "sha256": sha256_file(out_ttf),
            },
        }
        with open(os.path.join(loc_out, "manifest.json"), "w") as f:
            json.dump(manifest_out, f, indent=2)
        print(f"[{name}] {name}.ttf  ({len(symbols)} glyphs, {os.path.getsize(out_ttf)} bytes)")

    return 0


if __name__ == "__main__":
    sys.exit(main())
