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

## Fix (what this repo does): pin emscripten to match CI
The real trigger is **version drift**. CI pins emscripten to **3.1.74**
(`.github/workflows/pages.yml`, `emscripten-core/setup-emsdk`), but the local Docker build used
`EMSDK_TAG=latest` (→ em6). em **3.1.74 grows memory the old way** (allocate-new-buffer +
`updateMemoryViews`, a non-resizable ArrayBuffer), so it has **no** TextDecoder problem at all.
So the fix is to make the local build reproducible with CI, not to patch around em6:

- `tools/apps/web_runner/Dockerfile` + `build.sh`: `EMSDK_TAG` pinned to **3.1.74**.
- **No `-sGROWABLE_ARRAYBUFFERS=0` flag.** That setting only exists on em6+; passing it on
  3.1.74 is a hard error — `Attempt to set a non-existent setting: 'GROWABLE_ARRAYBUFFERS'` —
  which is exactly how the first attempt (adding the flag) broke CI while "working" locally on em6.

If the toolchain is ever intentionally moved to em6+ (in BOTH local and CI — keep them equal),
*then* `-sGROWABLE_ARRAYBUFFERS=0` is the lever to force the legacy non-resizable heap that
TextDecoder accepts. `-sTEXTDECODER=0` is NOT a valid alternative on em6 (removed; must be 1 or 2).

**Lesson:** pin the WASM toolchain and keep local == CI. A flag that fixes a "latest"-only
symptom can be invalid on the pinned CI version and turn a green local build into a red CI.

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
