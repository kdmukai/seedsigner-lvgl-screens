# Complex-script glyph-run pipeline — production constraints

_The offline HarfBuzz shaping pipeline (`tools/i18n/hb_shaper.py`,
`shape_inventory.py`, `build_fontpacks.py`'s complex-shaping path) that turns a
locale's `.po` catalog into a pre-shaped glyph-run pack
(`lang-packs/<loc>/{<loc>.ttf, runs.json, manifest.json}`). The de-risking spike
(`docs/knowledge/offline-harfbuzz-shaping-spike-findings.md`) proved the render
path on five hand-picked strings; productionizing it over the **whole catalog**
surfaced constraints the spike's clean inputs never exercised. This records the
non-obvious ones so they aren't rediscovered._

## 1. Shape per LINE — a newline has no glyph, and shaping across one is wrong

Real translations contain hard line breaks (`"You can remove\nthe SD card now"`).
HarfBuzz has no glyph for `U+000A`, so shaping a string containing one emits a
`.notdef` (silent tofu) for the newline — **and**, for cursive scripts, would join
across the break. The fix is structural, not a filter: split every string on `\n`
and shape each line independently (`hb_shaper.shape_lines`, and the same split
inside `decomposition_closure`). A run table entry therefore holds a LIST of line
runs; the device stacks them like any multi-line label. Each line run is
`{"glyphs": [...], "breaks": [...]}` — see §"Word-wrap" below.

## 1b. Word-wrap marks — line-break opportunities computed OFFLINE

A `\n` is a HARD break (above). Within a line, the device must also wrap to the
label width — but where a line MAY break is a language question, so it's decided
offline and the device just greedy-fits. `hb_shaper.line_break_indices()` runs
each line through **ICU's dictionary-aware line `BreakIterator`** (PyICU over the
system libicu): real WORD boundaries for the no-space scripts (Thai/Lao/Khmer/
Burmese, where `str.split()` finds nothing), spaces/UAX-14 elsewhere. The result
is a `breaks` list of GLYPH INDICES (break-before). Mapping is direct: ICU offsets
and uharfbuzz clusters are both codepoint indices (our scripts are all BMP), so a
boundary at codepoint `c` → the first glyph whose `cluster == c`. RTL returns `[]`
(the renderer doesn't wrap RTL yet). The system ICU version is recorded as
`icu_version` in the manifest, so an ICU bump (which can shift Thai boundaries) is
a reviewed re-segmentation event — exactly like `harfbuzz_version`.

Corollary: an empty line (`"a\n\nb"`) yields an empty HarfBuzz buffer whose
`glyph_infos` is `None`, not `[]` — `shape()` special-cases `text == ""`.

## 2. Subset closure must include HarfBuzz's *decomposition*, not just GSUB closure

(Carried from the spike, restated because the production builder MUST do it.)
`pyftsubset --layout-features='*'` keeps GSUB-reachable glyphs (conjuncts,
positional/ligature forms — none with codepoints). But HarfBuzz also decomposes at
the Unicode level **before** GSUB (Thai SARA AM `U+0E33` → `U+0E4D` + `U+0E32`).
Those targets have codepoints but aren't GSUB-reachable, so a literal subset drops
them → `.notdef`. `decomposition_closure()` recovers them: shape each line against
the FULL font, reverse-map every output gid back to a codepoint via the cmap, add
those to the subset request. A hard `.notdef`/gid-range assert
(`_assert_runs_clean`) gates the bake.

## 3. Pure-ASCII strings get NO run (and the mixed-font-line tension stays open)

Many msgstrs are untranslated (left English) or numeric/symbol-only — pure ASCII.
Shaping them would (wastefully, and slightly wrongly) draw Latin via the *script*
subset at the bumped script size. They need no shaping: classify as `ascii` and
emit no run, so the device renders them on the normal codepoint path (baked
OpenSans / fallback), exactly as embedded English is handled for the CJK packs.
This also fixed a spurious validation FAIL — Thai `"Seeds"` is untranslated, and
its Latin glyphs through NotoSansTH at 48px tripped the ink-IoU AA threshold.

**Still open (deferred to the render layer):** a MIXED string (`"BIP39 शब्द"`) has
non-ASCII, so it gets a run that shapes the WHOLE line — including `BIP39` — in the
script subset. That embedded Latin renders in the script font at the bumped size,
diverging from the CJK "embedded-English-at-English-size" design. Per-segment fonts
on one line (Latin run on OpenSans + script run on the subset) is the proper fix
and is a layout concern, not a shaping one (the spike flagged it lower-risk).

## 4. Placeholder shaping-safety is about *non-ASCII letters*, not "any letter"

`shape_inventory.classify()` segments a `{}`-template only when each hole sits at a
shape-safe boundary. The boundary is unsafe **only** if the adjacent character is a
non-ASCII letter (a complex-script letter that joins/conjoins) — then shaping the
two sides independently could diverge from shaping the joined result. A string
edge, whitespace, digit, punctuation, **or an ASCII/Latin letter** is safe: a
Latin `x` in `"Begin {}x{}"` shapes as a standalone glyph in a Devanagari/Thai/
Arabic run and never conjoins with the inserted numeric value. Treating "any
letter" as unsafe wrongly rejected that case; the real risks (a Devanagari ordinal
fused to its number, a Thai vowel abutting a hole) are correctly flagged
`unsupported` with a reason and reported loudly — never silently dropped.

## 5. Ingest EVERY plural form, not just `msgstr[0]` — keyed by text, deduped

A gettext plural entry carries one msgid but several translations — `msgstr[0]`,
`msgstr[1]`, … — and the runtime picks one via `ngettext(n)`. The earlier `.po`
reader mapped only `msgstr[0]` onto the singular msgid, so any *other* form was
never subset in nor shaped → a guaranteed tofu the moment the app showed it. The
concrete miss: Hindi `input`/`inputs` → `इनपुट` (form 0, shaped) but `इनपुट्स`
(form 1) **had no run**; the device showed `2 इनपुट्स` as boxes. (Thai is
`nplurals=1`, so it was accidentally fine; **Arabic is `nplurals=6`** — zero/one/
two/few/many/other — so this is not a corner case for the languages still to come.)

The fix is upstream in the `.po` reader, so every consumer benefits: one grammar
walk (`po_catalog.parse_records`) captures **all** forms indexed by the declared
`msgstr[N]` (no hardcoded 0/1 — arbitrary `nplurals`), and `iter_translations()`
yields one `(msgid, msgstr)` pair **per form**. The corpus builders and the
complex-script pipeline both source it, so every form's glyphs land in the subset
and every form gets a run. `parse_catalog` (msgid→`msgstr[0]`) stays the *singular
view* for plain msgid→display lookups (`gen_localized_scenarios`).

Two properties make this clean rather than a special case:

- **Runs are keyed by translated TEXT, not msgid** (`glyph_runs.cpp::by_text`).
  Plural forms share a msgid but differ in text, so they become distinct runs with
  no key collision; the singular msgid only travels along in `runs.json` for debug.
- **`build_units` dedups by `msgstr`.** Forms that coincide (Hindi
  `recipient`/`recipients` → both `प्राप्तकर्ता`; every Thai "plural" form;
  Arabic forms that share a string) collapse to one run — matching what the device
  would do anyway (`by_text` overwrite). This also removed 21 pre-existing
  duplicate-text runs in hi (337 → 317) that the old msgid-keyed loop emitted
  redundantly. Order is `sorted((msgid, msgstr))` so the build stays byte-identical.

Validation: `validate_runs.py --msgids-all` no longer collapses by msgid (it would
have hidden the second form), so the oracle IoU gate now covers `इनपुट` **and**
`इनपुट्स` (both PASS). This is the per-language readiness checklist's "all plural
forms shaped" item (the i18n release model's *Deliverable C*).

## Run-table footprint: `runs.bin` is what the device loads (`runs.json` is debug)

`runs.json` is verbose JSON (~0.5 MB for hi/th's ~330 runs; full per-glyph
records at ~87 bytes/glyph — almost all repeated field names + ASCII numbers, plus
a `cluster` field the device never reads). Fine as a build/desktop artifact, but on
the ESP32-S3 the acute cost is the transient `nlohmann` DOM built while parsing half
a megabyte of text on a 512 KB-SRAM part.

So the pipeline now also emits a compact **binary blob `runs.bin`** (`tools/i18n/
runs_bin.py`, magic `SSRB`, little-endian) — the same `with_runs` units, ~8 bytes/
glyph (**~10× smaller**), which the device walks straight into its run table with no
JSON DOM. `cluster` is dropped (informational; the device never read it — the word-
break info it fed is already baked into `breaks`), `y_advance` is dropped (always 0
for these horizontal scripts, **asserted offline**), and the hole token string is
dropped (`glyph_runs.cpp` only needs the `is_hole` flag). The byte layout is
authoritative in `runs_bin.py`'s docstring and mirrored by the device's `BinReader`
(`glyph_runs.cpp`), the same way `shape_spike.cpp` mirrored the spike's `SSR1`.

`runs.json` is still written next to it as the **human-readable debug mirror** and
the **`validate_runs.py` oracle input** (which reads it directly), so the offline
oracle gate is unaffected. The `manifest.json` `runs` entry binds `runs.bin`'s
sha256 (the shipped file); `gid`/advances/offsets are range-asserted to their `u16`/
`i16` fields at serialize time so an overflow fails the build loudly. Both files are
deterministic (sorted msgid order, no timestamps) — two builds are byte-identical.

Not yet done (deferred to on-target work, by design): dropping the second on-device
stb copy (`stb_glyph_metrics`) by carrying per-gid glyph boxes — a flash win that
needs pixel-exact box-match validation; the `SSRB` format can add it additively.

## Reproducibility

`build_units` iterates `sorted(msgid)`; `runs.json` is serialized with compact
separators and no timestamps; the subset is deterministic for a pinned
`fonttools`. Verified: two builds of hi produce byte-identical `runs.json` AND
`hi.ttf`. `manifest.json` binds `font_sha256` + `runs.sha256` + the bundled
`harfbuzz_version`, so a font/run mismatch or a shaper bump is caught/visible.
