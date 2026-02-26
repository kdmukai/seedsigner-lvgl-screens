# Performance Checkpoint: Stable ~13 FPS Display Render

Date: 2026-02-26
Branch: `rebuild/from-lvgl-seedsigner-components`

## Known-good configuration

The current stable visual state is ~13 FPS with correct UI colors and no severe frame corruption.

### `components/display_manager/display_manager.cpp`
- `disp_cfg.buffer_size = LCD_BUFFER_SIZE`
- `disp_cfg.trans_size = LCD_BUFFER_SIZE / 4`
- `disp_cfg.flags.buff_dma = false`
- `disp_cfg.flags.buff_spiram = true`

## Visual correctness
- Color mapping is corrected:
  - Continue button: orange
  - List buttons: gray

## LVGL debug config
- Keep PERF/MEM monitor enabled
- Disable REFR debug flashing

## Reverted experiment (documented)
An attempted DMA/internal row-buffer configuration caused severe display corruption (garbled lower section and partial updates). That configuration was reverted to restore stable rendering.

## Rationale
This checkpoint prioritizes stable, visually correct output over aggressive transfer-path optimization. Further DMA/internal buffering work should be treated as a separate experiment with explicit validation gates before adoption.
