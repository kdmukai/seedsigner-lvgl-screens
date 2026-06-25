# Font-instance dedup: why the share key depends on chain direction

When two text roles in one locale resolve to the same rasterized font, we share a single
`lv_tiny_ttf` instance instead of creating a duplicate — each instance carries its own 3 glyph
caches on the constrained internal LVGL pool, so duplicates are pure waste (see
`docs/font-memory-plan.md`, Task A, and `docs/knowledge/tiny-ttf-cache-spin-root-cause.md`).

The non-obvious part: **the dedup key is not the same for Primary and Fallback locales**, because the
script font sits at a *different position in the fallback chain* in each case. Getting this wrong is a
correctness bug, not just a missed optimization.

## The two chain shapes (`seedsigner_register_font`)

- **Primary** (CJK / Arabic-Persian / shaping packs): the script font becomes the role's font and the
  baked OpenSans baseline becomes *its* fallback:
  ```
  profile.<role> = script;   script->fallback = original   // script is the chain ROOT
  ```
  `original` is the role's compiled-in baseline (used to render embedded ASCII/digits inside a
  CJK/shaped label). It differs per role because each role's baseline is at its own px.

- **Fallback** (Greek / Cyrillic / Vietnamese block packs): the script is chained *under* a heap copy
  of the baked baseline:
  ```
  heap_copy = new lv_font_t(*original);   heap_copy->fallback = script;   profile.<role> = heap_copy
  // script is the chain LEAF; heap_copy is per-role and shares original's read-only glyph data
  ```

## Why the keys differ

Two roles produce a byte-identical `script` iff they pass the same `(buf, len, px)` to
`lv_tiny_ttf_create_data_ex`. But sharing that instance is only *safe* if every field we mutate on it
is also identical:

- **Primary** mutates `script->fallback`. So a shared root requires the **same `original`** too — key
  is `(buf, len, px, original)`. If you share on `(buf, px)` alone, the second role to register
  silently overwrites the first role's `->fallback`, and the first role's embedded ASCII renders at the
  wrong size. (This is the exact mistake the first draft of the plan made.)
  - Concrete: CJK `large_button` and `top_nav_title` both use base 23 → same px at every profile. Their
    baselines collide at 240 (both 20px-SemiBold, themselves deduped) so they share there; at 320/480
    the baselines are 23 vs 26 px, so `original` differs and they correctly do **not** share.

- **Fallback** does **not** touch the script — it only reads it as the leaf and wraps each role in its
  own `heap_copy`. So `(buf, len, px)` is sufficient; each role keeps a distinct `heap_copy` (a cheap
  struct copy that shares the baseline's glyph data and adds no caches).

## The matching reap consequence

Because a single `script` pointer can now appear in several `Registration`s (and several unloads can
accumulate before a reap), `seedsigner_reap_retired_fonts()` must destroy **each distinct instance
exactly once** — a per-registration `lv_tiny_ttf_destroy` would double-free the shared script
(use-after-free → crash / heap corruption). It keeps a small "seen" set of already-destroyed pointers.
`heap_copy`s are always unique, but tracking them in the same set is harmless and future-proofs any
later heap_copy sharing.

The regression guard is the locale dedup + retire/reap section in `tools/apps/runner_core/test` — it
loads zh/hi/ru, renders, then unloads + reaps across a screen swap. Run it under ASan and the
destroy-once invariant is checked for real.

## Verification that it's free

A cross-branch byte-diff of 670 screenshots (en/es/zh/hi/ru @480×320) before vs after the dedup is
pixel-identical: same `(buf, px, kerning, cache)` ⇒ same rasterized glyphs ⇒ same pixels. The dedup
only changes how many instances/caches exist, never what they draw.
