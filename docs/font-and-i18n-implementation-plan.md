# Multi-Language Font Support — Implementation Status

_Status: **Phase 1 implemented** on branch `feat/font-i18n`. CJK (zh/ja/ko) renders end-to-end through
the desktop screenshot generator across all four resolution profiles, including embedded English.
Companion to the design/rationale doc [font-and-i18n-rendering.md](font-and-i18n-rendering.md) and the
debugging notes in [knowledge/font-loading-binfont-vs-tiny-ttf.md](knowledge/font-loading-binfont-vs-tiny-ttf.md)._

> This supersedes the earlier "planned" version of this file. Several decisions changed during design +
> implementation: engine is **Tiny TTF** (not pre-baked `.bin`); chains are **CJK-primary** (not
> Latin-primary + fallback); the render layer **owns** the locale table and corpus extraction; the
> `@t:`/template/lint scenario machinery was dropped for a simple catalog lookup.

## What was built

### Render layer (`components/seedsigner/`)
- **`locale_fonts.{h,cpp}`** — canonical locale→{font, size, chain} table (single source of truth).
  CJK-primary entries for zh/ja/ko with per-role legibility-bumped base sizes. `supported_locales()`
  emits the per-profile JSON manifest (the only outward interface).
- **`font_registry.{h,cpp}`** — the I/O-agnostic seam: `seedsigner_set_locale` / `register_font` /
  `clear_registered_fonts`. Creates `lv_tiny_ttf` fonts from buffers (kerning off, `cache_size=0`),
  wires the CJK-primary → OpenSans-fallback chain, repoints the active profile.
- **`gui_constants.{h,cpp}`** — profiles made non-const; added `active_profile_mutable()`.

### Offline tooling (`tools/fontpack/`, see its README)
- **`build_fontpacks.py`** — single entry point (only script at the top level): derives the locale set
  from `--dump-locales` and runs the two steps below for it (keeping their locale sets in sync).
  `--locale` restricts it.
- **`steps/po_catalog.py`** — self-contained `.po` reader (no Babel): catalog + corpus extraction.
- **`steps/build_lang_font.py`** — reads `screenshot_gen --dump-locales`, subsets each locale's source
  TTF to its corpus via `fontTools` → one `.ttf` per locale (in gitignored `tools/fontpack/out/`).
- **`steps/gen_localized_scenarios.py`** — translates `tools/scenarios.json` display-text leaves via the
  catalog (English passthrough) → per-locale `tools/scenarios/<loc>.json` (gitignored).

### Assets & data
- Noto Sans TTFs (SC/JP/KR/AR/TH) vendored into `components/seedsigner/assets/` — **this repo owns all
  locale fonts**.
- `seedsigner-translations` added as a submodule at `tools/seedsigner-translations/` — **`.po` catalogs
  only** (translation content).

### Screenshot generator (`tools/screenshot_generator/`)
- `--dump-locales` (manifest for the build tooling), `--locale` / `--font-dir` (register a locale's
  subset `.ttf` and render its localized scenarios). Build uses `LV_USE_TINY_TTF`.

## How to run (Phase 1, desktop)

```bash
git submodule update --init tools/seedsigner-translations
cmake -S tools/screenshot_generator -B tools/screenshot_generator/build \
      -DDISPLAY_WIDTH=240 -DDISPLAY_HEIGHT=240 && cmake --build tools/screenshot_generator/build -j
python3 tools/fontpack/build_fontpacks.py   # subset fonts + localize scenarios, all manifest locales + en
./tools/screenshot_generator/build/screenshot_gen \
      --locale zh_Hans_CN --scenarios-file tools/scenarios/zh_Hans_CN.json \
      --font-dir tools/fontpack/out --out-dir tools/screenshot_generator/screenshots/i18n/zh_Hans_CN
```
`build_fontpacks.py` is the single entry point (orchestrates `build_lang_font.py` +
`gen_localized_scenarios.py`, locale set derived from `--dump-locales`). Independent `(locale)` render
runs parallelize cleanly (one process each).

## Known issues / follow-ups

- **Tiny TTF cache-path spins** on certain content at any cache size → `cache_size=0` for now (fine for
  the static tool, slow for the device). Needs an upstream patch/fork before the device relies on the
  cache. See the knowledge doc (bug #3).
- **Tiny TTF no-cache fallback bug FIXED** (`third_party/patches/lv_tiny_ttf-fallback-chain.patch`):
  absent codepoints now report as *not found*, so the OpenSans fallback engages and embedded English
  renders at the normal English size. CJK subsets no longer bake in ASCII. Push the fix upstream so the
  local patch can be dropped. See the knowledge doc (bug #2).
- **`main_menu_screen` now takes its title + button labels from JSON** (`top_nav.title` +
  `button_list`), so it localizes; the four icons stay fixed. Defaults reproduce the English home menu
  when called with no context.
- **Translated-layout overflow** (long translations clipping buttons) — separate concern; handled by the
  Python `overflow.py` detection work, not the renderer.
- **Rasterize-all pack-signing validation gate**, compiled + run per production architecture — to be built.

## Remaining roadmap
- Screenshot gallery: nested-by-locale manifest + `index.html` (locale nav + resolution tabs).
- Parallelize the generator across locale × resolution.
- Wire `web_runner` (locale selector) + `screen_runner` (locale switch).
- Phase 2: shaping-complex scripts (Arabic/Persian, Thai, Hindi) — config + presentation-form-aware
  subsetting (Arabic font + cmap coverage already vendored).
