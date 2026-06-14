# Font tiering redesign — baked Western core (TTF) + script packs + language packs

_Status: **design locked, not started.** Captured before context drift. Plan doc (not knowledge);
delete once implemented. Companion to `docs/font-and-i18n-rendering.md`._

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
  pre-warm (post bug #3) warms only the **active locale's used corpus** (derived at runtime), not the
  block. The rasterize-all **validation/signing gate runs once per font *version***, not per translation.
- **Bug #3 (tiny_ttf cache spin) graduates from CJK-only to a *general* production prerequisite** — every
  screen now rasterizes body/title. `cache_size=0` is fine for the static desktop tools but the **device
  needs a working glyph cache** for redraw/scroll. (See the upstream bug #2/#3 plan.)

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
4. **Device path (later, gated on bug #3).** Compile-in the OpenSans Western TTF for the `raspi-lvgl` and
   `micropython-builder` firmware builds; wire pack delivery (SD load + verify) for script + language
   packs; fix/patch the tiny_ttf cache (bug #3) before relying on the device cache.

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

## TODOs / follow-ups

- **Spike done (2026-06-14):** `screenshot_gen --baseline-ttf` repoints the 5 roles to OpenSans tiny_ttf
  (Regular body / SemiBold buttons+titles), keyboard + icons stay baked. Spanish accents render; English
  unchanged. Tool-local flag, loads full OpenSans from assets (subset/compile-in still to do).
- **TTF-vs-baked metric parity (line-height/advance).** The TTF body font wraps/spaces a hair differently
  than the baked bitmaps — on `large_icon_status` at 240 it pushed the "OK" button below the fold.
  Reconcile tiny_ttf line metrics with the baked sizes so it's drop-in before replacing the baked floor.
- **`large_icon_status` spacing (PRE-EXISTING, unrelated to fonts).** The status screen has its own
  layout/spacing issues that predate this work and need to be sorted out separately.

## Dependencies / cross-refs

- Builds on the `tools/` reorg (done) and the bug #2 fallback fix (done; `third_party/patches/`).
- **Bug #3 is now a general prerequisite** for the device — see `~/.claude/plans/` upstream bug #2/#3 plan.
- Design rationale: `docs/font-and-i18n-rendering.md`.
