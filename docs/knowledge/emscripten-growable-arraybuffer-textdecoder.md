# web_runner: blank UI from emscripten growable memory + TextDecoder

## Symptom
The WASM web_runner (`tools/apps/web_runner`) loads but the **whole UI is dead** — the
language and scenario dropdowns are empty, nothing renders. The browser console shows:

```
Uncaught (in promise) TypeError: Failed to execute 'decode' on 'TextDecoder':
  The provided ArrayBuffer value must not be resizable
    at UTF8ArrayToString (index.js)
    at UTF8ToString (index.js)
    at Object.getSource (index.js)
    at _emscripten_glShaderSource (index.js)
    ...
    at callMain (index.js)
```

The assets are fine (locales.json / scenarios all fetch HTTP 200, valid JSON) — this is a
**client-side WASM init abort**, not a data problem.

## Root cause
Modern emscripten (observed on **6.0.2**) backs `-sALLOW_MEMORY_GROWTH=1` with a **resizable
`ArrayBuffer`** (the setting `GROWABLE_ARRAYBUFFERS=1`, default in em6 — in-place growth via
`ArrayBuffer.prototype.resize`). Browsers' `TextDecoder.decode()` **rejects a view whose
backing buffer is resizable** ("must not be resizable"). Emscripten's `UTF8ArrayToString`
passes exactly such a view (`HEAPU8.subarray(...)`), and it's hit during **SDL2/WebGL shader
compilation** (`glShaderSource` → `getSource` → `UTF8ToString`) inside `callMain`. The app
aborts there — before the JS app script populates the dropdowns — so the UI never initializes.

A browser update (Chrome/V8 making WASM growable memory use resizable ArrayBuffers) is what
flips a previously-working bundle into this failure with the **same** emscripten/build.

## Fix
Force the **legacy copy-on-grow** memory mechanism, so the heap is a normal (non-resizable)
`ArrayBuffer` that `TextDecoder` accepts — memory still grows (needed for tiny_ttf's
rasterizer), just via allocate-new-buffer + `updateMemoryViews` instead of in-place resize:

```cmake
# tools/apps/web_runner/CMakeLists.txt  (target_link_options)
-sALLOW_MEMORY_GROWTH=1
-sGROWABLE_ARRAYBUFFERS=0
```

**`-sTEXTDECODER=0` does NOT work on em6** — that value was removed; it must be `1` or `2`, and
neither disables the TextDecoder path. `GROWABLE_ARRAYBUFFERS=0` is the correct lever.

## Verify without a browser
Grep the regenerated glue — the resizable-buffer markers must be gone:

```
grep -c "maxByteLength" build-wasm/index.js   # -> 0
grep -c "\.resize("     build-wasm/index.js   # -> 0
grep -oE "growMemory|resize_heap|updateMemoryViews" build-wasm/index.js  # legacy path present
```

## Gotcha within the gotcha
`tools/apps/web_runner/build.sh` ends its in-Docker command chain with `... || true`, so a
**failed link still exits 0** and prints "Done". Always grep the build log for `error:` — a
green "Done" does not mean the bundle relinked. (This masked a first, wrong attempt at
`-sTEXTDECODER=0`, whose link error left the old bundle in place.)
