# Complex-script text shaping for the LVGL screens — direction + open questions

_Status: **NOTES / agreed direction.** This captures the architecture conversation.
**The finalized, approved plan is now `docs/complex-script-shaping-spike-plan.md`**
(spike-first: Devanagari + Nastaliq-Urdu + Thai, migrate `fa` to HarfBuzz runs) — read
that to execute; this doc remains as the rationale/background. Nothing is built yet.
Companion to `docs/font-farsi-rtl-plan.md`, `docs/font-tiering-plan.md`,
`docs/font-and-i18n-rendering.md`._

## The goal driving this

Support **as many major world languages as possible** on the shared LVGL screens
(`components/seedsigner/`, consumed by both Pi Zero via `seedsigner-raspi-lvgl`
and ESP32-S3 via `seedsigner-micropython-builder`). Niche/very-hard languages can
be skipped, but major world languages are must-haves. **Hindi is a hard
requirement for the initial LVGL-screens release.** Other named targets: Urdu,
Arabic, Afrikaans, Traditional Chinese, and broadly the high-population languages.

## The core problem

LVGL + our `tiny_ttf` (stb) engine has **no general text shaping**. All it has is:
- `LV_USE_BIDI` (RTL reordering), and
- `LV_USE_ARABIC_PERSIAN_CHARS` — a *basic* Arabic shaper that rewrites base
  letters into **presentation-form codepoints** before glyph lookup.

That Arabic path is the only reason Farsi works today: Unicode happens to have
Arabic presentation-form codepoints (U+FE70–FEFF, U+FB50–FDFF), so we can shape
**offline** (`tools/i18n/shaper/lv_shape`, which runs LVGL's own shaper) and bake
a codepoint string that `tiny_ttf` renders by number. See
`docs/font-farsi-rtl-plan.md`.

**That trick does not generalize.** Devanagari (Hindi), the rest of the Indic
family, etc. need:
- **reordering** (e.g. the short-i matra ि is typed after its consonant but
  renders before it), and
- **conjunct glyphs** that exist only as glyph-IDs reachable through GSUB — they
  have **no Unicode codepoints**, so there is nothing to bake into a codepoint
  string.

Urdu raises the bar further: Urdu readers expect **Nastaliq** (Noto Nastaliq
Urdu), one of the most shaping-intensive scripts; our presentation-form path
would render it as plain Naskh (wrong/unloved).

Conclusion reached in the conversation: **broad world-language coverage requires a
general shaping engine — in practice HarfBuzz.** Per-script hacks are a dead end
at this scale. Solving Hindi the right way (HarfBuzz) simultaneously unlocks the
whole Indic + Arabic/Urdu/Hebrew + SE-Asian tier.

## Effort tiers (most coverage is cheap; only one tier is hard)

- **Latin — free / already done.** Afrikaans, Indonesian, Spanish, Portuguese,
  French, Swahili, Turkish, Vietnamese… Covered by the baked OpenSans Western
  baseline (+ diacritics). Huge population, ~zero work. Afrikaans ≈ a config line.
- **CJK + Cyrillic + Greek — fonts only, no shaping.** Already have SC/JP/KR +
  Cyrillic/Greek/Vietnamese packs. **Traditional Chinese** = add NotoSansTC
  (distinct glyphs from Simplified, but no shaping). Big fonts, simple pipeline.
- **Complex shaping — the HarfBuzz tier.** Arabic, **Urdu (Nastaliq)**, Hebrew,
  the **Indic** family (Hindi/Devanagari, Bengali, Tamil, Telugu, Gujarati…), and
  SE-Asian (Thai, Lao, Khmer, Myanmar…). This is what needs the engine.

(Thai is borderline: linear, marks self-position, so it renders *acceptably*
without a shaper for simple strings — but mark stacking needs GPOS for polish.
Hindi/Devanagari is genuinely broken without a shaper.)

## The architecture decision (made in conversation)

**Run HarfBuzz as an OFFLINE / build-time pre-shaping step. Do NOT put HarfBuzz
(or any runtime shaper) on the microcontroller if it can be avoided.** Rationale:
it's a signing device — minimize on-device attack surface + flash; fits the
existing project philosophy ("the rasterizer only ever sees trusted, pre-validated
content; firmware tier is corpus-independent"). HarfBuzz is the heavy, parser-rich,
complex part, and it stays at build time behind signed packs.

### Critical distinction: pre-shaped RUNS, not pre-rasterized bitmaps

"Deliver fully prepared glyph content to the screen" must mean **pre-shaped glyph
runs**, not pixels:
- **Pre-shaped glyph runs (the choice):** offline step emits, per string, a
  sequence of `(glyph-id, x/y offset, advance)` in final **visual** order. The
  device still **rasterizes** those glyph-ids at display size — it already does
  this (stb rasterizes glyphs today; we'd just drive it **by glyph-id** instead of
  by codepoint). Stays **resolution-independent** (shape once, raster at any size);
  storage is tiny (a few bytes/glyph).
- **Pre-rasterized bitmaps (rejected):** explodes as string × font-size × locale,
  loses scaling/AA. Only viable for a fixed tiny UI.

So the split is: **shape offline (HarfBuzz — removed from device), rasterize on
device (stb — already present, just by glyph-id).** We are removing the *shaper*,
not adding a rasterizer.

### What the subsystem requires

1. **Build step:** FriBidi + HarfBuzz (i.e. `raqm` minus the rasterizer) over each
   locale's catalog → a per-locale **run table** (`msgid → [glyph-id, dx, dy, adv]`
   in visual order). Generalizes today's Arabic `lv_shape` from "→ codepoints" to
   "→ glyph runs," lifting the codepoint-only limitation.
2. **Ship:** the run table **+ the subset font** (device needs the font to
   rasterize the glyph-ids).
3. **On device:** a "draw shaped run" path — rasterize glyph-ids via stb at display
   size, place by offset — replacing `lv_label`'s codepoint-string path for these
   locales.

### Known subtleties / risks (so they don't surprise the next session)

- **Glyph-ids are font-specific** → the offline shaping must run against the
  *exact* subset font the device ships (build them together, or carry a remap).
- **Resolution independence preserved** (runs are in font units).
- **Bidi also moves offline** (FriBidi in the prep step); device needs no bidi
  logic for these locales.
- **No LVGL turnkey** for any of this — it's a custom subsystem (run format +
  glyph-id render path + build pipeline). That engineering is the price of keeping
  the device minimal.
- **Closed-corpus assumption** — the device can only render strings that were
  pre-shaped. Mostly fine (UI is catalog msgids; user input is ASCII on the baked
  mono font), **except** for runtime variable insertion — see next section.

## THE OPEN WRINKLE: runtime variable insertion into translated text

This is where the conversation stopped and is the hardest part to design.

SeedSigner interpolates values into translated sentences at runtime, and the
inserted value can itself be a **translated word in the same complex script**.
User's example: "we could not verify your **change** address" vs "… your
**receive** address" — translators translate the word(s) and the right one is
inserted into the translated template.

**Why this breaks naive offline shaping:** shaping is **not compositional**. You
cannot pre-shape the template and the inserted word separately and concatenate —
for complex scripts the glyphs at the seam depend on their neighbors, so the join
would be wrong. (The one safe case: **word/space boundaries**, which break joining
and clustering in *every* script, so a space-delimited slot is a clean shaping
break.)

**Tractable handling for SeedSigner's patterns (to be fleshed out + verified):**
- **Closed-set translated-word insertions** (change/receive, single/multisig, …):
  **enumerate the combinations at build time** and shape each fully-assembled
  sentence. Correct for any script, zero boundary subtlety. Only risks
  combinatorial blow-up if one string has multiple multi-valued slots (SeedSigner's
  appear to be single, small sets — verify).
- **Numeric / ASCII insertions** (`{threshold}`, `{n}`, `{num_bits}`, `{network}`,
  `{mnemonic_length}`, `{derivation_path}`, denominations, addresses, fingerprints,
  seed words): these don't need complex shaping themselves. Shape the template
  **segments around the hole** offline; render the value on the simple/Latin
  baseline path; bidi-place the segments. Safe because the slot is at a
  word/space boundary and the value is non-complex.
- **Open-ended complex-script insertions** (arbitrary Devanagari spliced mid-word):
  would break offline. SeedSigner appears **not** to need this — must confirm.

**Honest caveat:** this insertion machinery is real pipeline complexity. Runtime
HarfBuzz handles all of it trivially (assemble string → shape). If the insertion
inventory turns out richer/messier than expected, it weakens the offline case and
should reopen the runtime-vs-offline decision.

### Evidence gathered (partial — finish the inventory)

From `seedsigner/src/`:
- `gui/screens/psbt_screens.py` and `seed_screens.py`: `_("change address")` /
  `_("receive address")` are translated as **whole phrases** assigned to an
  `addr_type` variable that is then inserted into larger text; `_("{} change")
  .format(denomination)` inserts an ASCII unit.
- Catalog placeholders (counts in `fa` `.po`): `{threshold}`×4, `{n}`×4,
  `{num_bits}`×2, `{network}`×2, `{mnemonic_length}`×2, `{derivation_path}`×2.
- **TODO for next session:** produce the *complete* inventory of interpolations
  across all catalogs/source — classify each as closed-set-word / numeric / ASCII /
  open-complex — to confirm the offline approach is sufficient and bound the
  enumeration.

## Relationship to the Python / SeedSigner OS release (important context)

The **production Python app already supports these complex scripts correctly** and
is **not** affected by any of this:
- It renders with PIL + **libraqm** (= FreeType + HarfBuzz + FriBidi) — runtime
  shaping from base letters via the font's GSUB. Per-locale full fonts mapped in
  `gui/components.py` `BASE_LOCALE_FONTS`.
- The SeedSigner OS build trims those fonts (`seedsigner-os/opt/build.sh`:
  `pyftsubset … --text="<corpus + ASCII>"`). **pyftsubset's default GSUB closure
  keeps all positional forms + ligatures + the GSUB/GPOS tables**, so the trimmed
  font shapes identically to the full font under libraqm. Verified concretely for
  Farsi (trimmed render == full render, pixel-identical, via PIL+raqm). Hindi/Thai
  work the same way (same closure mechanism). So the **upcoming Python/OS release
  needs nothing new** for Hindi/Thai/Urdu/etc. — only the LVGL screens do.
  - Note: that pyftsubset-preserves-shaping correctness is *implicit/untested* —
    a future "shrink the fonts" change to the pyftsubset flags (`--layout-features`,
    `--glyphs`, `--no-layout-closure`) would silently break complex scripts. A
    render/shape regression test on the trimmed font is cheap insurance.

**Convergence:** both platforms would shape with HarfBuzz — Python at runtime
(raqm), LVGL at build time (this subsystem) — and can share catalogs + fonts.
Visually identical output, engines agree; they differ only in *when* shaping runs.

## Open questions for the next session

1. **Finish the interpolation inventory** (above) and confirm offline is viable.
2. **Run-keying / plumbing model:** per-locale `msgid → run` lookup table in the
   render layer (leaning this way — keeps screen JSON locale-agnostic, mirrors how
   catalogs work) vs. runs embedded in scenario/screen config.
3. **Variable-insertion design:** concrete mechanism for enumeration (closed-set)
   + segment/slot (numeric/ASCII), incl. bidi placement of an LTR value in an RTL
   sentence.
4. **On-device render path:** modify `tiny_ttf`/stb to rasterize by glyph-id (stb
   has `stbtt_GetGlyphBitmap` by index) + a draw routine for runs; or evaluate
   `lv_freetype` by glyph-id. How it coexists with the existing codepoint path
   (Latin/CJK can stay on the simple path; only complex locales use runs — or move
   everything to runs for uniformity).
5. **Pre-shape pipeline:** where it lives (extend `tools/i18n/`), what it depends on
   (HarfBuzz + FriBidi build-time deps), run-table format + signing.
6. **ESP32-S3 feasibility check:** even offline, confirm the glyph-id render path +
   per-locale fonts + run tables fit flash/RAM. (Pi Zero is trivial.)
7. **Decision check:** does the variable-insertion complexity stay bounded? If not,
   re-weigh runtime HarfBuzz (heavier device, but trivial insertion handling).

## De-risking spike (recommended first action)

End-to-end on **one Devanagari + one Nastaliq-Urdu string**, plus **one
variable-insertion case**: offline FriBidi+HarfBuzz → run → on-device glyph-id
rasterize → verify it matches a libraqm reference render; measure the on-device
footprint of just the glyph-id render path. This proves the custom path and the
insertion handling before committing the roadmap.

## Pointers

- Existing Arabic offline shaper (the thing this generalizes):
  `tools/i18n/shaper/lv_shape.c` + `tools/i18n/build_fontpacks.py`.
- LVGL shaping reality: `third_party/lvgl/src/misc/lv_text_ap.c` (Arabic only),
  `src/misc/lv_bidi.c`; `src/libs/freetype/` is rasterization-only (no HarfBuzz /
  no complex shaping).
- Render layer: `components/seedsigner/{seedsigner,components,font_registry,
  locale_fonts,gui_constants}.cpp`.
- Python side: `seedsigner/src/seedsigner/gui/components.py` (`BASE_LOCALE_FONTS`,
  `get_font`), `seedsigner/controller.py` (libraqm check), `seedsigner-os/opt/
  build.sh` (pyftsubset trim), `seedsigner-translations/tools/
  extract_characters_from_babel_mo.py`.
- Prior i18n docs: `docs/font-farsi-rtl-plan.md`, `docs/font-tiering-plan.md`,
  `docs/font-and-i18n-rendering.md`, `docs/font-and-i18n-implementation-plan.md`.
