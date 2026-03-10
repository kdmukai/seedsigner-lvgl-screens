# TODO (seedsigner C modules)

This file tracks seedsigner-component-local TODOs.
For cross-cutting nav + runner work, use:
- `../../docs/INPUT_NAVIGATION_IMPLEMENTATION_PLAN.md`

## Screen behavior / parity

- [ ] `button_list_screen` compatibility option: support explicit initial selection/highlight for multi-button lists (e.g., `initial_selected_index`, default behavior aligned with Python SeedSigner view logic/presentation expectations).
  - Rationale: Python UX expects first actionable item to be highlighted in flows where initial focus is meaningful.
  - Scope: C screen config parsing + list component focus/visual-state initialization.

## Input navigation rollout

- [ ] Harden shared nav layer lifecycle + cleanup for repeated screen swaps.
- [ ] Finalize vertical-list navigation semantics in `button_list_screen`.
- [ ] Implement grid navigation semantics (2x2) for `main_menu_screen`.
- [ ] Enforce input-mode default focus policy:
  - touch: no default active body button unless single option
  - hardware: default active button in BODY (never TOP_NAV)

## Architecture separation (long-term)

- [ ] Migrate desktop/tooling LVGL dependency to standalone pinned clone workflow (e.g., `third_party/lvgl`) rather than ESP-IDF managed component discovery.
- [ ] Separate LVGL screen/core modules from ESP-specific integration so screen layer can compile/run without ESP target dependencies.
