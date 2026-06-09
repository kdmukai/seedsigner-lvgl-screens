# Font & Multi-Language Rendering (Display Layer)

_Status: design / planned. This documents how the LVGL display layer renders fonts and multi-language text, and the offline tooling that produces the font assets. It is the display-layer companion to the system-level vision in `seedsigner/docs/architecture/dual-platform-overview.md`. Most of what follows is not yet implemented; it records the agreed design and its rationale._

## Scope & Principle

This repo is the **render** layer: given a buffer of font bytes + a screen config + already-translated strings, it draws pixels. It is deliberately **ignorant of where assets come from and whether they are trusted** — it never touches the SD card and never runs cryptography. Acquisition (SD I/O) and trust (signature verification) live in the platform layers; policy (which language, which strings) lives in the Python C&C layer. See the four-verb model in the architecture overview.

The motivating problem: shipping pre-rasterized fonts compiled into the firmware does not scale to many languages × multiple resolution profiles. A single CJK TTF is ~8 MB; baking it as bitmaps at several sizes multiplies that and bloats every binary, even for languages a given user never selects. The design below moves fonts **out of the binary and onto the SD card**, loaded on demand, so binary size is independent of language count.

## The Registration Seam

Fonts enter the display layer through one interface (planned):

```c
// Receives already-verified bytes. Never opens files, never verifies signatures.
seedsigner_register_font(const char *logical_name, const uint8_t *buf, size_t len, /* size, fallback, ... */);
```

Screens and display profiles reference fonts by **logical name** (`"body"`, `"title"`, `"hero_icon"`), never by pointer or path. The platform layer loads + verifies a pack and registers each font under its logical name before rendering. This is what keeps the layer ignorant: it gets a buffer and a name, nothing else.

## Storage & Engine Model

Fonts live on the SD card as signed **language packs**, not in the binary. Two viable engines, both supported by LVGL v9.5.0:

| | Pre-baked `.bin` (recommended) | Tiny TTF (`LV_USE_TINY_TTF`) |
|---|---|---|
| On-board code | none — pure load via `lv_binfont_create_from_buffer` | rasterizer compiled in |
| Source file | one `.bin` per size | one `.ttf`, any size |
| First-use cost | none (already bitmaps) | rasterize-then-cache |
| Parser attack surface | small, simple format | full TTF rasterizer (stb) |
| SD cost | larger (bitmaps × sizes) | smallest |

**Recommendation: pre-baked `.bin`.** Because our corpus is *closed and known at build time* (see below), we can pre-rasterize exactly the needed glyphs at exactly the profile's sizes offline. That removes the need to carry a rasterizer at all (smaller, more auditable — important on a signing device), eliminates first-use latency, and gives pixel-tuned quality. Tiny TTF remains the fallback if resolution profiles proliferate enough that per-size bakes become unwieldy.

Either way, fonts on SD keep the **binary size flat** regardless of how many languages exist, and a device carries only the packs for **its** resolution profile.

### Closed corpus

All user *input* is ASCII (BIP-39 English wordlist + passphrase). Non-Latin scripts therefore appear **only** in UI labels sourced from the translation catalog. The complete set of glyphs that can legitimately render is thus enumerable at build time. (QR-scanned native-language labels are explicitly out of scope for correct rendering and must degrade gracefully — see Placeholder below.) This is what makes pre-baking viable: we never face an unanticipated glyph at runtime.

The relevant unit is the **unique glyph set, not the literal strings** — dedupe to codepoints across the whole catalog. For Arabic/Persian the "unique glyph" is defined in presentation-form space, not logical codepoints (see Shaping).

## Fallback, Font Manager, Glyph Cache

- **Fallback chains** (`lv_font_t.fallback`, chainable; walked in `lv_font_get_glyph_dsc`): keep a crisp, hand-tuned **Latin + icon** font as primary and chain script fonts (Hebrew, Arabic, CJK, …) as fallbacks. English stays sharp and costs nothing; the script font engages only when its codepoints appear.
- **Font Manager** (`LV_USE_FONT_MANAGER`): reference-counts fonts (no duplicate loads), concatenates fallbacks, and recycles recently-deleted fonts for fast language switching. This is the lifecycle layer for "load the active language, unload on switch."
- **Glyph cache + PSRAM**: target hardware has 8–16 MB PSRAM. Load the whole font file into PSRAM and, on language switch, **pre-warm** the cache by rasterizing/loading every glyph in the active language's (closed) corpus once. Vector-engine first-use latency becomes a bounded one-time cost; with pre-baked `.bin` there's no rasterization at all. Note PSRAM is slower than internal SRAM — fine for a static wallet UI, but the active glyph working set is the first thing to look at if a redraw is ever sluggish.

## Multi-Script Shaping

Shaping and bidi are **string-level transforms inside LVGL**, applied at set-text time and independent of which engine draws the glyph. Config (to be enabled): `LV_USE_BIDI = 1`, `LV_BIDI_BASE_DIR_DEF = LV_BASE_DIR_AUTO`, `LV_USE_ARABIC_PERSIAN_CHARS = 1`.

The four target scripts are three different problems:

- **Hebrew** — RTL, **non-cursive** (no joining). Needs only `LV_USE_BIDI` + an Hebrew font + correct `base_dir`. Fully handled.
- **Arabic / Persian** — RTL **+ cursive joining**. `LV_USE_ARABIC_PERSIAN_CHARS` substitutes each logical letter with its contextual **presentation form** before glyph lookup. Documented LVGL limitations: only *display* is shaped (Text Areas are **not** — irrelevant for us since input is ASCII); and **static text is not processed**, so shaped strings must use the allocating label setter, never `_static`.
- **Thai** — **LTR**, but a complex Brahmic script LVGL does not shape: combining marks need mark-to-base positioning LVGL doesn't do, and there are no inter-word spaces for line breaking. Mitigations are **offline** (see tooling): inject U+200B at word boundaries via a segmenter, and use a font whose combining marks have **zero advance width** so they stack visually without GPOS.

### The Arabic presentation-form gotcha (critical for subsetting)

Because LVGL renders the *substituted presentation forms*, the font subset must contain those forms (U+FB50–FDFF, U+FE70–FEFF), not just the base U+06xx block — plus mandatory ligatures (lam-alef) that are single glyphs corresponding to no single input codepoint. **Deriving the glyph set from logical input codepoints is insufficient for Arabic/Persian.**

This directly affects the existing extraction tool (next section): it operates on logical codepoints and therefore does **not** cover the Arabic conjoining case on its own.

## Pack-Generation Tooling (Produce)

The offline pipeline that produces signed packs lives in this repo's `tools/` (alongside `scripts/png_to_lvgl.py`, which already generates LVGL image assets). It is platform-agnostic — the same `.bin` works on ESP and Pi at a given resolution — so it must **not** live in a platform-specific builder.

Pipeline, per (language × profile):

1. **Corpus extraction.** Start from `seedsigner`'s `resources/seedsigner-translations/tools/extract_characters_from_babel_mo.py`, which already extracts the unique character set per locale from the compiled `.mo` (translation chars ∪ basic ASCII). **This gives logical codepoints only.**
2. **Presentation-form expansion (Arabic/Persian).** For Arabic-script locales the logical codepoints from step 1 are **not** what LVGL renders — `LV_USE_ARABIC_PERSIAN_CHARS` substitutes contextual presentation forms (U+FB50–FDFF, U+FE70–FEFF) before glyph lookup, plus the lam-alef ligatures. Without expanding to those, the subset is missing the glyphs that actually draw. **This is the gap the existing extraction tool does not cover** (it collects logical codepoints only).

   **Chosen approach: over-render the superset** (decided 2026-06-08). For each Arabic/Persian base letter present in the corpus, include **all four** positional forms (isolated/initial/medial/final), plus the lam-alef ligature forms whenever both components are present. This is conservative — it bakes a few forms that never occur in context — but on hardware that is not storage/memory-constrained the cost is trivial, and a superset **cannot under-include**, so it can never produce a missing-glyph box. The form set can be derived with stdlib `unicodedata` alone (read the compatibility decompositions of the presentation-form blocks to map base → forms); no joining algorithm or external data is needed.

   > **TODO:** Implement this expansion in `seedsigner/src/seedsigner/resources/seedsigner-translations/tools/extract_characters_from_babel_mo.py` (e.g. behind an Arabic/Persian-locale path or `--expand-arabic-forms` flag), so the tool emits the superset of presentation forms for those locales rather than bare logical codepoints. Until then, Arabic/Persian packs built from its output will render missing-glyph boxes.

   _Deferred alternative — exact set:_ if minimizing baked glyphs ever matters, the only *provably* LVGL-exact route is to run the corpus through LVGL's own shaper (`src/misc/lv_text_ap.c`) via a small C harness and collect the output codepoints — not a Python reimplementation, since under-inclusion there means a blank glyph. Not worth the extra machinery while storage is plentiful.
3. **Thai prep.** Inject U+200B at word boundaries (offline segmenter) into the catalog; select/validate a zero-advance-mark Thai font.
4. **Subset & rasterize.** Subset the source TTF to the resulting glyph set; rasterize to `.bin` at each of the profile's font sizes (`lv_font_conv --format bin`, with the appropriate `--range`/`--symbols`).
5. **Bundle & sign.** Bundle the `.bin` set with the resolution-independent `.mo` catalog into a language pack; sign it (planned: Schnorr/secp256k1).

## Trust Expectation (what this layer assumes)

The display layer assumes **it only ever receives verified bytes**. Verification is the platform layer's job and must happen *before* any byte reaches `lv_binfont_create_from_buffer` (verify-before-parse), and before gettext loads the `.mo`. The pre-baked `.bin` choice helps here too: a small, simple format behind the verification gate is far easier to audit than a full TTF rasterizer consuming attacker-influenceable input.

### Placeholder for out-of-corpus glyphs

Because QR-scanned native labels can contain codepoints outside the baked set, enable `LV_USE_FONT_PLACEHOLDER` so missing glyphs render a visible box rather than faulting. Graceful degradation, not a crash.

## Cross-References

- System vision & layer contracts: `seedsigner/docs/architecture/dual-platform-overview.md`
- Corpus extractor: `seedsigner/src/seedsigner/resources/seedsigner-translations/tools/extract_characters_from_babel_mo.py`
- LVGL font docs (vendored): `third_party/lvgl/docs/src/main-modules/fonts/` (`overview.rst`, `binfont_loader.rst`, `rtl.rst`, `font_manager.rst`, `../../libs/font_support/tiny_ttf.rst`)
