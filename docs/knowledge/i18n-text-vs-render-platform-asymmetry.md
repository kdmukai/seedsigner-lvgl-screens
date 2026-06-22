# i18n is two layers, and Layer 1 is asymmetric across platforms

## The one-sentence version
Showing a localized screen takes **two independent layers** — *text translation*
(msgid → translated `msgstr`) and *rendering* (fonts + glyph shaping). This repo
owns **only the rendering layer**, which never translates: it renders whatever
already-translated string the label holds. Producing that translated string is
the host's job, and the host story differs by platform — that asymmetry is the
thing to keep straight.

## The two layers

| Layer | What it produces | Where it lives | Inputs |
|---|---|---|---|
| **1 — text** | the translated `msgstr` for a msgid, with the correct plural form | the **Python host** (`seedsigner/compat/l10n`) | gettext `.mo` catalogs |
| **2 — render** | fonts + (for complex scripts) pre-shaped glyph runs to *draw* that string | **this repo** (`locale_loader.cpp`, `glyph_runs.cpp`, fonts) | the locale font packs (`*.ttf` + `runs.bin`) |

Layer 2 is `ss_load_locale()` + the glyph-run table. It is a **runtime switch**
(see [locale-switch-font-lifecycle.md](locale-switch-font-lifecycle.md)) and is
**done + validated on both platforms** — all currently-built packs render GREEN on
Pi Zero (CPython `.so`) and ESP32-P4 (MicroPython).

## Why the render layer can NOT do translation (the load-bearing invariant)
The glyph-run table is **keyed by the translated `msgstr`**, not the English
msgid (`glyph_runs.h` / `glyph_runs.cpp`: "keyed by the TRANSLATED string ... the
host supplies" it). A finished LVGL label holds translated text; the run table
matches on that text. So the string must *already be translated* before it reaches
C. Two consequences:

1. **`runs.bin` is not a translation dictionary.** It maps `msgstr → glyph run` —
   keyed *by* the translated text. You need the translated string to look one up,
   so it can never be the *source* of the translation.
2. **Plural selection can't be deferred to C.** Choosing the right plural form
   needs `n` *and* the locale's `Plural-Forms` rule at the call site. The render
   layer only ever sees a final string — it has lost `n`. So plural selection must
   happen in the text layer, on the host.

This is a deliberate split, not an omission: translation = host Python; shaping =
this repo. There is no competing plan to translate inside the C layer.

## The platform asymmetry (Layer 1)
- **Pi Zero (CPython):** Layer 1 = stdlib `gettext` selected by the `LANGUAGE`
  env var (`compat/l10n.set_locale`). Works today.
- **ESP32-P4 (MicroPython):** stock MicroPython has no `gettext` and no `.mo`
  on the import path, so the shim is currently an **English identity passthrough**
  — the app hands the render layer English msgids. A real on-device `.mo` reader +
  `Plural-Forms` evaluator is **specced but not yet built** (spec Deliverables A+B,
  in the other two repos — see below).

Net: the font packs (Layer 2) are needed *identically* on both platforms and carry
**no** source-of-truth translations. What differs is purely how the host obtains
the translated string (Layer 1).

## Status of the cross-repo deliverables (spec: `docs/i18n-ngettext-micropython-spec.md`)
| Deliverable | What | Repo | Status |
|---|---|---|---|
| Layer 2 render + runtime switch | fonts/glyph-run load + swap | **this repo** | ✅ done, both platforms |
| **C** — all-plural-form shaping | offline pipeline emits a run for every plural form (not just `msgstr[0]`) | **this repo** (`tools/i18n/`) | ✅ done + validated |
| **A** — on-device `.mo` catalog | `.po`→`.mo` build + on-device placement + integrity | `seedsigner` (build) + `seedsigner-micropython-builder` (deploy) | ❌ not built |
| **B** — MicroPython `.mo` reader + `Plural-Forms` engine | real `gettext`/`ngettext` on device behind the unchanged shim API | `seedsigner` (`compat/l10n` + new module) | ❌ not built |

Companion notes for A/B:
- `seedsigner/docs/knowledge/micropython-i18n-text-layer.md` (Deliverable B + the build half of A)
- `seedsigner-micropython-builder/docs/knowledge/micropython-i18n-mo-catalog-deploy.md` (the device/firmware half of A)

## Practical consequence for an interactive language-settings screen
- **Pi:** achievable now — wire the native Layer-2 call into the locale switch
  (Layer 1 already works), gate the picker to shipped packs, let navigation rebuild
  the screen.
- **ESP32-P4:** the picker would load the chosen script's **fonts** but text stays
  **English** until Deliverables A+B land. It degrades gracefully, not to tofu:
  English msgids fall back to the baked OpenSans Western floor that each locale pack
  chains under (`locale_fonts.h`), so menus read as correct English, just
  untranslated.

A truthful scope line: *interactive language selection is fully achievable on Pi
now; on ESP32-P4 it renders the chosen script's fonts but shows English strings
until the MicroPython `.mo` reader (spec Deliverables A+B) is implemented.*

## Coupling to remember
The `.mo` strings (Layer 1) and the `runs.bin` glyph runs (Layer 2) must be built
from the **same translation revision** — the shaper pre-bakes runs keyed to
specific translated strings, so a stale `runs.bin` against a newer `.mo` mis-matches
and falls back to unshaped/tofu. Separate files, separate load paths, content-locked
together.
</content>
</invoke>
