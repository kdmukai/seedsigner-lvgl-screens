# Screen conformance spec

**Status: authoritative.** This document defines the canonical structure, style, and
technique for every screen in `components/seedsigner/screens/` and the rules the shared
infrastructure follows. It was synthesized from a 39-agent review of all 52 in-scope
translation units (2026-07-10) after the monolith split, and is the instruction sheet
for bringing every screen into conformance. It **extends** —  never overrides —
`docs/screen-file-layout-and-naming.md` (the filename==symbol==registry==scenario
invariant and file-layout rules live there).

Audience note: this repo is maintained primarily by AI, and PR review is primarily
AI-driven. Every rule below is chosen for AI-legibility first: uniform greppable
patterns, self-contained files, explicit invariants, comments that state constraints.
"A human would prefer X" is not a tiebreaker.

**Hard constraint for all conformance work: rendered pixels must not change.** The
verification gate is a pixel-identical screenshot comparison across the six
representative locales (en / ru / ja / fa / hi / ur — one per glyph-rendering type).
Anything that looks like an actual bug is flagged in the tracking report, never fixed
as part of conformance.

---

## 1. The exemplar

**`screens/tools_calc_final_word_done_screen.cpp`** (as polished by the conformance
pass) is the template every scaffold-based screen conforms to. It demonstrates the
dominant screen shape end to end: curated per-annotated includes, complete file
banner with cfg contract, screen-name-prefixed required-field validation,
write-if-absent **structural** defaults annotated to their Python source (content
is host-supplied and validated, per the §5 content policy), both body idioms
(raw label + shared component with opts), shared-helper parity math, numbered build
steps matching the Python build order, and the canonical bind/load tail.

Supplementary patterns the stateless exemplar cannot show (see §6 lifecycle):

- **Stateful (Tier 2) screens**: `seed_add_passphrase_screen.cpp` (validate-before-
  allocate ordering), `qr_display_screen.cpp` / `seed_address_verification_screen.cpp`
  (identity-guarded module static + `LV_EVENT_DELETE` cleanup), `opening_splash_screen.cpp`
  (timer-nulled-before-host-callback re-entrancy safety).
- **Large interactive screens**: `seed_mnemonic_entry_screen.cpp`'s section-divider
  organization (state machine / view sync / transitions / callbacks).

## 2. Canonical file skeleton

Fixed section order for every `screens/*_screen.cpp`:

```
1. File banner (line 1, above all includes)      — see §3
2. Curated include block                          — see §4
3. using json = nlohmann::json;                   — explicit in every screen TU
4. File-scope constants / macros
5. Anonymous namespace: private types, helpers, LVGL callbacks
   (feature guards like #if LV_USE_QRCODE go OUTSIDE the namespace)
6. extern "C" host-push APIs (only if the screen has them)
7. void <name>_screen(void *ctx_json)             — the entry point, LAST in the file
```

Prefer anonymous namespace over bare `static` for new/edited files. `extern "C"`
host-push APIs sit between the namespace close and the entry point, each carrying
its availability stub (`#if`/`#else`) if feature-gated.

## 3. File banner

Line 1 of the file, above all includes. Required fields, in order:

```cpp
// <screen_name>
//
// Python provenance: <Class> (<file.py>)      [or: no Python screen class — <why>]
// <Behavior summary — what the screen shows and returns.>
// <Layout notes — including any documented deviation from Python, naming the
//  sibling screens it diverges from when relevant.>
//
// cfg:
//   <key>   (<type>, required)            <meaning>
//   <key>   (<type>, default <value>)     <meaning>
//   ...every key the file reads. The table MUST agree with the code.
```

Rules:

- Python provenance cites the class and file only — **no line numbers**. The
  linkage is transitional: the Python screens are scheduled for deletion at the
  LVGL cutover, so the citation aids porting-parity checks today and will be
  dropped near production; do not invest precision in it.
- The cfg contract enumerates **every** key the code reads — several screens' tables
  had drifted; a stale contract is a defect (an AI author regenerating a scenario
  from the contract must get a faithful screen).
- **Contract register (uniform across screens):** the table also lists keys the
  scaffold/nav layer reads on the screen's behalf, each marked by reading layer
  (e.g. `[scaffold]`, `[nav]`): `top_nav.*`, `button_list`/`is_bottom_list` where
  applicable, `initial_selected_index`, `input.mode` / `input.keys.*`, and
  `allow_screensaver` `[parse/scaffold]`. Chrome-free screens (no scaffold) omit
  the scaffold-layer keys entirely — `allow_screensaver` is normalized by the
  parse helper but unconsumed on that tier, so it is not tabled there.
- Every banner states the screen's **lifecycle tier** (§6) in one line, and
  chrome-free screens name their tier per §8.
- Comments describe **current code only**: no tombstones ("X now lives in Y"), no
  monolith-position references, no off-repo task IDs (A13, Task 0, Items 2b/2c).
  Historical rationale goes to `docs/knowledge/`.

## 4. Include block

Curated and self-contained: every directly-used header included directly; nothing
unused. One trailing comment per include naming what the file uses from it.
Canonical order:

```cpp
#include "screen_scaffold.h"    // parse_screen_json_ctx, create_top_nav_screen_scaffold, bind/load
#include "seedsigner.h"         // <only what's used — e.g. SEEDSIGNER_RET_*>
#include "<other project headers, only-what's-used>"
#include "lvgl.h"
#include <nlohmann/json.hpp>
#include <string>
#include <other std headers>
// platform/feature-gated includes last (#if LV_USE_QRCODE, ESP_PLATFORM)
```

The 15-header monolith "kitchen-sink" block (still carried by ~20 files) is the
reorg's largest residue: it destroys grep-signal (a `keyboard_core.h` include in a
screen with zero `kb_*` calls hides the real dependency graph). It is deleted
per-file during conformance — **never** replaced by a shared prelude header
(rejected: it would institutionalize the opacity).

## 5. Entry-point flow

Canonical order inside `void <name>_screen(void *ctx_json)`:

```cpp
void <name>_screen(void *ctx_json) {          // brace on the signature line
    // --- Config ---
    const char *json_str = (const char *)ctx_json;
    json cfg;
    parse_screen_json_ctx(json_str, cfg);     // or parse_optional_screen_json_ctx for the boot/overlay tier

    // required-field validation: one throw per field, immediately after parse,
    // BEFORE any scaffold/ctx allocation (no throw path may leak LVGL objects)
    if (!cfg.contains("<key>") || !cfg["<key>"].is_string())
        throw std::runtime_error("<screen_name>: <key> is required and must be a string");

    // structural defaults: write-if-absent, layout/behavior flags only —
    // never user-visible text (see the content policy below)
    ensure_top_nav_structure(cfg, /*default_show_back_button=*/true,
                                  /*default_show_power_button=*/false);
    require_top_nav_title(cfg, "<screen_name>");
    // forced (non-defaulted) overrides are explicit assignments after the call,
    // annotated with their Python constant

    // --- Scaffold ---
    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, /*scrollable=*/...);

    // --- Body ---
    // numbered component steps matching the Python build order;
    // all content parented inside screen.body / screen.upper_body
    // (screen.screen parenting allowed ONLY to escape body clipping — say so in a comment)

    // --- Geometry ---
    // at most ONE lv_obj_update_layout measure-and-place pass

    // --- Navigation + load ---
    // 1-3 line rationale comment naming which index idiom applies
    bind_screen_navigation(cfg, screen, /*default_initial_index=*/0);
    load_screen_and_cleanup_previous(screen.screen);
}
```

Dimension rules distilled:

- **Parsing.** `parse_screen_json_ctx` unconditionally; the boot/overlay tier whose
  ctx is legitimately optional (`main_menu`, `opening_splash`, `loading_spinner`, the
  two camera overlay screens) uses `parse_optional_screen_json_ctx` (NULL/empty →
  empty object, otherwise identical strict behavior including `allow_screensaver`
  normalization). **No screen ever calls `json::parse` or `merge_patch` directly.**
- **Error attribution, two tiers.** Shared helpers keep generic unprefixed messages;
  every screen-specific validation throw is prefixed `"<screen_name>: ..."`. Shared
  helpers never bake one caller's name into messages.
- **Content policy — no English content defaults.** Every user-visible string
  (titles, body text, button labels, status text) arrives **localized from the
  host view layer** via cfg — the translation layer lives host-side (gettext),
  so any string literal injected in C++ is English-only by construction and
  ships untranslated. A missing required content key is a **developer error**:
  throw a screen-name-prefixed error; never patch over it with an English
  default. Structural defaults (booleans, layout flags, geometry numbers —
  `show_back_button`, `is_bottom_list`, `border`, … — nothing rendered as text)
  remain write-if-absent, annotated to their Python source. Sanctioned
  exception: the boot/NULL-ctx tier (`main_menu`, `opening_splash`) renders
  built-in English defaults by documented per-screen contract — those screens
  can run before any host view layer exists to supply cfg.
- **Required vs optional.** Required fields (all content, plus any datum without
  a sensible structural default) throw when missing. Optional structural fields
  read via `cfg.value()`. Sanctioned fail-soft exceptions (e.g.
  `settings_locale_picker`'s per-row image fail-soft) are stated in the banner
  as policy.
- **Layout tiers.** Tier 1 (default): declarative flex in `upper_body`; widths
  derived from the parent via `lv_obj_get_content_width` (derive-internally rule);
  gaps as `pad_row=0` + per-child `margin_top`. Tier 2: scaffold chrome + measured
  coordinate body where Python-parity math demands it. Chrome-free tier (§8) is the
  only sanctioned scaffold bypass.
- **Callback surface.** Every weak host hook has exactly one prototype in
  `seedsigner.h`; screen-generic weak no-op defaults live only in `components.cpp`;
  no file locally re-declares host hooks (this class of drift already produced link
  fix 7f89913). Screens delegate all input through one `bind_screen_navigation` call
  in tail position, immediately before `load_screen_and_cleanup_previous`.
- **Load-tail invariant.** ALL screen construction/mutation completes BEFORE load:
  the RTL and glyph-run passes run once inside `load_screen_and_cleanup_previous`,
  so post-load mutation breaks shaped locales. (Two current violations are flagged,
  not fixed: `settings_qr_confirmation`, `tools_calc_final_word`.)

## 6. Lifecycle: two tiers

**Tier 1 (default — use unless the screen owns timers/animations/host-push state):**
fully stateless. No statics, no heap ctx, no cleanup callback; all state
widget-tree-owned or stack-local; final statement is
`load_screen_and_cleanup_previous(screen.screen)`. Rare per-widget heap data rides
its own widget's `LV_EVENT_DELETE` callback. Stack config/spec structs value-init
with `= {}` (not `lv_memzero`).

**Tier 2 (stateful):** exactly one heap ctx + one cleanup callback registered on the
screen root for `LV_EVENT_DELETE`, with six rules:

1. **Ordering** — parse and validate ALL required cfg (and throw) before creating
   the scaffold or allocating the ctx, so no throw path can leak.
2. **Allocation idiom by struct content**, stated in a one-line comment at the
   struct declaration: any C++ members (string/vector/map) → `new`/`delete`
   (`lv_malloc` would skip ctors/dtors — UB); pure POD → `lv_malloc` + `lv_memzero`
   + `lv_free`.
3. **OOM** — NULL-check every `lv_malloc` and throw.
4. **Cleanup callback** first line re-checks `lv_event_get_code(e) == LV_EVENT_DELETE`.
5. **Host-push state** — module-scope global set only after full construction,
   cleared by the cleanup callback under an identity guard
   (`if (g_ctx == ctx) g_ctx = nullptr;`).
6. **Timers/animations** — null the timer pointer before invoking any host callback
   that could re-enter screen construction (`opening_splash` pattern).

Sanctioned exceptions (documented contracts, keep as-is): `screensaver_screen` loads
via bare `lv_scr_load` and does not delete the previous screen (host save/restore
contract); process-lifetime state (memoized fonts, locale retire/reap) is never torn
down by screens.

## 7. Naming

Four mechanical rules (spec register: `power_off_not_required` / `seed_finalize` /
`font_registry`):

1. **Verbosity.** Full-word identifiers at every scope. Closed allowlist of short
   tokens: `i`/`j` loop counters, `e` for `lv_event_t` in callbacks, `cfg` for the
   parsed screen JSON, `ctx` for a screen's heap context struct, and proper
   initialisms treated as words (QR, RTL, JSON, URL). Everything else spelled out:
   `input_button_width` not `ibw`, `fingerprint_row` not `fp_row`, `display_width`
   not `W`. One exemption: `qr_core.cpp`'s QR-spec algorithm internals, where terse
   names mirror the published spec (exempt the algorithm body, not its enum values
   or header parameters). Rationale: AI maintainers navigate by grep; a comment at
   the declaration does not travel to the 30th use site.
2. **File-local prefix.** Every static/anon-namespace function, variable, and enum
   in `screens/<name>_screen.cpp` carries the full canonical stem `<name>_` — no
   invented truncations.
3. **Statics sigil.** File-scope mutable state carries `g_` (e.g. `g_qr_ctx`).
4. **Public-API prefixes** (infra): the current four live conventions
   (`seedsigner_*`, `ss_*`, `kb_*`, bare) are a documented rollout decision — do not
   invent a fifth; new public symbols in an existing module follow that module's
   convention.

## 8. Sanctioned screen tiers

- **Scaffold tier (default).** Everything not listed below:
  `create_top_nav_screen_scaffold` + `bind_screen_navigation` + shared components.
- **Chrome-free tier** (the only sanctioned scaffold bypass — bare root, custom
  input): `qr_display`, `seed_transcribe_whole_qr`, `seed_transcribe_zoomed_qr`,
  `opening_splash`, `screensaver`, `loading_spinner`, `io_test`, the two camera
  overlay screens, `main_menu` (bespoke Python-parity grid). The
  `load_screen_and_cleanup_previous` tail is still mandatory (except screensaver's
  documented contract).
- **Compose-and-overlay technique** (`settings_qr_confirmation`,
  `tools_calc_final_word`: mutate cfg → call `button_list_screen` → overlay): a
  known anti-pattern — it round-trips JSON in-process, discards object handles
  (forcing DFS re-discovery), and mutates after load (breaking the shaped-locale
  invariant, flagged). Documented decision needed at rollout: either bless a
  scaffold-level "chrome + custom band" mode or keep-and-contain. Do not add a
  third consumer.

## 9. Comment style

- Liberal why-comments at declarations and between logical steps; generous
  whitespace between steps (existing convention, kept).
- One section-divider dialect repo-wide: ASCII hyphens — full-width fences
  (`// ---------------------------------------------------------------------------`)
  or padded short form (`// --- Data ---`). Retire `// ====`, `──` box-drawing.
  Canonical in-function section names: `// --- Config ---`, `// --- Scaffold ---`,
  `// --- Body ---`, `// --- Geometry ---`, `// --- Navigation + load ---` (plus
  numbered component steps within Body, matching the Python build order).
- Component-opts calls annotate resolved defaults with a `// -> resolved default`
  trailing comment where a sentinel is passed.
- Infra headers keep contract-prose banners per section and per-declaration
  contracts (`glyph_runs.h` / `navigation.h` register); contract comments MUST match
  the implementation (several stale-doc defects are in the bug ledger).

## 10. Extraction ledger

Every verified duplication cluster, with disposition. **EXTRACTED** = done in the
conformance pass, corpus-wide, pixel-gated. **PARTIAL** = helper exists; call sites
migrate with each screen's conformance. **DOCUMENTED** = real duplication, but the
extraction needs a design/pixel-policy decision — do not extract ad hoc; revisit at
rollout. **REJECTED** = not extractable / anti-extraction.

| # | Cluster | Disposition | Helper (home) |
|---|---------|-------------|---------------|
| 1 | Hand-rolled JSON parse prologues (7 screen sites re-solve `parse_screen_json_ctx`) | **EXTRACTED** | `parse_optional_screen_json_ctx` (`screen_scaffold`) |
| 2 | `bind_screen_navigation` NULL-ternary ritual (18 identical sites) | **EXTRACTED** | 3-arg overload `bind_screen_navigation(cfg, screen, default_initial_index)` (`screen_scaffold`). Do NOT migrate donate/reset/power_off (`NULL, 0, NAV_INDEX_NONE` is a different, no-buttons contract). |
| 3 | Monospace char-width measurement (`"0000000000"/10` × 5 verbatim sites + canonical copy) | **EXTRACTED** | `monospace_char_width(font, run_length=10)` (`components`). `tools_address_explorer`'s single-'X' variant left as-is (font-property equivalence, not code-provable). |
| 4 | Aux-key recognition + forwarding (4 recognizer copies; `_on_aux_key` undeclared in `seedsigner.h`) | **EXTRACTED** | `nav_aux_key_index(key)` (`navigation`); `seedsigner_lvgl_on_aux_key` prototype added to `seedsigner.h`; weak default relocated to `components.cpp`; local extern re-declarations deleted. |
| 5 | Side-panel hardware-key geometry (3 verbatim derivations) | **EXTRACTED** | `kb_side_panel_geometry(...)` (`keyboard_core`); integer expressions copied verbatim; `screen_h` stays a caller-passed `lv_obj_get_height` value. |
| 6 | Glyph-ink extent measurer clones (admitted local copies of exported helpers) | **EXTRACTED** (mechanical legs) | Delete `measure_ascii_ink` (tools_calc_final_word) and the tight-line-space clone (settings_qr_confirmation); call `measure_text_ink_extents` / the `screen_helpers` export. `components.cpp`'s variant leg: DOCUMENTED. |
| 7 | top_nav chrome defaults injection (hand-copied into 22 screens) | **PARTIAL** | `ensure_top_nav_structure(cfg, default_show_back, default_show_power)` (structural flags only) + `require_top_nav_title(cfg, screen_name)` (`screen_helpers`). Title-defaulting was REMOVED from the helper per the §5 content policy (2026-07-10): rollout migrates the 22 inline sites to structure+require, deleting their English title defaults — a behavior change on malformed cfg (silent English → throw), noted per screen at migration. Forced overrides stay explicit assignments after the call. `apply_status_type_defaults`' content defaults get the same treatment when the status family conforms. |
| 8 | Post-layout "bottom of free band" measurement (4 token-identical scaffold sites + variants) | **PARTIAL** | `bottom_button_top_y(screen)` (`screen_scaffold`); precondition (caller ran `lv_obj_update_layout`) documented. Variant sites (op_return's uncorrected `y2`, overview/whole_qr geometry forks): DOCUMENTED. |
| 9 | Monolith kitchen-sink include prelude (md5-identical 38-line block × 17 + variants) | **PARTIAL** (per-file curation) | No helper — anti-extraction (see §4). Deleted per-file during conformance; conformed screens demonstrate the curated form. |
| 10 | Simple-text screen skeleton clones (reset / power_off / donate ~80% identical) | **DOCUMENTED** | Proposed `build_simple_text_screen(...)` composition in `screen_helpers`; whole-skeleton helper is a design decision (three files reduce to distinguishing constants). |
| 11 | Keypad-sink + indev-attach block (7 re-inlined copies of `kb_connect_indevs`'s loop) | **DOCUMENTED** | Promote `kb_connect_indevs` → `navigation` under a neutral name (`connect_keypad_indevs_to_group`) + `create_bare_root_screen()` / `attach_keypad_input_sink()` for the chrome-free tier. Touches input plumbing corpus-wide — rollout wave, not ad hoc. |
| 12 | Upper-body grow/center/spacer trio (6 verbatim sites) | **DOCUMENTED** | `grow_upper_body_over_button_gap(...)`; entangled with the vertical-slack policy (§11) — extract together with it. |
| 13 | Compose-chrome-tail forks (2 files) | **DOCUMENTED** | See §8 compose-and-overlay decision. |
| 14 | Ink-anchored centered body label (2 sites) | **DOCUMENTED** | `place_ink_anchored_centered_body_label(...)` (`screen_helpers`); measurement-source choice (raw arg vs stored text) is pixel-relevant — decide at rollout. |
| 15 | UTF-8 decoder triplication (4 production copies with divergent malformed-input behavior) | **DOCUMENTED** | Proposed dependency-free `utf8.h` with `utf8_next_codepoint`; malformed-input semantics differ between copies (nearly-unreachable via JSON, but the MicroPython binding path is unproven) — needs a semantics decision. |
| 16 | Keyboard-family wiring triplication (group binding, cleanup, mode-resolve, strip geometry re-owned by 3 keyboard screens) | **DOCUMENTED** | `kb_bind_joystick_group(...)` + friends (`keyboard_core` — the module explicitly created for shared keyboard mechanics). Largest extraction; its own rollout wave. Pixel-trap notes recorded in the digest (do not "improve" `kb_below_text_entry_y` to measured height). |

## 11. Open pixel-policy decisions (rollout, with sign-off)

These converge behavior that currently differs *by policy*, so they may legitimately
change pixels and are **out of scope for conformance**:

- **Vertical-slack policy.** One written rule for taller-than-reference profiles
  (exact Python top-anchor at `px_multiplier==100`, center-in-slack above), ideally
  a scaffold option replacing four inline centering techniques and per-screen
  `> 240` conditionals. Until then, each site keeps its exact current alignment.
- **`icon_large` scaling** is non-geometric across profiles (48→48→72 px at
  240→320→480) — flagged, needs a deliberate ruling.

## 12. Consolidation ledger (over-fragmentation / thematic merges)

Documented-only; each is a build-list + doc change and none block screen conformance:

- **`input_profile.{h,cpp}` → merge into `navigation.{h,cpp}`** (31 lines over 2
  files, single consumer; update the module list in the naming doc + five CMake lists).
- **Locale subsystem** (`locale_loader` / `locale_fonts` / `font_registry`): one
  tightly-coupled lifecycle split three ways with three naming schemes and the
  "active locale" state smeared across files; merge `locale_fonts` into one of its
  two consumers, or reorganize as one module.
- **Camera overlay twins** (`camera_preview_overlay` / `camera_entropy_overlay`):
  structural twins; extract `camera_overlay_common.{h,cpp}` (geometry quad, gutter +
  bottom-text machinery, lifecycle contract). Their screen wrappers should also share
  the synthetic-preview-background builder.
- **`components.cpp` (1817 lines)** is the anti-fragmentation outlier: button
  machinery / top-nav / Python-component ports are three coherent clusters. Split is
  optional; if done, follow the naming doc's explicit-listing rule for the five
  build lists.
- **Header hygiene** (rollout sweep): one include-guard idiom repo-wide
  (`#pragma once` vs three guard styles today); `text_top_leading` declared in
  `seedsigner.h` while its metric siblings live in `screen_helpers.h` — move next to
  its family; `screen_scaffold_t` + `SEEDSIGNER_SCAFFOLD_MAX_BUTTONS` typed in
  `seedsigner.h` but consumed only by C++ scaffold API; status-type trio's stranded
  doc block in `screen_helpers.cpp` belongs with `large_icon_status_screen`.
- **Dead code** (delete at rollout): `top_nav_from_screen_json` (zero callers — and
  it strands the only top_nav validation; see bug ledger),
  `overlay_manager_get_screensaver_timeout` (+ header doc), `gui_constants.h` dead
  macro accessors and the unreferenced `seedsigner_icons_36_4bpp` declaration,
  `find_last_label_child` fallback indirection.

## 13. Conformance checklist (per screen)

Applied to the conformed screens; the rollout applies it to the rest:

1. Banner per §3 (provenance = class + Python file, no line number; complete
   layer-marked cfg contract; lifecycle tier; deviations noted).
2. Includes per §4 (curated, purpose-commented, canonical order, kitchen-sink block deleted).
3. Explicit `using json = nlohmann::json;`.
4. File skeleton per §2 (anon namespace, entry point last).
5. Entry flow per §5 (parse → validate → structural defaults → scaffold → body →
   geometry → bind → load), section dividers per §9.
5a. Content policy per §5: no English content defaults — every user-visible
    string is host-supplied; required content throws when missing (boot/NULL-ctx
    tier excepted by documented contract).
6. Shared helpers only — no shadow reimplementation; migrate to ledger helpers
   (#1-#8) where the file contains those patterns.
7. Naming per §7 (full words, `<name>_` static prefix, `g_` sigil).
8. Lifecycle per §6 (correct tier, six Tier-2 rules).
9. No tombstones / stale comments / dead locals introduced or retained.
10. Pixel gate passes (6-locale screenshot comparison, zero diffs); behavior deltas
    on non-rendered paths (e.g. newly-injected defaults on previously-crashing
    malformed cfg) are noted in the PR description.
