# Spec: real `gettext`/`ngettext` plural support on MicroPython

**Status:** Design / hand-off. **Audience:** the session evolving `seedsigner/src/seedsigner/compat/l10n.py` (the MicroPython compat shim) and the i18n offline pipeline in `seedsigner-lvgl-screens/tools/i18n/`.

**Why now:** `ngettext` was just added to the shim — real `gettext.ngettext` on CPython, and an **English-plural-rule identity passthrough** (`_identity_ngettext`, `singular if n==1 else plural`) on MicroPython. That passthrough is correct as a *fallback* but does **no real translation and no per-locale plural selection**. This spec defines the real MicroPython implementation so it can be designed in before the interim shim is frozen. **The passthrough is kept** — the real impl is additive and degrades to it.

**Release context (see `i18n-lvgl-release-model.md`):** the migration is a **full LVGL cutover, no hybrid, no PIL fallback**. The immediate sprint is a **working MicroPython prototype** (English passthrough is fine for that bar). This spec's deliverables (real Layer-1 translation + all-plural-form shaping) are **production** prerequisites: a production release must reach **parity with the already-shipped language set** (V0.8.7: hi, th, vi, …) — so they gate a production LVGL release, not the prototype. Urdu's RTL glyph-run cases are deferred (ur is not yet a shipped language).

---

## 1. Goal / non-goals

**Goal:** On MicroPython (ESP32-S3), `ngettext(singular, plural, n)` and `gettext(msgid)` return the correctly **translated** string for the active locale, with the plural form selected by **that locale's** `Plural-Forms` rule — matching CPython/Pi Zero behaviour, behind the *unchanged* `compat/l10n.py` public API.

**Non-goals:**
- Changing the public API of `compat/l10n.py` (`gettext`, `ngettext`, `bindtextdomain`, `textdomain`, `set_locale`). Call sites stay as-is.
- Glyph rendering/shaping — that is the LVGL layer's job (`seedsigner-lvgl-screens`). This spec only produces the correct **string**; §6 covers the hard dependency it creates there.
- CPython behaviour — Pi Zero keeps stdlib `gettext`. The new code is the MicroPython path (and is unit-tested on CPython, see §7).

## 2. Why the passthrough is insufficient (motivation, with evidence)

`_identity_ngettext` returns the **English** source and selects with the **English** rule (`n==1`). Two failures:

1. **No translation** — on ESP32 every plural renders in English.
2. **Wrong form for most locales** — plural rules and form-counts vary. From the shipped catalogs:
   - `hi`: `nplurals=2; plural=(n != 1)` → `इनपुट` / `इनपुट्स`
   - `ru`: `nplurals=4; plural=(n%10==1 && n%100!=11 ? 0 : …)` → 4 forms
   - `pl`: `nplurals=4` (different rule) → 4 forms
   - `th`: `nplurals=1` → 1 form
   An English-collapsed result cannot reconstruct ru/pl's 4 forms — the form must be chosen **where `n` is known**, with the **target locale's** rule. This cannot be deferred to the render layer (it sees only a final string, not `n`).

## 3. Architecture decision (please confirm against the device-localization plan)

`compat/l10n.py` documents that "device-side localization is handled later through the LVGL font/locale seam." This spec resolves that into a clean split:

- **Text translation + plural selection → the `compat/l10n` layer** (this spec). Real on-device `gettext`/`ngettext` reading per-locale catalogs. *Because plural selection structurally requires `n` + the locale rule at the call site.*
- **Glyph/font selection + shaping → the LVGL layer** (`seedsigner-lvgl-screens`, already built). It shapes whatever **translated** string the label holds.

This keeps the invariant the LVGL run-table already relies on — **a finished label holds the translated `msgstr`** (the device's glyph-run table is keyed by translated text). So making `gettext` real on MicroPython is not just for plurals; it's what makes the label hold translated text at all. **Open question for the other session:** confirm there is no competing plan to translate inside the LVGL/C layer (which would conflict — see §10).

## 4. Deliverables

| # | Deliverable | Repo |
|---|---|---|
| A | On-device per-locale catalog: format, build step (`.po`→catalog), on-device location, integrity | `seedsigner` build + translations |
| B | MicroPython catalog reader + `Plural-Forms` engine behind the shim | `seedsigner/src/seedsigner/compat/l10n.py` (+ a new module) |
| C | **Offline shaping-pipeline companion change** — emit shaped runs + glyph coverage for *all* plural forms | `seedsigner-lvgl-screens/tools/i18n/` |

A and B make the device translate; C is required so complex-script plural forms actually render (see §6). All three ship together or complex-script locales regress.

## 5. Deliverable A — on-device catalog

**Format: standard gettext `.mo`** (recommended). Rationale: it already carries everything needed (the `Plural-Forms` rule in the metadata entry, plural translations as NUL-separated forms), Babel already produces it for CPython, it gives byte-for-byte parity with the Pi Zero catalog, and the reader is ~100 lines. A bespoke compact binary (à la `runs.bin`) is a *later* footprint optimisation, not a starting point — do not pre-optimise (measure first).

`.mo` essentials the reader must handle:
- Magic `0x950412de` (LE) / `0xde120495` (BE) → pick byte order.
- Header: `N` strings, offset of the original-strings table, offset of the translation table (each entry = `(length, offset)`).
- Plural entries: the **original** is `"singular\0plural"`; the **translation** is `"form0\0form1\0…\0form{nplurals-1}"`. The lookup key is the **singular** (the bytes before the first `\0`).
- The **metadata entry** has empty msgid; its translation is a MIME-ish header containing `Plural-Forms: nplurals=K; plural=(EXPR);`. Parse `K` and `EXPR` once at load.
- Context (`msgctxt`) entries use `ctxt\x04msgid` — SeedSigner doesn't use `pgettext`, so this can be ignored/unsupported (document it).

**Build step:** compile `.po`→`.mo` (Babel `compile_catalog` or `msgfmt`) per locale; this can reuse the existing Babel integration (`setup.py`). `.mo` are build artifacts (none are committed today — 22 `.po`, 0 `.mo`).

**On-device location & loading:** the `.mo` is a **Python-side asset** read by the compat layer (not the C font-pack loader). It lives on flash/SD per locale (e.g. `…/l10n/<locale>/LC_MESSAGES/messages.mo`), mirroring `bindtextdomain('messages', localedir=path)`. On the signing device it must be **integrity-checked before parse** (signature/hash), like the font packs — the reader must also be defensively bounds-checked regardless (treat the file as untrusted: validate every offset/length against file size, fail closed to the fallback). Keep `set_locale()` writing the locale state; the reader resolves the path from `localedir` + locale (as `bindtextdomain`/`textdomain` already wire up).

## 6. Deliverable C — offline shaping pipeline (hard dependency, `seedsigner-lvgl-screens`)

Once the device renders **translated plural forms**, the LVGL layer must be able to shape each of them for complex-script locales. Today it cannot:

- `tools/i18n/po_catalog.py::parse_entries` reads **only `msgstr[0]`** and explicitly ignores `msgstr[1..N]` and `msgid_plural`.
- Verified consequence: in the shipped `hi` pack, `इनपुट` (msgstr[0]) **has** a glyph run; `इनपुट्स` (msgstr[1]) **does not** → it would tofu / render unshaped on device.

Required changes (all reuse the same plural ingestion):
1. **`po_catalog.py`** — emit *all* plural forms. Suggest a new `parse_plural_entries()` yielding `(singular_msgid, [msgstr0, msgstr1, …])`, and have `corpus_chars` include every form's glyphs (so the font subset/closure covers them).
2. **`shape_inventory.py` / `build_fontpacks.py`** — build a shaping unit per plural form (each is its own `plain` or `segmented` string), add each form's glyphs to the decomposition/subset closure, and emit a run keyed by each translated form. The device already matches by final text, and the segmented `{}`-hole path already shapes inserted translated words — so a plural word slots in once it has a run; **no device-side C change** beyond having the runs.
3. **`validate_runs.py` / manifest** — extend the no-`.notdef` assert and counts to cover plural forms.

Note: `gen_localized_scenarios.py` also uses `parse_catalog` (singular only); the desktop gallery therefore only ever shows the singular form today — fine for now, but the validation gate above is the real coverage guarantee.

## 7. Deliverable B — reader + `Plural-Forms` engine (the core)

### 7.1 Module shape

Add a pure-Python module (e.g. `seedsigner/compat/_mo.py`) usable on **both** interpreters (so it is CPython-unit-testable), depending only on `int.from_bytes` (avoid `struct`/`re`; both are MicroPython-limited). `compat/l10n.py` wires it in:

```
on import:
    if real gettext available (CPython):   gettext/ngettext = stdlib  (unchanged)
    else (MicroPython):
        gettext   = catalog.gettext   if a catalog is loaded else _identity
        ngettext  = catalog.ngettext  if a catalog is loaded else _identity_ngettext
```

`set_locale()` / `bindtextdomain()` trigger a (lazy) catalog load for the locale; load failure → keep the passthrough fallback (never crash a screen over i18n).

### 7.2 Lookup semantics (fallback chain — keep the passthrough as the floor)

`ngettext(singular, plural, n)`:
1. No catalog / locale is source language / `singular` not found / its translation empty → **fallback** `singular if n==1 else plural` (the existing passthrough).
2. Found → `idx = plural_index(n)` (§7.3); `forms = translation.split('\0')`; return `forms[idx]` if `0 <= idx < len(forms)` else `forms[0]` (clamp; never index-error).

`gettext(msgid)`: catalog hit → translation; else `msgid` (identity).

### 7.3 `Plural-Forms` evaluator (the one genuinely tricky, security-sensitive piece)

The header gives a C expression over the single variable `n`. **Do not use `eval()`/`exec()`** — both for safety on a signing device (catalogs are external input even if signed; defence-in-depth) and because the result must be *bounded*. Implement a tiny parser/evaluator:

- **Grammar** (gettext subset): integer literals, `n`, parentheses, unary `!`, binary `* / %`, `+ -`, relational `< > <= >=`, equality `== !=`, `&&`, `||`, and the ternary `?:` (right-assoc). No assignment, no identifiers other than `n`, no function calls.
- **Approach:** recursive-descent over the standard C precedence ladder (`?:` < `||` < `&&` < `==` < relational < additive < multiplicative < unary), producing a small AST (or directly evaluating). Booleans are 0/1 ints (C semantics). Parse **once** when the catalog loads; store the AST/closure; evaluate per call (O(expr) on a tiny tree — negligible).
- **Hardening:** reject unknown tokens; cap expression length and paren depth; if `nplurals` or the expression is missing/invalid → fall back to `(n != 1)` with `nplurals=2` (the C/Germanic default) and log once. Clamp the final index into `[0, nplurals)`. A malicious/garbled catalog can then at worst pick a wrong (but valid) form — never execute code or fault.

A compact recursive-descent evaluator for exactly this grammar is ~120 lines and has no dependencies.

## 8. Test plan (port from / extend the tests the shim work just added)

Unit tests run on CPython (the pure module is interpreter-agnostic):
- **Plural-rule vectors** — for representative rules, assert `plural_index(n)` for `n ∈ {0,1,2,3,4,5,11,21,22,25,100,101,111}`:
  - `en`/`hi`/`de` `(n!=1)` → `{1→0, else→1}`
  - `ru` 4-form, `pl` 4-form (distinct), `ar` 6-form (`nplurals=6`), `fr` `(n>1)`, `ja`/`th`/`zh` `nplurals=1`→always 0.
- **`.mo` reader** — round-trip a Babel-compiled `messages.mo`: known msgid→msgstr; the `input/inputs` plural entry returns the right form per `n` for hi and ru; missing msgid → fallback; empty translation → fallback.
- **Reader robustness** — truncated/bad-magic/over-length-offset `.mo` → fall back, no exception.
- **Fallback parity** — with no catalog, `ngettext`/`gettext` behave exactly like the current passthrough (guards the other session's committed behaviour).
- **End-to-end on the live instance** — `ngettext("input","inputs",n)` in `psbt_screens.py` for several `n` under hi (Devanagari) and ru (Cyrillic).

## 9. MicroPython constraints
- Stock MP 1.27 has no `gettext`; `re`/`struct` are limited — the `.mo` reader and evaluator must avoid them (use `int.from_bytes`, hand tokenisation).
- Memory: a `.mo` for SeedSigner's ~few-hundred strings is ~20–50 KB. Start simple — parse into a dict on locale switch (a few tens of KB); if footprint demands, switch to binary-search over the resident `.mo` bytes (no dict) — same time↔memory trade-off `runs.bin` made, and the same "measure before optimising" discipline.
- Catalog bytes routed to PSRAM on ESP32 where applicable (consistent with the font-pack/PSRAM work).

## 10. Open questions for the other session
1. **Architecture confirm (§3):** is text translation intended to live in `compat/l10n` (this spec) rather than the LVGL/C layer? If the C layer were to translate, the two would conflict on what a label holds.
2. **Catalog acquisition:** reuse `bindtextdomain` `localedir` + a flash/SD path? And the integrity/signature mechanism for the `.mo` on the signing device — share the font-pack verification, or a separate one?
3. **Format:** OK to ship standard `.mo` (recommended), or is a compact custom catalog wanted from day one? (Recommend `.mo` first.)
4. **Sequencing:** A+B (this repo path: `seedsigner`) and C (`seedsigner-lvgl-screens` offline pipeline) must land together for complex-script locales. Who owns C? (It is self-contained and desktop-verifiable in `seedsigner-lvgl-screens`; this session can do it.)

## 11. Summary
Keep the just-added passthrough as the fallback. Add (A) a shipped per-locale `.mo`, (B) a defensive pure-Python `.mo` reader + a non-`eval` `Plural-Forms` evaluator behind the unchanged shim API, and (C) the offline pipeline change so every plural form gets a shaped glyph run. Result: correct, per-locale, plural-aware translation on ESP32, identical to Pi Zero, with complex scripts rendering correctly.
