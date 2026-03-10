# Input Navigation + Desktop Runner Implementation Plan

Status: **active tracker** (delete after completion)
Owner: SeedSigner C modules workstream
Canonical repo: `/home/keith/.openclaw/workspace-mp-project-lead/dev/seedsigner-c-modules`

## Purpose

Provide a restart-safe implementation checklist for:
1) hardware-input-aware C-module screens (joystick + KEY1/2/3), and
2) a fast desktop interactive runner to avoid slow ARMv6 emulation loops.

This plan is execution-oriented and intended to be checked off as work progresses.

---

## Behavioral contract (authoritative intent)

- Focus zones: `TOP_NAV` and `BODY`.
- `UP/DOWN` handle zone transitions where appropriate.
- BODY navigation depends on layout:
  - vertical: `UP/DOWN` traverse, `LEFT/RIGHT` no-op
  - grid (e.g., main menu 2x2): all directions active via neighbor mapping
- KEY1/2/3 defaults to ENTER behavior.
- KEY1/2/3 may be overridden per screen (`enter|noop|emit|custom`), without global hardwired ESC/HOME assumptions.
- Default selection policy:
  - touch mode: no default active button unless only one button exists
  - hardware mode: always default-select a BODY control (never TOP_NAV)

Reference behavior spec (hardware-facing):
- `../seedsigner-raspi-lvgl/docs/input-button-behavior.md`

---

## Architecture decisions (locked)

- Input mode is **runtime** profile, not build flag.
- Global input profile is set once at init in production (touch vs hardware).
- Optional test override can switch mode live in desktop runner.
- Screen implementations should read a shared profile/helper and avoid duplicated mode branching.

---

## Work phases

## Phase 1 — Shared navigation layer foundation

- [x] Add reusable nav module (`components/seedsigner/navigation.h/.cpp`)
- [x] Add zone/body-layout model and KEY1/2/3 policy scaffolding
- [x] Wire `button_list_screen` as first implementation target
- [ ] Add/verify explicit API docs in headers for reuse by other screens
- [ ] Ensure memory/lifecycle cleanup is stable under repeated screen swaps

Exit criteria:
- `button_list_screen` supports non-touch directional/key activation semantics per vertical-layout rules.

---

## Phase 2 — Input mode/profile plumbing

- [x] Add global runtime input profile API (set/get) in seedsigner component layer
- [x] Enforce default-selection policy from profile (touch vs hardware)
- [x] Add optional `initial_selected_index` support for screens that need explicit initial focus
- [x] Keep per-screen config override optional and minimal (no pollution)

Exit criteria:
- Same binary can switch behavior by runtime profile only.

---

## Phase 3 — Layout-specific navigation rollout

- [x] `button_list_screen` finalize vertical behavior + top-nav transitions
- [x] `main_menu_screen` implement grid (2x2) directional neighbor behavior
- [x] Ensure BODY default focus remains in BODY for hardware mode
- [x] Validate top-nav enters/exits predictably from each layout

Exit criteria:
- Vertical and grid screens both conform to intended navigation behavior.

---

## Phase 4 — Desktop interactive runner (fast loop)

Goal: fast host iteration without ARMv6 emulation.

- [ ] Add desktop runner target (SDL/LVGL host build path)
- [ ] Reuse screenshot-generator `scenarios.json` as scenario source
- [ ] Add runner chrome (top bar) with single scenario dropdown (`screen / variation`)
- [ ] Add hotkey to open scenario selector (mouse-first, keyboard supported)
- [ ] Map keyboard input:
  - arrows -> `UP/DOWN/LEFT/RIGHT`
  - Return -> `ENTER`
  - `1/2/3` -> KEY1/KEY2/KEY3
- [ ] Keep scenario browser/switcher always available during runtime
- [ ] On scenario load, auto-shift focus into rendered screen for immediate interaction

Exit criteria:
- User can swap scenarios at will and immediately interact with each rendered screen.

---

## Phase 5 — Validation + regression

- [ ] Compile sanity checks for updated component signatures and nav module
- [ ] Manual nav validation matrix (vertical + grid + top-nav transitions)
- [ ] KEY1/2/3 default + override validation
- [ ] Touch behavior regression check
- [ ] Optional scripted key-sequence replay for deterministic checks

Exit criteria:
- No regressions in touch behavior; hardware navigation conforms to contract.

---

## Current checkpoint notes

- Canonical implementation target confirmed (`dev/seedsigner-c-modules`).
- Initial nav layer + `button_list_screen` integration has begun.
- Main menu requires grid handling (2x2) and is explicitly in-scope for rollout.

---

## Architecture migration reminders (long-term)

- [ ] Standardize LVGL acquisition for desktop/simulator workflows via explicit standalone clone (e.g., `third_party/lvgl` pinned to a project tag), instead of relying on ESP-IDF managed component paths.
- [ ] Define and document the canonical pinned LVGL tag/commit and update setup docs/scripts to enforce it.
- [ ] Fully separate SeedSigner LVGL screen/core code from ESP-target integration glue so the screen layer builds independently across desktop and ESP targets.
- [ ] Keep ESP-specific transport/board/bootstrap code behind target-specific adapters/interfaces.

## Completion / cleanup policy

When all phases are done:
1) confirm behavior against spec,
2) move any lasting rules into durable docs,
3) delete this implementation-plan file.
