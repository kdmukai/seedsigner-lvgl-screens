# Upstream Deviations (Minimal)

This branch keeps `components/` copied from `lvgl_seedsigner_modular_test` as-is and only adds the minimum glue needed for MicroPython integration.

## 1) MicroPython binding layer (added)
- `bindings/moddisplay_manager_bindings.c`
- `bindings/modseedsigner_bindings.c`
- `bindings/micropython.cmake`
- `usercmodule.cmake`

Purpose:
- Expose minimal display functionality to MicroPython:
  - `display_manager.init()`
  - `display_manager.mem_stats()`
  - `seedsigner_lvgl.demo_screen()`

Why shim exists:
- MicroPython binding remains in C for compatibility.
- `display_manager` APIs are C++ symbols, so a tiny C++ shim exports C-callable wrappers.
- LVGL lock ownership is centralized in `run_screen(...)`; seedsigner UI calls are executed through this path so neither seedsigner nor MicroPython needs lock awareness.

## 2) No behavioral edits in imported components
- `components/` were imported from source project and kept unchanged for this checkpoint.

## 3) Build wiring outside this repo (required)
These changes live in the MicroPython ESP32 tree and are required for this branch to build/run:
- `ports/esp32/CMakeLists.txt`: add `EXTRA_COMPONENT_DIRS` to point to this repo's `components/`
- `ports/esp32/main/CMakeLists.txt`: append `display_manager` to `IDF_COMPONENTS`
- `ports/esp32/main/idf_component.yml`: add managed deps needed by imported stack (`esp_codec_dev`, `esp_io_expander_tca9554`, `esp_lcd_touch`)
- board `sdkconfig.board`: enable required LVGL font options

These are documented here so the integration remains reproducible.
