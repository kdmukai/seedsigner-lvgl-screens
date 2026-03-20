# LVGL v8 → v9 Migration Plan for seedsigner-c-modules

## Context

SeedSigner's shared LVGL screen code (`components/seedsigner/`) has been migrated from LVGL v8.4.0 to v9.5.0. The sibling 2048-esp32 project was migrated first and its migration plan (`/home/kdmukai/dev/misc/2048-esp32/lvgl-v9-migration-plan.md`) served as the primary reference.

This migration touched 4 areas of the repo:
1. **Platform-agnostic screens** (the shared code) — **complete**
2. **ESP32 LVGL port + display manager** — **deferred to `seedsigner-micropython-builder`** (ESP32 code is being extracted from this repo)
3. **Desktop tools** (screenshot generator, screen runner) — **complete**
4. **Image asset generation script** — **complete**

The MicroPython bindings (`bindings/modseedsigner_bindings.c`) have **zero LVGL API calls** — they only call `run_screen()` via `display_manager.h` — so they need no changes.

### Lessons Learned from 2048-esp32 Migration

These issues were encountered and fixed during the 2048-esp32 LVGL v9 migration (commits `4324b29`..`2a3732c` and `c4a89d5`..`1832402`):

1. **ARM NEON assembly breaks x86 desktop builds**: LVGL v9 enables ARM NEON assembly by default. Desktop builds on x86 must either add `-DLV_USE_DRAW_SW_ASM=LV_DRAW_SW_ASM_NONE` as a compile definition, or delete `.S` files from the LVGL source tree before building. The seedsigner-c-modules desktop tools glob `${LVGL_ROOT}/src/*.c` — the glob doesn't pick up `.S` files, but CMake may still try to compile them if they're in the source tree. Add the compile definition to be safe.

2. **Desktop color format: keep RGB565, not ARGB8888**: The 2048-esp32 desktop build uses `LV_COLOR_FORMAT_RGB565` with `LV_COLOR_DEPTH 16`, matching the ESP32 display format. The flush callback receives `uint8_t *px_map` containing 16-bit RGB565 pixels (pitch = `w * 2`). This is simpler than switching to ARGB8888 and keeps visual parity with the hardware. However, seedsigner-c-modules currently uses `LV_CONF_SKIP` without an `lv_conf.h` — need to decide whether to add one or configure via compile definitions.

3. **`lv_conf.h` vs `LV_CONF_SKIP` for desktop builds**: The 2048-esp32 ended up using an explicit `lv_conf.h` with widget enables and `LV_COLOR_DEPTH 16`. The seedsigner-c-modules desktop tools currently use `LV_CONF_SKIP`. In v9, `LV_CONF_SKIP` uses all defaults (including `LV_COLOR_DEPTH 32`). To keep RGB565 rendering, we should either: (a) add an `lv_conf.h` for desktop builds, or (b) add `-DLV_COLOR_DEPTH=16` and other needed defines via `target_compile_definitions`. Option (b) keeps things closer to the current approach.

4. **Timer `auto_delete` heap corruption**: v9 timers default to `auto_delete=true`. When a timer's `repeat_count` reaches 0, LVGL frees the timer object — any subsequent access to the handle is use-after-free. The seedsigner screensaver uses `lv_timer_create()` with no `repeat_count` (infinite), so it won't hit this. But if future code adds one-shot timers, use `lv_timer_set_auto_delete(timer, false)`.

5. **Widget config names in `lv_conf.h`**: `LV_USE_BTN` → `LV_USE_BUTTON`, `LV_USE_IMG` → `LV_USE_IMAGE`, `LV_USE_BTNMATRIX` → `LV_USE_BUTTONMATRIX`, `LV_USE_IMGBTN` → `LV_USE_IMAGEBUTTON`, `LV_USE_ANIMIMG` → `LV_USE_ANIMIMAGE`, `LV_USE_COLORWHEEL` → removed.

6. **v8-only `lv_conf.h` options removed in v9**: `LV_COLOR_16_SWAP`, `LV_MEM_CUSTOM` block, `LV_DPI_DEF`, `LV_TXT_ENC`.

7. **`LV_CONF_INCLUDE_SIMPLE`**: For CI builds that install LVGL as a system library, v9 needs `LV_CONF_INCLUDE_SIMPLE` so `lv_conf_internal.h` can find `lv_conf.h` via include paths.

### Verified v9 API Reference

All renames below have been confirmed against LVGL v9.5.0 source and documentation:

| v8 | v9 | Confirmed |
|----|----|----|
| `lv_btn_create()` | `lv_button_create()` | Yes |
| `&lv_btn_class` | `&lv_button_class` | Yes |
| `lv_obj_del()` | `lv_obj_delete()` | Yes |
| `lv_indev_get_act()` | `lv_indev_active()` | Yes |
| `lv_coord_t` | `int32_t` (typedef removed) | Yes |
| `lv_mem_alloc()` / `lv_mem_free()` | `lv_malloc()` / `lv_free()` | Yes |
| `lv_memset_00()` | `lv_memzero()` | Yes |
| `lv_img_create()` | `lv_image_create()` | Yes |
| `lv_img_set_src()` | `lv_image_set_src()` | Yes |
| `LV_IMG_DECLARE()` | `LV_IMAGE_DECLARE()` | Yes |
| `lv_img_dsc_t` | `lv_image_dsc_t` | Yes |
| `LV_IMG_CF_TRUE_COLOR` | `LV_COLOR_FORMAT_RGB565` | Yes |
| `lv_txt_get_size()` | `lv_text_get_size()` | Yes — **renamed** |
| `lv_disp_get_hor_res()` | `lv_display_get_horizontal_resolution()` | Yes |
| `lv_disp_get_ver_res()` | `lv_display_get_vertical_resolution()` | Yes |
| `indev->proc.state` | `lv_indev_get_state(indev)` | Yes — struct is now opaque |
| `lv_disp_drv_t` / `lv_disp_drv_init()` / `lv_disp_drv_register()` | `lv_display_create()` + `lv_display_set_flush_cb()` + `lv_display_set_buffers()` | Yes |
| `lv_indev_drv_t` / `lv_indev_drv_init()` / `lv_indev_drv_register()` | `lv_indev_create()` + `lv_indev_set_type()` + `lv_indev_set_read_cb()` | Yes |
| `lv_disp_flush_ready(drv)` | `lv_display_flush_ready(disp)` | Yes |
| `LV_INDEV_STATE_PR` / `LV_INDEV_STATE_REL` | `LV_INDEV_STATE_PRESSED` / `LV_INDEV_STATE_RELEASED` | Yes |
| `lv_color_t` (16-bit with `LV_COLOR_DEPTH=16`) | `lv_color_t` (always 3 bytes, RGB888) | Yes — **major change** |
| `LV_COLOR_16_SWAP` | Removed; use `LV_COLOR_FORMAT_RGB565_SWAPPED` at runtime | Yes |

**Unchanged in v9** (confirmed):
- `LV_COORD_MAX` — still exists
- `LV_TEXT_FLAG_NONE` — still exists
- `LV_LABEL_LONG_SCROLL_CIRCULAR` — still exists (NOT renamed)
- `LV_LABEL_LONG_CLIP` — still exists (NOT renamed)
- `lv_color_black()` — still exists
- `lv_color_to32()` — still exists
- `lv_refr_now()` — still exists
- `lv_obj_get_scroll_top()` / `lv_obj_get_scroll_bottom()` / `lv_obj_scroll_by()` — unchanged
- `LV_FONT_DECLARE()` / `lv_font_t` — unchanged
- `LV_CONF_SKIP` — still works in v9

---

## Part 1: Update LVGL Submodule

Update `third_party/lvgl/` from current v8.x commit (`4495f428`) to the `v9.5.0` tag.

```
cd third_party/lvgl
git fetch --tags
git checkout v9.5.0
```

Everything else depends on this.

---

## Part 2: Platform-Agnostic Screen Code (components/seedsigner/)

These are mechanical API renames. The screen architecture (JSON config → scaffold → nav bind → load) is unchanged.

### 2a: seedsigner.cpp (~20 changes)

| Line | v8 | v9 |
|------|----|----|
| 18 | `LV_IMG_DECLARE(seedsigner_logo_img)` | `LV_IMAGE_DECLARE(seedsigner_logo_img)` |
| 89 | `lv_obj_del(old_screen)` | `lv_obj_delete(old_screen)` |
| 344, 348-349, 413-416, 512-514, 582-583 | `lv_coord_t` | `int32_t` |
| 450 | `indev->proc.state == LV_INDEV_STATE_PRESSED` | `lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED` |
| 536, 559 | `lv_mem_alloc()` / `lv_mem_free()` | `lv_malloc()` / `lv_free()` |
| 546-547 | `lv_disp_get_hor_res(NULL)` / `lv_disp_get_ver_res(NULL)` | `lv_display_get_horizontal_resolution(NULL)` / `lv_display_get_vertical_resolution(NULL)` |
| 551-552 | `seedsigner_logo_img.header.w` / `.h` | Fields still exist in v9's `lv_image_header_t` — no change needed |
| 554 | `lv_img_create(scr)` | `lv_image_create(scr)` |
| 555 | `lv_img_set_src(...)` | `lv_image_set_src(...)` |
| 560 | `lv_memset_00(ctx, sizeof(*ctx))` | `lv_memzero(ctx, sizeof(*ctx))` |
| 626 | `lv_obj_clean(lv_scr_act())` | unchanged |

### 2b: components.cpp (~15 changes)

| Line(s) | v8 | v9 |
|---------|----|----|
| 21, 278, 317 | `lv_btn_create()` | `lv_button_create()` |
| 152, 231, 245, 307, 386 | `&lv_btn_class` | `&lv_button_class` |
| 171 | `lv_indev_get_act()` | `lv_indev_active()` |
| 20, 85, 95-96, 104 | `lv_coord_t` | `int32_t` |
| 111 | `lv_txt_get_size(&text_size, ..., LV_COORD_MAX, LV_TEXT_FLAG_NONE)` | `lv_text_get_size(&text_size, ..., LV_COORD_MAX, LV_TEXT_FLAG_NONE)` — only the function name changes; `LV_COORD_MAX` and `LV_TEXT_FLAG_NONE` are unchanged |

### 2c: navigation.cpp (~8 changes)

| Line(s) | v8 | v9 |
|---------|----|----|
| 258, 261, 272, 282, 284 | `lv_mem_alloc()` / `lv_mem_free()` | `lv_malloc()` / `lv_free()` |
| 274 | `lv_memset_00()` | `lv_memzero()` |

### 2d: gui_constants.h — No changes needed

`LV_FONT_DECLARE()` and `lv_font_t` are unchanged in v9.

---

## Part 3: Desktop Tools

### Key v9 change: `lv_color_t` is now 3 bytes (RGB888) internally

In v8 with `LV_CONF_SKIP`, `lv_color_t` was 2 bytes (RGB565). In v9, `lv_color_t` is **always 3 bytes (RGB888)** regardless of display color format. However, the display's configured color format determines what pixel format is used in draw buffers and flush callbacks.

**Approach (matching 2048-esp32)**: Use `LV_COLOR_FORMAT_RGB565` for the display. The flush callback receives `uint8_t *px_map` containing 16-bit RGB565 pixels. This keeps visual parity with the ESP32 hardware and minimizes changes to the framebuffer/conversion code.

Since the current codebase uses `LV_CONF_SKIP` (no `lv_conf.h`), we need to explicitly set `LV_COLOR_FORMAT_RGB565` via `lv_display_set_color_format()`. We should also add `-DLV_USE_DRAW_SW_ASM=LV_DRAW_SW_ASM_NONE` to disable ARM NEON assembly on x86 (lesson from 2048 CI fixes).

### 3a: Display init rewrite (both tools)

**Current v8 pattern** (screenshot_gen.cpp:510-520, screen_runner.cpp:819-829):
```cpp
static lv_disp_draw_buf_t disp_buf;
std::vector<lv_color_t> draw_buf((size_t)g_width * 40u);
lv_disp_draw_buf_init(&disp_buf, draw_buf.data(), NULL, (uint32_t)draw_buf.size());
static lv_disp_drv_t disp_drv;
lv_disp_drv_init(&disp_drv);
disp_drv.hor_res = g_width;
disp_drv.ver_res = g_height;
disp_drv.flush_cb = lvgl_flush_cb;
disp_drv.draw_buf = &disp_buf;
lv_disp_t *disp = lv_disp_drv_register(&disp_drv);
```

**v9 replacement (matching 2048-esp32 pattern):**
```cpp
lv_display_t *disp = lv_display_create(g_width, g_height);
lv_display_set_flush_cb(disp, lvgl_flush_cb);
lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
// Buffer size in bytes — full screen × 2 bytes per pixel (RGB565)
static std::vector<uint8_t> draw_buf((size_t)g_width * (size_t)g_height * 2);
lv_display_set_buffers(disp, draw_buf.data(), NULL, draw_buf.size(),
                       LV_DISPLAY_RENDER_MODE_FULL);
```

### 3b: Flush callback rewrite (both tools)

**v8:**
```cpp
static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p) {
    // copy lv_color_t (16-bit) pixels to framebuffer
    g_fb[di] = *color_p;
    lv_disp_flush_ready(drv);
}
```

**v9 (with RGB565 format, matching 2048-esp32):**
```cpp
static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    // px_map contains RGB565 pixels (2 bytes each)
    uint16_t *pixels = (uint16_t *)px_map;
    // copy to framebuffer...
    lv_display_flush_ready(disp);
}
```

### 3c: Framebuffer and color conversion changes (both tools)

The `g_fb` vector changes from `std::vector<lv_color_t>` to `std::vector<uint16_t>` (RGB565).

`framebuffer_to_rgb24()` needs updating: `lv_color_to32()` still exists but `lv_color_t` is now 3-byte RGB888, not 16-bit. Since our framebuffer stores raw RGB565 `uint16_t` values from the flush callback, we need to manually convert RGB565 → RGB24:
```cpp
uint16_t c = g_fb[si];
uint8_t r = (c >> 11) << 3;
uint8_t g = ((c >> 5) & 0x3F) << 2;
uint8_t b = (c & 0x1F) << 3;
```

`lv_color_black()` returns an `lv_color_t` (RGB888) — can't assign directly to `uint16_t g_fb`. For framebuffer init, use `memset(g_fb.data(), 0, g_fb.size() * sizeof(uint16_t))`.

### 3d: CMakeLists.txt (both tools)

Add to `target_compile_definitions`:
```cmake
target_compile_definitions(screenshot_gen PRIVATE
    LV_CONF_SKIP
    LV_USE_DRAW_SW_ASM=LV_DRAW_SW_ASM_NONE  # NEW: disable ARM NEON on x86
)
```

If `LV_CONF_SKIP` with v9 defaults causes missing widget errors, selectively enable with `-DLV_USE_BUTTON=1` etc. But v9 defaults enable most widgets, so this likely won't be needed.

### 3d: Input driver rewrite (screen_runner.cpp only)

**v8 (lines 832-842):**
```cpp
static lv_indev_drv_t indev_drv;
lv_indev_drv_init(&indev_drv);
indev_drv.type = LV_INDEV_TYPE_KEYPAD;
indev_drv.read_cb = keypad_read_cb;
lv_indev_drv_register(&indev_drv);
```

**v9:**
```cpp
lv_indev_t *kb_indev = lv_indev_create();
lv_indev_set_type(kb_indev, LV_INDEV_TYPE_KEYPAD);
lv_indev_set_read_cb(kb_indev, keypad_read_cb);
```

Same pattern for pointer driver. Callback signatures change:
- `keypad_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)` → `keypad_read_cb(lv_indev_t *indev, lv_indev_data_t *data)`
- `pointer_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)` → `pointer_read_cb(lv_indev_t *indev, lv_indev_data_t *data)`
- `lv_coord_t` casts in pointer_read_cb → `int32_t`
- `LV_INDEV_STATE_PRESSED` / `LV_INDEV_STATE_RELEASED` — these names are the same in v9 (the v8 shorthand `LV_INDEV_STATE_PR` / `LV_INDEV_STATE_REL` was only used in the ESP32 display_manager, not in the desktop tools)

---

## Part 4: ESP32 LVGL Port + Display Manager

### 4a: Delete `components/esp_lv_port/` entirely

This 623-line custom port is wall-to-wall v8 API (`lv_disp_drv_t`, `lv_disp_draw_buf_t`, software rotation with `LV_DISP_ROT_*`, etc.). The espressif/esp_lvgl_port v2.7.2 registry component replaces it entirely, as proven in the 2048-esp32 migration.

### 4b: Rewrite `components/display_manager/display_manager.cpp`

Current file (153 lines) depends on the custom `lv_port.h` and uses v8 APIs:
- `LV_DISP_ROT_90` / `LV_DISP_ROT_180` / `LV_DISP_ROT_NONE` → v9 rotation constants
- `lv_indev_drv_t` / `lv_indev_drv_init()` / `lv_indev_drv_register()` → v9 `lv_indev_create()` API
- `LV_INDEV_STATE_PR` / `LV_INDEV_STATE_REL` → `LV_INDEV_STATE_PRESSED` / `LV_INDEV_STATE_RELEASED`
- `lv_coord_t` in touchpad_read → `int32_t`
- `bsp_touch.h` custom touch code → replaced by `esp_lcd_touch_new_i2c_axs15231b()` from registry

The rewrite follows the 2048-esp32 pattern:
- Use `esp_lvgl_port` v2.7.2 for display init (`lvgl_port_add_disp()` v9 version)
- Touch via standard `esp_lcd_touch` + `lvgl_port_add_touch()`
- **Public API unchanged**: `init()`, `run_screen()`, `lvgl_port_lock()`, `lvgl_port_unlock()` keep same signatures so bindings need zero changes

#### AXS15231B QSPI Display — Critical Hardware Limitation

The Waveshare ESP32-S3 Touch LCD 3.5B uses the AXS15231B display controller over QSPI. This controller has a **confirmed hardware defect**: CASET (0x2A) and RASET (0x2B) commands have NO EFFECT over QSPI. The draw coordinates are ignored — `esp_lcd_panel_draw_bitmap()` always starts from (0,0).

**Consequence**: LVGL's default partial-buffer rendering produces shredded/corrupted output because dirty regions are rendered at the wrong screen position.

**Current v8 workaround** (`esp_lv_port/lv_port.c`): Uses `full_refresh = 1` with software rotation and chunked DMA transfers. The custom port software-rotates pixels in chunks then calls `esp_lcd_panel_draw_bitmap()`.

**v9 solution** (proven in 2048-esp32): Use `direct_mode` with a custom flush callback:

```c
// Display config — direct_mode keeps a persistent SPIRAM framebuffer
lvgl_port_display_cfg_t disp_cfg = {
    .buffer_size = LCD_H_RES * LCD_V_RES,  // full framebuffer in SPIRAM
    .trans_size = 0,                        // custom flush handles chunking
    .hres = LCD_H_RES, .vres = LCD_V_RES,
    .color_format = LV_COLOR_FORMAT_RGB565,
    .flags = { .buff_spiram = true, .direct_mode = true },
};
lvgl_disp = lvgl_port_add_disp(&disp_cfg);
lv_display_set_flush_cb(lvgl_disp, axs15231b_flush_cb);
```

The custom flush callback:
1. In `direct_mode`, LVGL calls flush once per dirty area but only the LAST flush should send the framebuffer (checked via `lv_display_flush_is_last(disp)`)
2. Sends the **entire framebuffer** in bands (80 rows each) to work around the RASET bug
3. Byte-swaps into DMA-capable SRAM bounce buffers (double-buffered for ping-pong) using `lv_draw_sw_rgb565_swap()`
4. The SPIRAM framebuffer is never modified by the swap — only the bounce buffer copy is swapped

**Why not other approaches** (all failed in 2048-esp32 testing):
- `full_refresh` + `swap_bytes`: software swap on 300KB buffer triggers watchdog timeout
- `full_refresh` + `LV_COLOR_16_SWAP`: v9 full_refresh re-renders entire screen every frame → watchdog
- `direct_mode` + `LV_COLOR_16_SWAP`: incompatible — LV_COLOR_16_SWAP swaps at render time, but in direct_mode only dirty pixels re-render, so unchanged pixels get double-swapped

**Touch I2C fix**: The default `ESP_LCD_TOUCH_IO_I2C_AXS15231B_CONFIG()` macro does NOT set `scl_speed_hz`, which defaults to 0 and causes `i2c_master_bus_add_device()` to fail. Must manually set `touch_io_config.scl_speed_hz = 400000` after the macro.

### 4c: Update `components/display_manager/CMakeLists.txt`

Replace `"esp_lv_port"` with `"esp_lvgl_port"` (or `"espressif__esp_lvgl_port"`) in REQUIRES.

### 4d: Delete custom touch code from `components/esp_bsp/`

Remove `bsp_touch.c` / `bsp_touch.h` and update its CMakeLists.txt — the registry `esp_lcd_axs15231b` touch driver replaces it.

### 4e: Update `bindings/micropython.cmake`

Remove the line adding `../components/esp_lv_port/include` to include directories.

---

## Part 5: Image Asset Script + Regeneration

### 5a: Update `scripts/png_to_lvgl.py`

The v9 image descriptor struct has new required fields:

```c
// v9 lv_image_dsc_t (confirmed from source):
const lv_image_dsc_t symbol = {
    .header = {
        .magic = LV_IMAGE_HEADER_MAGIC,   // NEW: must be 0x09
        .cf    = LV_COLOR_FORMAT_RGB565,   // Changed from LV_IMG_CF_TRUE_COLOR
        .flags = 0,                        // NEW
        .w     = W,
        .h     = H,
        .stride = W * 2,                   // NEW: bytes per row (width × bytes_per_pixel)
        .reserved_2 = 0,                   // NEW
    },
    .data_size = SIZE,
    .data      = symbol_map,
    .reserved  = NULL,                     // NEW: alignment field
};
```

Script changes:
- `lv_img_dsc_t` → `lv_image_dsc_t`
- `LV_IMG_CF_TRUE_COLOR` → `LV_COLOR_FORMAT_RGB565`
- Remove `.always_zero` and `.reserved` (v8 bitfields)
- Add `.magic = LV_IMAGE_HEADER_MAGIC`
- Add `.flags = 0`
- Add `.stride = W * 2` (for RGB565: width × 2 bytes/pixel)
- Add `.reserved_2 = 0`
- Add `.reserved = NULL` to outer struct

**Byte order handling**: v9 has `LV_COLOR_FORMAT_RGB565_SWAPPED` as a runtime display format. For images, we can either:
- Store only native (LE) byte order and use `LV_COLOR_FORMAT_RGB565` — the display flush callback handles swapping
- Or use `LV_COLOR_FORMAT_RGB565_SWAPPED` if the image data is pre-swapped

Since the 2048-esp32 approach handles byte swapping in the flush callback's bounce buffer, store images in native LE order only. Remove the `#if LV_COLOR_16_SWAP` dual-array approach.

### 5b: Regenerate image asset

After updating the script:
```
python3 scripts/png_to_lvgl.py docs/img/seedsigner_logo.png \
    components/seedsigner/images/seedsigner_logo_img.c \
    --height 80 --name seedsigner_logo_img
```

### 5c: Font files — no changes needed

The font .c files already contain v9-compatible conditional code paths (`LVGL_VERSION_MAJOR >= 9`). Confirmed they should compile cleanly.

---

## Implementation Status

| Part | Scope | Status |
|------|-------|--------|
| **1** | Update LVGL submodule to v9.5.0 | **Done** |
| **2** | Platform-agnostic screen API renames | **Done** |
| **3** | Desktop tools (screenshot_gen, screen_runner) | **Done** |
| **4** | ESP32 display manager + port rewrite | **Moved to `seedsigner-micropython-builder`** |
| **5** | Image asset script (`png_to_lvgl.py`) + regeneration | **Done** |

Parts 1–3 and 5 are complete and verified. CI passes on all platforms (Linux, macOS, Windows).

Part 4 (ESP32 display/touch initialization) will be implemented directly in `seedsigner-micropython-builder` as part of the planned migration of all ESP32-specific code out of this repo. The ESP32 components here (`esp_lv_port/`, `esp_bsp/`, `display_manager/`, `bindings/`) still use LVGL v8 APIs and will be removed from this repo once the micropython-builder has its own v9 implementations.

### Additional v9 API changes discovered during implementation

These were not in the original plan but were caught during compilation:

| v8 | v9 | Notes |
|----|-----|-------|
| `lv_obj_clear_flag()` | `lv_obj_remove_flag()` | OR'd flags need cast: `(lv_obj_flag_t)(A \| B)` |
| `lv_event_send(obj, ...)` | `lv_obj_send_event(obj, ...)` | Signature changed; v9 `lv_event_send` takes `lv_event_list_t*` |
| `lv_event_get_target(e)` | `(lv_obj_t *)lv_event_get_target(e)` | Returns `void*` in v9; needs explicit cast in C++ |
| `timer->user_data` | `lv_timer_get_user_data(timer)` | `lv_timer_t` is opaque in v9 |

---

## Notes for seedsigner-micropython-builder

The following information is relevant when implementing the ESP32 LVGL v9 display/touch initialization in `seedsigner-micropython-builder`. This is based on the working migration in the `2048-esp32` project and the research documented above.

### Dependencies

Update `idf_component.yml`:
```yaml
dependencies:
  lvgl/lvgl: "9.5.0"                             # was "^8"
  espressif/esp_lvgl_port: "2.7.2"               # replaces custom esp_lv_port
  espressif/esp_lcd_axs15231b: "2.1.0"           # provides touch driver
  espressif/esp_io_expander_tca9554: "2.0.3"
```

### sdkconfig.defaults — remove v8-only options

```
# REMOVE these (no longer exist in v9):
CONFIG_LV_MEM_CUSTOM=y
CONFIG_LV_TXT_ENC_UTF8=y
CONFIG_LV_COLOR_16_SWAP=y
```

### Display initialization (AXS15231B QSPI)

See Part 4b above for the full details on the AXS15231B hardware limitation and the `direct_mode` + custom flush callback solution. Key points:

- Must use `direct_mode` with full-screen SPIRAM buffer — partial rendering fails due to CASET/RASET hardware defect
- Custom flush callback sends entire framebuffer in 80-row bands with DMA bounce buffer byte-swapping
- Do NOT use `full_refresh`, `swap_bytes`, or `LV_COLOR_16_SWAP` — all fail for different reasons (see Part 4b table)
- Override flush callback after `lvgl_port_add_disp()`: `lv_display_set_flush_cb(lvgl_disp, axs15231b_flush_cb)`

### Touch initialization

- Use `esp_lcd_touch_new_i2c_axs15231b()` from the registry driver
- **Critical**: manually set `touch_io_config.scl_speed_hz = 400000` after the `ESP_LCD_TOUCH_IO_I2C_AXS15231B_CONFIG()` macro — the macro leaves it at 0 which causes `i2c_master_bus_add_device()` to fail
- Do NOT use the `_EX` variant macro — it has a parameter name collision bug
- Use `lvgl_port_add_touch()` from `esp_lvgl_port` to register with LVGL

### MicroPython bindings

The `seedsigner_lvgl` bindings (`modseedsigner_bindings.c`) have zero LVGL API calls — they only call `run_screen()`, `lvgl_port_lock()`, and `lvgl_port_unlock()` from `display_manager.h`. If the display manager's public API is preserved, the bindings need no changes.

### Timer pitfall (v9)

`lv_timer_create()` defaults to `auto_delete=true` in v9. If a timer has a finite `repeat_count`, LVGL frees the timer object when it expires. Any subsequent access to the timer handle is use-after-free. Use `lv_timer_set_auto_delete(timer, false)` for timers you intend to reset/reuse. See `docs/lvgl-timer-auto-delete.md` in the `2048-esp32` repo for the full debugging story.

### Widget config names (if using lv_conf.h)

If the micropython-builder uses `lv_conf.h` or Kconfig for widget enables:
- `LV_USE_BTN` → `LV_USE_BUTTON`
- `LV_USE_IMG` → `LV_USE_IMAGE`
- `LV_USE_BTNMATRIX` → `LV_USE_BUTTONMATRIX`
- `LV_USE_IMGBTN` → `LV_USE_IMAGEBUTTON`
- `LV_USE_ANIMIMG` → `LV_USE_ANIMIMAGE`
- `LV_USE_COLORWHEEL` → removed

---

## Verification

1. **Desktop screenshot test**: Build `screenshot_gen` after Parts 1-3+5. Generate screenshots. Compare against current v8 baseline using existing CI diff infrastructure. **Done — all 6 scenarios render correctly.**
2. **Desktop interactive test**: Build `screen_runner`. Test keyboard nav (arrow keys, enter, KEY1/2/3), mouse clicks, scroll, scenario switching, mode toggle. **Done — verified on macOS.**
3. **CI**: Both `screenshots.yml` and `screen-runner.yml` pass on all platforms. **Done.**
4. **ESP32 flash test** (Part 4, in micropython-builder): Build firmware, flash device. Verify display, touch, all screens, screensaver bounce + dismiss.
