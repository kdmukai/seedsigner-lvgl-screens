# SeedSigner C Modules

Platform-agnostic LVGL screen implementations for [SeedSigner](https://github.com/SeedSigner/seedsigner), the air-gapped Bitcoin signing device. This repo contains the shared rendering layer that runs on both the Raspberry Pi Zero (via CPython extension) and ESP32-S3 microcontrollers (via MicroPython firmware).

## Role in the SeedSigner Ecosystem

SeedSigner is evolving from a Pi Zero-only Python application into a dual-platform system. The same Python business logic and the same LVGL screens run on both hardware targets. This repo is the shared piece in the middle:

```
seedsigner-c-modules  (this repo: LVGL screens in C/C++)
       |                          |
       v                          v
seedsigner-raspi-lvgl       seedsigner-micropython-builder
(CPython .so for Pi Zero)   (MicroPython firmware for ESP32-S3)
       |                          |
       v                          v
            seedsigner  (Python business logic)
      (runs on both Pi Zero and ESP32-S3)
```

**[seedsigner](https://github.com/SeedSigner/seedsigner)** -- The production Python application (views, models, business logic). Currently renders screens with PIL; migrating to consume LVGL screens from this repo. Target: run on both CPython 3.10 (Pi Zero) and MicroPython 1.27.0 (ESP32-S3).

**[seedsigner-raspi-lvgl](https://github.com/kdmukai/seedsigner-raspi-lvgl)** -- Compiles the platform-agnostic screens from this repo into a CPython C extension (`.so`) for the Pi Zero. Provides the ST7789 SPI display backend and GPIO input handling. Docker QEMU ARMv6 cross-compilation.

**[seedsigner-micropython-builder](https://github.com/kdmukai/seedsigner-micropython-builder)** -- Build orchestration for ESP32-S3 firmware. Patches MicroPython v1.27.0 + ESP-IDF v5.5.1, integrates this repo as a git submodule, and produces flashable firmware for the Waveshare ESP32-S3 Touch LCD boards.

Both platforms call the same screen functions with the same JSON config:
```python
# Pi Zero (CPython C extension)
seedsigner_lvgl_native.button_list_screen(cfg_dict)

# ESP32-S3 (MicroPython C module)
seedsigner_lvgl.button_list_screen(cfg_dict)
```


## What's in This Repo

### Screens and Components (`components/seedsigner/`)

The core of the repo: platform-agnostic LVGL v9.5.0 screen implementations built in C/C++.

| File | Purpose |
|---|---|
| `seedsigner.cpp` | Screen entry points: `main_menu_screen`, `button_list_screen`, `screensaver_screen` |
| `components.cpp` | LVGL widget builders: top nav bar, buttons, button lists, large icon buttons |
| `navigation.cpp` | Hardware input navigation (joystick + KEY1-3), focus management, grid/list layouts |
| `input_profile.cpp` | Touch vs hardware input mode selection |
| `gui_constants.h/cpp` | Display profiles, colors, fonts, layout constants, PX_MULTIPLIER scaling |

Screens are JSON-configured and return results via callback, keeping the C layer stateless and the Python layer in control of application flow.

### Desktop Tools (`tools/`)

Development and testing without hardware. See the [Desktop Tools](#desktop-tools) section below.

### LVGL (`third_party/lvgl/`)

Git submodule pinned to LVGL v9.5.0.


## Display Profiles

The UI scales across multiple display sizes via a `PX_MULTIPLIER` system. All layout constants (padding, button height, font sizes) are defined as base values scaled by the multiplier. Font files are pre-rendered at each target size.

| Display | Resolution | PX_MULTIPLIER | Notes |
|---|---|---|---|
| Pi Zero (1.3" SPI) | 240x240 | 100 | Base scale, no-op -- matches original Python UI |
| Pi Zero (2.0" SPI) | 320x240 | 100 | Wider but same height, same scaling |
| ESP32-S3 (3.5" touch) | 480x320 | 150 | Aesthetic upscale, slightly larger than linear |
| ESP32-S3 (4.3" touch) | 800x480 | 200 | Matched physical sizing across DPI differences |

The active profile is selected at runtime with `set_display(width, height)`. Hardware builds compile in only the profile they need; desktop tools compile all profiles and switch between them dynamically.


## Input Modes

Screens adapt their behavior based on the active input mode:

- **Hardware** -- Joystick (4-direction + press) and physical keys (KEY1, KEY2, KEY3). Navigation highlights move between focusable elements. This is the Pi Zero input model.
- **Touch** -- Direct tap interaction with buttons. No navigation highlight until an element is touched. This is the ESP32-S3 touchscreen model.

The input mode is set at runtime and can be overridden per-screen via JSON config. The screen runner desktop tool supports toggling between modes with the `T` key.


## JSON Screen Configuration

Screens accept a JSON dictionary that controls their content and behavior. Example for `button_list_screen`:

```json
{
  "top_nav": {
    "title": "Network",
    "show_back_button": true
  },
  "button_list": [
    {"label": "Mainnet", "value": "main"},
    {"label": "Testnet", "value": "test"},
    {"label": "Regtest", "value": "regtest"}
  ],
  "initial_selected_index": 0
}
```

The `tools/scenarios.json` file defines all screen configurations used by the screenshot generator and screen runner.


## Desktop Tools

Each tool has its own README with build instructions and usage details:

- **[Screenshot generator](tools/screenshot_generator/README.md)** -- Renders every screen at every resolution to PNG. Used in CI to detect visual regressions via before/after comparison.
- **[Screen runner](tools/screen_runner/README.md)** -- Interactive SDL2 desktop application for live screen testing with keyboard navigation, mouse/touch input, and runtime resolution switching.


## CI/CD

CI runs on GitHub Actions, GitLab CI, Codeberg, and Forgejo. All platforms use a shared script (`scripts/ci/ci.sh`) for consistency.

On every push and pull request:
- Builds the screenshot generator and screen runner
- Generates screenshots at all resolutions
- On PRs: compares against the base branch and posts a visual diff report showing changed, new, and removed screenshots

On merge to main:
- Deploys the screenshot gallery to the platform's pages site


## ESP-IDF Integration

For ESP32-S3 builds, `components/seedsigner/` is an ESP-IDF component registered via `idf_component_register()`. The `seedsigner-micropython-builder` repo includes this repo as a git submodule and compiles the screens into the MicroPython firmware.

The `usercmodule.cmake` file at the repo root registers MicroPython user C modules. The `bindings/` directory contains the MicroPython bindings that expose screen functions to Python. (Both are being migrated to `seedsigner-micropython-builder` as part of the ongoing platform separation.)
