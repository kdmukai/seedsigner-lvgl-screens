# tools

Utilities for desktop/local validation workflows around SeedSigner C-module screens.

## Contents

- `scenarios.json`
  - Shared scenario source of truth used by multiple tools.
  - Defines screen contexts and optional variations.

- `screenshot_generator/`
  - Deterministic renderer for producing screenshots (and optional GIFs) from real LVGL screen paths.
  - Primary docs: `screenshot_generator/README.md`

- `screen_runner/`
  - Interactive desktop runner (in progress) for live input/navigation testing.
  - Primary docs: `screen_runner/README.md`

## How these tools relate

Both tools consume the same `scenarios.json` so:
- visual snapshot testing (`screenshot_generator`) and
- live interaction testing (`screen_runner`)

stay aligned on identical screen definitions/variations.

## Workflow intent

1. Edit or add scenarios in `scenarios.json`.
2. Use `screenshot_generator` for fast visual/regression checks.
3. Use `screen_runner` for interactive navigation/input behavior checks.

Keeping one shared scenario file reduces drift and makes results reproducible across tools.
