# Locale-switch font lifecycle: why script fonts must outlive the screen that drew them

## Symptom
Switching locales repeatedly in interactive use crashed on the ESP32-P4: a
`Load access fault` with a **null `font->dsc`** inside `ttf_get_glyph_dsc_cb`
(`lv_tiny_ttf.c`), reached from `lv_draw_label_iterate_characters` →
`lv_font_get_glyph_width` while drawing a label. Observed most reliably switching
into Hindi (`hi`), but it is **not language-specific** — any switch away from a
complex-script / CJK / Fallback-pack locale was vulnerable.

Crucially: **single-shot renders never crashed** (the desktop screenshot gallery and
the ESP32 single-render gallery both produced correct `hi`/`th`/`ur`). Only
**repeated interactive switching** tripped it. That is the fingerprint of a
lifecycle/ordering bug, not a shaping bug.

## Root cause — a dangling `lv_font_t` across a locale switch
A locale switch is **two separate host calls**:

1. `seedsigner_lvgl.load_locale(next, packs)` → `ss_load_locale()` →
   `ss_unload_locale()` → `seedsigner_clear_registered_fonts()`, which (pre-fix)
   immediately `lv_tiny_ttf_destroy()`'d the previous locale's script fonts.
   `lv_tiny_ttf_destroy` does **both** `font->dsc = NULL` and `lv_free(font)`.
2. `seedsigner_lvgl.button_list_screen(cfg)` → builds the new screen and only then
   deletes the old one (`load_screen_and_cleanup_previous`).

Between (1) and (2) the **old screen is still `lv_screen_active()`**, and its labels
hold **raw `lv_font_t*` pointers** captured at build time (LVGL stores the font
pointer in each label's style — it does not re-resolve through the display profile).
Destroying the fonts in step (1) leaves every still-live label dangling.

On the ESP32 a dedicated LVGL render task runs `lv_timer_handler()` continuously. It
acquires the LVGL lock in the gap between the two host calls and **redraws the
freshly-invalidated old screen** — through fonts step (1) just freed. The freed
block still reads back the `dsc = NULL` that `destroy` wrote, so the device gets a
null-deref rather than garbage. Desktop screenshot tools never hit it because they
render each screen once, synchronously, and never free fonts while a built screen
stays live and about-to-be-redrawn.

Only **unmatched** labels crash: a glyph-run-matched label has `text_opa = TRANSP`
and LVGL skips its draw entirely (early `opa <= LV_OPA_MIN` return), so it never
queries the font. An ASCII/Latin label drawn through a script **Primary** still
queries that primary first (to discover the glyph is absent) before falling back to
the baked OpenSans — so even a non-script label faults.

## The invariant
> A registered script font (and its backing byte buffer, which tiny_ttf reads
> lazily) must stay alive until **every screen built while it was active has been
> deleted** — not merely until the next `load_locale`.

The screen layer already has the exact moment the dangling references vanish: the
`lv_obj_delete(old_screen)` in `load_screen_and_cleanup_previous`.

## Fix — defer font destruction to the screen swap
- `seedsigner_clear_registered_fonts()` no longer destroys; it **retires** each
  registration (restores the profile's compiled-in font pointer, moves the
  `lv_font_t` aside) into a `g_retired` list.
- `ss_unload_locale()` likewise **moves** its owned byte buffers (`g_owned`) into
  `g_owned_retired` instead of freeing them (the retired fonts still read them).
  Glyph-run tables *can* clear eagerly — a matched label draws its own pre-baked A8
  mask (owned by the label, freed on `LV_EVENT_DELETE`), which references neither the
  run table nor the stb metrics handle.
- `seedsigner_reap_retired_fonts()` / `ss_reap_retired()` destroy the retired fonts
  then free the retired buffers (fonts first — they read the buffers).
- `load_screen_and_cleanup_previous()` calls `ss_reap_retired()` **immediately after**
  `lv_obj_delete(old_screen)`. At that point the only live screen is the new one,
  built with the freshly-registered (non-retired) fonts, so the retired set is
  provably unreferenced. It is a no-op for same-locale navigation (nothing retired).

This runs under the same LVGL lock the host holds for the whole screen-build call, so
the render task cannot interleave between the delete and the reap.

### Why not simpler alternatives
- *Free immediately but blank the old screen first in `ss_unload_locale`*: couples the
  I/O-agnostic loader to LVGL screen management and adds a blank flash.
- *Reap at the start of the next `load_locale`*: unsafe if a host issues two
  `load_locale`s with no screen build between (the first retired set's screen is still
  active when the second load would reap it).
- *Move buffer ownership into the font registry*: loses the loader's per-file dedup
  (a CJK/shaping locale shares one `.ttf` across all five roles → one buffer, five
  fonts; per-role copying would duplicate it 5×).

## Reproducing + validating (desktop, deterministic)
The race was made deterministic by performing, in one thread, what the device does by
chance: load `hi` → build+render the `hi` screen → `load_locale(th)` (retires/freed
`hi`'s fonts) → **redraw the still-active `hi` screen**. Built against the real
component sources with AddressSanitizer:

- **Pre-fix:** ASan `heap-use-after-free`, read in `lv_font_get_line_height` /
  `ttf_get_glyph_dsc_cb`, freed by `lv_tiny_ttf_destroy`, from
  `lv_draw_label_iterate_characters` — the exact device backtrace.
- **Post-fix:** survives the race-window redraw; a 6-round × 8-locale churn
  (en/hi/th/ur/fa/zh/vi/ru) with a forced race-window redraw at every switch runs
  clean under ASan **and** LeakSanitizer (the deferred reap frees everything — no
  accumulation).

No rendering changed (the fix only moves *when* fonts are freed): the `hi`/`th`/`ur`/
`fa`/`zh` galleries render identically.

## Not addressed here
The `ur` (Nastaliq RTL) "vertical bars" rendering is a **separate** issue (RTL
right-anchored word-wrap / shaping), not this lifecycle crash. `ur` no longer crashes
on switch; its glyph layout is a distinct, pre-existing loose end.
