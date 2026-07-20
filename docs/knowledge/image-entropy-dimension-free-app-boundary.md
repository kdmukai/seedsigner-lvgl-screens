# The image-entropy native↔app boundary is deliberately dimension-free

## The constraint

The captured final image crosses from the native layer into the Python app as
**raw bytes and nothing else**. The app never learns the frame's width or
height, and must not be made to.

This is not an oversight — it is what lets each camera backend choose its own
capture geometry, and change it later, with **zero app-side changes**.

## Why it holds

The app's only three uses of `final_image_bytes` are flat-byte and
length-agnostic (`seedsigner` `views/tools_views.py`,
`generate_mnemonic_from_camera_entropy`):

1. empty check — `not final_image_bytes`
2. constant-fill check — `== bytes([b[0]]) * len(b)` (rejects a *dead* sensor,
   never a *dim* one — no Shannon/variance scoring)
3. `hashlib.sha256(seed_entropy + final_image_bytes).digest()`

Hashing a byte string needs no geometry. The app receives a Python `bytes`
object, so the length travels with the data; it **cannot** recover `w` and `h`
separately from the byte count (460,800 bytes is equally 480×480, 640×360, or
800×288) — and never needs to.

Dimensions live entirely native: each backend passes explicit `src_w/src_h` to
`image_entropy_process()` at its own call site, and that call never crosses into
Python.

## The trap this creates

A length **equality** check looks like cheap defence-in-depth and is the obvious
thing to reach for:

```python
# DON'T
expected_len = ENTROPY_FRAME_WIDTH * ENTROPY_FRAME_HEIGHT * 2
if len(final_image_bytes) != expected_len:
    raise ValueError(...)
```

It couples the app to a value it cannot see and does not own. Capture geometry
is chosen in the camera backends (`seedsigner-raspi-lvgl`,
`seedsigner-micropython-builder`), and it **changes**: both backends currently
capture at preview resolution and are slated to move to a larger, wider still.
The moment either lands, an equality check hard-fails seed derivation with a
`ValueError` — in a flow where the failure surfaces as "your seed could not be
generated."

Because no single repo has the full view of the pipeline, the check would also
be *silently* wrong: the repo asserting the value is not the repo that sets it.

## What to do instead

Assert only what the app can legitimately know — a **floor**:

```python
# The smallest panel we support is 240x240 RGB565; anything shorter is a
# truncated or malformed buffer, never a legitimate capture.
MINIMUM_FINAL_IMAGE_BYTES = 240 * 240 * 2  # 115200
if len(final_image_bytes) < MINIMUM_FINAL_IMAGE_BYTES:
    raise ValueError("...")
```

A floor still catches truncated buffers, wrong-format buffers, and a preview
thumbnail slipping through, and pairs with the constant-fill check to cover
empty and dead-sensor captures. It stays correct as capture sizes **grow**,
which is the only direction they move.

(No f-strings — `tools_views.py` runs under MicroPython as well as CPython and
currently contains none.)

## Note on where the display copy fits

`image_entropy_process()` (this repo) *does* take explicit dimensions — but it
operates on a **throwaway display copy** used only to render the CONFIRM review
frame. It never touches the entropy chain and never becomes the bytes returned
to the app. Its aspect-fit, capped letterbox, and box-filter downscale are
cosmetic-only. The dimension-free rule above is about the **returned raw
latched frame**, which is a separate object.

## Related

- `components/seedsigner/image_entropy.h` — the display-copy contract.
- The entropy chain/latch invariants are specified in the cross-repo
  image-entropy native contract (local working doc, not committed).
