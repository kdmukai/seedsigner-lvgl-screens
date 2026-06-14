# Screenshot generator: LVGL 64KB pool exhaustion hung CJK gallery renders

## Symptom

`screenshot_gen` rendering the full scenario gallery for a CJK locale (observed: `ja`, `ko`)
**hung CPU-bound** part-way through — it produced the first ~11 of 16 scenarios at 240x240, then
spun forever on the next screen (`seed_add_passphrase_screen`, a keyboard-heavy screen). `zh_Hans_CN`
and `en` completed the identical scenario sequence fine, so it looked locale-specific and intermittent.

Confusing clues that sent the diagnosis sideways at first:
- The hung scenario (`seed_add_passphrase`) rendered **fine in isolation** and fine as scenario #2 of a
  2-screen run. It only hung as scenario #12 of the full run.
- Disabling animated-GIF generation (no imagemagick) did **not** help — same hang point.
- Reverting the `lv_tiny_ttf` fallback patch changed the *symptom* (clean-ish crash instead of an
  infinite spin) but not the failure point — a red herring that made the patch look guilty.

## Root cause

The desktop build compiles LVGL with `LV_CONF_SKIP`, so every `lv_conf` value is the LVGL default —
including `LV_USE_STDLIB_MALLOC = LV_STDLIB_BUILTIN` with a **64KB `LV_MEM_SIZE` fixed pool**. Tiny TTF's
stb rasterizer also allocates from that same pool (`STBTT_malloc = lv_malloc`).

A memory monitor (`lv_mem_monitor`) printed after each scenario showed the pool steadily filling and
fragmenting across scenarios rendered back-to-back in one process:

| after scenario | used | largest free block | frag |
|---|---|---|---|
| main_menu | 24 KB | 36 KB | 2% |
| large_icon_status (base) | 35 KB | 18 KB | 19% |
| large_icon_status (warning) | 38 KB | 13 KB | 33% |
| large_icon_status (dire) | 42 KB | 7 KB | 50% |
| large_icon_status (error) | 45 KB | **3.7 KB** | 66% |

By scenario #12 the 64KB pool is ~70% used and badly fragmented — no contiguous block remains for the
keyboard screen's allocations. The allocation-failure path then spins (or crashes). It is **fragmentation
of a too-small pool**, not a single huge allocation. CJK locales tip over first because their per-screen
peak runs a few KB higher than Latin; `zh` happened to stay just under the cliff while `ja`/`ko` crossed
it — i.e. the whole tool was running at the ragged edge of 64KB, and the i18n gallery growth exposed it.

## Fix

Build the desktop tool with the **C library allocator** instead of the embedded fixed pool
(`tools/screenshot_generator/CMakeLists.txt`):

```cmake
LV_USE_STDLIB_MALLOC=LV_STDLIB_CLIB
```

This is a desktop tool on a machine with gigabytes of RAM; there is no reason to emulate a tiny embedded
pool (and 64KB isn't even a realistic proxy — target hardware has 8–16MB PSRAM). Real `malloc`/`free`
has the whole heap and handles fragmentation far better. After the switch, every locale renders the full
gallery across all four resolutions.

## Caveat / follow-up

The monitor showed `used` climbing without fully recovering between scenarios (~2KB/scenario). That may
be a genuine per-scenario leak rather than just high-water render caching. CLIB malloc makes it a
non-issue for the desktop tool's scenario count, but the **on-device builds run a real, limited pool**,
so the growth is worth a separate look there (candidates: screen-teardown completeness, tiny_ttf
`cache_size=0` structures). Note `lv_mem_monitor` only works under `LV_STDLIB_BUILTIN`, so re-add the
builtin pool temporarily if you want to re-measure.
