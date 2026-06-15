# Patches against the vendored LVGL submodule

`third_party/lvgl/` is pinned to **v9.5.0**. These patches carry SeedSigner-local
fixes that have not (yet) landed upstream. They are applied **on top of** the
pinned commit; re-checking out or re-initializing the submodule discards them, so
re-apply after any `git submodule update`.

## Applying

From the repo root:

```bash
cd third_party/lvgl
git apply ../patches/lv_tiny_ttf-fallback-chain.patch
```

`git apply --check ../patches/<file>` first if you want to confirm it still
applies cleanly against the pinned tree.

## Patches

### `lv_tiny_ttf-fallback-chain.patch`

Fixes `ttf_get_glyph_dsc_cb()`'s **no-cache path** (`cache_size == 0`) in
`src/libs/tiny_ttf/lv_tiny_ttf.c`. The stock path computes the glyph index with
`stbtt_FindGlyphIndex()` but ignores a `0` result (codepoint absent from the
font) and unconditionally returns `true`. That makes a Tiny TTF font report
*every* codepoint as "found", so LVGL's fallback walk (`lv_font_get_glyph_dsc`
following `font->fallback`) never advances to the next font — an absent glyph is
drawn as an empty `.notdef` box instead of deferring.

The patch returns `false` when the glyph is absent, matching what the *cached*
path already does (via `tiny_ttf_glyph_cache_create_cb` returning `false`).

**Why SeedSigner needs it.** The i18n font design chains a CJK-primary script
font over the baked-in OpenSans floor. Embedded English (technical terms that
aren't catalog msgids) must fall through the CJK font to OpenSans so it renders
at the *normal English size* rather than the bumped CJK size. Without this fix
the fallback never engages, so the CJK subset would have to bake in ASCII and
embedded English would render at the bumped size. With it applied,
`tools/fontpack/steps/build_lang_font.py` can keep ASCII out of the subsets.

See `docs/knowledge/font-loading-binfont-vs-tiny-ttf.md` (bug #2) and
`docs/font-and-i18n-rendering.md` (chain model) for the full story.

> Note: this patch (bug #2) is the only carried `lv_tiny_ttf` change; pushing it
> upstream lets it be dropped. The former "bug #3" (cache `cache_size>0` "spin")
> is **not** an `lv_tiny_ttf` bug and needs no patch — it is out-of-memory on a
> constrained pool surfaced by LVGL's halt-on-assert handler. The cache is now on
> by default (`SEEDSIGNER_TTF_CACHE_SIZE=256`); each host backs it with adequate
> RAM/PSRAM (see `docs/knowledge/tiny-ttf-cache-spin-root-cause.md`).
