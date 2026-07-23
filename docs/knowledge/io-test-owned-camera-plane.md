# io_test_screen owns its camera pixel-plane (behind scaffold chrome)

## The problem
`io_test_screen` (the hardware self-test) shows a captured camera still behind its chrome
(D-pad + KEY1/2/3). The original banner assumed the ESP "two-clock model": a board adapter
blits the camera into a plane *behind* the LVGL layer, and the screen is pure chrome on top.

That model was **never actually wired** — and could not be, because the scaffold gives every
top-nav screen an **opaque** background (`screen_scaffold.cpp` sets the screen root **and**
`create_standard_body_content` sets the body to opaque `BACKGROUND_COLOR`). An adapter drawing
"behind LVGL" has nothing to show through. On the Pi there is no adapter at all. So on both
platforms KEY1's grab/preview had nowhere to render.

## The fix: the screen owns the plane, the host blits into it
Mirror the owned-plane model of `seedsigner-raspi-lvgl/native/python_bindings/camera_preview.cpp`
— but *inside the screen* (io_test builds a full scaffolded screen; it isn't a black-box overlay
the platform can layer over its own plane). The screen creates an RGB565 `lv_image` with a stable
backing buffer and exposes a host-push API:

- `io_test_get_camera_plane_dims(w, h)` — the exact frame size the host must produce.
- `io_test_blit_camera(rgb565, nbytes)` — memcpy into the buffer, unhide, invalidate.
- `io_test_set_capture_state(IO_TEST_CAPTURE_IDLE)` — re-hide (the KEY2 "Clear").

This matches what Python's `IOTestScreen` actually does: a **single-frame grab on KEY1** pasted
as the background (no live feed). One model now serves both Pi and ESP — the ESP host just blits
its captured still into the same plane. The plane starts **hidden**, so the screen is byte-identical
to the pre-plane chrome on any platform whose host hasn't wired the blit yet (no regression).

## The reusable technique: a full-bleed plane BEHIND opaque scaffold chrome
The scaffold's body is opaque, and the chrome (D-pad in `body`, keys/band on the screen root)
is created *before* the screen adds anything. To slip a full-bleed pixel plane **under** all of
it:

1. Make the body **transparent** — `lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, LV_PART_MAIN)`.
   The screen root's own opaque `BACKGROUND_COLOR` now provides the backdrop, so the hidden state
   looks unchanged.
2. Parent the plane to the **screen root** (full-bleed, escaping the body's `EDGE_PADDING` clip,
   the same reason the "Capturing…" band is root-parented).
3. Send it to the background — `lv_obj_move_to_index(plane, 0)`. It then draws first: under the
   top_nav (opaque, covers the plane's top strip), under the transparent body (the plane shows
   through the body region), and under the keys/band (created later → higher indices → on top).

## Why a CENTERED SQUARE (not a full landscape frame)
The ESP dev boards mount the camera **rotated 90°** from our landscape screen, so the sensor
yields a square/portrait frame in landscape. The host **center-crops to a square** and fills the
plane edge-to-edge — no stretch, no letterbox. The plane side = the display's short dimension:

- **Square Pi (240²):** square == whole screen → full-bleed, like Python's paste.
- **Wide panels (320×240 / 480×320 / 800×480):** the square is horizontally centered and
  **pillarboxed** — the left/right gutters fall outside the plane and show the root
  `BACKGROUND_COLOR` (through the transparent body).

The plane's top is deliberately tucked under the (opaque) top_nav — a camera self-test only needs
to prove the camera works, so cropping a few rows behind the nav is fine and lets us use the
largest square that fits the height.

## Consumers
`io_test_get_camera_plane_dims` + `io_test_blit_camera` are the contract the Pi binding
(RASPI-2 gap 3) and a future ESP hardware-button build call. io_test is hardware-input only
(`INPUT_MODE_HARDWARE` forced; the app gates it off touch-only builds), so on the Pi the square
degenerates to full-bleed; the pillarbox path is only reachable on a wide hardware-button build.
