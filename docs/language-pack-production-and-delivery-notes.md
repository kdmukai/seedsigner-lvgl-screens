# Language-pack production & delivery — decision record

_Originally (2026-07-05, morning) this doc posed the **open question** of where
language packs should be produced and delivered. That question is now **decided and
bootstrapped**. This is the decision record; the authoritative runbook lives in the
new pack repo's `README.md`._

## Decision

Language packs are produced by a **new, dedicated, self-sufficient repo,
`seedsigner-language-packs`** — NOT by this repo, and NOT bundled into the app.

- The pack repo owns the **format** (manifest / SSA8 endonym / SSRB glyph-runs), the
  **locale policy** (`locales.h`), the **producer tooling**, and the **source fonts**.
  Its only submodule is `seedsigner-translations` (the `.po`); LVGL (for the `lv_shape`
  `fa` oracle) is pinned in a **Docker image**, not submoduled. It depends on nothing
  here.
- **This repo is a consumer.** Its C parsers conform to the format, and it takes
  locale policy from a **vendored copy of `locales.h`** (see
  `docs/knowledge/language-pack-format-and-policy-authority.md`) plus runtime pack
  manifests (`ss_register_pack_manifest`).
- **Reproducible + signed delivery.** The Docker image emits byte-identical *unsigned*
  packs (verified: two builds `diff`-clean; all device artifacts match this repo's
  previously-built packs, differing only in the recorded `icu_version`). Only **signed**
  release packs are committed (offline signer, key never in CI, per-locale cadence),
  carrying a content hash for keyless rebuild-and-verify. **Delivery is a copy step**
  into each runtime path (`src/lang-packs`, `/sd`).

Rejected alternatives (for the record): bundling packs into the app (fonts are large;
production is render-layer-coupled) and app-submodules-screens (drags the whole C/LVGL
build stack into Python business logic). The decided model separates production from
consumption via a signed, copyable release — without any consumer submoduling screens.

Full rationale + rollout sequence: the approved plan
(`~/.claude/plans/there-s-a-new-to-do-toasty-hamster.md`) and memory
`project_language_pack_production_delivery`.

## What was built (this session)

- **Phase 0** — exported this repo's live locale policy to seed the master `locales.h`
  (X-macro table; verified to reproduce `screenshot_gen --dump-locales` exactly).
- **Phase 1** — bootstrapped `seedsigner-language-packs`: `seedsigner-translations`
  submodule (pinned), tooling + source fonts copied in, `locales.h` + its ~Python
  reader (`locales_h.py`, replacing `--dump-locales`), a pinned-by-digest Docker
  builder (+ LVGL v9.5.0 for `lv_shape`), `scripts/build_packs.sh`, determinism pins
  (`--no-recalc-timestamp`, `PYTHONHASHSEED=0`, exact `Pillow`/tool pins), and CI
  (build-twice determinism gate + parity gate). Reproducibility verified.
- **Phase 2b (screens) — full cutover** — vendored `locales.h` here and refactored
  `locale_font_table()` to **generate** its rows from it via the `SS_LOCALE` X-macro
  (behavior-preserving: `--dump-locales` byte-unchanged); then submoduled the pack repo
  at `deps/language-packs` (recursive), repointed `scripts/ci/ci.sh` `build-fontpacks`
  to build `lang-packs/` via the submodule (`LVGL_ROOT` → this repo's LVGL for the `fa`
  oracle; no more `screenshot_gen --dump-locales`), dropped the now-needless
  `build-screenshots` step from `runner-core-test`, repointed `gen_localized_scenarios`
  at the submodule's translations and removed this repo's own translations submodule,
  and **deleted all screens-owned pack-build tooling** (`build_fontpacks.py` + friends,
  the source fonts already live in the submodule, the `lv_shape` shaper, the spikes).
  `tools/i18n/` now holds only the gallery helpers.

Distribution nuance: the screens gallery/CI build **functional** packs natively (host
toolchain); **byte-reproducible / signable** packs come from the submodule's Docker
path and matter only for signed production delivery.

## What remains (deferred)

- **Signing scheme** (algorithm / key mgmt / content-hash format / bootloader path) and
  **reproducible-build governance** (publish the per-release input closure). Until the
  scheme lands, the pack repo commits no signed packs.
- **Consumers, in their own repos:** app stage step → `src/lang-packs` (Phase 2a); Pi
  re-source + drop the device symlink (Phase 3); ESP32 `sd_format_push.py` repoint
  (Phase 4). These want *signed* packs (copy-step delivery), so they follow the signing
  decision.
