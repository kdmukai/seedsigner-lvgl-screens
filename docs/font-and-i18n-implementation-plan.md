# Multi-Language Font Rendering + Catalog-Sourced Test Harness — Implementation Plan

_Status: planned / not yet implemented. This is the implementation companion to the design doc
[font-and-i18n-rendering.md](font-and-i18n-rendering.md). It records the concrete first slice agreed
in planning: the runtime font-registration seam, the offline font-production tooling, a catalog-sourced
scenario generator, and full multi-language wiring of the desktop tools._

## Context

`seedsigner-c-modules` is the **render** layer of SeedSigner's dual-platform LVGL
architecture. Today every font is a compiled-in C array covering only ASCII
(`0x20-0x7E`) + icons, and the desktop test tools ([screen_runner](../tools/screen_runner/screen_runner.cpp),
[screenshot_generator](../tools/screenshot_generator/screenshot_gen.cpp)) render hardcoded
English strings from [tools/scenarios.json](../tools/scenarios.json). There is **no way to
render any non-Latin script** and no way to test the other ~21 translated languages.

The agreed design ([font-and-i18n-rendering.md](font-and-i18n-rendering.md)) moves
language fonts onto the SD card as signed packs, loaded at runtime through a
`register_font(buffer)` seam that keeps this layer ignorant of I/O and crypto. This plan
implements the first concrete slice and validates it on the **desktop tools** (no hardware):
the registration seam, the offline font-production tooling (reusing SeedSigner's existing
corpus extractor), a **catalog-sourced scenario generator**, and full multi-language wiring
of both desktop tools.

Production translations/fonts stay in the `seedsigner-translations` repo; this repo gets only
the minimal support to drive its own internal tests.

### Decisions locked in planning
- **Catalog-sourced display strings.** Test strings are pulled from the actual `.po`/`.mo`
  catalogs, never hand-maintained in this repo (no parallel maintenance; matches the real
  production gettext data flow).
- **Full-coverage, locale-driven harness**, with **staged font support**:
  - **Phase 1 (this effort):** every *correct-rendering* script — Latin/Latin-extended
    (incl. Vietnamese), Greek, Cyrillic, and all CJK (zh/ja/ko). ~19 of 21 translated locales.
  - **Phase 2 (deferred, documented):** shaping-complex scripts — Arabic/Persian
    (presentation-form expansion), Thai (segmentation + zero-advance marks), Hindi (Brahmic
    reordering). They render with graceful degradation until then.
- **Per-size fallback (v2):** register a distinct subset `.bin` per profile text size; set each
  profile text font's `.fallback` to its exact size-match (pixel-correct).
- **Engine: pre-baked `.bin`** via `lv_binfont_create_from_buffer` (matches production seam).
- **`seedsigner-translations` added as a git submodule of THIS repo** (data source for tooling).
- **`scenarios.json` evolves into a translatable template + a generator** (see Step 3).

### Verified facts (read from source / data, not assumed)
- **LVGL v9.5.0:** `lv_binfont_create_from_buffer(void*, uint32_t)`
  ([lv_binfont_loader.h:55](../third_party/lvgl/src/font/binfont_loader/lv_binfont_loader.h#L55))
  is gated by `LV_USE_FS_MEMFS` (default 0) and needs a real `LV_FS_MEMFS_LETTER` (default
  `'\0'`). Desktop builds use `-DLV_CONF_SKIP`, so both must be added as compile defs.
- **No `lv_font_set_fallback`** in v9.5.0 — assign the `const lv_font_t *fallback;` field
  directly. `lv_font_get_glyph_dsc` walks the chain and defers placeholder glyphs, so a real
  glyph later in the chain wins → **Latin/icon primary + script fallback works.**
- Compiled-in fonts are **`const lv_font_t`** shared across profiles → the seam installs **heap
  copies** of the primaries and repoints the active profile; never mutate the const originals.
- `LV_USE_FONT_PLACEHOLDER` is on by default (out-of-corpus glyphs draw a box, not a fault).
- **OpenSans (already vendored at [components/seedsigner/assets/](../components/seedsigner/assets/))**
  covers Greek (75), Cyrillic (255), Latin-Ext-A (128), and Vietnamese (90/96, all sampled
  tone-stacked vowels present) — so it is the subset source for all non-CJK Phase-1 locales.
- `.mo` files are **not committed** in the translations repo → tooling must `pybabel
  compile_catalog` from `.po` first.
- **scenario↔msgid reality:** only ~42% of current scenario strings match catalog msgids; the
  misses are structural identifiers (`scroll_many`, `success`, …) and imagined examples (the
  long German compound). Real menu labels like `About`/`Camera`/`Network`/`Back` are **not**
  catalog msgids, while `Settings`/`Done`/`Scan` are. → the template must reference *verified*
  msgids and the generator must lint them (Step 3).
- `lv_font_conv` (`/usr/local/bin`) supports `--format bin` and `--symbols "<chars>"`.

---

## Step 0 — Add translations submodule & resolve working tree
- Add `seedsigner-translations` as a submodule at **`tools/seedsigner-translations/`** (pin to
  current `origin/dev`, tip `708961a`). It supplies: `l10n/<loc>/LC_MESSAGES/messages.po`, the
  reusable extractor `tools/extract_characters_from_babel_mo.py`, and CJK source fonts
  `fonts/NotoSans{SC,JP,KR}-Regular.ttf` (+ AR/TH for Phase 2). OpenSans (non-CJK source) is
  already vendored locally, so the submodule + local assets cover all Phase-1 fonts. (Devanagari
  for Phase-2 Hindi lives only in the main app `resources/fonts/` — handled in Phase 2.)
- **Placement rationale (forward-looking):** keep it as a **shared peer under `tools/`** (sibling to
  `tools/fontpack/`, `tools/screen_runner/`, and a future `tools/glyphset/`), not nested inside one
  tool and not in `third_party/`. It is a shared *input* consumed by multiple produce-tools (font-pack
  builder, scenario generator, and the future LVGL-based glyph extractor in the TODO below). `third_party/`
  is reserved for the vendored LVGL engine; the SeedSigner-specific corpus belongs with the
  SeedSigner-specific tooling. The future extractor being "owned by this repo" is about its *code* living
  in `tools/`, independent of where this data submodule sits — so this placement holds either way.
- **Prereq for the developer's existing sibling checkout:** that submodule working tree is dirty
  with *pre-existing, non-ours* changes (deleted `ca/de/fr/it/ja/ko/zh` `.po`; newer `nl/pl`
  edits) blocking a fast-forward. Recommended: `git stash` there then pull, preserving the local
  edits. This repo's new submodule is an independent clean checkout, so this only matters if we
  reuse the sibling path. **Confirm before discarding anything.**

## Step 1 — Production `register_font` seam (`components/seedsigner/`)
New: `components/seedsigner/font_registry.{h,cpp}`.
```c
// Ignorant of I/O + crypto. Registers one already-verified subset .bin, sized for a specific
// profile text size, as the script fallback. Buffer must outlive the font. v2 = one call per size.
bool seedsigner_register_font(const char *logical_name, const uint8_t *buf, size_t len, int font_px_size);
void seedsigner_clear_registered_fonts();  // detach, destroy scripts, restore compiled primaries
```
Integration with the profile mechanism in [gui_constants.cpp](../components/seedsigner/gui_constants.cpp):
- Change profile storage `static const DisplayProfile` → `static DisplayProfile`; `active_profile()`
  still returns `const&` (call sites unchanged). Add internal `DisplayProfile& active_profile_mutable();`.
- `seedsigner_register_font`: `lv_binfont_create_from_buffer(buf,len)` → validate `font_px_size`
  against the active profile's matching text-font size (size-aware contract) → for each text font of
  that size (`body/button/large_button/top_nav_title/main_menu_title/keyboard` — **not** icon fonts),
  heap-copy the const primary, set `copy->fallback = script`, repoint the profile pointer. Track
  `{field, original, copy, script}` so `clear` can `lv_binfont_destroy` + free + restore.
- Caller registers one `.bin` per distinct profile text size (v2).

## Step 2 — Offline font-production tooling (`tools/fontpack/`)
New: `tools/fontpack/build_lang_font.py`, `font_map.py` (locale→script→source-font), `README.md`.
Per locale × profile text size:
1. `pybabel compile_catalog -d tools/seedsigner-translations/l10n -l <loc>` (.mo not committed).
2. Run the **existing extractor** with CWD = `tools/seedsigner-translations/tools/` (it hard-codes
   `os.pardir/l10n/...`); capture stdout = corpus string. **Reuse, don't fork.**
3. Resolve source font via `font_map`: Latin/Greek/Cyrillic/Vietnamese → `components/seedsigner/assets/OpenSans-SemiBold.ttf`;
   `zh_Hans_CN`→NotoSansSC, `ja`→NotoSansJP, `ko`→NotoSansKR (submodule `fonts/`). Phase-2 locales
   (`fa`/`th`/`hi`) are on a **skip list** (logged). **Coverage safety net:** for OpenSans-sourced
   locales, if the corpus contains codepoints OpenSans lacks, warn + skip (auto-defers any future
   locale that needs shaping/unavailable glyphs).
4. `lv_font_conv --bpp 4 --no-compress --stride 1 --align 1 --size N --font <ttf> --symbols "<corpus>"
   --format bin -o out/<loc>/<loc>_<N>px.bin` (pass `--symbols` as one argv element via subprocess
   list form — no shell). Write `out/<loc>/manifest.json` (sizes, source font, corpus length, sha256).
   Signing/bundling deferred to the platform layer (manifest leaves the seam).

## Step 3 — Scenarios template + catalog-sourced generator
**Format change** ([tools/scenarios.json](../tools/scenarios.json) → `tools/scenarios.template.json`):
display-text leaves that should be translated are marked with a `@t:` prefix carrying the **msgid**
(which is also the English fallback). Structural fields (variation `name`, keyboard charset tokens,
screen keys) and synthetic numeric content (satoshi amounts) stay **bare**. A third marker `@scrolltitle`
supplies the **horizontal-scrolling-title** stress test, filled per-locale + per-resolution (see below).
```json
"button_list": ["@t:Settings", "@t:Tools", "@t:Seeds"],
"top_nav": { "title": "@t:Settings" },
"name": "scroll_many",                     // structural — NOT translated
"top_nav": { "title": "@scrolltitle" }     // long-scroll test — filled per-locale, in-script
```
New generator `tools/fontpack/gen_scenarios.py`:
- `--lint`: verify every `@t:X` exists as a msgid in the canonical catalog (`.pot`/es `.po`); warn on
  misses (this is what forces the template to use *real* SeedSigner text — fixes the `About`/`Camera`
  problem by flagging unverified strings so we replace them with real msgids or mark them bare).
- Per locale (incl. `en`): deep-copy template, translate `@t:` leaves via that locale's `.mo`
  (fallback to English msgid when untranslated), strip markers, write `tools/scenarios/<loc>.json`
  (plain strings). The desktop tools always load these generated files, so their loaders stay dumb.
- **`@scrolltitle` resolution (per-locale unit + runtime per-resolution expansion):** the generator
  resolves it to a **title-appropriate unit string** — the longest *translated title* drawn from a small
  **curated pool of real screen-title msgids** (`title_msgids` in the generator config, lint-verified),
  NOT the longest string in the whole catalog (that would be a body/help paragraph and read absurdly as
  a title). The unit is emitted with a passthrough marker (e.g. `@fill:<unit>`) so the final length is
  decided **per-resolution at render time** (Step 4). It is NOT pre-expanded in the generator, because the
  required length depends on screen width × title-font size, which differ per profile (a title that
  overflows 240px need not overflow 800px, and one sized for 800px is overkill at 240px). The unit is
  always a real in-script title from the locale, so the scrolling title reads as a title and renders in
  the target script. (The `title_msgids` pool is curated once from SeedSigner's actual screen titles —
  small, stable, language-independent; translations come from the catalogs automatically.)
- Authoring the template is curation work: upgrade scenario display text to **verified** msgids
  (most of it real SeedSigner UI text); keep purely-synthetic stress content (satoshi amounts,
  scroll_many filler) bare, and use `@scrolltitle` for the scrolling-title case.

## Step 4 — Wire desktop tools (locale-driven, full coverage)
**Runtime `@fill:` expansion (shared by both tools, per-resolution + per-script for free):** when a
title field carries the `@fill:<unit>` marker, the tool repeats the unit (separator-joined) and measures
the rendered width with the **active profile's title font** via LVGL's `lv_text_get_size`/`lv_text_get_width`
(which walks the registered fallback chain, so wide CJK glyphs are measured accurately), stopping once
width exceeds the current screen width × ~1.5. Because this runs against the active profile, each
resolution gets a title just long enough to scroll — not the artificially-long single value a precomputed
string would force. Screenshot-gen expands inside its per-profile loop; screen_runner re-expands on
resolution switch.

[screen_runner.cpp](../tools/screen_runner/screen_runner.cpp):
- `--locale <loc>` (optional initial language; default English) / `--font-dir`. On load of a non-English
  locale, load every `<loc>_<N>px.bin` for the active profile's text sizes into long-lived buffers and
  `seedsigner_register_font(...)` each (v2); load `scenarios/<loc>.json`.
- Add a **language drop-list selector to the chrome** (alongside the existing resolution control),
  populated from the available generated locales (+ an "English" entry). Selecting a language: `clear` →
  load+register the new locale's per-size `.bin`s (or just `clear` for English) → load the matching
  `scenarios/<loc>.json` → redraw. (Drop-list selector per the requested UX, not a cycle key.)
- In `switch_resolution()`: after `set_display()`, if a non-English language is active, `clear` +
  re-register for the **new** profile's sizes (validates per-profile size-matched re-registration).

[screenshot_gen.cpp](../tools/screenshot_generator/screenshot_gen.cpp):
- **Default: English only** — with no flag, behavior is unchanged (no font registration, English baseline).
- `--locale <loc>[,<loc>…]` (specific locales) or `--all-locales` (every generated locale) opts in to more.
- Output is **grouped by locale** (`screenshots/<loc>/…`, with `en` the default group; preserve the
  existing gallery path for `en` if CI depends on it). In the per-profile loop, for each non-English
  locale: `clear` + register that profile's sizes before rendering. This turns the CI gallery into an
  opt-in multi-language visual-regression artifact without changing the default English run.

## Step 5 — LVGL config for desktop builds
Add to `target_compile_definitions` in both [screen_runner/CMakeLists.txt](../tools/screen_runner/CMakeLists.txt)
and [screenshot_generator/CMakeLists.txt](../tools/screenshot_generator/CMakeLists.txt):
```cmake
  LV_USE_FS_MEMFS=1
  LV_FS_MEMFS_LETTER='M'
```
Add `font_registry.cpp` to `SEEDSIGNER_CORE_SOURCES` in both. The memfs driver
(`third_party/lvgl/src/libs/fsdrv/lv_fs_memfs.c`) is already swept by the existing `GLOB_RECURSE`.
**Hardware builds** must set the same two flags in their real `lv_conf.h` before the seam ships
(out of scope here; flagged for the platform layer).

---

## Phase 2 (deferred, documented — not this effort)
Shaping-complex scripts: Arabic/Persian presentation-form superset expansion (the upstream
`extract_characters_from_babel_mo.py` TODO), Thai offline U+200B segmentation + zero-advance-mark
font, Hindi Brahmic handling (+ source the Devanagari TTF from the main app). Until done, `fa`/`th`/`hi`
are on the tooling skip list and render with graceful degradation.

### TODO (follow-on) — own glyph-set extraction in this repo, LVGL-based
Phase 1 reuses the upstream Python extractor (`extract_characters_from_babel_mo.py`) via the submodule.
Now that the translations repo is a submodule *here*, evaluate migrating unique-glyph-set extraction into
a **c-modules-owned tool**, potentially **driven by LVGL itself**. Rationale: (a) the render layer is the
correct owner of "what glyphs will actually be drawn"; (b) for shaping scripts it could compute the
**provably-exact** presentation-form set by running the corpus through LVGL's own shaper
(`third_party/lvgl/src/misc/lv_text_ap.c`) via a small C harness — the route the design doc flags as the
only LVGL-exact one — instead of the Python over-render-the-superset workaround; (c) it colocates the
four-verb "Produce" tooling with the renderer. This dovetails with the Phase-2 Arabic/Persian work and
would let the extractor and the renderer share one definition of the corpus.

## Files
- **New:** `components/seedsigner/font_registry.{h,cpp}`; `tools/fontpack/{build_lang_font.py,font_map.py,
  gen_scenarios.py,README.md}`; `tools/scenarios.template.json`; submodule `tools/seedsigner-translations/`;
  generated `tools/scenarios/<loc>.json` + `tools/fontpack/out/<loc>/*.bin`.
- **Modify:** `components/seedsigner/gui_constants.{h,cpp}` (non-const profiles + `active_profile_mutable`);
  `tools/screen_runner/screen_runner.cpp`; `tools/screenshot_generator/screenshot_gen.cpp`; both tool
  `CMakeLists.txt`; `.gitmodules`.

## Verification (end-to-end)
```bash
git submodule update --init tools/seedsigner-translations
python3 tools/fontpack/gen_scenarios.py --lint                       # template uses real msgids
python3 tools/fontpack/gen_scenarios.py --all-locales                # -> tools/scenarios/<loc>.json
python3 tools/fontpack/build_lang_font.py --locale zh_Hans_CN --all-multipliers  # -> out/zh_Hans_CN/*.bin
python3 tools/fontpack/build_lang_font.py --locale ko --all-multipliers
python3 tools/fontpack/build_lang_font.py --locale el --all-multipliers          # OpenSans subset (Greek)
cmake -S tools/screenshot_generator -B tools/screenshot_generator/build \
      -DDISPLAY_WIDTH=480 -DDISPLAY_HEIGHT=320 && cmake --build tools/screenshot_generator/build -j
./tools/screenshot_generator/build/screenshot_gen --locale zh_Hans_CN \
      --font-dir tools/fontpack/out --out-dir tools/screenshot_generator/screenshots/zh_Hans_CN
# interactive: pick a language from the drop-list selector; click the resolution control to switch profile
cmake -S tools/screen_runner -B tools/screen_runner/build \
      -DDISPLAY_WIDTH=480 -DDISPLAY_HEIGHT=320 && cmake --build tools/screen_runner/build -j
./tools/screen_runner/build/screen_runner --locale zh_Hans_CN --font-dir tools/fontpack/out
```
**Pass criteria:** English screens stay pixel-identical to the no-`--locale` baseline (fallback only
engages for non-ASCII). zh/ko/Greek catalog strings render in the correct script with no missing-glyph
boxes for catalog chars; legibility holds at the 240px/17px profile (the CJK quality stress). Switching
resolution keeps each language rendering at the right per-size fonts (validates v2). `--lint` passes
(no unverified `@t:` msgids).

## Open risks / notes
- **Template curation effort:** aligning scenario display text to verified msgids is the main manual
  task; `--lint` enforces honesty. Imagined examples remain bare by design.
- Making `DisplayProfile` storage non-`const` — access is only via `active_profile()`/macros
  (grep-confirmed) → safe.
- Buffer lifetime managed by the desktop tools (free only after `clear`).
- `LV_FONT_FMT_TXT_LARGE` stays 0 (subset corpora are small); flip only if a huge subset fails to load.
- Two checkouts of `seedsigner-translations` on a dev machine (here + main app) are independent pins — fine.
- Signing/bundling intentionally deferred to the platform layer (manifest + sha256 seam left).
