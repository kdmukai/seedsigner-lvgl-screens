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
import sys

from po_catalog import parse_catalog

# This file lives at tools/i18n/; repo root is two levels up.
REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

# Keys whose string value is display text. Everything else (status_type,
# is_bottom_list, name, initial_text/initial_mode, show_* booleans) is left as-is.
TRANSLATABLE_STRING_KEYS = {"title", "text", "status_headline"}
# Keys whose value is a list of display labels (strings, or [label, ...] tuples).
TRANSLATABLE_LIST_KEYS = {"button_list"}


def _translate_label(label, catalog):
    if isinstance(label, str):
        return catalog.get(label, label)
    # Button entries may be [label, icon, ...]; translate the label only.
    if isinstance(label, list) and label and isinstance(label[0], str):
        out = list(label)
        out[0] = catalog.get(label[0], label[0])
        return out
    return label


def localize(node, catalog):
    """Recursively translate display-text leaves in place."""
    if isinstance(node, dict):
        for key, value in node.items():
            if key in TRANSLATABLE_STRING_KEYS and isinstance(value, str):
                node[key] = catalog.get(value, value)
            elif key in TRANSLATABLE_LIST_KEYS and isinstance(value, list):
                node[key] = [_translate_label(e, catalog) for e in value]
            else:
                localize(value, catalog)
    elif isinstance(node, list):
        for item in node:
            localize(item, catalog)
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
                    default=os.path.join(REPO_ROOT, "tools/i18n/seedsigner-translations"))
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
