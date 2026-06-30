# Camera Live-Preview Overlay — Zone/Spec Definition

This repo is the **spec home** for the UI drawn over the camera's live preview (the
`seedsigner-micropython-builder/docs/camera-pipeline-integration-plan.md` "Overlay
definition"). The overlay is defined as a **declarative spec, split from rendering**,
so one spec can be realized ≥3 ways while the look stays faithful to the Python
`ScanScreen` (pixel-parity requirement).

Source: [`components/seedsigner/camera_preview_overlay.h`](../components/seedsigner/camera_preview_overlay.h)
· [`camera_preview_overlay.cpp`](../components/seedsigner/camera_preview_overlay.cpp).
Tooling host: `camera_preview_overlay_screen()` in
[`components/seedsigner/seedsigner.cpp`](../components/seedsigner/seedsigner.cpp).

## Why "leveraged differently" from a full screen

A normal screen owns all its pixels. The camera overlay does **not**: the camera
preview is a separate, fast-updating layer the overlay sits on top of. So the overlay
is a **passive view** with an update API, positioned relative to a preview-square rect
the host supplies — never a self-contained screen that redraws the camera.

### Two layers, two clocks
| Layer | Rate | Owner |
|---|---|---|
| Camera preview square | camera fps (~10) | `esp-camera-pipeline` engine + `esp-board-common` display adapter |
| **This overlay** | a few updates/sec | this repo (spec + renderer); host pushes state |

The overlay never polls the camera or the decoder. The host (Python, via the binding
layer) raises the status bar and advances progress:

```
ScanView / CameraScanner  (a few times/sec)
  payload = camera_manager.qr_poll()      # non-blocking ring buffer
  decode_qr.add(payload)
  overlay.set_scanning(True)              # on first animated-QR part
  overlay.set_progress(decode_qr.get_percent_complete(), frame_status)
```

### Definition vs rendering (the split)
The spec declares **what** shows and **where** (relative to the square); it carries no
rotation, stride-blit, dummy-draw, or black-keyed-alpha detail. One spec, ≥3 renderers:
- **ESP Path A** — these LVGL widgets composited over the camera image object.
- **ESP Path B** — LVGL stopped (dummy-draw); the same spec is rendered to an
  off-screen canvas and composited manually by `esp-board-common`.
- **Pi Zero** (later) — PIL or CPython-LVGL.

ESP-specific rendering stays in `esp-board-common`. See its
`docs/knowledge/text-overlay-architecture.md` ("Unified Display Zone Renderer").

## The spec (`camera_preview_overlay_spec_t`)
| Field | Meaning |
|---|---|
| `instructions_text` | Full hardware-mode bottom line; the host composes the whole string (e.g. `"< back  \|  Scan a QR code"`), already localized. Ignored in touch mode. |
| `square_x/y/w/h` | Preview rect within the parent. The host passes the real geometry. The tooling default is **resolution-dependent**: higher-resolution DSI panels (short dimension > 240 — the 480×320 / 800×480 touch displays) **default to a landscape center-cut square** with static side gutters; the **Pi Zero (≤240) fills the display** (Python `ScanScreen` parity — `render_rect` defaults to the whole canvas). Squaring the DSI panels is intentional, not opt-in: those panels have per-frame update limits along their long axis, so keeping the gutters static minimizes the redrawn long-axis span. `fill_landscape` overrides this in either direction (`true` opts a DSI panel into full landscape width; `false` forces a square on the Pi Zero); `square` sets an explicit rect. |
| `scanning_active` | `false` → back affordance; `true` → status bar. |
| `progress_percent` | 0–100 animated-QR percent. |
| `frame_status` | `NONE`/`ADDED`/`REPEATED`/`MISS` — most-recent frame, drives the status dot. Mirrors Python `ScanScreen.FRAME__*`. |

Input mode is **not** a spec field: it is read at render time from
`input_profile_get_mode()`.

## State model

```
            not scanning                         scanning
        ┌────────────────────┐   set_scanning(true)   ┌────────────────────┐
        │ back affordance     │  / set_progress(...)  │ status bar          │
        │  (mode-dependent)   │ ────────────────────► │  (both modes)       │
        └────────────────────┘ ◄──────────────────── └────────────────────┘
                                  set_scanning(false)
```

- **Back affordance** is input-mode dependent:
  - `INPUT_MODE_HARDWARE` (joystick): on-preview, bottom-center `instructions_text`
    with a 1px drop shadow (Python parity). Joystick LEFT → back is wired by the host.
  - `INPUT_MODE_TOUCH`: the shared `back_button()` (the *same* definition the top nav
    uses — `components.cpp`) placed alone in the parent's **top-left gutter**, outside
    the square so it never overlaps live pixels and needs no per-frame recompositing.
    The instruction text is omitted.
- **Status bar** appears in **both** modes once progress is reported, **clipped to the
  square** (not the full landscape width). In hardware mode it replaces the
  instruction text (mutually exclusive, like Python); in touch mode the gutter back
  button persists.

## Pixel-parity layout (vs Python `ScanScreen`, scan_screens.py)
All values use the profile-scaled layout macros (`gui_constants.h`) relative to the
square, so proportions hold across 240/320/480.

| Element | Geometry |
|---|---|
| Status-bar container | width `square_w − 2·EDGE_PADDING`, height `BUTTON_HEIGHT`, bottom-inset by `EDGE_PADDING`; black `bg_opa≈191` (75%), radius `BUTTON_RADIUS`. |
| Progress track | thickness `LIST_ITEM_PADDING` (≈4px), `INACTIVE_COLOR`, width `bar − 2·EDGE_PADDING − width("100%") − EDGE_PADDING/2`, vertically centered, pill radius. |
| Progress fill | same track geometry, width `percent·track/100`, `GREEN_INDICATOR_COLOR`. |
| Percent label | right-aligned at `−EDGE_PADDING`, vertically centered, `BUTTON_FONT`, `BODY_FONT_COLOR`. |
| Status dot | `≈10px` (scaled), top-right just above the bar; `SUCCESS_COLOR` (added) / `INACTIVE_COLOR` (repeated) / hidden (miss/none); 1px black outline. |
| Instruction text | bottom-center of the square, `BUTTON_FONT`, `BODY_FONT_COLOR`, black shadow at +2px. |

`INACTIVE_COLOR` (#414141) and `GREEN_INDICATOR_COLOR` (#00ff00) were added to
`gui_constants.h` to match the Python `GUIConstants` of the same names.

## Performance note (transparency)
The status bar's `bg_opa≈191` means it must be **re-blended each camera frame** while
visible (the pixels under it change every frame), unlike an opaque element. The
footprint is small and the cost is bandwidth-bound, so it does not move the ~10 fps
ceiling — but the actual cost is a **render-path property** (Path A = LVGL alpha blend;
Path B = a real per-frame blend, not black-keyable) owned and measured by
`esp-board-common`. The touch back button is free (static gutter, no camera beneath).

## Testing without hardware
`camera_preview_overlay_screen` synthesizes a placeholder gray square so the overlay
renders standalone in all three tools (screenshot generator, native runner, WASM web
runner) via the shared `runner_core` registry. Scenario JSON drives the state
(`scanning`, `progress`, `frame_status`, `fill_landscape`, `square`); the preview
geometry defaults per resolution (DSI panels = center square, Pi Zero = full display),
and `fill_landscape` overrides either way. The back affordance follows the tool's
per-resolution input mode (240 = hardware, larger = touch). See
`tools/scenarios/scenarios.json` → `camera_preview_overlay_screen` (the `force_square`
variation forces the square at a Pi-class resolution; the `fill_landscape` variation
forces full landscape width on the DSI panels).
```
make_body … screenshot_gen --scenarios-file tools/scenarios/localized/<locale>.json --out-dir <dir>
```

## Not in this layer
- The result-flash headline (`✓ #N  B bytes`) — ESP debug HUD, lives in `esp-board-common`.
- ESP rotation / gutter compositing / dummy-draw / black-keyed alpha (`esp-board-common`).
- The production `CameraScanner`/`ScanView` consumer wiring (lands later in `seedsigner`).
