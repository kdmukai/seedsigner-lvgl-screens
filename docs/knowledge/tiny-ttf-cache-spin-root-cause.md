# Tiny TTF glyph-cache "spin" is out-of-memory, not a cache bug

The `lv_tiny_ttf` glyph/draw cache (`cache_size > 0`) is correct and effective. On a memory-constrained
target it can CPU-spin — but that is an **out-of-memory** condition, **not** a defect in the cache or
LVGL's red-black tree, and **unrelated** to upstream
[lvgl/lvgl#9765](https://github.com/lvgl/lvgl/issues/9765) (a different, glyph-*vanish* corruption bug).
This is the canonical reference for the issue formerly tracked as "bug #3." It is the same root cause as
[`screenshot-gen-lvgl-mem-pool-exhaustion.md`](screenshot-gen-lvgl-mem-pool-exhaustion.md) (small fixed
pool → allocation-failure spin).

## TL;DR

With the `lv_tiny_ttf` glyph/draw cache enabled (`cache_size > 0`), rendering CJK content CPU-spins on a
memory-constrained target; `cache_size = 0` is fine. The mechanism is:

1. With `cache_size > 0` the **draw-data cache holds rasterized glyph bitmaps** in memory (the point of
   the cache — avoid re-rasterizing on redraw/scroll).
2. The cache is **count-bounded** (`lv_cache_class_lru_rb_count`, default 128 entries), not byte-bounded.
   CJK A8 bitmaps at UI sizes are comparatively large, so "128 entries" can be hundreds of KB.
3. On a target with a small **fixed** LVGL heap (the embedded default `LV_MEM_SIZE = 64 KB`,
   `LV_USE_STDLIB_MALLOC = LV_STDLIB_BUILTIN`), the held bitmaps **exhaust the pool**.
4. The next `lv_malloc` returns `NULL`.
5. LVGL's `LV_ASSERT_MALLOC(p)` fires, and the **default `LV_ASSERT_HANDLER` is `while(1);`** ("Halt by
   default"). That infinite busy-loop **is** the "spin."

With `cache_size = 0` the bitmap is freed immediately after each draw (`ttf_release_glyph_cb` →
`lv_draw_buf_destroy`), so the pool never fills and there is no OOM, hence no spin. The desktop tools set
`LV_USE_STDLIB_MALLOC = LV_STDLIB_CLIB` (real `malloc`, gigabytes), so they never hit OOM and never
reproduced the spin — which is why it looked mysterious and "content-dependent."

So it is **intended behavior exposing a memory-provisioning mistake**, not a defect in LVGL's cache or
red-black tree. The only mild design weakness is that tiny_ttf bounds a cache of *variable-size* bitmaps
by *count*, which lets it silently over-commit; LVGL's own image cache is byte-size-bounded for exactly
this reason.

## How it was proven (isolated clone, instrumentation only — no debugger available)

Work was done in a throwaway clone (`seedsigner-c-modules-ttf-debug`) so the main repo was untouched.
Tools: a standalone repro (`tools/ttf_repro/`) that drives the real LVGL label draw path
(`lv_font_get_glyph_dsc` → `lv_font_get_glyph_bitmap` → release) over many distinct CJK glyphs; a
`SIGALRM` watchdog dumping an `execinfo` backtrace; per-loop spin-guards added to every red-black-tree
traversal in `lv_rb.c`; and rasterization counters in `lv_tiny_ttf.c`.

| Allocator | `LV_MEM_SIZE` | `cache_size` | Result |
|---|---|---|---|
| CLIB | n/a (heap) | 0 / 128 / 4096 | completes; **never spins** — any glyph count, kerning on/off, fallback chain, scrolling |
| BUILTIN | 64 KB | 0 | completes (bitmaps freed each draw) |
| BUILTIN | 64 KB | 128 | **infinite spin** (single frame, ≥100 distinct CJK glyphs; never finished at 90 s) |
| BUILTIN | 64 KB | 128, `LV_USE_ASSERT_MALLOC=0` | OOM moves but stays non-graceful: small renders **abort** in stb's own assert; larger still spin in `lv_draw_buf` |
| BUILTIN | 2 MB (≈PSRAM) | 128 / 4096 | completes; cache works well (cache=4096 → 147 rasterizations vs 5178 at cache=128) |

Key observations:
- The **RB spin-guards never tripped** → the tree is never corrupted; not a cycle. Rules out the
  kerning-comparator / `lv_rb`-corruption hypotheses from the earlier upstream plan.
- The watchdog backtrace bottoms out in `rb_create_node` (via `ttf_get_glyph_bitmap_cb` →
  `lv_cache_acquire_or_create` → `add_cb` → `alloc_new_node` → `lv_rb_insert`). `rb_create_node` has no
  loop — the `while(1)` lives in the inlined `LV_ASSERT_MALLOC` at the `lv_malloc_zeroed` line.
- Disabling **only** `LV_USE_ASSERT_MALLOC` removed *that* spin but did **not** yield graceful
  degradation: with the LVGL assert gone, OOM next surfaces as `stb_truetype`'s own
  `assert(z != NULL)` (`stbtt__new_active`) → `abort`, and at higher glyph counts a different spin in the
  draw-buffer path. **The tiny_ttf + stb + lv_cache stack is not OOM-safe; you cannot make it degrade
  cleanly from the tiny_ttf side.**
- The kerning cache is created at size 256 regardless of `cache_size`, so it's exercised even when
  `cache_size = 0` (which renders fine) — independently exonerating the kerning path.

Note: a larger cache count (e.g. 4096) does not help — it caches **more** bitmaps before count-eviction,
so it exhausts the pool **sooner**. The spin is memory exhaustion, not count-eviction.

## Why "detect and degrade" isn't worth building

- A **self-tracked byte budget** inside tiny_ttf only knows tiny_ttf's own footprint, not total system
  memory — a best-effort heuristic that fails when anything else uses the pool.
- **Reacting to real allocation failure** doesn't work either: the stack isn't OOM-safe (stb aborts;
  draw-buf path spins). There is no single chokepoint tiny_ttf can guard.

So the robust answer is **don't run out of memory** (provision enough heap, ideally PSRAM), where the
cache is correct and very effective.

## The fix lives in the host applications, not in c-modules

c-modules' font code is allocator-agnostic — it just calls
`lv_tiny_ttf_create_data_ex(buf, len, px, kerning, cache_size)`
([`font_registry.cpp`](../../components/seedsigner/font_registry.cpp),
[`gui_constants.cpp`](../../components/seedsigner/gui_constants.cpp)). **Where** that memory lands is
decided entirely by how each host configures LVGL. tiny_ttf has two allocation sinks, both host-controlled:

1. **`lv_malloc`** (selected by `LV_USE_STDLIB_MALLOC`) — cache RB nodes *and* the stb rasterizer scratch.
2. **`font_draw_buf_handlers`** (`LV_GLOBAL_DEFAULT()->font_draw_buf_handlers`, used by
   `lv_draw_buf_create_ex`) — the **glyph bitmaps** themselves (the big allocations).

### Pi Zero (`seedsigner-raspi-lvgl`) — must use CLIB malloc

Under `LV_CONF_SKIP` LVGL defaults to `LV_STDLIB_BUILTIN` with a **64 KB fixed pool**, boxing LVGL into
64 KB regardless of the board's 512 MB. With the cache on by default that pool OOM-spins, so `setup.py`
**must** carry `("LV_USE_STDLIB_MALLOC", "LV_STDLIB_CLIB")` in `define_macros` (mirrors the desktop tools)
so `lv_malloc` uses glibc `malloc` and the 512 MB becomes available. **(Applied — see that repo's
`docs/knowledge/`.)**

### ESP32-S3 (`seedsigner-micropython-builder`) — route tiny_ttf to PSRAM

The board's ESP-IDF config already enables PSRAM (`CONFIG_SPIRAM=y`, `CONFIG_SPIRAM_USE_MALLOC=y`).
**Caveat:** `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=8192` serves allocations ≤ 8 KB from *internal* RAM by
default, and CJK glyph bitmaps are only a few hundred bytes — so plain `malloc`/CLIB would keep them
internal and re-trigger the exhaustion. Reliable options:
- **Targeted (recommended):** override `font_draw_buf_handlers` with PSRAM-backed alloc/free
  (`heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`) so the bitmaps go to PSRAM while small node/scratch
  allocations stay in fast internal SRAM.
- **Whole-heap:** point LVGL's allocator at PSRAM — `LV_STDLIB_BUILTIN` with the pool in PSRAM
  (`LV_MEM_ADR`/`LV_MEM_POOL_ALLOC`), `LV_STDLIB_CUSTOM` wrapping `heap_caps_malloc(MALLOC_CAP_SPIRAM)`,
  or `LV_STDLIB_MICROPYTHON` if the MicroPython GC heap is placed in PSRAM.

(Confirm in the builder which `LV_USE_STDLIB_MALLOC` the firmware actually compiles with and where the MP
GC heap lives — that determines whether anything beyond the `font_draw_buf_handlers` override is needed.)

### c-modules

The cache is **enabled by default** (`SEEDSIGNER_TTF_CACHE_SIZE = 256` in `gui_constants.h`) for every
target. Each host must therefore back LVGL with adequate memory (Pi Zero CLIB malloc; ESP32-S3 PSRAM) —
this is now a hard requirement of bumping the c-modules submodule, not an opt-in. A build that genuinely
runs on a tiny fixed pool can override the size to 0 (`-DSEEDSIGNER_TTF_CACHE_SIZE=0`). As crash-hygiene,
hosts may also set a non-halting `LV_ASSERT_HANDLER` so a genuine unexpected OOM logs/reboots instead of
freezing the UI — but that does **not** make OOM safe (stb still aborts); it's not a substitute for
provisioning memory.

## Performance & pre-warm (deferred — lazy caching is sufficient)

With the cache on, it fills **lazily**: the first paint of a screen rasterizes each distinct glyph once
(cache miss, ~1–3 ms/glyph on ESP32 per the original estimate) and every redraw/scroll afterward is a hit.

- **Cold per-screen first paint is a one-time, generally unnoticeable cost.** A typical screen (~20–40
  distinct glyphs) is tens of ms during a transition. The one case to watch is a **dense CJK body**
  (~150–300 distinct glyphs → a few hundred ms on first entry); still one-time, and cached thereafter.
- **Animations are the reason the cache is on.** A scrolling/animated title redraws the same glyphs every
  frame; without a cache that is per-frame re-rasterization → stutter. With the cache, frame 1 warms a
  small set (well under the 256/font cap, so it never evicts mid-animation) and the rest are hits → smooth.
- **Pre-warming is deferred as premature.** A full pre-warm was estimated at ~2–5 s on ESP32, to buy
  smoother *first* paints that lazy caching already delivers after one frame; lazy filling is also more
  memory-frugal (footprint = the working set actually displayed, not the whole corpus). Revisit only if a
  *measured* first-paint problem on a specific dense screen appears — and then pre-warm just that screen's
  glyph set, not everything. (Worst-case sizing, if ever needed, is a role-aware corpus scan: distinct
  `(codepoint, role-px, weight)` tuples; the corpus is closed, so it is computable at build time.)

## Reproduce (in a throwaway clone, never the live tree)

The diagnosis used a standalone harness (`tools/ttf_repro/`) plus env-controlled cache knobs and `lv_rb.c`
spin-guards, all in a disposable clone that has since been removed. To recreate it: build a small program
that links LVGL with `LV_USE_STDLIB_MALLOC=LV_STDLIB_BUILTIN` (and optionally `LV_MEM_SIZE`), create a
`lv_tiny_ttf_create_data_ex(NotoSansSC, …, cache_size=128)` font, and render a label of many distinct CJK
glyphs. A 64 KB builtin pool + `cache_size>0` spins (OOM → `LV_ASSERT_MALLOC` `while(1)`); CLIB malloc or
a multi-MB pool renders the full corpus with the cache on and no spin.
