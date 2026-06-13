# Font & Multi-Language Rendering (Display Layer)

_Status: Phase 1 implemented (CJK zh/ja/ko proven end-to-end via the desktop screenshot generator);
Phase 2 (shaping-complex scripts) deferred. This documents how the LVGL display layer renders fonts
and multi-language text and the offline tooling that produces the font assets. Display-layer companion
to the system-level vision in `seedsigner/docs/architecture/dual-platform-overview.md`. For the
debugging story behind the engine choice, see [knowledge/font-loading-binfont-vs-tiny-ttf.md](knowledge/font-loading-binfont-vs-tiny-ttf.md)._

## Scope & Principle

This repo is the **render** layer: given font bytes + a screen config + already-translated strings, it
draws pixels. It is deliberately **ignorant of where assets come from and whether they are trusted** —
it never touches the SD card and never runs cryptography. Acquisition (SD I/O) and trust (signature
verification) live in the platform layers; locale *selection* lives in the Python C&C layer. But the
render layer **owns the locale→{font, size, chain} mapping and the glyph-set extraction** — i.e. "what
does this locale need, at what size, and how do the glyphs get drawn." (This sharpens the four-verb
model: selection = Decide; resolution + rendering = Render.)

The motivating problem: shipping pre-rasterized fonts compiled into the firmware does not scale to many
languages × resolutions. A full CJK TTF is multiple MB; baking it as bitmaps at several sizes bloats
every binary, even for languages a user never selects. So non-Latin fonts move **out of the binary and
onto the SD card** as language packs, loaded on demand — binary size stays independent of language count.

## Ownership: the canonical locale table

`components/seedsigner/locale_fonts.{h,cpp}` is the **single source of truth** for which additional
(non-baked) fonts a locale needs, at what per-role sizes, and how they chain with the baked-in floor.
`supported_locales()` serializes it to a JSON manifest for the active display profile — the **only**
outward interface. The host fetches this once at startup, reconciles it against the fonts present in its
store (and the available `.mo` catalogs), and offers the satisfied locales. A locale absent from the
manifest is fully covered by the baked floor (no pack needed).

Python's `gui/components.py` font/size tables are slated to be removed once this owns it; the view layer
will report only the active locale.

## The Registration Seam

`components/seedsigner/font_registry.{h,cpp}`:

```c
void seedsigner_set_locale(const char *locale);                  // looks up the canonical table
bool seedsigner_register_font(const char *logical_name,          // role: "body"/"button"/"top_nav_title"/...
                              const uint8_t *buf, size_t len, int font_px_size);
void seedsigner_clear_registered_fonts();                        // restore compiled-in fonts
```

The platform loads + verifies a pack and registers each font under its **role name** before rendering.
The seam turns the bytes into an `lv_font_t` and repoints the active profile's text-font pointer (the
profiles are non-const; `active_profile_mutable()` exists for this). **The font buffer must outlive the
font** (Tiny TTF reads outlines lazily) — the host frees it only after `clear`.

## Storage & Engine Model

Fonts live on the SD card as signed **language packs**, not in the binary. The engine is **Tiny TTF**
(`LV_USE_TINY_TTF`, `lv_tiny_ttf_create_data`): one subset `.ttf` per locale, loaded from a buffer,
rasterized on demand. A single `.ttf` serves **every size and every resolution** (create at any px), so
packs are resolution-independent and small (a CJK subset is ~100KB; the full source TTF is multiple MB).

> **Why not pre-baked `.bin`?** That was the original plan (no runtime rasterizer, smallest attack
> surface). It does not work with our toolchain: `lv_font_conv` 1.5.3 output hangs LVGL 9.5.0's
> `lv_binfont_loader` for any non-trivial CJK font, via both memfs and file loads. See the knowledge
> doc. Tiny TTF is the design's documented fallback engine and is what ships. Trade-off: the stb
> rasterizer is compiled in (attack surface) — contained by verify-before-parse + an offline
> rasterize-all validation gate (below).

### Closed corpus

All user *input* is ASCII (BIP-39 wordlist + passphrase). Non-Latin glyphs appear **only** in UI labels
from the translation catalog, so the complete glyph set is enumerable at build time. (QR-scanned native
labels are out of scope — they degrade to the placeholder.) This is what makes subsetting safe and the
rasterize-all validation tractable: we never face an unanticipated glyph at runtime.

## Chain model: dominant script is PRIMARY

The baked floor (OpenSans + Inconsolata + SeedSigner icons, ASCII only) is always present. Per locale:

- **CJK (`ChainRole::Primary`)** — the script font is the **primary** for each text role at a per-role
  legibility-bumped size (base 240: body 18 / button 20 / large-button & title 23 / main-menu 26;
  scaled by the profile multiplier). The baked OpenSans is its `.fallback`. Line metrics come from the
  (taller) script primary — this is *why* it's primary, so the bumped CJK isn't clipped by a smaller
  primary's line box. Matches the production Python per-locale size bumps.
- **Same-size scripts** (Greek/Cyrillic/Vietnamese; Phase 2+) would chain as a *fallback* under the
  OpenSans primary (no size bump → no metric issue). Not yet in the table.

**Embedded English in a CJK screen** (technical terms that aren't catalog msgids) currently renders from
the **CJK font's own Latin glyphs at the bumped size**. The cleaner "English at the English size via the
OpenSans fallback" is blocked by a Tiny TTF bug (its no-cache path reports absent codepoints as *found*,
so the fallback never engages — see knowledge doc); until that's patched upstream, the build tool
**includes ASCII in primary-script subsets** so embedded English renders (in Noto Latin), matching Python.

### Glyph cache

Tiny TTF has an LRU glyph cache. It currently spins on certain content at any cache size (LVGL bug — see
knowledge doc), so fonts are created with `cache_size=0` (rasterize-direct) for now. That's correct for
the static screenshot tool but slow for the interactive device; restoring a working cache (or patching
the spin) is a production follow-up. Target hardware has 8–16 MB PSRAM, so caching/pre-warming the closed
corpus is viable once the cache is usable.

## Multi-Script Shaping (Phase 2)

Shaping and bidi are **string-level transforms inside LVGL**, applied at set-text time, independent of the
engine. Config to enable: `LV_USE_BIDI = 1`, `LV_BIDI_BASE_DIR_DEF = LV_BASE_DIR_AUTO`,
`LV_USE_ARABIC_PERSIAN_CHARS = 1`.

- **Hebrew** — RTL, non-cursive. Needs only `LV_USE_BIDI` + correct `base_dir`.
- **Arabic / Persian** — RTL + cursive joining. `LV_USE_ARABIC_PERSIAN_CHARS` substitutes each logical
  letter with its **presentation form** (U+FB50–FDFF, U+FE70–FEFF) before glyph lookup. The vendored
  `NotoSansAR-Regular.ttf` cmap already covers those forms (631 + 141 glyphs) and all Farsi letters, so
  Tiny TTF resolves them by codepoint. stb ignores GSUB/GPOS, so anything beyond LVGL's substitution
  (e.g. GPOS mark positioning) isn't rendered — same ceiling any engine has here.
- **Thai** — LTR Brahmic; LVGL doesn't shape it. Offline mitigation: inject U+200B at word boundaries,
  use a zero-advance-mark font.

### The Arabic presentation-form gotcha (subsetting)

Because LVGL renders the *substituted presentation forms*, an Arabic/Persian subset must retain those
forms (not just base U+06xx) plus lam-alef ligatures. So `pyftsubset` for Arabic locales must KEEP the
presentation-form ranges and layout tables (unlike CJK, where we drop them).

## Pack-Generation Tooling (Produce)

Owned here, in `tools/fontpack/` (see its README). c-modules owns the corpus extraction too — it no
longer depends on the main app's extractor. Pipeline per locale:

1. **Corpus** — `po_catalog.py` parses the locale's `.po` directly (no Babel) → unique non-ASCII glyphs
   (ASCII added back for primary-script locales, per the chain note above).
2. **Subset** — `build_lang_font.py` reads the render layer's `--dump-locales` manifest for the source
   family, then `fontTools.subset` → one `.ttf` per locale (CJK: drop GSUB/GPOS/GDEF; Arabic: keep them).
3. **Bundle & sign** — left to the platform layer (the tool emits a per-locale `manifest.json` with
   sha256). Signing planned: Schnorr/secp256k1.

Phase-2 follow-on: an LVGL-shaper-driven exact presentation-form extractor (`lv_text_ap.c`) instead of
keeping the full Arabic presentation blocks.

## Trust Expectation

The display layer assumes **it only ever receives verified bytes** — verification happens in the platform
layer *before* any byte reaches `lv_tiny_ttf_create_data` (and before gettext loads the `.mo`). To contain
the stb rasterizer's attack surface, the pack-signing process runs an offline **rasterize-all validation
gate**: render every glyph in the (closed) corpus at every profile size through the same stb, under
ASan/UBSan, and — because stb behavior can diverge per target — **compiled and run for each production
architecture** (ESP32-S3, future ESP targets, Pi Zero), on-target or via QEMU where ASan isn't available.
A pack isn't signable until it passes. Combined with verify-before-parse, the device only ever rasterizes
a pre-proven, signed corpus.

### Placeholder for out-of-corpus glyphs

`LV_USE_FONT_PLACEHOLDER` draws a box for out-of-corpus codepoints rather than faulting. (Note: the
current Tiny TTF no-cache fallback bug means truly-absent glyphs render blank rather than reaching the
placeholder; revisit when that's patched.)

## Cross-References

- System vision & layer contracts: `seedsigner/docs/architecture/dual-platform-overview.md`
- As-built status & remaining work: [font-and-i18n-implementation-plan.md](font-and-i18n-implementation-plan.md)
- Debugging story (engine choice, tiny_ttf bugs): [knowledge/font-loading-binfont-vs-tiny-ttf.md](knowledge/font-loading-binfont-vs-tiny-ttf.md)
- Font-pack tooling: [../tools/fontpack/README.md](../tools/fontpack/README.md)
