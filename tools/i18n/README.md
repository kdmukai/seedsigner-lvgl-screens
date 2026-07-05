# tools/i18n — gallery/scenario localization

This directory now holds only the **gallery-side** i18n helpers. Language-pack
**production** (subset fonts, glyph runs, endonym images, `.mo` catalogs) moved to
the dedicated **`seedsigner-language-packs`** repo, vendored here as the submodule
**`deps/language-packs`**. This repo no longer owns any pack-build tooling — it
*delegates* to that submodule (see `docs/knowledge/language-pack-format-and-policy-authority.md`).

## What lives here

- **`gen_localized_scenarios.py`** — translates the display-text leaves of
  `tools/scenarios/scenarios.json` via a locale's `.po` catalog, emitting
  `tools/scenarios/localized/<locale>.json` for the desktop apps (screenshot_gen /
  screen_runner / web_runner). Test-only output, not part of a font pack; `en` is the
  identity passthrough. Reads `.po` from the submodule's translations
  (`deps/language-packs/seedsigner-translations`) by default.
- **`po_catalog.py`** — the shared `.po` reader (`parse_catalog()` →
  `{msgid: msgstr}`). Imported by `gen_localized_scenarios.py`.
- **`supported_locales.json`** — canonical locale list (`{code, english, native}` +
  `pack_locales`) for the multi-language gallery
  (`tools/apps/screenshot_generator/gen_gallery.py`) and the web playground's language
  list (`tools/apps/web_runner/stage_assets.py`).

`tools/scenarios/localized/` is gitignored — regenerated from `scenarios.json` + the
catalogs.

## Building the actual language packs

Packs are built by the `deps/language-packs` submodule's tooling (reads its own
`locales.h` — no `screenshot_gen --dump-locales`), into the repo-root `lang-packs/`:

```bash
git submodule update --init --recursive deps/language-packs
scripts/ci/ci.sh build-fontpacks     # → lang-packs/ via the submodule builder
# (points the submodule's lv_shape fa oracle at this repo's third_party/lvgl)
```

For a fully reproducible / signable build, use the submodule's own Docker path
(`deps/language-packs/scripts/build_packs.sh`); the `ci.sh build-fontpacks` native
path above is for the gallery/tests, where functional packs — not byte-reproducible
ones — are what matter.

## Typical gallery run

```bash
git submodule update --init --recursive
scripts/ci/ci.sh build-fontpacks                 # font packs → lang-packs/
python3 tools/i18n/gen_localized_scenarios.py     # localized scenarios (test input)
# then render a locale (the consumer step):
./tools/apps/screenshot_generator/build/screenshot_gen \
      --locale zh_Hans_CN --scenarios-file tools/scenarios/localized/zh_Hans_CN.json \
      --font-dir lang-packs --out-dir tools/apps/screenshot_generator/screenshots/i18n/zh_Hans_CN
```
