#!/usr/bin/env python3
"""
Shared scenario localizer for the SeedSigner desktop tools.

Takes the English tools/scenarios/scenarios.json and a locale's translation
catalog and emits tools/scenarios/localized/<locale>.json with the same shape but
display-text leaves translated. Every app (screenshot_gen, screen_runner,
web_runner) consumes the generated per-locale files through its existing loader,
so no tool reimplements localization. Test-only output, not part of a font pack.

Localization is best-effort by design: a leaf whose English text is a catalog
msgid is translated; anything else (synthetic stress strings, structural ids,
passphrase input) passes through unchanged. We deliberately do NOT reproduce the
Python view layer's full translation+interpolation prep -- glyph-rendering
correctness comes from the font corpus, not from these scenarios.
"""

import argparse
import json
import os
import re
import sys

from po_catalog import parse_catalog

# This file lives at tools/i18n/; repo root is two levels up.
REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

_DIGIT_RUN = re.compile(r"\d+")
_QUOTED_RUN = re.compile(r'"([^"]*)"')
# Delimiters that join independently-translated label parts, e.g.
# "{sig} - {script} ({network})". Captured so re.split keeps them for reassembly.
_SEGMENT_SPLIT = re.compile(r"( - | \(|\))")


def translate_string(text, catalog):
    """Translate one display string against the catalog.

    First an exact-content match (the common case). Failing that, a Python str.format
    template fallback: the Python view formats some strings with interpolated values, so
    the scenario's PRE-formatted string never equals the msgid template and would otherwise
    pass through untranslated. Two abstractions are tried, each firing ONLY when the
    abstracted form is a real catalog msgid (so strings that merely contain digits/quotes —
    addresses, derivation paths, passphrases, amounts, config names — are left untouched):

    1. Digit runs -> "{}": ``_("Seed Words: {}/{}").format(page, total)`` produces the
       pre-formatted "Seed Words: 1/3", whose msgid is "Seed Words: {}/{}".
    2. Double-quoted runs -> '"{}"': ``_('Your input: "{}"').format(word)`` produces
       'Your input: "satoshi"', whose msgid is 'Your input: "{}"'. The digit-run pass
       already covers a quoted *number* ("0010101"); this covers quoted *words* ("satoshi",
       "say") that carry no digits.
    """
    if text in catalog:
        return catalog[text]

    nums = _DIGIT_RUN.findall(text)
    if nums:
        template = _DIGIT_RUN.sub("{}", text)
        translated = catalog.get(template)
        if translated is not None and translated.count("{}") == len(nums):
            try:
                return translated.format(*nums)
            except (IndexError, KeyError, ValueError):
                pass

    quoted = _QUOTED_RUN.findall(text)
    if quoted:
        template = _QUOTED_RUN.sub('"{}"', text)
        translated = catalog.get(template)
        if translated is not None and translated.count("{}") == len(quoted):
            try:
                return translated.format(*quoted)
            except (IndexError, KeyError, ValueError):
                pass

    # Composite label: strings assembled from independently-translated parts, e.g.
    # "Single Sig - Native Segwit (Testnet)" == "{sig} - {script} ({network})". Split on the
    # joining delimiters (kept), translate each SEGMENT that is itself a catalog msgid, and
    # leave delimiters + unknown segments as-is. Only rebuilds when at least one segment
    # matched, so arbitrary text (addresses, etc.) is untouched.
    segments = _SEGMENT_SPLIT.split(text)
    if len(segments) > 1:
        rebuilt = [catalog.get(seg, seg) for seg in segments]
        if any(seg in catalog for seg in segments):
            return "".join(rebuilt)
    return text


def localize(node, catalog):
    """Recursively translate display-text leaves by CONTENT, not by key name.

    Any string whose exact value is a catalog msgid is replaced with its
    translation; every other string (structural ids, status_type, scenario
    names, synthetic stress text, passphrase input, icon names, version) is not a
    msgid and so passes through unchanged. Matching on content rather than an
    allowlist of keys means a new translatable field (e.g. the splash sponsor
    line, or a button [label, icon] tuple's label) needs no upkeep here — if its
    English text is in the catalog, it translates.
    """
    if isinstance(node, dict):
        for key, value in node.items():
            node[key] = localize(value, catalog)
        return node
    if isinstance(node, list):
        return [localize(item, catalog) for item in node]
    if isinstance(node, str):
        return translate_string(node, catalog)
    return node


def available_locales(translations_dir):
    l10n = os.path.join(translations_dir, "l10n")
    out = []
    if os.path.isdir(l10n):
        for name in sorted(os.listdir(l10n)):
            if os.path.exists(os.path.join(l10n, name, "LC_MESSAGES", "messages.po")):
                out.append(name)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--scenarios",
                    default=os.path.join(REPO_ROOT, "tools/scenarios/scenarios.json"))
    ap.add_argument("--translations-dir",
                    # Local fixture-regeneration only — CI reads the COMMITTED
                    # tools/scenarios/localized/. The .po comes from a language-packs
                    # checkout; point SS_LANGPACKS_REPO_DIR at one (default: sibling).
                    default=os.path.join(
                        os.environ.get("SS_LANGPACKS_REPO_DIR",
                                       os.path.join(REPO_ROOT, "..", "seedsigner-language-packs")),
                        "seedsigner-translations"))
    ap.add_argument("--out-dir", default=os.path.join(REPO_ROOT, "tools/scenarios/localized"))
    ap.add_argument("--locale", action="append",
                    help="only this locale (repeatable); default = en + all catalogs")
    args = ap.parse_args()

    with open(args.scenarios, encoding="utf-8") as f:
        base = json.load(f)

    locales = args.locale or (["en"] + available_locales(args.translations_dir))
    os.makedirs(args.out_dir, exist_ok=True)

    for locale in locales:
        if locale == "en":
            catalog = {}  # identity: English passthrough
        else:
            po = os.path.join(args.translations_dir, "l10n", locale, "LC_MESSAGES", "messages.po")
            if not os.path.exists(po):
                print(f"[{locale}] SKIP: no catalog at {po}", file=sys.stderr)
                continue
            catalog = parse_catalog(po)

        localized = localize(json.loads(json.dumps(base)), catalog)  # deep copy then translate
        out_path = os.path.join(args.out_dir, f"{locale}.json")
        with open(out_path, "w", encoding="utf-8") as f:
            json.dump(localized, f, ensure_ascii=False, indent=2)
        print(f"[{locale}] -> {os.path.relpath(out_path, REPO_ROOT)} "
              f"({len(catalog)} catalog entries)")

    return 0


if __name__ == "__main__":
    sys.exit(main())
