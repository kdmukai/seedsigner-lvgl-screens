# Language-pack format & locale policy: who owns what

**One-line:** the language-pack **format** and the **locale→font policy** are owned
by the separate **`seedsigner-language-packs`** repo. This (screens) repo is a
**consumer**: it renders/parses packs and takes locale policy from a **vendored copy
of `locales.h`**. Don't hand-author policy here.

## Why this split exists

Language selection gave the SeedSigner app a hard runtime dependency on language
packs (subset fonts + `runs.bin` + endonym images + `messages.mo`, one self-contained
unit per locale). Those packs must be **produced reproducibly and signed** for
distribution — work that does not belong in the C/C++ render layer. So pack
production, the source fonts, and the master locale policy moved into a dedicated,
**self-sufficient** repo (`seedsigner-language-packs`) that depends on nothing here.
The dependency is one-directional (screens → pack repo, by *copying* `locales.h`) —
no cycle.

See that repo's `README.md` for the producer/reproducibility/signing model, and this
repo's `docs/language-pack-production-and-delivery-notes.md` for the decision record.

## The policy authority: `locales.h`

`components/seedsigner/locales.h` is a **vendored, verbatim copy** of the master file
in the pack repo. It is an **X-macro table** — the single source of truth for which
non-baked-floor locales exist and each one's `{source_family, chain, unicode_range,
rtl, shaping, script, endonym}`. Two worlds read the *same* file with no codegen:

- **C** (`locale_fonts.cpp`) `#include`s it with `SS_LOCALE` defined to expand each
  row into a `LocaleFontEntry`. So `locale_font_table()` is **generated**, not
  hand-written.
- **Python** (the pack repo's `tools/locales_h.py`) parses the same
  `SS_LOCALE(...)` / `SS_DISPLAY_PROFILE(...)` lines.

`locales.h` **replaced `screenshot_gen --dump-locales`** as the builder's policy input:
the pack builder no longer compiles the render layer to learn policy. (`--dump-locales`
still exists here for this repo's *own* multilingual screenshot gallery — see "current
state" below.)

### Non-obvious bits

- The X-macro expansion in `locale_fonts.cpp` derives the per-role sizes from the
  chain via `default_locale_roles(chain)` (Primary = CJK legibility bump; Fallback =
  same-size baseline), and **ignores** the `endonym` arg — screens gets endonyms from
  pack manifests / the picker, not from this table. The builder is what uses the
  endonym (to pre-render the picker image), and it reads `locales.h` directly.
- `locales.h` also carries an `SS_DISPLAY_PROFILE(...)` table + button-base constants
  so the builder can compute per-height endonym sizes **without** `--dump-locales`.
  These are inert for the C `#include` (guarded by `#ifdef SS_DISPLAY_PROFILE`).
- The vendored copy is kept **byte-identical** to the master so re-vendoring is a
  plain `cp`. To change policy: edit `locales.h` **upstream**, rebuild/re-release
  packs, then re-vendor the copy here. Never hand-edit rows in either the header or
  `locale_fonts.cpp`.

## Verifying the refactor is safe

`locale_font_table()` generating rows from `locales.h` is behavior-preserving:
`screenshot_gen --dump-locales` is **byte-identical** before and after. Any future
`locales.h` edit that must not change rendering can be checked the same way.

## Current state (2026-07-05) — cutover done

The screens side is fully cut over:

- **`deps/language-packs`** is the pack repo, vendored as a submodule (recursive — it
  brings its own `seedsigner-translations`). Screens no longer carries a separate
  translations submodule.
- **Screens owns no pack-build tooling.** `tools/i18n/` now holds only the gallery
  helpers (`gen_localized_scenarios.py`, `po_catalog.py`, `supported_locales.json`);
  `build_fontpacks.py` + friends + the source fonts + the `lv_shape` shaper all live in
  the submodule.
- **`scripts/ci/ci.sh build-fontpacks`** builds `lang-packs/` by delegating to
  `deps/language-packs/tools/build_fontpacks.py` (reads the submodule's `locales.h`, no
  `screenshot_gen --dump-locales`), pointing its `fa` oracle at this repo's LVGL via
  `LVGL_ROOT`. `runner-core-test` no longer compiles `screenshot_gen` to get packs.

The vendored `components/seedsigner/locales.h` (which drives `locale_font_table()`) is
still a plain copy of the submodule's `deps/language-packs/locales.h`; keeping the C
`#include` copy in-tree means a normal C build needs no submodule checkout, while the
pack build reads the submodule's master. Re-vendor with:
`cp deps/language-packs/locales.h components/seedsigner/locales.h`.

`lang-packs/` is gitignored — built on demand. Screens' gallery/CI build **functional**
packs natively (host toolchain); byte-reproducible / signable packs are the submodule's
Docker path (`deps/language-packs/scripts/build_packs.sh`) and matter only for signed
production delivery, not the gallery.
