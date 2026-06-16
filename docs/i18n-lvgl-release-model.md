# LVGL i18n: release model & parity gate

_Strategic context for the LVGL screen migration's i18n work, so the per-feature
specs (e.g. `i18n-ngettext-micropython-spec.md`) and the cross-repo wiring share
one picture. Captures decisions settled in design discussion (2026-06)._

## Release model: full cutover, no hybrid, no PIL fallback

The end state is **LVGL screens on both platforms, PIL removed** — one screen
codebase, two hardware targets. There is **no hybrid release** and **no
per-language fallback to PIL**. We would not ship "LVGL for everything except
Hindi." A language either renders fully correctly in LVGL or the LVGL release that
would include it does not ship yet.

Driver of this: **ESP32 (MicroPython) cannot run PIL/libraqm at all**, so it is
LVGL-only by necessity. The shared-codebase goal then makes LVGL-only the end
state on CPython/Pi Zero too. PIL is not a safety net to lean on; it is the thing
being replaced.

## Two bars (do not conflate)

1. **MicroPython prototype — the current sprint.** Goal: a fully working
   MicroPython SeedSigner port. Requires all screens implemented in LVGL + the
   ESP32 host wiring (C module, display/input, PSRAM font-cache routing, the
   `ss_load_locale` pack-provider). **i18n is not on this critical path** —
   English via the existing `compat/l10n` passthrough proves the port boots and
   renders. (Optional: include one shipped complex-script language — hi or th — to
   validate the shaping pipeline on real hardware, which the `runs.bin` footprint
   work was prepping for. That pulls Layer-1 + plural-form shaping into the sprint.)

2. **Production release — the parity gate (below).** A production LVGL release
   (CPython cutover, or a production MicroPython release) must match the **shipped
   language set's** coverage with no regressions.

## The parity gate (production)

The bar for a production CPython release that switches to LVGL is **parity with the
already-shipped language set**, not incremental per-language opt-in. V0.8.7 ships
**hi (Devanagari), th (Thai), vi (Vietnamese), and the rest** in PIL — so the LVGL
cutover must render *all* of them correctly first. The set is fixed by what already
ships; you cannot drop a shipped language to make the cutover easier.

### Scope consequence: the hardest *unsolved* problems are deferred

The genuinely open hard cases are **RTL glyph-run** rendering — Nastaliq word-wrap
and segmented `{}` insertion (these are LTR-only / unwrapped today). Those are
**Urdu**, which is **not in the shipped set** and **not yet a hard requirement**.
The ur accommodations already here (the minimal `ur` stub, the global RTL text
post-pass used by `fa`) are forward-investment. So the cutover blockers are the
**LTR complex scripts already shipped — hi and th** — not the scary RTL ones.

(If `fa` is in the shipped set, its RTL is already handled by a *different*
mechanism — LVGL BiDi + presentation forms + the RTL post-pass — not glyph-run
shaping, so it is not an additional blocker. The unsolved RTL work is specifically
the glyph-run path, i.e. Urdu.)

## Per-language readiness checklist (the parity criteria)

For each shipped language, the LVGL stack must satisfy (✓ = solved this far):

1. **All plural forms shaped/renderable.** Every `ngettext` form must render — for
   complex scripts that means a glyph run per form (today only `msgstr[0]` is
   ingested; see `i18n-ngettext-micropython-spec.md` deliverable C). Scales with
   `nplurals` (hi=2, th=1; ru/pl=4 but codepoint-rendered so no runs needed).
2. **No un-shapeable dynamic complex-script text.** Arbitrary runtime strings not
   in the `.po` corpus cannot be shaped for Devanagari/Thai (Latin/CJK render by
   codepoint and are fine). Either keep such fields ASCII/numeric, route them
   through the segmented `{}` mechanism, or accept they will not shape. **This is
   the real contract change vs. PIL** (which shaped anything via libraqm) and the
   main thing to prove per shipped complex-script language.
3. **No translation/runs skew.** The translation source and the glyph runs must be
   built from the **same `.po` snapshot**, or a translated string won't match a run
   → silent tofu. One `.po`-derived pipeline feeding both avoids it by construction.
4. **RTL glyph-run wrap + insertion.** ⏸ **Deferred — Urdu only, post-cutover.**
   Not required while `ur` is not a shipped language.

## Status against the gate (this work)
- vi typographic/horn-letter tofu — **fixed** (a literal parity blocker; vi is
  shipped). Baked floor extended with Latin Ext-B horn letters + General Punctuation.
- `runs.bin` compact on-device format — **done** (ESP32 footprint path).
- Remaining parity items for hi/th: deliverable C (all plural forms), the dynamic-
  text contract (#2), and the single-`.po`-source guarantee (#3).
- Urdu RTL glyph-run wrap/insertion (#4): deferred until ur is a hard requirement.
