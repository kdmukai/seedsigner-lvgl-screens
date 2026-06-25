# Font-memory plan — `seedsigner-lvgl-screens`

_Status: planned, not yet implemented. Handoff doc so a new session can resume in this repo. The
companion doc lives in `seedsigner-micropython-builder/docs/font-memory-plan.md` (instrumentation, the
stb→PSRAM build-time patch, data-gathering, thresholds). **Do the measurement work (builder doc) before
making any CJK size compromises here.**_

## Why this exists

The LVGL UI renders translated text via `lv_tiny_ttf`. On the constrained firmware targets (ESP32-P4
128 KB / ESP32-S3 64 KB internal LVGL pool), glyph **bitmaps** and complex-script **masks** are already
in PSRAM, but the per-`(font,px)` **cache index** nodes live in the small internal pool and accumulate
across screens. They share that pool with the live widget tree, and the stack is **not OOM-safe**: a
pool overflow freezes (`LV_ASSERT_HANDLER` `while(1)`) → watchdog reboot, or NULL-derefs → reboot. It
does not degrade gracefully. So the cure is to **not run out** — reduce avoidable instance count, then
measure (builder repo) before cutting font sizes further.

Full rationale: [`docs/knowledge/tiny-ttf-cache-spin-root-cause.md`](knowledge/tiny-ttf-cache-spin-root-cause.md),
[`docs/font-and-i18n-rendering.md`](font-and-i18n-rendering.md), and the
`TODO.md` "Font memory / glyph-cache optimization" section (this work supersedes/implements that note).

This repo owns the **render layer**, so it owns: how many `lv_tiny_ttf` instances exist, and at what
sizes. Two tasks: (A) dedup instances by `(weight, px)`; (B) data-gated CJK size collapses.

---

## Task A — px+weight-keyed font-instance dedup ("same font @ same size loaded once")

**Status: DONE (branch `feat/font-memory-dedup`).** Both parts landed; the builder repo can now bump
the submodule and measure (see Sequencing). Verified: the `runner_core` headless test gained a locale
dedup + retire/reap lifecycle section (zh CJK Primary share @240, hi shaping Primary, ru Fallback pack
share @480×320, English-baseline restore) — green under ASan, so the shared-instance destroy-once
bookkeeping holds. SS_FONT_TRACE instrumentation (since removed) confirmed the dedup actually fires:
baseline `5 roles → 4 instances` at every profile; CJK `top_nav_title` reuses `large_button`'s 23px
script @240; ru `large_button` reuses `button`'s 23px Cyrillic leaf @320. A cross-branch byte-diff of
670 screenshots (en/es/zh/hi/ru @480×320) is **pixel-identical** — pure memory win, zero render change.

**One correctness refinement vs the original plan** (the plan was wrong here, fixed in code): a shared
**Primary** script keys on `(buf, len, px, original)`, not just `(buf, px)`. The Primary script is the
chain ROOT (`script->fallback = original`), so two same-`(buf,px)` roles may share it ONLY when their
fallback `original` also matches — otherwise the second role would overwrite the first's embedded-ASCII
fallback size. Equal-px Primary roles share an `original` only when their baselines also collide (e.g.
large_button & top_nav both at 20px-SB @240, after part-1 dedup), so the CJK `large_button==top_nav`
collision shares at 240 but NOT at 320/480 (baselines 23 vs 26). **Fallback** packs are unaffected: the
script is the chain LEAF (a per-role `heap_copy` wraps the original), so `(buf, len, px)` suffices and
each role keeps its own copy. See `docs/knowledge/font-instance-dedup-chain-direction.md`.

**Problem.** One `lv_tiny_ttf` instance (with its own 3 caches) is created per text *role*, even when
two roles resolve to the same `(weight, px)`. Each redundant instance is a separate cache → extra
internal-pool index pressure for zero visual benefit.

**Confirmed natural collisions** (no size change, pure dedup):

`install_western_baseline()` roles → px after `px_scale(base, mult)`:

| Profile (mult) | main_menu(26) | top_nav(20) | large_button | button(18) | body(17) | Collision (same weight) |
|---|---|---|---|---|---|---|
| 240 (100) | 26 | 20 | 20¹ | 18 | 17 | **top_nav == large_button @20 (SemiBold)** |
| 320 (133) | 34 | 26 | 23 | 23 | 22 | **button == large_button @23 (SemiBold)** |
| 480 (200) | 52 | 40 | 36 | 36 | 34 | **button == large_button @36 (SemiBold)** |

¹ `large_button` base is 20 at 240-height, 18 at 320/480 (the quirk in `install_western_baseline` and
`locale_role_render_px`). `body` is Regular; the other four are SemiBold — dedup must key on **weight
AND px** (body never shares with a SemiBold role even at equal px).

For CJK locale packs (single Regular Noto weight), `cjk_primary_roles()` already collides
**large_button == top_nav_title @ base 23** at every profile.

**Where to change:**

1. `components/seedsigner/gui_constants.cpp` → `install_western_baseline()` (~L279–328). The 5-role loop
   (`RoleSpec roles[]`, L292–298) calls `lv_tiny_ttf_create_data_ex` once per role (L313). Add a small
   `(weight, px) → lv_font_t*` map within the function; on a repeat key, point the second profile field
   at the **existing** instance instead of creating a new one. `px_scale` is L9–11. These baseline fonts
   are process-lifetime (never destroyed), so dedup here is purely "don't create the duplicate."

2. `components/seedsigner/font_registry.cpp` → `seedsigner_register_font()` (L95–176), the `Registration`
   struct (L16–25), `seedsigner_clear_registered_fonts()` (L178–190), `seedsigner_reap_retired_fonts()`
   (L192–200). For locale packs, the same `(buf, px)` requested for two roles should share one `script`
   instance. Constraints to preserve:
   - **Primary** chains `script->fallback = original`; **Fallback** wraps a `heap_copy` of the baked
     primary with `heap_copy->fallback = script`. A shared `script` must chain identically for both roles
     (it does, since chain role + buf + px are the same).
   - **CRITICAL correctness fix:** the retire/reap path must destroy each `lv_tiny_ttf` and each
     `heap_copy` **exactly once, by pointer**. Today reap iterates registrations and destroys per entry;
     if two registrations share one instance, that double-frees. Track destroyed pointers (e.g., a
     `seen` set in `seedsigner_reap_retired_fonts`) or refcount the shared instance.
   - Keep the retire-then-reap lifecycle intact (the previous locale's screen may still hold raw font
     pointers; see the existing comments at L29–39).

**Properties.** No visual change. Safe to land independently. Highest-leverage no-compromise lever and a
prerequisite for clean measurement. After this lands, the builder bumps the submodule and measures.

---

## Task B — CJK size-collapse decisions (DATA-GATED)

Do **not** apply these until the builder-repo `mem_stats()` instrumentation shows where the P4 sits.
Preserve the **Latin** body/button size distinction (explicitly desired); compromises are CJK-only.

- **B-trivial (apply once measurement confirms it helps, low cost):** CJK `body == button`.
  `components/seedsigner/locale_fonts.cpp` → `cjk_primary_roles()` (L59–67) currently `Body 18`,
  `Button 20`. Collapse to one shared size. Saves 1 instance per profile per CJK/shaping locale (combines
  with the Task-A `(weight,px)` sharing automatically).
- **B-bigger (defer; only if data demands):** collapse `top_nav_title` / `main_menu_title` / main-menu
  button sizes. These do **not** collide naturally across profiles (e.g. @200: 40 / 52 / 36), so this is
  a genuine legibility compromise, not free dedup. Hold until the monitor shows the P4 over the ~70–75 %
  margin. `locale_role_render_px()` (L130–146) and `cjk_primary_roles()` are the change sites.

When changing locale sizes, the size guard in `seedsigner_register_font()` (validates the host-supplied
px against `locale_role_render_px`) and the manifest in `supported_locales_json()` stay consistent
automatically — both derive from the same table.

---

## Sequencing & cross-repo coordination

1. ~~Land **Task A** (dedup) here. No size changes.~~ **DONE** (`feat/font-memory-dedup`).
2. Builder repo: bump this submodule, add `mem_stats()` + stb→PSRAM patch, build, **measure** the worst
   CJK path (see builder doc). ← **NEXT**
3. Return here for **B-trivial**, re-measure; only then **B-bigger** if still over margin.

## Verification (after Task A; full budget verification in builder doc)

- Desktop screenshot generator / interactive runner: confirm CJK + a shaping locale (`ur`/`hi`) still
  render correctly and that locale switching does not crash (exercises the dedup'd reap path — the
  double-free risk).
- On-device (builder): `mem_stats()` `max_used` should drop on a warmed CJK screen vs pre-dedup; no crash
  on locale switch / screen teardown.
