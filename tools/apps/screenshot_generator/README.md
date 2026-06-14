# screenshot_gen (LVGL screen screenshot generator)

Generates deterministic screenshots from real LVGL screen render paths using JSON-defined scenarios.

## Build (CMake)

From repo root:

```bash
cmake -S tools/apps/screenshot_generator \
      -B tools/apps/screenshot_generator/build
cmake --build tools/apps/screenshot_generator/build -j
```

Executable:

- `tools/apps/screenshot_generator/build/screenshot_gen`

## Usage

```bash
tools/apps/screenshot_generator/build/screenshot_gen [options]
```

Options:

- `--out-dir <path>` output root (default `tools/apps/screenshot_generator/screenshots`)
- `--width <px>` render width (default `480`)
- `--height <px>` render height (default `320`)
- `--scenarios-file <path>` scenario config file
  - default from repo root: `tools/scenarios/scenarios.json`

## Scenario configuration

Scenario definitions are loaded from JSON.

Schema:

- top-level keys are screen function names
- each screen contains:
  - `context` (base input context)
  - optional `variations` array
- each variation contains:
  - `name`
  - optional `context` (merged onto base context)

`animated` is optional in context and defaults to `false`.

## Output model

The generator runs in **single-output overwrite mode**:

- output directory is cleared before each run
- image assets are written to `img/`
- `manifest.json` is regenerated each run with dynamic presentation data


## Animated GIF behavior

For scenarios marked animated in context:

- if ImageMagick is available (`magick` or `convert`), a GIF is generated
- temporary frame files are cleaned up after GIF generation

## Screenshot comparison (PR diff reports)

`compare_screenshots.py` compares two screenshot sets (before/after) and generates
an HTML diff report. Used in CI to show visual changes in pull requests.

### Usage

```bash
python3 tools/apps/screenshot_generator/compare_screenshots.py \
  --before-dir baseline/ \
  --after-dir after/ \
  --out-dir report/ \
  --pr-number 42
```

Arguments:

- `--before-dir` baseline screenshots directory (with `manifest.json` and `img/`)
- `--after-dir` new screenshots directory
- `--out-dir` output directory for the HTML report
- `--pr-number` PR number (used in report title)

### Output

```
report/
  index.html            # visual diff report (dark theme, static HTML)
  compare_result.json   # machine-readable summary
  before/img/...        # baseline images (changed + removed only)
  after/img/...         # new images (changed + new only)
  diff/...              # ImageMagick diff images (changed only)
```

### Classification logic

Screenshots are matched by `name` field from `manifest.json`:

- **New**: present in after only
- **Removed**: present in before only
- **Changed/Unchanged**: present in both — compared via SHA-256 of the `.png` file

For animated scenarios (`.gif` path in manifest), the static `.png` snapshot is used
for hash comparison. The `.gif` is displayed in the report for visual review.

### CI integration

The unified `pages.yml` workflow runs this automatically on PRs (read-only — no
write token, no PR comment):

1. `build` job generates screenshots from the PR branch and assembles the
   combined site (gallery + web runner), uploaded as the `site` artifact.
2. `screenshot-diff` job generates the **base** branch screenshots in the same
   run, compares them against the PR's gallery, writes a summary to the job
   summary, and uploads the HTML report as the `screenshot-diff` artifact.

The gallery (at `/`) and web-runner playground (at `/play/`) are published via
the official GitHub Pages action only on push to `main` / manual dispatch.

### Dependencies

- Python 3 (standard library only — no pip packages)
- ImageMagick (`magick` or `convert`) for generating visual diff images

## Dependencies

- `nlohmann/json` is fetched automatically by CMake (`FetchContent`)
- `libpng` is required for PNG output (`sudo apt install -y libpng-dev` on Ubuntu/Debian)
- ImageMagick for animated GIF generation and screenshot comparison
