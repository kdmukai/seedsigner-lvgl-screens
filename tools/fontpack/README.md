# fontpack — offline i18n font + scenario tooling

Offline "Produce" tooling for SeedSigner's multi-language support. Self-contained (no Babel): it parses
`.po` catalogs directly and shells out only to `fontTools`. It never duplicates the render layer's
locale table — it reads that from the binary via `screenshot_gen --dump-locales`.

See `docs/font-and-i18n-rendering.md` (design) and `docs/font-and-i18n-implementation-plan.md` (status).

## Files

- **`build_fontpacks.py`** — the single entry point (the only script at the top level). Derives the
  locale set once from `screenshot_gen --dump-locales` (the render layer's source of truth) and runs the
  two steps below for it, keeping their locale sets in sync. `--locale` restricts it. Use this unless you
  specifically want one step.

`steps/` holds the building blocks it drives (each still runnable on its own):

- **`steps/po_catalog.py`** — minimal `.po` reader. `parse_catalog()` → `{msgid: msgstr}` (non-empty
  only); `corpus_chars()` → the unique glyph set used by a locale's translations (non-ASCII by default).
- **`steps/build_lang_font.py`** — builds one subset `.ttf` per locale. Reads the per-profile font
  manifest from `screenshot_gen --dump-locales` (source family + which locales), extracts each locale's
  corpus, and subsets the vendored source TTF (`components/seedsigner/assets/`) with `fontTools`. Output →
  `tools/fontpack/out/<locale>/<locale>.ttf` (+ `manifest.json` with sha256). One `.ttf` serves all
  sizes/resolutions (Tiny TTF rasterizes on demand). Subsets **exclude ASCII** (the baked OpenSans floor
  covers it; embedded English defers to it via the fallback chain — needs
  `third_party/patches/lv_tiny_ttf-fallback-chain.patch`). CJK ("primary"-chain) subsets drop
  GSUB/GPOS/GDEF; Arabic/Thai (Phase 2) must keep layout tables + presentation forms.
- **`steps/gen_localized_scenarios.py`** — translates `tools/scenarios.json` display-text leaves
  (`title`/`text`/`status_headline`/`button_list`) via a locale's catalog, English passthrough for
  anything that isn't a msgid. Output → `tools/scenarios/<locale>.json`, which every desktop tool loads
  unchanged. (`en` is the identity passthrough.)

`tools/fontpack/out/` and `tools/scenarios/` are gitignored — reproducible from the committed source.

## Prerequisites

- `tools/seedsigner-translations` submodule initialized (`.po` catalogs).
- `fontTools` (`pip install fonttools`).
- A built `screenshot_gen` (provides `--dump-locales`).

## Typical run

```bash
git submodule update --init tools/seedsigner-translations
# build screenshot_gen first (any resolution; it compiles all profiles)
cmake -S tools/screenshot_generator -B tools/screenshot_generator/build \
      -DDISPLAY_WIDTH=240 -DDISPLAY_HEIGHT=240 && cmake --build tools/screenshot_generator/build -j

# one command: subset fonts + localize scenarios for every locale in the manifest (+ en)
python3 tools/fontpack/build_fontpacks.py

# then render a locale (the consumer step — not part of the pack build):
./tools/screenshot_generator/build/screenshot_gen \
      --locale zh_Hans_CN --scenarios-file tools/scenarios/zh_Hans_CN.json \
      --font-dir tools/fontpack/out --out-dir tools/screenshot_generator/screenshots/i18n/zh_Hans_CN
```

`--locale` is repeatable / omittable (defaults to all locales the manifest declares). To build just
one: `build_fontpacks.py --locale ja`.
