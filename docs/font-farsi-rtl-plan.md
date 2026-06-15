# Farsi (Persian) support — RTL + Arabic shaping plan (Phase 2)

_Status: **IMPLEMENTED** (first pass, desktop tools). `fa` ships in
`tools/i18n/supported_locales.json` with a 13.8 KB corpus-shaped NotoSansAR pack
and renders shaped, right-to-left across all profiles. Companion to
`docs/font-and-i18n-rendering.md` and `docs/font-tiering-plan.md`._

## What was actually built (vs. the plan below)

- **Subsetting: corpus-shaped, not block-range.** Rather than subsetting the whole
  Arabic blocks, the pack contains exactly the presentation forms the renderer
  emits for the `fa` corpus. The glyph set is produced by a NEW standalone tool,
  **`tools/i18n/shaper/lv_shape`**, which compiles LVGL's OWN shaper
  (`lv_text_ap_proc`, `LV_USE_ARABIC_PERSIAN_CHARS=1`) and turns stdin text into
  the JSON code-point set it resolves to. `build_fontpacks.py` pipes each
  Arabic-script locale's `.po` corpus through it (`needs_arabic_shaping` gate) and
  subsets to those forms (ASCII excluded — baked floor). Using the real shaper
  (not a Python port) means the pack can never drift from render-time behavior and
  the tricky edge cases are handled for free: the buggy digit row
  `{0x06F0,-1,2,0}` only ever emits its isolated form in real contexts; ZWNJ is
  preserved as a join-breaker; `لا` collapses to the lam-alef ligature. Result for
  the current `fa` corpus: **105 glyphs / 13.8 KB** (the whole font is 194 KB).
  Why standalone (not a screenshot_gen subcommand): the packs are consumed by
  every renderer (Pi Zero, ESP32, desktop), so pack prep is its own shared tool.
- **Render config:** `LV_USE_BIDI=1` + `LV_USE_ARABIC_PERSIAN_CHARS=1` added to all
  four desktop targets' compile defs. (LTR locales unaffected; English verified
  pixel-unchanged.)
- **Table:** `fa` is `ChainRole::Primary` (NotoSansAR primary at the CJK bump,
  baked OpenSans fallback for embedded Latin/digits) with a `bool rtl` field;
  `supported_locales_json()` emits `"rtl": true` and `seedsigner_locale_is_rtl()`
  exposes it to the screen layer.
- **RTL is TEXT-ONLY, applied in ONE global place (not per-builder, not full
  mirroring).** `load_screen_and_cleanup_previous()` — the single screen-load path
  every builder already calls — runs `apply_rtl_text_to_labels()` on the finished
  screen tree for RTL locales: a recursive walk that sets `LV_BASE_DIR_RTL` on
  every `lv_label` and nothing else. So builders/components stay direction-agnostic
  (zero per-label maintenance), and because only labels are touched, element
  **layout keeps its left-to-right order**: LVGL's `base_dir` is one inherited
  property driving BOTH text bidi AND layout (flex order + the coordinate origin
  for `lv_obj_set_pos`/`lv_obj_align`), so a root-level RTL would also mirror the
  Scan tile, the nav buttons, and the passphrase cursor — scoping it to labels
  avoids all of that. The walk **skips `lv_textarea` subtrees**, so the ASCII
  passphrase entry box + cursor stay LTR; the keyboard is a buttonmatrix (no child
  labels) and is unaffected. Forcing `RTL` (vs `AUTO`) also fixes the strings that
  *start* with a Latin term (e.g. `"SeedQR …"`), which AUTO's first-strong-char
  detection would otherwise mis-base to LTR.
- **Why text-only:** the original plan (step 4 below) called for mirroring the UI
  (swapping nav button sides, flipping alignment) as conventional RTL practice,
  but flagged "how much screen mirroring is enough?" as an open question. Per
  product guidance we are NOT mirroring on-screen elements until we have actual
  Farsi-user UX input — back stays left/pointing-left, hardware-mapped buttons
  stay put, the Scan tile stays top-left. The single `apply_rtl_text_to_labels()`
  pass is also where any future selective mirroring would hook in.

**Known refinements (not blockers):** at the 240px icon-grid main menu, long Farsi
labels (e.g. "عبارات بازیابی") clip to the cell width — a general long-label
constraint (CJK fits because it's compact), not an RTL bug. Hindi/Thai remain
Phase-2 follow-ons.

---

_Original plan (kept for reference):_

## TL;DR — it's feasible with the existing stack (no HarfBuzz)

Arabic/Persian is the first script that needs **two things the Latin/Greek/Cyrillic/
CJK tiers didn't**: (1) **cursive shaping** (each letter has isolated/initial/medial/
final forms), and (2) **RTL bidi** (right-to-left, with embedded LTR runs for Latin
technical terms + digits). Both are available in-tree:

- **Shaping:** LVGL's built-in `LV_USE_ARABIC_PERSIAN_CHARS` (`src/misc/lv_text_ap.c`)
  rewrites the logical Arabic/Persian code points into **presentation-form** code
  points (U+FE70–FEFF, a few U+FB50–FDFF) *before* glyph lookup. So tiny_ttf only
  ever has to rasterize a presentation-form code point by number — **no GSUB / no
  HarfBuzz**. This is "basic" shaping (the 4 positional forms + the common
  lam-alef ligatures), not full OpenType shaping, but it's correct for SeedSigner's
  UI strings.
- **Bidi:** LVGL's `LV_USE_BIDI` (Unicode bidi algorithm) handles the RTL paragraph
  direction and mixed LTR runs.
- **Font:** `components/seedsigner/assets/NotoSansAR-Regular.ttf` is already vendored
  and **contains the presentation-form glyphs the shaper emits** (verified:
  141 glyphs in U+FE70–FEFF, 631 in U+FB50–FDFF, plus the 256 base U+0600–06FF). So
  the same lv_tiny_ttf path used for CJK works for Farsi.

The hard/unknown part is **not the glyphs — it's the RTL layout** of the screens
(they were all built LTR). That's the bulk of the work; budget accordingly.

## Implementation steps

### 1. Turn on bidi + Arabic shaping in the LVGL build
The desktop tools build with `LV_CONF_SKIP` (no `lv_conf.h`), so features are enabled
as compile definitions (like `LV_USE_TINY_TTF=1`). Add to **every** desktop target's
`target_compile_definitions` (screenshot_generator, screen_runner, web_runner,
runner_core/test) and to the ESP32 component build:

```
LV_USE_BIDI=1
LV_USE_ARABIC_PERSIAN_CHARS=1
```

For the firmware (`raspi-lvgl`, `micropython-builder`) set the same in their
`lv_conf.h`. Note `LV_USE_BIDI` enables the bidi processor globally; the default base
direction stays auto/LTR unless a label/screen sets RTL (step 4).

### 2. Build a Farsi font pack from NotoSansAR
`tools/i18n/build_fontpacks.py`. Two viable approaches — **prefer block-range** (it's
corpus-independent, like the Greek/Cyrillic/Vietnamese packs, so a translation edit
never changes the pack):

- **Block-range (recommended):** subset NotoSansAR to the base block **plus the
  presentation forms the shaper actually emits** — start from `U+0600-06FF`
  (Arabic) + `U+FE70-FEFF` (Presentation Forms-B) + the Persian-specific forms in
  `U+FB50-FDFF` (e.g. peh/tcheh/jeh/gaf/farsi-yeh forms ≈ U+FB56-FB95, U+FBFC-FBFF —
  VERIFY against `lv_text_ap.c`'s `ap_chars_map` and a real Farsi `.po`). Drop GSUB
  (`--drop-tables+=GSUB,GPOS,GDEF`) — LVGL's basic shaper does the form selection, so
  the font's OpenType layout is unused. Two weights? Noto Sans Arabic ships a single
  Regular; like the CJK packs, one weight serves every role.
- **Corpus:** subset to the `.po` corpus, but you must also pull in each corpus
  char's presentation forms (run the same base→form mapping LVGL uses). More code,
  smaller pack. Only worth it if the block-range pack is too big for the SD tier.

The block-range pack should be a few tens of KB (the whole font is 194 KB). Wire it
the same way the other packs are produced (`lang-packs/fa/...` + manifest).

### 3. Add the `fa` entry to the render layer
`components/seedsigner/locale_fonts.{h,cpp}`:
- Add a `fa` → `NotoSansAR` entry. Chain: **`ChainRole::Primary`** like CJK (the
  Arabic font is primary; the baked OpenSans Western baseline stays as fallback so
  embedded Latin technical terms / digits still render). Pick per-role sizes (start
  at the CJK bump and tune for Arabic x-height legibility).
- Add an **RTL marker** to `LocaleFontEntry` (e.g. `bool rtl`) so the platform/
  screen layer knows to set base direction RTL for this locale. `locale_role_render_px`
  / the manifest are otherwise unchanged.
- Decide block-range vs corpus via the existing `unicode_range` field (block-range)
  or the corpus path (no `unicode_range`).

### 4. RTL layout (the big one)
With `LV_USE_BIDI` on, set the **base direction** to RTL for Farsi screens — per-label
`lv_obj_set_style_base_dir(obj, LV_BASE_DIR_RTL, 0)` or on the screen root. Then audit
each screen in `components/seedsigner/` for mirroring:
- `top_nav` (components.cpp): back-chevron and power button swap sides; title
  alignment flips. The scrolling-title marquee direction.
- Button lists / `button_list_screen`: text alignment (right), any leading icons.
- `large_icon_status_screen`, passphrase keyboard (the keyboard stays **LTR/ASCII** —
  passphrase input is not translated; only the chrome/labels flip).
- Right-align body text; check padding/edge constants that assume LTR.
This is invasive and the main risk/effort. Consider a single "is this locale RTL?"
switch feeding alignment + base_dir, rather than per-call flips.

### 5. Enroll the locale + verify
- Add `fa` to `tools/i18n/supported_locales.json` (`pack_locales` too). Display label
  "Persian (فارسی)" — note the native name itself is RTL; the picker `<select>` will
  show it fine.
- Regenerate localized scenarios (`gen_localized_scenarios.py` — the `fa` `.po` exists
  in the `seedsigner-translations` submodule) and the font pack.
- Render `fa` in the screenshot gallery (`gen_gallery.py`) and the web runner; verify:
  letters **connect** (shaping), the paragraph is **right-to-left**, embedded Latin/
  digits stay LTR (bidi), and nothing boxes.

## Open questions / risks for the next session
- **Exact presentation-form ranges** the shaper emits for the Farsi corpus — verify
  against `third_party/lvgl/src/misc/lv_text_ap.c` (`ap_chars_map`) and the actual
  `fa` `.po`, rather than trusting the ranges above. Persian adds پ چ ژ گ ک ی ه gaps
  vs Arabic.
- **RTL layout depth** — how much screen mirroring is "enough"? Text alignment + nav
  button sides is the minimum; full geometric mirroring is more.
- **tiny_ttf × bidi × shaper end-to-end** — confirm the pipeline order (bidi reorders
  → AP shaper substitutes presentation forms → tiny_ttf rasterizes by code point)
  actually holds in LVGL v9.5 with `cache_size=0`. Smoke-test early.
- **On-device glyph cache** needs memory provisioning before it can be enabled (it OOMs on a small
  fixed pool — the former "bug #3", not a cache defect); desktop `cache_size=0` is fine for verification.
  See `docs/knowledge/tiny-ttf-cache-spin-root-cause.md`.
- Same `fa` work template applies to the other Phase-2 corpus scripts: **Thai** (`th`,
  no shaping but needs Noto Thai + the combining marks) and **Hindi/Devanagari**
  (`hi`, needs real GSUB shaping → likely HarfBuzz, a bigger lift than Arabic).

## Pointers
- Shaper: `third_party/lvgl/src/misc/lv_text_ap.{c,h}` (`_lv_text_ap_proc`, `ap_chars_map`).
- Font seam: `components/seedsigner/font_registry.cpp`, `locale_fonts.{h,cpp}`,
  `gui_constants.cpp` (`install_western_baseline` / `set_display`).
- Pack tooling: `tools/i18n/build_fontpacks.py` (corpus + block-range modes).
- Vendored font: `components/seedsigner/assets/NotoSansAR-Regular.ttf` (has the
  presentation forms).
