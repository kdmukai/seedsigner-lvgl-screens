# `tools/` reorganization plan

_Status: **design locked, not yet executed.** Captured so the details survive the large refactor. This
is a planning doc (not knowledge); delete it once the reorg lands._

## Why

`tools/` accreted into a flat pile that no longer reads clearly:

- `tools/common/` is named "common" but is shared only by `screen_runner` + `web_runner` (the
  `screenshot_generator` has its own rendering) — it overclaims.
- The `seedsigner-translations` submodule sits parallel to the apps (`screenshot_generator`, etc.), which
  reads oddly — it's an i18n *input*, not a peer tool.
- The font-pack output lives buried at `tools/fontpack/out/` even though it's a **production deliverable**,
  not a tool's byproduct.
- The localized scenarios (test-only) and the font packs (production) are produced by the same orchestrator,
  implying they travel together — they don't.

## Target structure

```
/  (repo root)
  lang-packs/                       # production pack output (gitignored)  ← i18n/fontpack writes here

tools/
  i18n/                             # the prep TOOLS (internationalization machinery)
    po_catalog.py                   #   shared .po reader (the i18n primitive)
    build_fontpacks.py              #   font-pack builder → /lang-packs/<loc>/{<loc>.ttf, manifest.json}
    gen_localized_scenarios.py      #   scenario localizer → ../scenarios/localized/<loc>.json
    seedsigner-translations/        #   submodule (.po catalogs) — now grouped with the i18n tooling
  scenarios/                        # DATA only (the screen catalog)
    scenarios.json                  #   source (committed)
    localized/                      #   generated <loc>.json (gitignored)
  apps/                             # things that render the screens
    runner_core/                    #   was tools/common/ (runner_core.{cpp,h}, runner_sdl.{cpp,h}, test/)
    screen_runner/
    web_runner/
    screenshot_generator/
```

## Key decisions & rationale

- **`i18n/` holds the machinery, not the data.** Fonts + translations + the `.po` reader are pure
  internationalization tooling. `scenarios.json` is the *screen catalog* (what the apps render) — English
  is the source content, not a "translation" — so it's app *data*, not i18n, and stays out of `i18n/`.
- **Both i18n tools write to their consumer's home, not their own dir.** `build_fontpacks.py` → repo-root
  `lang-packs/` (a production deliverable); `gen_localized_scenarios.py` → `tools/scenarios/localized/`
  (test input for the apps). The tools live together in `i18n/`; outputs land where consumers expect them.
- **`scenarios/` depends on `i18n/`, one direction only.** The localizer reads `../i18n/po_catalog.py` +
  `../i18n/seedsigner-translations/`. That mirrors the real dependency (you can't localize without the
  i18n machinery) and never points back (fonts don't need scenarios).
- **`po_catalog.py` lives at `i18n/` top level** — shared by `build_fontpacks` (glyph corpus) and
  `gen_localized_scenarios` (catalog lookup). Both are now under/near `i18n/`, so it has a natural home.
- **`apps/` groups all four screen-rendering consumers** (interactive runners + the batch generator).
  `runner_core/` is renamed from `common/` so the name no longer claims to be universally common.
- **`lang-packs/` is at the repo root**, not under `tools/` — it's a deliverable, not tooling.

### Collapse the font-pack tooling into one file

The earlier `tools/fontpack/` had `build_fontpacks.py` (orchestrator) over a `steps/` subdir holding
`build_lang_font.py` + `gen_localized_scenarios.py` + `po_catalog.py`. This reorg pulls `po_catalog.py`
and `gen_localized_scenarios.py` out to `i18n/`, leaving `steps/` with a single step
(`build_lang_font.py`). So **merge `build_fontpacks.py` + `build_lang_font.py` into one
`i18n/build_fontpacks.py` and drop the `steps/` subdirectory** — a single-file font-pack builder.

## Execution (moves)

Use `git mv` throughout to preserve history.

1. `tools/common/` → `tools/apps/runner_core/`
2. `tools/screen_runner/`, `tools/web_runner/`, `tools/screenshot_generator/` → `tools/apps/…`
3. `tools/fontpack/build_fontpacks.py` + `tools/fontpack/steps/build_lang_font.py` → merge into
   `tools/i18n/build_fontpacks.py`; remove `tools/fontpack/steps/` and `tools/fontpack/`.
4. `…/steps/po_catalog.py` → `tools/i18n/po_catalog.py`
5. `…/steps/gen_localized_scenarios.py` → `tools/i18n/gen_localized_scenarios.py`
6. `tools/seedsigner-translations/` (submodule) → `tools/i18n/seedsigner-translations/`
   (`git mv` the path **and** edit `.gitmodules` `path =`, then `git submodule sync`).
7. `tools/scenarios.json` → `tools/scenarios/scenarios.json`; localized output → `tools/scenarios/localized/`.
8. Font-pack output dir → repo-root `lang-packs/`.

## Path references to update (the whole point of this doc — don't miss any)

- **`.gitmodules`** — submodule `path` → `tools/i18n/seedsigner-translations`; `git submodule sync`.
- **`.gitignore`** — add `/lang-packs/` and `tools/scenarios/localized/`; remove `tools/fontpack/out/` and
  the blanket `tools/scenarios/` (now `scenarios.json` is committed; only `localized/` is ignored).
- **App `CMakeLists.txt`** (`screen_runner`, `web_runner`, `screenshot_generator`) — each gains one `../`
  of depth to reach `components/`, `third_party/lvgl/`, etc. (now `tools/apps/<x>/`). Update `common/` →
  `runner_core/` include dirs. `screenshot_generator/CMakeLists.txt` keeps its `LV_USE_STDLIB_MALLOC=LV_STDLIB_CLIB`.
- **`web_runner/`** scripts — `build.sh`, `Dockerfile`, `gen_scenarios.py`, `serve.sh`, `shell.html`:
  internal relative paths + any `common/`/scenarios references.
- **`scripts/ci/ci.sh`** — `screenshot_generator` → `apps/screenshot_generator`; `fontpack` →
  `i18n`; scenarios paths. (Also a good place to wire the i18n locale rendering later — separate work.)
- **`screenshot_gen.cpp`** — `DEFAULT_SCENARIOS_FILE` → `tools/scenarios/scenarios.json`; `--font-dir`
  default → repo-root `lang-packs`.
- **`build_fontpacks.py`** (merged) — defaults: `--gen-bin` → `tools/apps/screenshot_generator/build/…`;
  `--translations-dir` → `tools/i18n/seedsigner-translations`; `--out-dir` → repo-root `lang-packs`;
  `REPO_ROOT` depth; `po_catalog` import (same dir). `--assets-dir` stays `components/seedsigner/assets`.
- **`gen_localized_scenarios.py`** — defaults: `--scenarios` → `tools/scenarios/scenarios.json`;
  `--out-dir` → `tools/scenarios/localized`; `--translations-dir` → `tools/i18n/seedsigner-translations`;
  `REPO_ROOT` depth; `po_catalog` import (same dir).
- **Docs** — `tools/README.md`, `tools/i18n/README.md` (renamed from fontpack/README), `docs/font-and-i18n-*.md`,
  knowledge docs, and any path mentions. Delete this plan doc when done.

## Verification

- Rebuild `apps/screenshot_generator` (CMake reconfigure picks up the new depth) → builds.
- `python3 tools/i18n/build_fontpacks.py` → writes `lang-packs/<loc>/`.
- `python3 tools/i18n/gen_localized_scenarios.py` → writes `tools/scenarios/localized/<loc>.json`.
- Render one CJK locale end-to-end (proves the new `--font-dir lang-packs` + `--scenarios-file
  tools/scenarios/localized/<loc>.json` paths).
- `apps/screen_runner` builds (SDL); `apps/web_runner` builds via its Docker/emscripten flow (slower —
  may defer to CI).
- `git submodule status tools/i18n/seedsigner-translations` resolves after the `.gitmodules` move.
