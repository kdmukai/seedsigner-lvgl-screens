# tools

Desktop/local development tooling for the SeedSigner C-module screens, split into three groups by role.

```
tools/
  apps/         screen-rendering apps (they consume scenarios + fonts and draw screens)
    runner_core/        shared runner core (SDL-free runner_core + SDL glue), used by the two runners
    screen_runner/      interactive SDL2 desktop runner — live input/navigation testing
    web_runner/         WASM browser playground (Emscripten) — shared runner_core, single-file build
    screenshot_generator/  batch PNG/GIF renderer — used by CI for visual-regression diffs
  i18n/         internationalization machinery (produces the inputs the apps consume)
    build_fontpacks.py          subset fonts → repo-root lang-packs/<loc>/ (production-ready)
    gen_localized_scenarios.py  localize scenarios → scenarios/localized/<loc>.json (test-only)
    po_catalog.py               shared .po reader
    seedsigner-translations/    submodule (.po catalogs)
  scenarios/    the screen catalog (data, not tools)
    scenarios.json    source of truth: screen contexts + variations, shared by all apps
    localized/        generated per-locale variants (gitignored)
```

The production font packs land at the **repo-root `lang-packs/`** (a deliverable, not a tool), gitignored
and reproducible from `scenarios/scenarios.json` + the catalogs + the vendored fonts.

## How the pieces relate

All apps under `apps/` render from the **same** `scenarios/scenarios.json` (or its localized variants), so
visual snapshot testing (`screenshot_generator`), interactive testing (`screen_runner`), and the browser
playground (`web_runner`) stay aligned on identical screen definitions. The `i18n/` tools prepare the
per-locale fonts and scenarios those apps consume; see `i18n/README.md`.

## Typical workflow

1. Edit or add scenarios in `scenarios/scenarios.json`.
2. (i18n) `python3 tools/i18n/build_fontpacks.py` and `python3 tools/i18n/gen_localized_scenarios.py`.
3. Render/validate with `apps/screenshot_generator` (regression checks) or `apps/screen_runner`
   (interactive). Each app's `README.md` has its build/run details.
