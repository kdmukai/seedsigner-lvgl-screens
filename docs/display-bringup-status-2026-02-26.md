# Display Bring-up Status — 2026-02-26

## Scope
Waveshare ESP32-S3 Touch LCD 3.5B display bring-up through custom C module (`lcdaxs`) integrated into MicroPython ESP32 port.

## What is working
- Build/flash pipeline is stable for board `WAVESHARE_ESP32_S3_TOUCH_LCD_35B`.
- Custom module import works in REPL.
- `lcdaxs` function calls return expected status values (mostly `0`).
- Backlight control works (`power(0/1)` visibly toggles backlight).
- PMU-related code path compiles after adding missing XPowers `REG/` headers.

## Current blocker
- No visible pixel output on the panel despite successful return codes from:
  - `lcdaxs.init()`
  - `lcdaxs.power(1)`
  - `lcdaxs.fill(...)`
  - `lcdaxs.bsp_selftest()`
- Observed behavior: brief initial flash/garble, then black panel with backlight on.

## Key diagnostics so far
- Earlier `fill()` failures with `257` (`ESP_ERR_NO_MEM`) were fixed by moving away from full-frame DMA allocations.
- Orientation/gap/mirror/invert sweeps produced no visible output even with all calls returning `0`.
- Full BSP init table was restored verbatim; still no sustained output.
- Current likely root cause is PMU/power-rail enable mismatch (I2C pin/bus/config mismatch), with init path not hard-failing on PMU issues.

## Attempts made (high level)
1. Minimal lcd module and bindings.
2. Added `espressif/esp_lcd_axs15231b` dependency and board wiring constants.
3. Multiple init strategies (custom table, full BSP table, mode forcing, reduced clocks, warm-reinit).
4. Added tuning APIs (`set_swap_xy`, `set_mirror`, `set_gap`, `set_invert`, MADCTL/COLMOD setters).
5. Added C-side `bsp_selftest()` render path.
6. Integrated PMU bring-up bridge (`bsp_i2c` + `bsp_axp2101` + XPowersLibInterface).

## Recommended next approach
- Pivot to PMU-first validation before any display draw calls:
  1. Confirm PMU init success explicitly and fail-fast if PMU init fails.
  2. Verify/adjust PMU I2C pins for this board (candidate mismatch: copied 7/8 vs board expectation 9/8).
  3. Add explicit PMU diagnostics callable from REPL.
  4. Only then re-test display draw path.

## Useful files
- Module core: `components/display_axs15231b/src/display_axs15231b_core.c`
- Bindings: `components/display_axs15231b/modlcdaxs_bindings.c`
- PMU bridge: `components/display_axs15231b/pmu/`
- XPowers vendored: `components/display_axs15231b/third_party/XPowersLib/src/`
