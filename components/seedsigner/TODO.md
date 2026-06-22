# TODO (seedsigner LVGL screens)

## Screen behavior / parity

- [ ] `button_list_screen`: `initial_selected_index` JSON config applies the initial highlight in
  hardware mode only. Touch mode does not apply it (no button is pre-highlighted on load).
  If touch-mode pre-selection is needed for specific flows, extend `nav_bind` to call
  `update_visual_focus` (or directly `button_set_active`) when a valid `initial_body_index`
  is provided, regardless of input mode.

- [ ] `button_list_screen`: support per-button **icons**. `read_button_list_labels` currently keeps
  only the label (a bare string, or the string at index 0 of a `[label, …]` array) and builds each
  button via `button(body, label, NULL)` — text only. The `[label, …]` array form is the intended
  hook: read an icon name at index 1 and render via `large_icon_button` (which already takes icon +
  text). Needed so the SeedSigner menus keep their icons — notably ToolsMenuView, where two entries
  both read "New seed" and are distinguished *only* by icon (camera vs dice). The Python side already
  drops icons when serializing for the native screen (`ButtonOption.to_lvgl()` → translated label
  only); wire both ends together when this lands.

## Architecture separation (long-term)

- [x] Migrate desktop/tooling LVGL dependency to standalone pinned clone workflow (e.g.,
  `third_party/lvgl`) rather than ESP-IDF managed component discovery. _(Done: the desktop
  tool CMake now uses `third_party/lvgl`, the pinned LVGL submodule, as the preferred LVGL
  root; ESP-IDF managed-component paths remain only as fallbacks.)_
- [ ] Separate LVGL screen/core modules from ESP-specific integration so screen layer can
  compile/run without ESP target dependencies.

## Font memory / glyph-cache optimization

- [ ] **Reduce the number of distinct runtime `tiny_ttf` font instances** to cut LVGL
  internal-pool pressure — important on memory-constrained targets and **especially for
  non-Latin (shaping) locales**. Each distinct `(font, px)` instance carries its own 3
  glyph caches (`SEEDSIGNER_TTF_CACHE_SIZE`, 256 entries each), all allocated from the
  fixed LVGL builtin pool (`CONFIG_LV_MEM_SIZE_KILOBYTES`). This was exposed when the
  P4-43 pool (64 KB default) exhausted while rendering a large button list after a status
  screen — first a NULL-deref crash, then (with the cache shrunk to fit) dropped glyphs.
  Mitigated for now by growing the pool to 128 KB, but fewer font instances is the durable
  fix. Initial findings from the audit:

  - **Icon and keyboard fonts are baked 4bpp bitmap fonts, not `tiny_ttf`** — they use no
    runtime glyph cache and cost the pool nothing. The non-trivial icon sizes (24/36/48 px
    base) are *not* a concern. (`fonts_for_multiplier()` in `gui_constants.cpp`: the icon /
    `keyboard` slots point at `seedsigner_icons_*_4bpp_*` / `inconsolata_semibold_*_4bpp_*`.)

  - **Only 5 roles are runtime `tiny_ttf`** — the `nullptr` slots filled at `set_display()`
    by the Western floor / locale pack: `main_menu_title`, `title`, `large_button`,
    `button`, `body`. These are the *entire* glyph-cache story.

  - **The `body` auto-fit range is the biggest multiplier.** `body` re-rasterizes to
    whatever size fits between `body_font_min_size` (base 15) and `body_font_max_size`
    (base 20 = title), so one role can spawn instances at every intermediate px. Narrowing
    that range, or quantizing it to 2–3 discrete steps, is the highest-leverage change.

  - **`button` and `large_button` resolve to the same px on the larger profiles** (both
    base 18 at 320h/480h; they differ only at 240h, 18 vs 20). They are still created as
    two separate instances. `font_registry.cpp` (`install_western_baseline` loop) creates
    **one `tiny_ttf` per role** with no dedup-by-px — so add px-keyed sharing: when two
    roles land on the same px, share one font + cache. It auto-separates where they
    legitimately differ. `title` (base 20) likewise coincides with `body`'s max.

  - **Non-Latin doubles the text-font count.** A shaping locale loads each distinct text px
    from the language pack *in addition to* the retained Western floor. So instances ≈
    (distinct text sizes) × 2 — collapsing ~5 roles spanning ~5–6 effective sizes down to
    ~3 distinct sizes roughly halves the non-Latin pool pressure.

  - **Measure before/after**: add an `lv_mem_monitor` (pool used / free / frag) binding so
    the high-water mark can be checked on-device for Latin vs. a loaded Farsi pack and used
    to size the pool/cache from data rather than estimates.

## Cross-screen overlays

- [ ] **SD-card insert/remove toast over any screen** — significant, still-undesigned. The seedsigner
  app shows a transient toast (icon + message strip along the bottom) when a microSD card is inserted or
  removed. It fires **asynchronously** — a hardware event at any moment — and must composite **over
  whatever screen is currently shown**, then vanish and leave that screen untouched. On the PIL/CPython
  app this is a Python thread (`ToastManagerThread`) that takes the renderer lock and draws over the PIL
  canvas; that approach does not carry over to LVGL screens, where the native module owns the display and
  its render loop and an app-side thread can't safely mutate LVGL state. So it needs a native,
  screen-independent overlay on the LVGL side, triggered by an async signal from the app — the toast
  analogue of the cross-cutting screensaver concern. Design TBD (do not assume the PIL thread model).
