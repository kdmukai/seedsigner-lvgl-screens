# Locale picker + endonym images + runtime pack discovery

_How the language-selection screen shows every language in its own native script
without loading every font pack, and how a brand-new language pack works with no
firmware rebuild. Written 2026-07-03 alongside the initial implementation._

## The two problems this solves

1. **The picker must show every onboard language's name in its OWN script**
   (日本語, हिन्दी, Русский, فارسی, …) on one screen. The native LVGL path can't do
   this as a generic `button_list_screen`: there is one active locale with
   screen-wide role fonts (no per-button font), and on ESP32 keeping ~7–11 script
   fonts resident at once overruns the small internal glyph-cache pool → watchdog
   reboot, no graceful degradation. (See `docs/locale-picker-font-plan.md`, the
   original sketch this implements.)
2. **Adding a language must not require a firmware rebuild** — copy a pack onto the
   SD card and it works. The runtime learns *which locales exist and how to render
   each* from each pack's **`manifest.json`**, registered via
   `ss_register_pack_manifest` (`locale_loader.cpp`); screens bakes no locale table.
   A locale whose pack is absent falls through to the English baked floor. (This
   runtime-registration path landed 2026-07-06, replacing a compiled-in `locale_font_table()`.)

## Solution 1 — pre-rendered endonym images (zero runtime font in the picker)

Each row is a SINGLE line: the **English name**, a `|` separator, then the **native
name** — e.g. `Spanish | Español`, `Hindi | हिन्दी`, `Korean | 한국어`. The English
name is always live text (baked baseline). The native name is either live text (a
Latin native the baked floor covers) or a **pre-rendered A8 alpha image** drawn just
after the English text (a non-Latin script). The image is blitted tinted to the
row's live text color, so the whole row highlights/inverts on focus with **no
runtime font** for the native part. A row that overflows scrolls like any
button-list row.

Every pack ships its native-name image pre-rendered offline, one per display-profile
height (`endonym_240.bin` / `_320` / `_480`). Two draw details worth knowing
(`locale_picker.cpp::endonym_draw_cb`): the image is positioned at the English
label's content-left + its measured text width + a gap; and it is clipped
**horizontally only** to the button content box (long rows don't bleed past the
right edge) while its vertical extent is left alone — the image is a hair taller
than the content box and box-clipping it would shave the bottom AA row of tall CJK
strokes (한국어 / 日本語).

- **Rule: a row is an image row iff its native name has glyphs the baked Western
  floor does NOT cover.** The floor is `opensans_western` = ASCII + Latin-1 + Latin
  Extended-**A** + General Punctuation (see the header comment in
  `components/seedsigner/fonts/opensans_western_regular.c`). So:
  - Latin natives within that coverage render as LIVE text — this includes the
    accented European names (Čeština, Türkçe, Español; Ext-A + Latin-1).
  - Everything else is an IMAGE: non-Latin scripts (CJK, Greek/Cyrillic, Arabic,
    Devanagari, …) AND **Vietnamese** — "Tiếng Việt" uses ế/ệ from Latin Extended
    **Additional** (U+1E00–1EFF), a DIFFERENT block that is not baked (Vietnamese
    gets those only from its own font pack, which isn't loaded while the picker
    shows other locales). Vietnamese is therefore an image row even though its name
    is Latin. (Decision 2026-07-03: keep it an image rather than grow the floor by
    ~64 KB to bake Ext-Additional in — Vietnamese stays a pack-tier language.)
  - This is NOT simply "ships a pack ⇒ image": `vi` ships a pack yet its name is
    Latin, and a hypothetical new Latin-Ext-A pack language would render live. The
    real test is baked-floor glyph coverage of the native NAME.
  - The app (`LocaleSelectionView`, B3) makes this decision per language when it
    builds the picker rows; a brand-new non-Latin SD language always ships an
    endonym image, so it lists correctly without loading its font.
- **Offline rasterizer**: the pack repo's `tools/render_endonym.py` uses **PIL + libraqm**
  (the same shaper the pack-validation oracle uses) → complex-script shaping
  (Devanagari conjuncts, Thai stacking, Arabic/Nastaliq joining + RTL) is resolved
  at build time; the device does zero shaping for the picker. Rendered at the locale's
  BUTTON px per profile from the pack repo's `locales.h` size policy
  (`SS_ENDONYM_BUTTON_BASE_*` × the `SS_DISPLAY_PROFILE` multiplier), so the tool never
  duplicates the render layer's size policy. Emitted from the FULL source TTF (the
  corpus subset may not cover the endonym's own glyphs).
- **"SSA8" container** (self-describing, little-endian): `"SSA8"` magic, u8 version,
  u8 bpp(=8), u16 w, u16 h, u16 reserved, then `w*h` A8 bytes (stride == width).
  Parsed in `locale_picker.cpp::parse_ssa8` — fails closed on any short/garbled
  blob so a half-copied pack yields no image, never an out-of-bounds read.
- **Draw path** reuses the glyph-run mechanism exactly: an `lv_draw_image_dsc` with
  `src` = the A8 buffer, `recolor` = the row label's live text color,
  `recolor_opa`/`opa` = COVER. See `glyph_runs.cpp::glyph_run_draw_cb`.
- **Screen**: `locale_picker_screen(cfg)` in `seedsigner.cpp` (it needs the shared
  top-nav scaffold / navigation / screen-load helpers). cfg = `{top_nav,
  active_locale, rows:[{locale, english, native, image?}]}`; each row is a
  `button_ex(..., CHECKED_SELECTION)` (radio: the current locale is check-marked)
  whose label is `"English | native"` (Latin) or `"English |"` + image (non-Latin).
  `"image"` is a filename or `true` (derive `endonym_<active-height>.bin`). Image
  rows additionally call `locale_picker_attach_endonym()`. Selection uses the
  standard body-button result path — `seedsigner_lvgl_on_button_selected(row_index,
  ...)`; the host maps the index back to the locale it placed at that position.

## Solution 2 — runtime, manifest-driven locale registration (no recompile)

`ss_register_pack_manifest(manifest_json, len)` (`locale_loader.cpp`) parses a
pack's own `manifest.json` into a `LocaleFontEntry` and adds it to a
**runtime-augmented locale table**. `find_locale_font_entry()` and
`supported_locales_json()` now compose *compiled ∪ runtime* entries (runtime wins
on a code collision), so `ss_load_locale()` / `ss_locale_pack_files()` work for an
SD-discovered locale **unchanged** — the smallest possible change to the proven
load path. Per-role px is derived from the manifest's `chain` via
`default_locale_roles()` (or an explicit self-describing `roles` array), reusing
the exact policy the compiled packs use. Fails closed on malformed JSON.

The build tool (`build_fontpacks.py`) now writes a **self-describing** manifest on
every branch: `rtl` (fa previously lost it — the CJK/generic branch omitted it),
`endonym`, and `endonym_images`, in addition to the existing
`chain`/`shaping`/`script`/`unicode_range`/font fields.

## Trust boundary (deliberately deferred)

Packs will eventually be individually signed and authorized by a future stage-2
secure bootloader (unbuilt, no spec). We did NOT build verification, but kept the
single accommodation that makes it a one-line future change: **every pack byte —
manifest, fonts, `runs.bin`, and endonym images — flows through the one
`ss_pack_provider` chokepoint**. The picker's endonym provider
(`locale_picker_set_image_provider`) is that same seam/signature, so signature
checking slots in there without touching the render layer.

## Status / not-yet-wired

- **Desktop validation is complete** via the screenshot generator
  (`locale_picker_screen` scenario in `tools/scenarios/scenarios.json`; the
  filesystem provider serves endonym images). Verified across all scripts +
  profiles, incl. image-row invert-on-focus. Runtime discovery is unit-tested in
  `tools/apps/runner_core/test/` (a not-compiled-in locale becomes loadable from a
  registered manifest; fail-closed + override cases).
- **`web_runner` interactive endonym images ARE wired.** The browser playground
  stages pack blobs keyed by filename only, which collides for the picker (every
  pack has an `endonym_<h>.bin`), so a SECOND, locale-keyed staging map was added:
  `ss_stage_endonym(locale, file, …)` + `endonym_provider` (web_runner.cpp,
  `_ss_stage_endonym` in EXPORTED_FUNCTIONS), a `renderPicker()` JS path
  (shell.html) that fetches each image row's `endonym_<displayH>.bin` and stages it
  before loading the screen, and `stage_assets.py` copying `endonym_*.bin` into the
  bundle. Built via the Docker/emsdk toolchain (`build.sh`) and served over
  `serve.sh`.
- **On-device wiring is downstream** (see the `language-selection-integration-todo.md`
  notes left in `seedsigner-raspi-lvgl`, `seedsigner-micropython-builder`, and
  `seedsigner`).
