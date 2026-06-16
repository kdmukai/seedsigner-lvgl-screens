# Dynamic-text contract audit — runtime values in complex-script labels

_Parity-gate audit for the LVGL i18n cutover (release-model checklist item #2:
"no un-shapeable dynamic complex-script text"). Done 2026-06-16 against the
`seedsigner` Python app at `integration/lvgl-mpy`. Line numbers are from that
branch and will drift; treat them as starting points, not anchors._

## Why this matters

The old PIL renderer shaped **any** string at draw time (FreeType + libraqm), so a
view could freely splice runtime values into a translated label and Devanagari/Thai
still rendered. The LVGL path is **shape-offline, render-by-glyph-id**: the offline
pipeline pre-shapes each catalog `msgstr` into a glyph run, and the device matches a
label's text against that run table (`glyph_runs.cpp::by_text`, then the segmented
`{}`-template matcher, then a codepoint fallback). A complex-script string the
pipeline never saw has **no run** → the device falls to the codepoint path → it
draws each code point with no GSUB/GPOS → combining marks and conjuncts land wrong =
tofu. So the contract is: **every complex-script label handed to an LVGL screen must
be a catalog `msgstr`, a segmented template the device can fill, or ASCII/numeric.**

## Headline result

**The contract holds.** No site splices *arbitrary runtime non-corpus* text into a
complex-script label, except four external-data fields where SeedSigner makes **no
i18n guarantees** (all acknowledged). Every other dynamic hole is numeric/ASCII or a
**closed-set translated word** (a catalog msgid).

But the audit surfaced a *distinct, narrower* class that is **not** a contract
violation yet still breaks under LVGL: translated **corpus** words assembled via
**f-string / concatenation** into a composite that is not itself a catalog msgid
(category C). And the offline build already flags a small `unsupported` template
tail (category D). Both are hi/th parity items; neither involves arbitrary
non-corpus text.

## A. Dynamic holes that are safe (the bulk — ~30 sites)

- **Numeric / ASCII**: counts, indices, percentages, dice/coin/bit entropy
  (`Your input: "{}"`), fingerprints (hex), addresses (base58/bech32), xpubs, BIP39
  words (English wordlist), derivation paths, `{}/{}` page counters, `{}x{}` module
  counts. ASCII renders via the baked OpenSans floor, never the script subset.
- **Closed-set translated words** (each a standalone catalog msgid → has its own
  run): `_("change")`/`_("self-transfer")`, the network display name
  (mainnet/testnet/regtest), the denomination/units (`btc`/`sats`/`tBtc`/`tSats`),
  the sig/script-type display names (`Single Sig`, `Native Segwit`, …), and settings
  option display names. Bounded and translatable — not arbitrary.

The `_("...").format(value)` templates (e.g. `_("input {}")`, `_("{} change")`,
`_("Transaction's {} address could not be verified…")`) are catalog msgids, so the
offline pipeline classifies them as `segmented` and the device fills the hole at
render time — the proven `{}`-template mechanism (commit e454c86). A direct-attribute
sweep (`text = x.label/.name/.message/.data`) found **nothing**: no view prints a
user-defined label/name straight into a complex-script field.

## B. External data — no i18n guarantees (all acknowledged, all fine)

| Field | Site | Notes |
|---|---|---|
| Settings-QR optional title | `settings_screens.py:333` `text=f'"{self.config_name}"'` | Already commented "User-supplied string (from SettingsQR); don't wrap to translate." Momentary display, not stored, not mission-critical. |
| OP_RETURN message | `psbt_screens.py:726` / `:739` | `op_return_data.decode(errors="strict")` — valid UTF-8 (incl. **non-ASCII**) is shown decoded; only invalid UTF-8 falls back to `.hex()`. So non-ASCII-but-valid is shown best-effort, *not* hex. No guarantees either way. |
| Signed-message text | `seed_screens.py:1620` `sign_message_data["message"]` (paged) | Arbitrary message-signing payload. May contain unsupported glyphs; accepted. |
| Python exception text | exception handler | Inherently English/ASCII; naturally safe. |

## C. Translated corpus words in non-catalog composites — a migration GUARDRAIL, not a bug list

These build a complex-script string by **f-string / concat** of translated words +
separators. The pieces are corpus (each is a catalog msgid with a run), but the
**composite** is never a msgid, so `by_text` misses, the segmented matcher has no
template, and it tofus. Worked under PIL+libraqm; breaks under LVGL.

**Do NOT patch these in place.** Four of the five live in the legacy PIL **screen**
classes, which are exactly what the LVGL migration replaces. In the LVGL model the
screen receives already-translated text via JSON — it assembles nothing; the **view**
(host code) builds the final string. So this assembly relocates into the view as part
of the migration, and *that* is the moment to express it as a `_()` template instead
of an f-string. The list below is the guardrail for that refactor, not a to-fix-now
list against dying code.

1. `psbt_screens.py:681` (screen) — `f"{addr_type} {index_num_symbol}{index}"`, where
   `addr_type = _("change address")` / `_("receive address")` (validated runs:
   चेंज पता / प्राप्ति पता). Comment even says "NOT marking this for translation,
   hoping … var ordering will not need to change" — a named-placeholder template
   removes that worry (translators control order).
2. `seed_screens.py:1447` (screen) — `f"{sig_type} - {script_type}"` (+ `f" ({network})"`);
   all three are translated display names. The conditional network suffix needs a
   second template (or a mainnet variant).
3. `scan_screens.py:59` (screen) — `"< " + _("back") + "  |  " + _(self.instructions_text)`.
4. `tools_screens.py:107` (screen) — `"< " + _("back") + "  |  " + _("click a button")`
   (carries a `TODO: Render with UI elements instead of text`).
5. `scan_views.py:67` (**already view-layer**) — `_(self.invalid_qr_type_message) + ', received "X" format'`
   (translated prefix + ASCII suffix). The only one already in the "right" place, so
   it'd want the template treatment whenever that view is next touched.
   - Related: the amount component (`components.py:1286`) draws the unit as its own
     element `text=f" {unit_text}"` (leading space + `btc`/`sats`). Closed-set, but
     the leading space means `" सैट्स" ≠ "sats"` msgid — the LVGL amount reimpl
     should key the unit on the bare word.

**Refactor pattern (already supported by the render layer):** express each composite
as a single `_()` template so it becomes a catalog msgid. A closed-set translated word
in a hole is shaped by the **segmented** path (the `_("change")`/`_("self-transfer")`
mechanism, e454c86); numeric/ASCII holes segment trivially. E.g. `psbt:681` →
`_("{address_type} #{index}")` (or per-type msgids); `seed:1447` →
`_("{sig_type} - {script_type}")` + a network-suffix variant; the `"< back | …"`
hints → one template, or render via UI elements (the existing TODO).

## D. The `unsupported` template tail (already flagged loudly by `build_fontpacks.py`)

These ARE catalog templates, but a hole sits at a shape-unsafe boundary (a value
abutting a script letter) or the template is multi-line, so they can't be segmented
→ no run. Values are numeric/closed-set (not arbitrary), so again not contract
violations — but they need a render-layer enhancement or a translation tweak (add a
separating space / restructure) for full hi/th parity:

- hi: `"The {mnemonic_length}th word is built from {num_bits} more entropy bits…"`
  (`{mnemonic_length}` abuts व).
- th: `"{} change"` (`{}` abuts เ); 2× `"Transaction's {} address could not be …"`
  (multi-line + placeholder).

The offline build prints these every run, so they stay visible until fixed.

## Bottom line for the parity gate

- Contract (no arbitrary non-corpus splicing): **PASS** — the four external-data
  fields are the only places, and all are acknowledged no-guarantee.
- Category **C** is a **migration guardrail, not a task against current code**: the
  string assembly relocates from the legacy PIL screens into the views during the
  LVGL migration; do it as a `_()` template there, not an f-string. (The lone
  view-layer site, `scan_views:67`, gets the same treatment when next touched.)
- Category **D** (the `unsupported` tail) is the one independent follow-up: those are
  already `_()` templates, so it's a `.po`/translation or render-layer fix (shape-safe
  boundary / multi-line), unrelated to the screen→view refactor.
