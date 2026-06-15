# Font tiering redesign — baked Western core (TTF) + script packs + language packs

_Status: **Stages A + B implemented** (see the "DONE" sections below); remaining tiers (CJK was already
done pre-plan; Arabic/Persian/Thai/Hindi) are Phase 2, and the device path is later. Captured before
context drift. Plan doc (not knowledge); delete once fully implemented. Companion to
`docs/font-and-i18n-rendering.md`._

## Context / why

Spanish (and every accented-Latin / Greek / Cyrillic) UI label renders á/ñ/¿/¡ as `.notdef` boxes. Root
cause: the baked OpenSans fonts are compiled-in **`.c` LVGL bitmap arrays clamped to ASCII**
(`range_start=32, range_length=95` → U+0020–U+007E). They are *not* `.bin` (that was the abandoned CJK
path) and *not* TTF. The accents are in the OpenSans TTF the whole time — they were just never baked.

Two realizations reshape the fix into an architecture change:

1. **The real dividing line is "does this font render *translated* text?"** — not ASCII-vs-accents.
   - `keyboard_font` (fixed-width Inconsolata: keys, text-entry box, ABC/123) + the `icon_*` fonts are
     **never translated** (passphrase/keyboard input is ASCII; icons are PUA). They also carry the
     *user-controlled / security-sensitive* text. → keep them **baked**, off the rasterizer.
   - The five `TextRole`s (`body`, `button`, `large_button`, `top_nav_title`, `main_menu_title`) render
     **translated labels**. → these go **TTF** (for English too, uniformly).
2. **Firmware rebuilds must be avoided** (high priority). A corpus-subset baked font re-couples
   translation edits to firmware (add a glyph → reflash). So **anything baked must be
   corpus-*independent*** (fixed Unicode blocks); everything that tracks translations ships as a signed
   **pack**.

## Target: three tiers

| tier | contents | subsetting | updatable w/o firmware rebuild |
|---|---|---|---|
| **Firmware-baked (frozen)** | OpenSans **Western Latin** (full **Latin-1 Supplement + Latin Extended-A** blocks, Regular + SemiBold, compiled-in **TTF** for the 5 roles) · **Inconsolata** (mono/keyboard, baked `.c`) · **icons** (baked `.c`) | **block range** (not corpus) | n/a — frozen; never changes for translations |
| **Script packs** (full-block, shared, same-size `ChainRole::Fallback` over the baked OpenSans) | **Cyrillic** (≈256) · **Greek** (≈121) · **Vietnamese** / Latin Ext Additional (≈256) — OpenSans subsets, R+SB | **block range** (not corpus) | yes — and rarely even need a *new* pack |
| **Language packs** (per-language) | **CJK** (Noto SC/JP/KR, `Primary`, bumped) · **Arabic/Persian** · **Thai** · **Hindi** (Noto) | **corpus** (full block too big to ship) | yes — translation edits ship a new signed pack |

Western-Latin / Greek / Cyrillic / Vietnamese languages remain **absent from the manifest** (floor- or
script-pack-covered), exactly like English today. The only corpus-coupled tier is the big Noto scripts —
and there the coupling is to a *pack*, never to firmware.

## Key principles (the "why it's safe / cheap")

- **Rasterizer only ever sees translated, trusted, closed-corpus label text.** User input (BIP-39 words,
  passphrase keystrokes) and security-sensitive display (seed words, addresses, fingerprints) stay on the
  **baked Inconsolata** and never touch stb. **Invariant to preserve as screens are built:** anything
  user/attacker-influenceable renders in the baked mono font, not the TTF body/title.
- **Block-range, not corpus, for the small tiers** → hand `pyftsubset` a `--unicodes=U+0400-04FF`-style
  range, never read the `.po`. Frozen glyph repertoire; a translation edit can't change the baked core or
  a script pack (or its signature). Corpus extraction (`corpus_chars`, reading catalogs) runs **only** for
  the Noto language packs.
- **Unused block glyphs cost ~nothing.** tiny_ttf rasterizes lazily, so unused glyphs are never
  rasterized, never cached, never pre-warmed — only a few KB of outline bytes on the SD card. Cache
  pre-warm (once the on-device cache is enabled) warms only the **active locale's used corpus** (derived
  at runtime), not the block. The rasterize-all **validation/signing gate runs once per font *version***,
  not per translation.
- **The glyph cache is on by default; backing it with memory is a host task, not a code fix** — every
  screen rasterizes body/title and the cache holds those bitmaps (`SEEDSIGNER_TTF_CACHE_SIZE=256`). Each
  target must give LVGL adequate memory (Pi Zero: CLIB malloc; ESP32-S3: glyph bitmaps in PSRAM),
  otherwise it OOMs and halts. See `docs/knowledge/tiny-ttf-cache-spin-root-cause.md`.

## Implementation order

1. **Spike on the screenshot tool (lowest risk, proves the direction).**
   - Generate an OpenSans **Western-block** subset TTF (Latin-1 + Latin Extended-A range), Regular +
     SemiBold, and compile it into `apps/screenshot_generator` (replace the baked OpenSans `.c` for the 5
     roles; **keep Inconsolata + icons baked**). The five `DisplayProfile` role pointers default to the
     compiled-in OpenSans TTF (`cache_size=0` is fine here).
   - Render **English + Spanish**; confirm: accents (á/ñ/¿/¡) render, the keyboard still draws from the
     baked Inconsolata, and English is visually unchanged (modulo bitmap→rasterized AA).
2. **Script packs.** Teach `build_fontpacks` a **block-range mode** (subset by `--unicodes` range, skip
   `.po`). Produce Cyrillic / Greek / Vietnamese OpenSans subsets. Add them to `locale_fonts` as
   `ChainRole::Fallback` (same-size, OpenSans family). Render el / ru / vi.
3. **Floor-vs-pack routing (optional automation).** `build_fontpacks` already reads each `.po` and has
   OpenSans's cmap — it can compute "is this locale's corpus ⊆ OpenSans?" to route floor vs pack and
   *validate* the hand-written `locale_fonts` table.
4. **Device path (later).** Compile-in the OpenSans Western TTF for the `raspi-lvgl` and
   `micropython-builder` firmware builds; wire pack delivery (SD load + verify) for script + language
   packs. Before enabling the on-device glyph cache, provision LVGL memory (Pi Zero: CLIB malloc;
   ESP32-S3: glyph bitmaps in PSRAM) per `docs/knowledge/tiny-ttf-cache-spin-root-cause.md` — no
   `lv_tiny_ttf` patch is required.

## Files (anticipated)

- `components/seedsigner/gui_constants.{h,cpp}` — the 5 role fonts default to the compiled-in OpenSans
  TTF; keyboard/icon fonts unchanged (baked).
- `components/seedsigner/font_registry.{h,cpp}` — already repoints the 5 roles for CJK; extend so the
  *baseline* is the compiled-in TTF.
- `components/seedsigner/locale_fonts.{h,cpp}` — add Cyrillic/Greek/Vietnamese `Fallback` entries; Noto
  language entries as today.
- `tools/i18n/build_fontpacks.py` — add a **block-range** mode (vs the existing corpus mode for Noto).
- `tools/apps/screenshot_generator/CMakeLists.txt` — stop compiling baked OpenSans for the 5 roles;
  compile in the OpenSans Western TTF subset bytes. Keep Inconsolata + icon `.c`.
- A generation step for the compiled-in OpenSans Western TTF subset (block range → embedded bytes).

## Verification

- **Spike:** Spanish accents render correctly; the keyboard/text-entry still uses baked Inconsolata;
  English unchanged. CJK still renders (chain unchanged, OpenSans fallback now TTF).
- **Script packs:** el/ru/vi render via the same-size fallback packs; adding a second Cyrillic language
  needs no new pack.
- **No-rebuild proof:** a translation edit that introduces a new (already-in-block) glyph requires no
  firmware change and no new pack for the baked/script tiers.

## Stage A implementation spec — DONE (2026-06-14)

**Landed:** the five translated text roles now render from the compiled-in OpenSans Western TTF
(`opensans_western_{regular,semibold}.c`), rasterized at runtime via `lv_tiny_ttf`; keyboard
(Inconsolata) + icons stay baked. `fonts_for_multiplier` leaves the five slots null; `gui_constants.cpp
set_display()` installs the per-role baseline the first time it runs after `lv_init()` (per-profile lazy
cache, never destroyed; sizes hardcoded incl. the large_button 20@240 / 18@320·480 quirk; body=Regular,
other four=SemiBold). The baked `opensans_*` `.c` files are deleted and dropped from all 5 CMakeLists
(4 apps + the ESP32 `components/seedsigner` component). The generator's `--baseline-ttf` spike is removed.

**Two deviations from the "no per-consumer edits" assumption** (note for Stage B / device path):
1. **`tools/apps/runner_core/runner_core.cpp init()`** had to reorder `lv_init()` *before* `set_display()`
   — the shared runner core called `set_display` only pre-`lv_init`, so the install (gated on
   `lv_is_initialized()`) never fired. One shared edit covers all three runner apps; the apps themselves
   are unchanged.
2. **`LV_USE_TINY_TTF=1` + `LV_USE_STDLIB_MALLOC=LV_STDLIB_CLIB`** added to screen_runner, web_runner, and
   runner_core/test (previously only the generator had them) — the baseline now always rasterizes, so
   every runner needs tiny_ttf and the real allocator.

**Verified (generator @ 240/320/480):** es renders accented Latin (`Menú`) via the baseline; passphrase
keyboard stays baked Inconsolata; el boxes correctly (no Greek pack yet — faithful); zh_Hans_CN still
renders (CJK Primary chain intact, fallback is now the OpenSans TTF). `screen_runner` builds; the
`runner_core` test passes incl. render-after-resize (proves per-profile install across resolution switch).

### Original spec (for reference)

**Foundation done (uncommitted):** `build_fontpacks`-style block subsets generated — Western (383
glyphs), Greek (162), Cyrillic (303), Vietnamese (468), Regular+SemiBold each. The **Western baseline is
bin2c'd** into `components/seedsigner/fonts/opensans_western_{regular,semibold}.{c,h}` (~27.6KB TTF →
~88KB C array each), exposing `opensans_western_{regular,semibold}_ttf[]` + `_len`.

**Wiring (remaining) — install via `set_display`, no per-consumer edits:**
- `set_display()` (gui_constants.cpp) installs the baseline when `lv_is_initialized()` — every consumer
  already calls it, so generator/runners/firmware need no changes. Pre-`lv_init` calls (generator's
  usage/`--dump-locales`) skip install.
- **Per-profile font cache, created lazily, NEVER destroyed** (4 profiles × 5 roles = 20 tiny_ttf, tiny
  structs, embedded bytes shared). Stable pointers → safe with the locale `clear`/restore (which captures
  the baseline pointer as `original`), and CJK's embedded-English fallback automatically becomes the
  OpenSans **TTF** baseline (aligns the plan) instead of the baked floor.
- `fonts_for_multiplier`: the 5 OpenSans slots → `nullptr` (icon/keyboard stay baked). Static `DisplayProfile`s
  start with null text-role pointers; `set_display` fills them post-`lv_init` before any render.
- **Per-role px per profile — replicate the baked sizes exactly (parity):** main_menu_title `26·m`,
  title `20·m`, button `18·m`, body `17·m`; **large_button = `20·m` at 240 but `18·m` at 320/480** (the
  quirk). Weights: body = Regular, the other four = SemiBold. (Hardcode in gui_constants — don't `#include`
  locale_fonts to avoid a cycle.)
- Remove baked `opensans_*_4bpp*.c` from gui_constants + all 4 CMakeLists; add `opensans_western_*.c`;
  delete the baked files. Remove the generator's `--baseline-ttf` spike (superseded by the always-on
  install).
- **Verify:** generator renders en/es via the baseline; el/ru/vi correctly BOX (no packs yet — faithful);
  `screen_runner` builds (web_runner code updated, Docker build deferred).

## Stage B — DONE (2026-06-14)

Greek/Cyrillic/Vietnamese now render as same-size `ChainRole::Fallback` script packs over the baked
Western baseline. Landed:

- **`build_fontpacks.py` block-range mode** — when a manifest locale carries a `unicode_range`, subset
  OpenSans by that fixed block (NOT the `.po` corpus), two weights (Regular for body, SemiBold for the
  other four roles) → `lang-packs/<loc>/<loc>_{regular,semibold}.ttf` + manifest. Corpus mode (Noto/CJK)
  is unchanged. Packs stay gitignored / regenerable (same as the CJK packs).
- **`locale_fonts`** — `el`/`ru`/`vi` entries (`ChainRole::Fallback`, source OpenSans, ranges
  `U+0370-03FF` / `U+0400-04FF` / `U+0300-036F,U+1E00-1EFF`). New `LocaleFontEntry.unicode_range`; the
  manifest emits `unicode_range` + a per-role `weight` and weight-split file names.
- **Size source of truth** — `locale_role_render_px()` (shared by the manifest and the registration
  guard) returns the baseline's exact px per role, replicating the large_button quirk (20 at 240, 18 at
  320/480) so the script glyphs match the Latin baseline. `font_registry` validates against it.
- **Verified:** el/ru/vi render (Greek Αρχική/Σάρωση, etc.) via the fallback packs; the chain relies on
  the bug-#2 tiny_ttf fallback patch (already applied). CJK Primary chain unchanged.

Remaining tiers (Arabic/Persian/Thai/Hindi as Noto Primary/corpus packs) are Phase 2.

## TODOs / follow-ups

- **Spike (2026-06-14) — SUPERSEDED by Stage A.** `screenshot_gen --baseline-ttf` proved the direction;
  it's now removed in favour of the always-on compiled-in Western subset install in `set_display()`.
- **TTF-vs-baked metric parity (line-height/advance).** The TTF body font wraps/spaces a hair differently
  than the baked bitmaps — on `large_icon_status` at 240 it pushed the "OK" button below the fold.
  Reconcile tiny_ttf line metrics with the baked sizes so it's drop-in before replacing the baked floor.
- **`large_icon_status` spacing (PRE-EXISTING, unrelated to fonts).** The status screen has its own
  layout/spacing issues that predate this work and need to be sorted out separately.

## Dependencies / cross-refs

- Builds on the `tools/` reorg (done) and the bug #2 fallback fix (done; `third_party/patches/`).
- **Enabling the on-device glyph cache requires memory provisioning** (Pi Zero CLIB malloc / ESP32 PSRAM),
  not a code fix — see `docs/knowledge/tiny-ttf-cache-spin-root-cause.md`.
- Design rationale: `docs/font-and-i18n-rendering.md`.
