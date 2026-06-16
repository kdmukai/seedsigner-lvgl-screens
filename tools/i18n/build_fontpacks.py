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

import hb_shaper
import shape_inventory
from po_catalog import corpus_chars, parse_catalog, parse_entries

# This file lives at tools/i18n/; repo root is two levels up.
REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

# Base Unicode blocks whose letters the renderer reshapes into presentation forms
# (LV_USE_ARABIC_PERSIAN_CHARS) BEFORE glyph lookup. A corpus containing these
# must be subset to the forms the shaper emits, not the base letters (see
# shaped_codepoints / lv_shape). Other scripts pass through the shaper unchanged.
ARABIC_BASE_BLOCKS = (
    (0x0600, 0x06FF),  # Arabic
    (0x0750, 0x077F),  # Arabic Supplement
    (0x08A0, 0x08FF),  # Arabic Extended-A
)


def needs_arabic_shaping(symbols):
    """True if `symbols` (a corpus's char set) contains base Arabic-script
    letters. Those locales need presentation-form subsetting via lv_shape."""
    return any(any(lo <= ord(c) <= hi for lo, hi in ARABIC_BASE_BLOCKS)
               for c in symbols)


def corpus_text(po_path):
    """All translated strings (msgstr) joined by newlines. Unlike corpus_chars
    (a deduped char set) this preserves each string's internal adjacency, which
    Arabic shaping depends on. Newlines break cross-string joining, matching how
    the renderer treats them."""
    return "\n".join(mstr for _mid, mstr in parse_entries(po_path) if mstr)


def ensure_shaper(shaper_bin):
    """Return the path to the lv_shape binary, building it on demand if absent.
    lv_shape (tools/i18n/shaper) is the LVGL Arabic/Persian shaping oracle; it
    must link the SAME third_party/lvgl the consumers render with, so we build it
    from in-repo source rather than expecting a system copy."""
    if os.path.exists(shaper_bin):
        return shaper_bin
    src_dir = os.path.join(REPO_ROOT, "tools/i18n/shaper")
    build_dir = os.path.join(src_dir, "build")
    print(f"[shaper] lv_shape not found at {shaper_bin}; building it...", file=sys.stderr)
    subprocess.run(["cmake", "-S", src_dir, "-B", build_dir,
                    "-DCMAKE_BUILD_TYPE=Release"], check=True)
    subprocess.run(["cmake", "--build", build_dir, "-j", str(os.cpu_count() or 2)],
                   check=True)
    if not os.path.exists(shaper_bin):
        raise FileNotFoundError(f"lv_shape build did not produce {shaper_bin}")
    return shaper_bin


def shaped_codepoints(po_path, shaper_bin):
    """Run a locale's corpus through lv_shape (the real LVGL shaper) and return
    the set of code points the renderer will request for that text — i.e. the
    presentation forms the corpus's base letters resolve to, plus any pass-through
    code points (digits, punctuation, ZWNJ)."""
    text = corpus_text(po_path)
    res = subprocess.run([shaper_bin], input=text, check=True,
                         capture_output=True, text=True, encoding="utf-8")
    return set(json.loads(res.stdout))


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
    _run_subset(src_ttf, unicodes, out_ttf, drop_layout)


def subset_range(src_ttf, unicode_range, out_ttf):
    """Subset src_ttf to a fixed Unicode block range (pyftsubset --unicodes form,
    e.g. "U+0400-04FF" or "U+0300-036F,U+1E00-1EFF") -> out_ttf. Used for the
    same-size script packs: the repertoire is the block, NOT the .po corpus, so a
    translation edit can never change the pack (or its signature). Layout tables
    are kept (Vietnamese mark positioning may need GPOS, and the packs stay tiny)."""
    _run_subset(src_ttf, unicode_range, out_ttf, drop_layout=False)


def _run_subset(src_ttf, unicodes, out_ttf, drop_layout):
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
            # unicode_range (script packs) selects block-range vs corpus mode;
            # shaping/script/rtl select the complex-script (glyph-run) mode.
            locales.setdefault(name, {"source_family": loc["source_family"],
                                      "chain": loc["chain"],
                                      "unicode_range": loc.get("unicode_range"),
                                      "shaping": loc.get("shaping", False),
                                      "script": loc.get("script"),
                                      "rtl": loc.get("rtl", False)})
    return locales


def build_block_range_pack(name, entry, out_dir, assets_dir):
    """Build a same-size SCRIPT pack: OpenSans subset to a fixed Unicode block
    (NOT the .po corpus), two weights to match the baked Western baseline this
    chains under -- Regular for body, SemiBold for the title/button roles.
    Writes lang-packs/<name>/<name>_{regular,semibold}.ttf + manifest.json."""
    unicode_range = entry["unicode_range"]
    weights = [("regular", "Regular"), ("semibold", "SemiBold")]

    loc_out = os.path.join(out_dir, name)
    os.makedirs(loc_out, exist_ok=True)

    fonts_meta = []
    for tag, asset_weight in weights:
        src = os.path.join(assets_dir, f"{entry['source_family']}-{asset_weight}.ttf")
        if not os.path.exists(src):
            print(f"[{name}] SKIP: source TTF not found: {src}", file=sys.stderr)
            return
        out_ttf = os.path.join(loc_out, f"{name}_{tag}.ttf")
        subset_range(src, unicode_range, out_ttf)
        fonts_meta.append({
            "weight": tag,
            "file": f"{name}_{tag}.ttf",
            "source_ttf": os.path.relpath(src, REPO_ROOT),
            "bytes": os.path.getsize(out_ttf),
            "sha256": sha256_file(out_ttf),
        })

    manifest_out = {
        "locale": name,
        "source_family": entry["source_family"],
        "chain": entry["chain"],
        "unicode_range": unicode_range,
        "fonts": fonts_meta,
    }
    with open(os.path.join(loc_out, "manifest.json"), "w") as f:
        json.dump(manifest_out, f, indent=2)
    total = sum(m["bytes"] for m in fonts_meta)
    print(f"[{name}] block-range {unicode_range}  ({len(fonts_meta)} weights, {total} bytes)")


def _assert_runs_clean(units, glyph_count, locale):
    """Hard gate before baking: NO .notdef anywhere (silent tofu) and every gid in
    range for the shipped subset. A failure means the subset closure is incomplete
    or a gid escaped the subset — never ship that."""
    for u in units:
        runs = []
        if u["kind"] == "plain":
            runs = [ln["glyphs"] for ln in u["lines"]]  # each line is {glyphs, breaks}
        elif u["kind"] == "segmented":
            runs = [p["glyphs"] for p in u["parts"] if "glyphs" in p]
        for run in runs:
            for g in run:
                if g["gid"] == 0:
                    raise SystemExit(f"[{locale}] FATAL .notdef in run for {u['msgid']!r} "
                                     f"(subset closure incomplete)")
                if not (0 <= g["gid"] < glyph_count):
                    raise SystemExit(f"[{locale}] FATAL gid {g['gid']} out of range "
                                     f"(subset has {glyph_count} glyphs) for {u['msgid']!r}")


def build_complex_shaping_pack(name, entry, out_dir, assets_dir, translations_dir):
    """Build a COMPLEX-SCRIPT pack (Devanagari/Thai/Nastaliq/…): a keep-layout
    subset .ttf + a pre-shaped glyph-run table (runs.json) the device draws by
    glyph-id. Subset-then-shape with HarfBuzz (hb_shaper); classify each string
    into plain/segmented/unsupported (shape_inventory). Writes
    lang-packs/<name>/{<name>.ttf, runs.json, manifest.json}."""
    po = os.path.join(translations_dir, "l10n", name, "LC_MESSAGES", "messages.po")
    if not os.path.exists(po):
        print(f"[{name}] SKIP: no catalog at {po}", file=sys.stderr)
        return
    catalog = parse_catalog(po)
    if not catalog:
        print(f"[{name}] SKIP: empty catalog", file=sys.stderr)
        return

    src = source_ttf(entry["source_family"], assets_dir)
    if not os.path.exists(src):
        print(f"[{name}] SKIP: source TTF not found: {src}", file=sys.stderr)
        return

    script = entry["script"]
    direction = "rtl" if entry.get("rtl") else "ltr"
    language = name  # HarfBuzz language tag; locale id is close enough for our set

    # Subset to the corpus KEEPING layout closure, plus HarfBuzz's pre-GSUB
    # decomposition targets (see hb_shaper.decomposition_closure) so nothing
    # renders .notdef. Shaping runs against THIS subset so gids match on-device.
    texts = [s for s in catalog.values() if s]
    needed = hb_shaper.decomposition_closure(src, texts, script, language, direction)

    loc_out = os.path.join(out_dir, name)
    os.makedirs(loc_out, exist_ok=True)
    out_ttf = os.path.join(loc_out, f"{name}.ttf")
    hb_shaper.subset_keep_layout(src, needed, out_ttf)
    glyph_count = hb_shaper.subset_glyph_count(out_ttf)

    with open(out_ttf, "rb") as fh:
        subset_bytes = fh.read()

    # Shape every catalog string against the subset. shape_inventory splits on
    # '\n', classifies holes, and shapes literal segments; upem is read once.
    upem = hb_shaper.shape(subset_bytes, ".", script, language, direction)[1]
    shape_line = lambda t: hb_shaper.shape(subset_bytes, t, script, language, direction)[0]
    # Per-line wrap marks: ICU dictionary word boundaries (Thai/…) -> glyph indices.
    break_line = lambda t, glyphs: hb_shaper.line_break_indices(t, glyphs, language, direction)
    units, report = shape_inventory.build_units(catalog, shape_line, break_line)

    _assert_runs_clean(units, glyph_count, name)

    if report["unsupported"]:
        print(f"[{name}] WARNING: {len(report['unsupported'])} string(s) not shapeable "
              f"(no run baked) — render layer must handle these:", file=sys.stderr)
        for mid, reason in report["unsupported"]:
            print(f"    - {mid!r}: {reason}", file=sys.stderr)

    # runs.json: the pre-shaped run table, keyed by English msgid (stable across
    # translation edits), bound to the exact subset via font_sha256. Only
    # plain/segmented carry runs; unsupported are listed (with reason) so the gap
    # stays visible but the device can ignore them.
    font_sha = sha256_file(out_ttf)
    with_runs = [u for u in units if u["kind"] in ("plain", "segmented")]
    runs_doc = {
        "locale": name,
        "script": script,
        "direction": direction,
        "upem": upem,
        "font": f"{name}.ttf",
        "font_sha256": font_sha,
        "harfbuzz_version": hb_shaper.harfbuzz_version(),
        "icu_version": hb_shaper.icu_version(),
        "runs": with_runs,
        "unsupported": [{"msgid": m, "reason": r} for m, r in report["unsupported"]],
    }
    runs_path = os.path.join(loc_out, "runs.json")
    # Deterministic serialization (sorted msgid order from build_units; no
    # timestamps) so two builds of the same inputs are byte-identical.
    with open(runs_path, "w", encoding="utf-8") as f:
        json.dump(runs_doc, f, ensure_ascii=False, separators=(",", ":"))
        f.write("\n")

    manifest_out = {
        "locale": name,
        "source_family": entry["source_family"],
        "chain": entry["chain"],
        "shaping": True,
        "script": script,
        "rtl": bool(entry.get("rtl")),
        "corpus_glyphs": glyph_count,
        "font": {
            "file": f"{name}.ttf",
            "source_ttf": os.path.relpath(src, REPO_ROOT),
            "bytes": os.path.getsize(out_ttf),
            "sha256": font_sha,
        },
        "runs": {
            "file": "runs.json",
            "sha256": sha256_file(runs_path),
            "plain": report["plain"],
            "segmented": report["segmented"],
            "ascii_passthrough": report["ascii"],
            "unsupported": len(report["unsupported"]),
        },
        "harfbuzz_version": hb_shaper.harfbuzz_version(),
    }
    with open(os.path.join(loc_out, "manifest.json"), "w") as f:
        json.dump(manifest_out, f, indent=2)
    print(f"[{name}] {name}.ttf ({glyph_count} glyphs, {os.path.getsize(out_ttf)} bytes) + "
          f"runs.json (plain={report['plain']} segmented={report['segmented']} "
          f"ascii={report['ascii']} unsupported={len(report['unsupported'])})")


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
    ap.add_argument("--shaper-bin",
                    default=os.path.join(REPO_ROOT, "tools/i18n/shaper/build/lv_shape"),
                    help="lv_shape binary (Arabic/Persian shaping oracle); built on demand if absent")
    args = ap.parse_args()

    locales = manifest_locales(args.gen_bin, args.locale)
    if not locales:
        print("No matching locales in manifest.", file=sys.stderr)
        return 1

    for name, entry in locales.items():
        # Complex-script packs (Devanagari/Thai/Nastaliq/…): keep-layout subset +
        # offline HarfBuzz glyph-run table. Routed by the render layer's `shaping`
        # flag (single source of truth), NOT a Python script-block guess.
        if entry.get("shaping"):
            build_complex_shaping_pack(name, entry, args.out_dir, args.assets_dir,
                                       args.translations_dir)
            continue

        # Script packs (Greek/Cyrillic/Vietnamese): block-range subset, no .po.
        if entry.get("unicode_range"):
            build_block_range_pack(name, entry, args.out_dir, args.assets_dir)
            continue

        po = os.path.join(args.translations_dir, "l10n", name, "LC_MESSAGES", "messages.po")
        if not os.path.exists(po):
            print(f"[{name}] SKIP: no catalog at {po}", file=sys.stderr)
            continue
        symbols = corpus_chars(po)
        if not symbols:
            print(f"[{name}] SKIP: empty non-ASCII corpus", file=sys.stderr)
            continue

        # Arabic-script locales (e.g. fa): the renderer reshapes base letters into
        # presentation forms (LV_USE_ARABIC_PERSIAN_CHARS) before glyph lookup, so
        # the base letters from the .po are never drawn — their positional FORMS
        # are. Ask lv_shape (the real LVGL shaper) which code points the corpus
        # resolves to, and subset to exactly those. This is corpus-driven (only
        # the letters actually used) yet form-complete (every form the renderer
        # will request), and the digit/ZWNJ/lam-alef edge cases are handled by the
        # real shaper, not a re-implementation. Non-Arabic corpora skip this.
        shaped = needs_arabic_shaping(symbols)
        if shaped:
            cps = {cp for cp in shaped_codepoints(po, ensure_shaper(args.shaper_bin))
                   if cp > 0x7F}  # ASCII = baked OpenSans floor
            symbols = "".join(sorted(chr(cp) for cp in cps))
            if not symbols:
                print(f"[{name}] SKIP: empty shaped corpus", file=sys.stderr)
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
        # This relies on the render layer running the tiny_ttf glyph cache ON (the
        # default, SEEDSIGNER_TTF_CACHE_SIZE): the cached glyph path reports an
        # absent codepoint as "not found", so the fallback chain advances and ASCII
        # can stay out of the subset. (LVGL's no-cache path has a bug here — "bug
        # #2" — but no target runs cache_size=0; see the knowledge doc.)
        subset_ttf(src, symbols, out_ttf, drop_layout)

        manifest_out = {
            "locale": name,
            "source_family": entry["source_family"],
            "chain": entry["chain"],
            # True ⇒ glyph set = presentation forms from lv_shape (not base letters).
            "shaped": shaped,
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
