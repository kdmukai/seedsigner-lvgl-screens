# Language-pack format & locale policy: who owns what

**One-line:** the language-pack **format** and the **locale→font policy** are owned
by the separate **`seedsigner-language-packs`** repo. This (screens) repo is a pure
**consumer**: it renders/parses packs and learns each locale's policy from that pack's
**`manifest.json` at runtime**. Screens bakes **no** locale table and does **not**
vendor `locales.h` — don't hand-author locale policy here.

## Why this split exists

Language selection gave the SeedSigner app a hard runtime dependency on language
packs (subset fonts + `runs.bin` + endonym images + `messages.mo`, one self-contained
unit per locale). Those packs must be **produced reproducibly and signed** for
distribution — work that does not belong in the C/C++ render layer. So pack
production, the source fonts, and the master locale policy (`locales.h`) live in a
dedicated, **self-sufficient** repo (`seedsigner-language-packs`) that depends on
nothing here. The dependency is one-directional (screens consumes packs; the pack repo
knows nothing about screens) — no cycle.

See that repo's `README.md` for the producer/reproducibility/signing model, and this
repo's `docs/language-pack-production-and-delivery-notes.md` for the decision record.

## The policy authority: `locales.h` (lives ONLY in the pack repo)

`locales.h` is the master locale policy — an **X-macro table** that is the single
source of truth for which non-baked-floor locales exist and each one's
`{source_family, chain, unicode_range, rtl, shaping, script, endonym}`. It lives in
the **pack repo**, where it serves two builder-side consumers with no codegen:

- **C** — the pack repo's `lv_shape` Arabic/Persian oracle `#include`s it.
- **Python** — the pack repo's `tools/locales_h.py` parses the `SS_LOCALE(...)` /
  `SS_DISPLAY_PROFILE(...)` lines to drive subsetting + per-height endonym rendering.

The builder **stamps this policy into every pack's `manifest.json`**, which is the
render-time contract. Screens reads the manifest, never `locales.h`. (`locales.h`
replaced the old `screenshot_gen --dump-locales` as the builder's policy input, so the
builder no longer compiles the render layer to learn policy.)

> **History:** screens used to vendor a byte-identical copy of `locales.h` and expand
> it into a compiled-in `locale_font_table()`. That vendored copy + baked table were
> **deleted (2026-07-06)** once every rendering host learned to register pack manifests
> (see below). If you find a reference to `components/seedsigner/locales.h`,
> `locale_font_table()`, or re-vendoring policy, it is stale.

## How screens learns a locale's policy: runtime manifest registration

A locale becomes renderable by **registering its pack's `manifest.json`**:

- `ss_register_pack_manifest(json, len)` (`locale_loader.cpp`) parses a manifest into a
  `LocaleFontEntry` and adds it to the runtime set. `find_locale_font_entry()` and
  `supported_locales_json()` (`locale_fonts.cpp`) read **only** that runtime set; an
  **unregistered** locale returns `nullptr` and renders on the **baked English floor**.
- The render layer is deliberately **I/O-free** — it never opens files. Each **host**
  scans the available packs and calls `ss_register_pack_manifest` before rendering:
  - **screenshot_gen** — `register_packs_from_dir(--font-dir)` (sorted → deterministic),
  - **web runner** — JS fetches each `manifest.json` and calls the `ss_register_manifest`
    keepalive export before `ss_pack_files`,
  - **device bindings** (Pi / ESP32) + the **app runner** — scan each pack's
    `manifest.json` at discovery / `set_locale`.

So **adding or changing a locale needs no screens recompile** — drop the pack, the host
discovers and registers it.

### What the manifest carries vs. what stays baked

- **From the manifest (per-locale):** `chain`, `rtl`, `shaping`, `script`, and — via
  `chain` → `default_locale_roles()` — the per-role base sizes. These are everything
  the renderer consumes at runtime. (`source_family`, `unicode_range`, `endonym` are in
  the manifest but are **builder-only** — never read at render.)
- **Baked in screens (NOT per-locale):** the **English/Latin floor**; the chain→role
  presets `default_locale_roles()` (`cjk_primary_roles` = CJK legibility bump;
  `opensans_fallback_roles` = same-size baseline); and **display-profile scaling**
  `locale_role_render_px()` (a function of the display, incl. the `large_button` 20↔18
  quirk). These are functions of `chain`/display, not of a specific locale, so they stay.
- **Endonyms** are decoupled entirely: the picker renders the pre-baked
  `endonym_<h>.bin` image through the provider seam, not from any policy table.

## Verifying the migration was safe

The manifest-driven set **reproduces the former baked table by construction** (each
manifest was generated from `locales.h`). This was verified with
`screenshot_gen --dump-locales`: its output is **byte-identical** to the pre-migration
baked-table dump across all four display profiles, both before and after deleting the
table. `--dump-locales` still exists in screens (it scans + registers manifests, then
dumps) as a parity/diagnostic tool for its own gallery — it is no longer a builder input.

## Current state & the full production/delivery model

Format/policy ownership above is stable and **implemented**. The **production +
delivery model** (where packs are built, how consumers get them, signed-pack releases)
is recorded authoritatively — with the finalized model, per-repo migration, and open
items — in:

> **`docs/language-pack-production-and-delivery-notes.md`** (read that as the spec).

Short version relevant to *this* repo: the **live pack-repo checkout is the local-dev
source of truth** and *pushes* built packs into the SeedSigner app's `src/lang-packs`
(`build_packs.sh --out-dir $SS_APP_DIR/src/lang-packs`) — screens does **not** build into
the app. Screens needs packs only for its **gallery + runner-core-test**: local dev builds
them from the live sibling pack checkout; **CI** pulls the pack repo's pinned **GHCR
toolchain image** and builds them at a pinned pack-repo ref. **Nothing commits built
packs** — `lang-packs/` is gitignored and built on demand. Packs are **purely additive**
(no packs → the baked English floor, so the desktop tools / web runner render English-only
cleanly with no packs).
