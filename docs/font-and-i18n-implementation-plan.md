# Multi-Language Font Support — Implementation Status

_Status: **Phase 1 implemented** on branch `feat/font-i18n`. CJK (zh/ja/ko) renders end-to-end through
the desktop screenshot generator across all four resolution profiles, including embedded English. The
font-tiering work (`docs/font-tiering-plan.md`) has since landed: a compiled-in OpenSans Western-Latin
TTF baseline for the five translated roles (Stage A) and same-size Greek/Cyrillic/Vietnamese script
packs (Stage B). Companion to the design/rationale doc [font-and-i18n-rendering.md](font-and-i18n-rendering.md)
and the debugging notes in [knowledge/font-loading-binfont-vs-tiny-ttf.md](knowledge/font-loading-binfont-vs-tiny-ttf.md)._

> This supersedes the earlier "planned" version of this file. Several decisions changed during design +
> implementation: engine is **Tiny TTF** (not pre-baked `.bin`); chains are **CJK-primary** (not
> Latin-primary + fallback); the render layer **owns** the locale table and corpus extraction; the
> `@t:`/template/lint scenario machinery was dropped for a simple catalog lookup.

## What was built

### Render layer (`components/seedsigner/`)
- **`locale_fonts.{h,cpp}`** — canonical locale→{font, size, chain} table (single source of truth).
  CJK-primary entries for zh/ja/ko with per-role legibility-bumped base sizes, plus same-size
  block-range script-pack entries for el/ru/vi (`ChainRole::Fallback`, OpenSans source, with a
  `unicode_range`). `locale_role_render_px()` is the shared per-role px source of truth (it replicates
  the baseline's large_button quirk: 20 px base at 240 height, 18 at 320/480). `supported_locales()`
  emits the per-profile JSON manifest (the only outward interface).
- **`font_registry.{h,cpp}`** — the I/O-agnostic seam: `seedsigner_set_locale` / `register_font` /
  `clear_registered_fonts`. Creates `lv_tiny_ttf` fonts from buffers (kerning off, glyph cache on via
  `SEEDSIGNER_TTF_CACHE_SIZE`),
  wires the chain (CJK-primary → OpenSans-baseline fallback; or a same-size script pack as a fallback
  under a heap copy of the baseline primary), validates the supplied px against `locale_role_render_px()`,
  and repoints the active profile.
- **`gui_constants.{h,cpp}`** — profiles made non-const; added `active_profile_mutable()`. The five
  translated text roles are no longer baked bitmaps: `set_display()` lazily installs the compiled-in
  OpenSans Western-Latin TTF baseline per profile (via `lv_tiny_ttf`) once LVGL is initialized, never
  destroyed. The keyboard (Inconsolata) and icon fonts stay baked `.c`.

### Offline tooling (`tools/i18n/`, see its README)
Two independent steps — fonts (production) and scenarios (test-only) are decoupled:
- **`build_fontpacks.py`** — reads `screenshot_gen --dump-locales`, then subsets each locale's source TTF
  in one of two modes → `manifest.json` + `.ttf`(s) per locale in repo-root **`lang-packs/<loc>/`**
  (gitignored, production-ready). **Corpus mode** (CJK/Noto): subset to the locale's `.po` glyph corpus →
  one `<loc>.ttf`. **Block-range mode** (script packs: el/ru/vi, keyed off a `unicode_range` in the
  manifest): subset OpenSans to a fixed Unicode block, two weights → `<loc>_{regular,semibold}.ttf`.
  `--locale` restricts the set.
- **`gen_localized_scenarios.py`** — translates `tools/scenarios/scenarios.json` display-text leaves via
  the catalog (English passthrough) → per-locale **`tools/scenarios/localized/<loc>.json`** (gitignored,
  test-only).
- **`po_catalog.py`** — self-contained `.po` reader (no Babel): catalog + corpus extraction; shared by both.

### Assets & data
- Noto Sans TTFs (SC/JP/KR/AR/TH) plus OpenSans (Regular + SemiBold, the source family for the Western
  baseline and the el/ru/vi script packs) vendored into `components/seedsigner/assets/` — **this repo
  owns all locale fonts**.
- `seedsigner-translations` added as a submodule at `tools/i18n/seedsigner-translations/` — **`.po`
  catalogs only** (translation content).

### Screenshot generator (`tools/apps/screenshot_generator/`)
- `--dump-locales` (manifest for the build tooling), `--locale` / `--font-dir` (register a locale's
  subset `.ttf` and render its localized scenarios). Build uses `LV_USE_TINY_TTF`.

## How to run (Phase 1, desktop)

```bash
git submodule update --init tools/i18n/seedsigner-translations
cmake -S tools/apps/screenshot_generator -B tools/apps/screenshot_generator/build \
      -DDISPLAY_WIDTH=240 -DDISPLAY_HEIGHT=240 && cmake --build tools/apps/screenshot_generator/build -j
python3 tools/i18n/build_fontpacks.py              # font packs → lang-packs/
python3 tools/i18n/gen_localized_scenarios.py      # localized scenarios → tools/scenarios/localized/
./tools/apps/screenshot_generator/build/screenshot_gen \
      --locale zh_Hans_CN --scenarios-file tools/scenarios/localized/zh_Hans_CN.json \
      --font-dir lang-packs --out-dir tools/apps/screenshot_generator/screenshots/i18n/zh_Hans_CN
```
Independent `(locale)` render runs parallelize cleanly (one process each).

## Known issues / follow-ups

- **The glyph cache is on by default (`SEEDSIGNER_TTF_CACHE_SIZE=256`); each host must back it with
  memory.** The cache holds rasterized bitmaps; against a too-small fixed pool it OOMs and LVGL's default
  assert handler turns that into a CPU spin (the former "bug #3" — not a cache/`lv_tiny_ttf` defect). So
  every target provisions memory: Pi Zero `LV_STDLIB_CLIB`, ESP32-S3 glyph bitmaps in PSRAM; a genuinely
  tiny-pool build overrides the size to 0. See `docs/knowledge/tiny-ttf-cache-spin-root-cause.md`.
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
