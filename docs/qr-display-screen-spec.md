# `qr_display_screen` — spec / integration reference

Native LVGL QR display, parity with the Python `QRDisplayScreen`. Chrome-free full-bleed:
a centered QR square (black modules on a brightness-gray background) with black gutters on
non-square displays. This is the reference for host integrators (the `seedsigner` app's
`QRDisplayView`); the authoritative C signatures live in `components/seedsigner/seedsigner.h`.

## Encoder
qrcodegen (Nayuki) — **already bundled inside the LVGL submodule** (`third_party/lvgl/src/libs/
qrcode/`); we enable `LV_USE_QRCODE` and call it directly (not the `lv_qrcode` widget, which
hardcodes ECC=MEDIUM + byte mode + a variable quiet zone). Always **ECC=L**, `boostEcl=false`,
`mask=auto`, fixed **2-module** quiet zone — mirroring Python's `qrencode -l L` / non-boosting
`qrcode` lib. The module matrix is painted onto an `lv_canvas` (RGB565), integer-scaled to the
display's short dimension.

> Parity note: any spec-compliant encoder is scannable; auto-mask choice may differ from the
> device's `libqrencode`. Verified against the Python `qrcode` lib: SeedQR/xpub/BBQR come out
> byte-identical, CompactSeedQR matches on content+version (mask aside).

## JSON config
```jsonc
{
  "qr_data": "<first frame payload>",   // REQUIRED, non-empty
  "qr_mode": "auto",                     // numeric | alphanumeric | byte | auto
  "data_encoding": "utf8",               // utf8 | hex | base64 — how qr_data is decoded
  "border": 2,                            // quiet-zone modules (default 2)
  "initial_brightness": 62,               // 31..255 (host seeds from SETTING__QR_BRIGHTNESS)
  "show_brightness_tips": true,           // default true
  "brighter_text": "Brighter",            // REQUIRED when show_tips — pre-translated (hardware)
  "darker_text": "Darker",                // REQUIRED when show_tips — pre-translated (hardware)
  "input": { "mode": "hardware" },        // usually omitted; defaults to the display profile
  "tips_visible": false                   // demo aid: show the tip persistently (no auto-hide)
}
```

### Modes and encodings, by SeedSigner QR type
| QR type | qr_mode | data_encoding | why |
|---|---|---|---|
| SeedQR | `numeric` | `utf8` | 4-digit word-index string → numeric = SeedQR-standard version |
| CompactSeedQR | `byte` | `hex` | raw bytes can't cross JSON; hex-encode them |
| xpub / address / signed msg | `byte` (or `auto`) | `utf8` | mixed-case base58 → byte mode |
| BBQR (`B$…`) | `auto` | `utf8` | base32+uppercase → `auto` picks alphanumeric (denser) |
| UR (`ur:…`) | `byte` (or `auto`) | `utf8` | lowercase → byte |

`auto` picks the most compact standard mode the payload allows (numeric > alphanumeric > byte),
matching Python `qrcode`'s auto-detect. Binary payloads must arrive **hex** or **base64**
(JSON can't carry raw/NUL bytes); `data_encoding` only applies to `qr_data`, not to pushed frames.

## Animation (host-driven)
The UR **fountain** sequence is stateful Python and generates parts on the fly, so frames are
**not** precomputed. The screen renders the initial `qr_data`; the host pushes each subsequent
frame:

```c
void qr_display_set_frame(const void *data, size_t len);  // raw bytes; no data_encoding
```

Cadence + the tip/hold/restart orchestration below live in the host (~6 FPS, i.e. Python's
`sleep(5/30)`). A static QR simply never calls `set_frame`.

## Brightness tip + animation gating (Python parity)
- The brightness panel is shown **on start** (auto-hides) and briefly after each brightness change.
  Hardware = up/down chevrons + translated **Brighter/Darker** text (physical KEY_UP/DOWN adjust;
  any other key exits). Touch = a draggable slider flanked by dim/bright **sun** icons, plus a
  top-right **X** close in the gutter.
- **The host must HOLD the animation while `qr_display_is_tip_active()` is true, and RESTART the
  sequence from frame 0 when it clears.** Rationale: joystick brightness isn't obvious (so greet
  the user with the tip), and for a UR fountain the first parts are the pure, full-data frames —
  holding on them and re-delivering after any brightness change maximizes their scan value.

```c
bool qr_display_is_tip_active(void);        // host polls; hold frames while true
void seedsigner_lvgl_on_qr_brightness(uint8_t brightness);  // weak; fires per change + on exit
```

`on_qr_brightness` is the host's cue to `encoder.restart()` (and to persist
`SETTING__QR_BRIGHTNESS`). Exit reports the final value and fires
`seedsigner_lvgl_on_button_selected(SEEDSIGNER_RET_BACK_BUTTON, "qr_display_done")`.

## Desktop tools
The interactive runners have no Python host, so a scenario may carry a **`test_frames`** array;
`runner_core` stands in for the host — cycling frames at ~6 FPS, holding on `is_tip_active`, and
restarting on the tip-clear edge. The screenshot generator renders the initial frame statically
(the on-start tip is live-only; `tips_visible` composites it into a still). Scenarios:
`tools/scenarios/scenarios.json` → `qr_display_screen` (seedqr / compactseedqr / xpub_export /
animated_bbqr).

## Build flag
`LV_USE_QRCODE=1` in each consumer (the four desktop-tool CMakeLists are done here; downstream:
`seedsigner-raspi-lvgl` `setup.py` `define_macros`, and `seedsigner-micropython-builder`
`CONFIG_LV_USE_QRCODE=y` per board — `CONFIG_LV_USE_CANVAS=y` too). No LVGL source-list edits are
needed; the recursive glob already compiles `qrcodegen.c`.
