# Language-pack production & delivery — finalized design record

**Status:** Design **re-examined and finalized 2026-07-06**, superseding the earlier
2026-07-05/06 notes and the prior handoffs/integration-todos in every repo. This is the
authoritative record; where any earlier commit, note, or downstream doc disagrees, **this
wins**. Implementation is a rework (§7) — several repos must *undo* work done against the
superseded design.

**Implementation status (2026-07-06):**

| Repo | State |
|---|---|
| **`seedsigner-language-packs`** (producer) | ✅ **DONE** — GHCR toolchain image published + public + **digest-pinned** in `build_packs.sh`; `--out-dir` defaults to `$SS_APP_DIR/src/lang-packs`; native-build docs. |
| **`seedsigner-lvgl-screens`** (this repo) | ✅ **DONE** (PR #52) — committed fixtures dropped + re-gitignored; CI builds packs from the pinned pack repo via the GHCR image; **`locales.h` + the baked `locale_font_table()` deleted** and every screens host registers pack manifests (§3a complete here). |
| **`seedsigner`** (app) | ⏳ pending — drop the `deps/language-packs` submodule + stage script → pure reader; wire the app runner seam to register manifests. |
| **`seedsigner-micropython-builder`** (ESP32) | ⏳ pending — repoint deploy at `$SS_APP_DIR/src/lang-packs`; ESP32 binding registers manifests. |
| **`seedsigner-raspi-lvgl`** (Pi) | ⏳ pending — repoint deploy at `$SS_APP_DIR/src/lang-packs`; Pi binding registers manifests. |

Deleting `locales.h` from screens **breaks the downstream Pi/ESP32/app builds** until each
wires manifest registration — expected and spec-sanctioned (it is all local dev; §3a). The
sections below remain the authoritative design; where they read as future tense for the pack
repo or screens, that work is **done**.

---

## 0. Why this was re-examined (read first)

The previous version of this doc over-rotated on one idea — *"commit nothing, build
everywhere"* — and that choice cascaded into a set of decisions that were more complex than
the problem needs, and in one place actually inverted the dependency direction the design is
supposed to have. Re-examining with fresh context, the corrections are:

- **The live pack-repo checkout is the source of truth in local dev — it *pushes* built
  packs into the app.** The superseded design had *screens* build packs into the app, and
  had the app *pull* from a pinned submodule via its own stage step. Both put a frozen pin
  (or the wrong repo) between "the pack change I'm testing" and "what renders." Removed.
- **The app is a pure reader with zero pack-build plumbing.** No submodule, no stage
  script. (Superseded design said "app submodules nothing" but then relied on it carrying
  `deps/language-packs` and a Docker stage script — internally inconsistent.)
- **Packs are purely additive; English-only-with-no-packs is a supported release.** A
  developer can clone the app and work on view logic without ever setting up packs.
- **Translations stay a pinned submodule** (reproducible, offline-signable), *not* a
  build-time floating fetch.
- **A GHCR toolchain image** is the accepted way for the pack repo, screens, and app CI to
  rebuild packs without reinstalling the shaping stack each run.
- **Device deployers point only at the app** (`SS_APP_DIR`), never at the pack repo.

What the earlier design got *right* and is kept: a self-sufficient `seedsigner-language-packs`
producer that owns format + policy (`locales.h`) + fonts + tooling; reproducible Docker builds
gated by a determinism check; signing as the deferred production channel.

**Decision taken here (2026-07-06): fast-track the manifest-driven migration (§3a).** Rather
than keep a vendored `locales.h` in screens as a lasting interim, we retire it entirely — every
non-English locale becomes fully self-described by its pack, and screens bakes only English. The
vendored table survives only as short-lived transitional scaffolding while the host-registration
wiring lands. This is more moving parts up front (it touches every rendering host) in exchange
for the clean destination: no vendoring, no drift, no drift guard, and "add a language with zero
screens recompile" made real.

---

## 1. Invariants

1. **Local dev: the live pack-repo checkout is the single source of truth.** Its build
   *pushes* output into the app's `src/lang-packs`. Nothing pins a frozen version between a
   pack edit and what every consumer renders.
2. **Packs are purely additive.** With no packs present the app degrades to the **baked
   English floor**, gracefully, in *every* environment (CPython/Pi, MicroPython/ESP32,
   desktop screenshot tool, web runner). A streamlined **English-only image with zero packs
   is a supported release**, and a clean `git clone` of the app runs English-only with **no
   pack setup at all**.
3. **Nothing commits built packs** — dev or signed-yet. Reproducibility comes from *pinned
   inputs + a determinism gate*, never from committed blobs.

---

## 2. The three modes (keep these distinct)

| Mode | Pack source | Pins | Committed packs |
|---|---|---|---|
| **Local dev** (all repos co-evolving) | rebuild from your **live pack-repo checkout** | none — you *are* the truth | none |
| **Reproducible-verify** (check a would-be release) | Docker, **pinned inputs**, build-twice `diff` | image digest + submodule SHAs | none |
| **Production** (signed) | fetch **signed** packs (deferred, §6) | release tag | signed only |

Committing unsigned dev packs serves nothing here: in local dev they'd be stale the moment
translations/fonts/policy move, and they add git binary bloat. So they are never committed.

---

## 3. Repos & responsibilities (final)

### seedsigner-language-packs — the builder / source of truth (PUBLIC)
Owns the pack **format** (manifest / SSA8 endonym / SSRB glyph-runs), the locale **policy**
`locales.h` (X-macro; C `#include`s it, `tools/locales_h.py` parses it — replaces
`screenshot_gen --dump-locales`), the **source fonts** (`fonts/`), and the **tooling**.

- **Translations:** a **pinned git submodule** (`seedsigner-translations`, currently the
  `lvgl-screens-stubs` fork branch carrying the `ur` stub). Pinning it is what makes builds
  reproducible and offline-signable; a dev who wants a different translation state checks out
  a different ref in the submodule. *(Fork-branch hygiene: slim the ~22 MB unused `fonts/`
  from that branch — the build reads only `l10n/<loc>/LC_MESSAGES/messages.po`.)*
- **Build:** `scripts/build_packs.sh` (Docker → reproducible/byte-identical) **or native**
  (`tools/build_fontpacks.py` directly, if you have the toolchain). **Docker is optional in
  local dev**; reproducible/signed builds require it. `--skip-if-built` / `--catalogs-only`
  fast paths stay.
- **Populate the app directly:** the local-dev entry point is
  `build_packs.sh --out-dir $SS_APP_DIR/src/lang-packs` (default there when `SS_APP_DIR` is
  set). Running the build from your live checkout materializes the truth into the app.
- **GHCR toolchain image:** publish the build *environment* (ubuntu + fontTools / uharfbuzz /
  PyICU / Pillow + the pinned-LVGL `lv_shape` fa oracle). It bakes **no** translations or
  pack content. Pack-repo/screens/app CI **pull** it to build without reinstalling the stack.
  Manually published from the pinned `tools/Dockerfile`; pin it by digest in consumers.
- **Commits no packs.** CI = reader parity + build-twice `diff` (determinism) + parity gate,
  read-only, never signs, never commits.

### seedsigner — the app: pure reader, zero pack plumbing
- Reads `src/lang-packs` (CPython/Pi, CWD-relative `"lang-packs"`) / `/sd`
  (MicroPython/ESP32) via `SettingsConstants.get_catalog_root()`. **No app-code change.**
- **No `deps/language-packs` submodule. No `stage_language_packs.sh`.** The pack-repo build
  pushes into `src/lang-packs` (gitignored). CI pins the pack repo via a workflow
  checkout-at-ref (not a tracked submodule).
- **English-only-without-packs is guaranteed and tested** — a no-packs run proves the app
  starts and renders English on every target; absent packs is never an error.
- Language tests read whatever the build deposited in `src/lang-packs`; **no bundled `.mo`
  fallback, no committed fixture** — they **skip** when packs are absent. A view-logic dev
  clones the app and runs `pytest` with language tests auto-skipping.
- Drop the runtime-dead `resources/seedsigner-translations` submodule **after** repointing
  the screenshot-generator `.po` progress stat off it. ⚠ First resolve **fork-vs-org**: the
  packs' `.mo` come from the kdmukAI-bot fork (`ur` stub); the bundled submodule is the
  SeedSigner-org canonical — they differ (§8).

### seedsigner-micropython-builder (ESP32) & seedsigner-raspi-lvgl (Pi) — deploy the app payload
- **One pointer: `SS_APP_DIR`.** Read `$SS_APP_DIR/src/lang-packs` (the app's already-staged,
  deployable payload) and copy to `/sd` (ESP32) / device `lang-packs` (Pi). **No pack-repo
  knowledge, no `SS_LANGPACKS_REPO_DIR`, no `SS_LANGPACKS_USE_SIGNED`, no build-on-first-use.**
  Signed-vs-dev is invisible: whatever the app bundled is what deploys.
- **Empty/absent `src/lang-packs` = a valid English-only deploy**, not an error.
- builder: `sd_format_push.py` stays the runtime-file-filtered byte copy (`.ttf`, `runs.bin`,
  `manifest.json`, `endonym_*.bin`, `LC_MESSAGES/messages.mo`; skips `runs.json`).
- Pi: `deploy-dev.sh` keeps the straight-into-`lang-packs` rsync + leftover-symlink removal.
  Its `sources/seedsigner-lvgl-screens` submodule (builds the `.so`) stays.

### seedsigner-lvgl-screens (this repo) — render layer + its own gallery
- **Vendored `locales.h` has been retired (§3a — DONE).** `components/seedsigner/locales.h` +
  the baked `locale_font_table()` are **deleted**; screens bakes only the English floor and reads
  every non-English locale's policy from its pack `manifest.json` at runtime. `locales.h` lives
  only in the pack repo now. No drift guard (the table is gone).
- **Its own hosts register manifests:** the gallery/screenshot-gen and the web runner each scan
  available packs and call `ss_register_pack_manifest` before rendering, same as the device hosts
  (§3a). (The desktop SDL2 runner is English-only by design — no locale machinery, nothing to register.)
- Needs packs only for its **multilingual gallery** and **runner-core-test**. **Local dev**
  builds them from the live sibling pack checkout (`build-fontpacks` helper, default
  `../seedsigner-language-packs`). **CI** pulls the GHCR image, checks out the pack repo at a
  pinned ref, builds packs, regenerates localized scenarios, and renders. **No committed
  packs / fixtures.**
- Keeps gallery helpers: `gen_localized_scenarios.py`, `po_catalog.py`, `supported_locales.json`.
- Desktop tools + web runner must render **English-only cleanly with no packs**.

---

## 3a. Locale policy: retiring `locales.h` from screens (FAST-TRACKED)

The vendored `locales.h` / baked `locale_font_table()` in screens is the unfinished half of the
runtime-discovery migration, and **we are finishing it now** (decided 2026-07-06). The mechanism
to retire it **already exists and is exercised by tests**:

- `ss_register_pack_manifest()` parses a pack's `manifest.json` into the **same
  `LocaleFontEntry`** the baked table produces; `find_locale_font_entry()` prefers **runtime
  entries over compiled rows**. So a registered manifest already overrides the baked row.
- The on-disk manifest already carries **every field the renderer consumes at runtime**
  (`chain`, `rtl`, `shaping`, and — via `chain` → `default_locale_roles()` — per-role base
  sizes). `unicode_range` / `script` / `source_family` are builder-only, never read at render.
- What correctly **stays baked in screens regardless**: the English/baseline floor (invariant
  #2) and **display-profile size scaling** (`locale_role_render_px()` — a function of the
  display, not the locale).

**Target:** screens bakes only English; every non-English locale is fully self-described by
its pack; `components/seedsigner/locales.h` is **deleted from screens** and lives only in the
pack repo as the builder's input (which stamps policy into each manifest). This removes
vendoring, drift, and the drift guard entirely, and makes "add a language with zero screens
recompile" actually true.

**Status:** items 1, 3, 4 are **DONE in screens** (PR #52); item 2 (the device/app hosts) is
**pending** in the downstream repos. The work items:

1. ✅ **Screens** — the scan→register loop is wired into screens' own hosts (`screenshot_gen`'s
   `register_packs_from_dir()`; the web runner's `ss_register_manifest` export + JS fetch). The
   pack builder emits every render-consumed field into `manifest.json` (`chain`, `rtl`, `shaping`;
   per-role base sizes come from `chain` via `default_locale_roles()`). (The desktop SDL2 runner is
   English-only — no locale machinery, so nothing to register there.)
2. ⏳ **Every device/app host** — wire the **scan → `ss_register_pack_manifest`** loop: the Pi
   binding, the ESP32 binding, and the app runner seam each read each available pack's
   `manifest.json` and register it at discovery / `set_locale`. (Pending in those repos.)
3. ✅ **Deleted** `components/seedsigner/locales.h` + the baked `locale_font_table()` rows from
   screens. `locales.h` now lives only in the pack repo as the builder's input.
4. ✅ **Parity verified** — `screenshot_gen --dump-locales` is byte-identical to the pre-migration
   baked-table dump across all four profiles (before and after the delete); the runner-core test
   loads zh/hi/ru from registered manifests (ALL OK).

**Ordering is a convenience, not a gate.** This is a **local-dev-only** sweep across repos, so a
broken intermediate state is expected and fine — non-English will render wrong (or fall back to
English) until every host registers manifests. Rip out `locales.h` whenever convenient and wire
hosts before/after; nothing in production depends on the interim. This is the **same work as
SD-pack-discovery "Part B"** already flagged in the downstream repos — folded into this effort.

---

## 4. The local-dev flow (one build, everyone sees it)

```
# Optional — only if you're working on translations/fonts/rendering:
edit ../seedsigner-language-packs
  → ./scripts/build_packs.sh --out-dir ../seedsigner/src/lang-packs     # Docker or native
      → app runtime + app language tests see it
      → sd_format_push.py / deploy-dev.sh (SS_APP_DIR) deploy it to ESP32 / Pi
      → a screens gallery rebuild renders it

# The common path — view logic, models, anything non-i18n:
git clone seedsigner && run           # English-only, zero pack setup, tests auto-skip i18n
```

No committed packs. No Docker required in the loop. The live pack-repo checkout is the truth;
`src/lang-packs` is where it's deposited; every consumer reads that one location.

---

## 5. CI model (per repo — no sibling checkouts exist in CI)

- **Pack repo:** reader parity + build-twice `diff` (determinism) + parity, via the image.
  Never signs, never commits.
- **Screens:** pull the GHCR image → checkout the pack repo at a pinned ref → build packs +
  regenerate localized scenarios → gallery + runner-core-test.
- **App:** pull the GHCR image → build packs at a pinned pack-repo ref → language tests. Plus
  a **no-packs run** asserting English-only starts/render.
- **Builder / Pi:** deploy-only (no pack build).

---

## 6. Signed-pack / production channel (DEFERRED)

Until the signing scheme lands there are no signed packs and everything builds locally. When
it lands: reproducibly build → embed a **content hash of the unsigned bytes** → **sign
offline** (key never in CI). Two independent checks for consumers — rebuild→hash→compare
(keyless integrity) and signature verify (authenticity); on-device via the `ss_pack_provider`
chokepoint. **Open:** whether signed packs are committed `signed-packs/<locale>/` or shipped
as GitHub-Release assets indexed by a committed manifest — decide with the scheme, not now.
Consumers' contract is unchanged either way: signed packs land in the app's `src/lang-packs`
and deployers copy from there.

---

## 7. Migration — what each repo must change / **undo**

The four downstream repos built against the superseded design; several changes must be
reversed. (Edits happen in each repo's own session — this doc is the shared spec.)

**Pack repo** — ✅ **DONE.** Kept the translations submodule, local Docker build,
determinism/parity CI, empty `signed-packs/`. Published the GHCR image (public) + pointed
`build_packs.sh` at it **pinned by digest** (local `docker build` fallback retained); `--out-dir`
defaults to `$SS_APP_DIR/src/lang-packs` when set; native build documented. Did **not** convert
translations to a fetch or add release-asset scaffolding.

**Screens (this repo — PR #52 `feat/consume-locales-h-from-language-packs`) — ✅ DONE:**
- **Remove committed fixtures** `lang-packs/` (66 files) + `tools/scenarios/localized/` (23
  files); re-gitignore both.
- CI: pull the GHCR image, checkout the pack repo at a pinned ref, build packs, regenerate
  localized scenarios, render gallery + runner-core-test.
- Keep the gallery helpers (`gen_localized_scenarios.py`, `po_catalog.py`, `supported_locales.json`).
- **Manifest migration (see §3a + the cross-cutting block below):** wire screens' own hosts
  (gallery/screenshot-gen, web + desktop runners) to `ss_register_pack_manifest`, and delete the
  vendored `locales.h` + baked rows (order is free — it's all local dev, breakage in the middle is
  expected). **No drift guard** (the table is being removed).
- Clean up stale references: `stage_assets.py` docstring/`--regen-packs` + `deps/language-packs`
  mentions; `.gitignore` `tools/i18n/{spike_out,validate_out}/` entries.

**App (`feat/language-selection` — simplify to pure reader):**
- **Drop `deps/language-packs` submodule; delete `scripts/stage_language_packs.sh`.**
- Keep the runtime repoint (`get_catalog_root`) + the `/src/lang-packs/` gitignore.
- Remove the conftest fallback to `resources/seedsigner-translations`; language tests read
  `src/lang-packs`, **skip if absent**.
- Add the **no-packs English-only** CI run/test.
- Repoint `tests/screenshot_generator/generator.py`'s `.po` stat, then drop the
  `resources/seedsigner-translations` submodule. Resolve fork-vs-org (§8).
- **Manifest migration:** the app runner seam (`lvgl_screen_runner.py`, around
  `discover_locale_packs` / `set_locale`) reads each available pack's `manifest.json` and
  registers it via the binding's `ss_register_pack_manifest` before/at locale activation.

**Builder (ESP32):**
- Repoint `sd_format_push.py` / `_devenv.resolve_packs()` to read **`$SS_APP_DIR/src/lang-packs`**.
- Delete `SS_LANGPACKS_REPO_DIR`, `SS_LANGPACKS_USE_SIGNED`, build-on-first-use, and the
  pack-repo `.po`-submodule bootstrap. Absent packs = English-only deploy.
- **Manifest migration:** the ESP32 binding registers each `/sd/<locale>/manifest.json` via
  `ss_register_pack_manifest` at discovery (part of moving `bindings/` here anyway).
- Rewrite the stale `.claude/handoff.md` + `docs/language-pack-repo-integration-todo.md`.

**Pi:**
- Repoint `scripts/deploy-dev.sh` pack source to **`$SS_APP_DIR/src/lang-packs`**; delete
  `SS_LANGPACKS_REPO_DIR`/`USE_SIGNED`/build-on-first-use. Keep the symlink-removal + straight
  rsync. Absent packs = English-only deploy. Advance the `sources/seedsigner-lvgl-screens`
  pin post-#52. Rewrite the stale handoff + integration todo.
- **Manifest migration:** the Pi binding (`module.cpp`) registers each
  `lang-packs/<locale>/manifest.json` via `ss_register_pack_manifest` at discovery.

**Cross-cutting: the `locales.h` retirement (fast-track, §3a).** Work items across repos —
screens exposes/uses the register path in its own hosts; app + Pi + ESP32 bindings each wire
scan→register; screens deletes `components/seedsigner/locales.h` + the baked rows; verify
`supported_locales_json` parity at the end. **Order is a convenience, not a gate** — it's all
local dev, so non-English will be broken mid-sweep and that's expected; do the pieces in whatever
order is easiest. `locales.h` ends up living only in the pack repo.

**Note:** the builder's and Pi's committed code already de-hardcoded paths and carry **no**
`deps/language-packs` submodule — but they still resolve packs from `SS_LANGPACKS_REPO_DIR`
(the pack repo), which this design replaces with `$SS_APP_DIR/src/lang-packs` (the app). Their
handoff/todo docs describe *un-started* work that is partly already done and partly now
wrong — rewrite them to this spec.

---

## 8. Open / deferred

1. **Signing scheme** — algorithm, key management, content-hash/manifest format, on-device
   `ss_pack_provider` path; and committed-`signed-packs/` vs. release-asset delivery (§6).
2. **GHCR image mechanics** — exact contents + the manual publish/versioning trigger; pin by
   digest in consumers.
3. **Fork-vs-org translations source-of-truth** for the app's `.mo` tests: packs build from
   the kdmukAI-bot fork (`ur` stub); the app bundles the SeedSigner-org canonical. Reconcile
   before dropping the app's bundled submodule.
4. Repo name stays **`seedsigner-language-packs`** (producer + home + release channel).

*(The manifest-driven `locales.h` retirement is no longer open — it was **decided (fast-track)**
2026-07-06 and is specced in §3a + §7.)*
</content>
</invoke>
