# web_runner — Browser (WASM) Screen Playground

An interactive, browser-based playground for the SeedSigner LVGL screens. It
compiles the **same** platform-agnostic screen code (`components/seedsigner/`)
to WebAssembly via Emscripten and renders the real LVGL screens to an HTML
`<canvas>`. You can:

- pick a **screen** and a **scenario** (the prebuilt scenarios from
  `tools/scenarios/scenarios.json`),
- **live-edit the JSON** that drives the screen and watch it re-render,
- drive it with **keyboard nav + mouse/touch**, plus an **on-screen joystick**
  (D-pad, center-select, KEY1/2/3) so hardware-input mode works with no keyboard,
- switch **resolution** (240×240 / 320×240 / 480×320), toggle **input mode**,
  **zoom**, and go **fullscreen**.

It shares its LVGL/display/input plumbing with the native `screen_runner` via
`tools/apps/runner_core/runner_core` (SDL-free) and `tools/apps/runner_core/runner_sdl`.

## Build

Emscripten is provided by a Docker image (the official `emscripten/emsdk`
extended with **ccache** — see `Dockerfile`), so no host toolchain is needed:

```bash
bash tools/apps/web_runner/build.sh
# or a different default resolution:
DISPLAY_WIDTH=480 DISPLAY_HEIGHT=320 bash tools/apps/web_runner/build.sh
```

The first run pulls the base image and compiles the SDL2 port + LVGL; ccache and
the Emscripten port cache are persisted under `tools/apps/web_runner/.ccache` and
`.emcache`, so subsequent builds are fast. Output is a single self-contained
file: `tools/apps/web_runner/build-wasm/index.html` (the `.wasm` is inlined, fonts are
compiled in, and the scenario catalog is embedded — no runtime fetches).

## Run

**No server (local):** open `build-wasm/index.html` directly in a browser
(double-click). Because the bundle is single-file with nothing to fetch, this
works over `file://`.

**Remote development / sharing:** serve it over HTTP and open from any browser —
this is the intended loop when building on a remote machine. `serve.sh` binds all
interfaces (`0.0.0.0`) so the page is reachable from external clients on the
**LAN** and the **tailnet**, and prints the URLs:

```bash
bash tools/apps/web_runner/serve.sh          # port 8000 (or: serve.sh 9000)
#   Local:   http://localhost:8000/index.html
#   LAN:     http://192.168.x.y:8000/index.html
#   Tailnet: http://<machine>.<tailnet>.ts.net:8000/index.html
```

(If a host firewall is active, allow the port, e.g. `sudo ufw allow 8000/tcp`.)

## CI / GitHub Pages

The screenshot gallery and this playground are published together by a single
workflow, `.github/workflows/pages.yml`, using the **official** GitHub Pages
action (`actions/upload-pages-artifact` + `actions/deploy-pages`):

- Combined site: gallery at `https://<owner>.github.io/<repo>/`, playground at `…/play/`.
- **Deploys only on push to `main` / manual dispatch** (trusted, post-merge).
- **`pull_request` runs are read-only**: they build the site (uploaded as the
  `site` artifact to download and open locally — it's `file://`-runnable) and run
  a read-only screenshot regression diff (base vs PR) surfaced in the job summary
  + a `screenshot-diff` artifact. No PR ever holds a write token.

Security notes: the top-level token is `contents: read`; only the deploy job
takes the minimal `pages: write` + `id-token: write` scopes (no `contents:
write` anywhere, nothing pushes to a branch). All actions are pinned to commit
SHAs. **One-time setup:** Settings → Pages → Source = "GitHub Actions"; optionally
restrict the `github-pages` environment to `main` for platform-enforced
protection. See `docs/knowledge/fork-pr-ci-token-permissions.md`.

## How it fits together

- `web_runner.cpp` — Emscripten entry point: sets up the SDL canvas + LVGL via
  `runner_core`, runs the cooperative main loop, exposes the exported C API
  (`ss_load_screen`, `ss_set_resolution`, `ss_set_input_mode`, `ss_send_key`,
  `ss_get_width/height`), and forwards screen result callbacks to JavaScript
  (`window.ssOnResult`).
- `shell.html` — the page chrome: screen/scenario selectors, JSON editor,
  on-screen joystick, results log, and the JS glue that calls the exported API.
- `gen_scenarios.py` — expands `tools/scenarios/scenarios.json` (base + variations,
  RFC 7396 merge-patch) into the embedded `window.SS_SCENARIOS` catalog.
- `CMakeLists.txt` — reuses the LVGL + seedsigner sources and the shared
  `runner_core`/`runner_sdl`, links with `-sUSE_SDL=2 -sSINGLE_FILE=1 -fexceptions`.

## Notes

- C++ exceptions are enabled (`-fexceptions`) because the screens throw on
  malformed JSON; `ss_load_screen` catches and keeps the last good render, and
  the editor pre-validates with `JSON.parse`.
- Touch works on phones/tablets via SDL2 touch→mouse emulation; the page sets
  `touch-action: none` and disables pinch-zoom so gestures drive the screen.
