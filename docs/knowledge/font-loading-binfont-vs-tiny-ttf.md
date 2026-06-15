# Runtime font loading: why Tiny TTF, and the LVGL bugs that shaped the design

This records why SeedSigner's i18n font support loads fonts at runtime via **Tiny TTF** rather than the
originally-planned pre-baked **`.bin`** format, and two `lv_tiny_ttf` bugs that forced specific
workarounds. All findings are against **LVGL v9.5.0** (vendored at `third_party/lvgl/`) and
**`lv_font_conv` 1.5.3** (the latest npm release at the time).

## 1. `lv_font_conv` `.bin` + LVGL `lv_binfont_loader` hang on non-trivial fonts

**Symptom.** `lv_binfont_create_from_buffer()` (and `lv_binfont_create()` from a real file) **hangs**
(CPU-spinning, not blocked) on any CJK subset font ≳48 KB. Small fonts load fine:

| px | file size | `loca_fmt` | result |
|---|---|---|---|
| 12–16 | ≤48 KB | 0 (16-bit) | loads |
| 18 | 60 KB | 0 | **hang** |
| 52 | 442 KB | 1 (32-bit) | **hang** |

**What it is NOT** (all ruled out by experiment):
- Not memfs — loading the identical `.bin` from a real file via `LV_USE_FS_STDIO` hangs identically.
- Not `lv_font_conv` flags — `--no-compress`/`--no-prefilter`/default all hang on the large font.
- Not the converter version — 1.5.3 is the latest.
- Not a header/format mismatch — the header parses perfectly (version 1, correct `font_size`, `cmap`
  at the right offset) and the counts read correctly (`cmaps_subtables_count=1`, `loca_count=392`).
  Instrumenting `lvgl_load_font()` shows it gets past the header/cmap/loca into the per-glyph loop and
  spins there.

**Conclusion.** It is a glyph-data incompatibility between `lv_font_conv` 1.5.3's `.bin` output and LVGL
9.5.0's `lv_binfont_loader.c` for fonts above ~48 KB. Both the 16-bit-`loca` region (18px) and the
32-bit region (52px) fail, so it is not a simple offset-overflow either. The exact bug was not pinned
(no debugger available in the build env), but the boundary is reproducible and the whole `.bin` path is
unusable for our CJK sizes.

**Decision.** Abandon pre-baked `.bin`. Use **Tiny TTF** (`lv_tiny_ttf_create_data`), which is the
design's documented fallback engine: subset one `.ttf` per locale with `fontTools` and rasterize on
demand. Loads and renders correctly for all our sizes. (See `docs/font-and-i18n-rendering.md` for the
engine trade-offs — chiefly the stb rasterizer's attack surface, contained by verify-before-parse + an
offline rasterize-all validation gate.)

## 2. Tiny TTF: no-cache path reports absent glyphs as "found" → breaks fallback chains *(FIXED via patch)*

In `third_party/lvgl/src/libs/tiny_ttf/lv_tiny_ttf.c`, `ttf_get_glyph_dsc_cb()`'s no-cache path
(`cache_size == 0`) calls `stbtt_FindGlyphIndex()` (which returns 0 for a codepoint absent from the
font) but then **unconditionally `return true`** — i.e. it reports the glyph as *found* (rendering an
empty `.notdef`) instead of returning `false`.

LVGL's fallback chain (`lv_font_get_glyph_dsc` walking `f->fallback`) only advances to the next font when
the current one returns `false`. So a Tiny TTF font in `cache_size=0` mode **never defers to its
fallback** — a CJK-primary font with ASCII excluded would render embedded English as blank `.notdef`
boxes instead of falling through to the baked OpenSans.

**Symptom seen:** Chinese menu title + first (translated) button rendered, but the untranslated English
buttons ("Persistent Settings", "Camera", "Network") were **blank**.

**Fix (`third_party/patches/lv_tiny_ttf-fallback-chain.patch`):** the no-cache path now `return false`
when `stbtt_FindGlyphIndex()` yields 0, matching what the *cached* path already does (its
`tiny_ttf_glyph_cache_create_cb` returns `false` for an absent glyph). The fallback chain then advances
correctly. With the fix applied, `build_fontpacks.py` **no longer bakes ASCII into the CJK subsets**, so
embedded English defers to OpenSans and renders at the **normal English size** (a deliberate divergence
from single-font Python, which has no fallback and draws embedded English at the bumped CJK size).

The patch is carried against the pinned LVGL submodule (see `third_party/patches/README.md`); push it
upstream so the patch can eventually be dropped. The earlier workaround — include ASCII in CJK subsets so
the script font carries its own Latin glyphs (English then at the bumped size) — is no longer used.

## 3. Tiny TTF: the glyph cache holds bitmaps — give it memory; it is not a cache bug

Enabling the tiny_ttf glyph/draw cache (`cache_size > 0`) is correct and effective **when LVGL has enough
memory**. The cache holds rasterized glyph bitmaps; on a constrained fixed pool (the embedded default is
`LV_MEM_SIZE = 64 KB`, `LV_STDLIB_BUILTIN`) large CJK bitmaps exhaust the heap, the next `lv_malloc`
returns NULL, and LVGL's default `LV_ASSERT_MALLOC` handler (`while(1);`) busy-loops — that is the "spin."
It is **out-of-memory, not a cache / red-black-tree defect**, and unrelated to upstream #9765. Full
analysis, evidence and a standalone repro: [`tiny-ttf-cache-spin-root-cause.md`](tiny-ttf-cache-spin-root-cause.md).

**Current state:** the cache is **enabled by default** (`SEEDSIGNER_TTF_CACHE_SIZE = 256` in
`gui_constants.h`). Every host must therefore back LVGL with adequate memory, not patch `lv_tiny_ttf`:
Pi Zero uses `LV_USE_STDLIB_MALLOC=LV_STDLIB_CLIB` (otherwise boxed into a 64 KB pool regardless of its
512 MB); ESP32-S3 routes glyph bitmaps to PSRAM. A build on a genuinely tiny fixed pool can override the
size to 0 (rasterize-direct).

## Net effect on the design

- Engine: **Tiny TTF**, glyph cache on by default (`SEEDSIGNER_TTF_CACHE_SIZE=256`; each host provisions
  the memory to back it — see §3), kerning off.
- CJK subsets **exclude ASCII**; embedded English defers to the OpenSans fallback at the English size
  (bug #2 fixed by `third_party/patches/lv_tiny_ttf-fallback-chain.patch`).
- The remaining bug-#2 follow-up is to push that fix upstream so the local patch can be dropped. (The
  former "bug #3" is not a bug — see §3.)
- The bin/loader hang (§1) and the fallback bug (§2) are exactly what the planned **rasterize-all,
  per-architecture validation gate** would surface (render every corpus glyph at every size, under ASan,
  on each target build).
