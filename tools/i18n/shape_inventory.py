#!/usr/bin/env python3
"""
shape_inventory.py — turn a locale's .po catalog into pre-shaped "shaping units".

Each translated string becomes one unit, classified by how its runtime value
holes (if any) interact with shaping:

  ascii        Pure-ASCII translation (untranslated string, numeric/symbol-only).
               No shaping, no run: the device renders it on the normal codepoint
               path (baked OpenSans / fallback), like embedded English in a CJK
               pack. Skipped from the run table entirely.

  plain        Has non-ASCII text and no `{}` placeholder. Shape the whole msgstr
               (split on '\\n' into line runs). The bulk of the catalog.

  segmented    Has placeholder(s), each sitting at a SHAPE-SAFE boundary in the
               msgstr (flanked by whitespace, a string edge, or a non-letter such
               as digit/punctuation — anything that can't join/conjoin across the
               hole). Split the template into literal segments (each shaped) and
               hole markers. At runtime the device draws the shaped literals and
               renders the inserted value (a number / ASCII / a closed-set word)
               between them. The spike proved this concatenation is visually exact
               at word boundaries.

  unsupported  Has a placeholder that is NOT shape-safe (a hole flanked by a script
               letter, so shaping the two sides independently could differ from
               shaping the joined result), or a multi-line template, or a
               placeholder mismatch between msgid and msgstr. Emitted with a
               reason and NO run data — never silently dropped. These are the tail
               the render layer must handle another way (or that needs a
               translation/UX fix); reported loudly so the set stays visible.

This module is shaping-mechanism-agnostic: the caller passes a `shape_line(text)`
callable (binding the subset font + script/lang/direction) so the same classifier
serves every locale. See hb_shaper.py and docs/complex-script-shaping-spike-plan.md.
"""

import re

# A gettext placeholder: positional `{}` or named `{name}` (str.format style).
HOLE_RE = re.compile(r"\{[^}]*\}")


def _holes(s):
    """Multiset (sorted list) of placeholder tokens in `s`, e.g. ['{}','{}'] or
    ['{network}']. Used to confirm a translation preserves the msgid's holes."""
    return sorted(HOLE_RE.findall(s))


def _shape_safe(neighbor):
    """A hole boundary is shape-safe when the adjacent character can't form a
    shaping cluster (join / conjunct / ligature) with whatever is inserted:
      - a string edge (None),
      - any non-letter (whitespace, digit, punctuation, symbol), or
      - an ASCII/Latin letter — which shapes as a standalone glyph in a
        Devanagari/Thai/Arabic run and never conjoins with an inserted numeric or
        ASCII value (e.g. the literal 'x' in "Begin {}x{}").
    Only a NON-ASCII letter (a complex-script letter that joins/conjoins) is a
    real risk, so independent shaping of the two sides could diverge there."""
    return neighbor is None or not neighbor.isalpha() or neighbor.isascii()


def split_segments(template):
    """Split a placeholder template into ordered parts:
        ('lit', text)   a literal run between/around holes ('' is dropped)
        ('hole', token) a runtime value hole, token e.g. '{}' or '{network}'
    """
    parts = []
    last = 0
    for m in HOLE_RE.finditer(template):
        if m.start() > last:
            parts.append(("lit", template[last:m.start()]))
        parts.append(("hole", m.group(0)))
        last = m.end()
    if last < len(template):
        parts.append(("lit", template[last:]))
    return parts


def classify(msgid, msgstr):
    """Return (kind, reason). reason is None unless kind == 'unsupported'."""
    # Pure-ASCII translations (untranslated strings, numeric/symbol-only, etc.)
    # need NO shaping and NO run: the device renders them on the normal codepoint
    # path (the baked OpenSans floor / fallback), exactly as embedded English is
    # handled for the CJK packs. Baking a run here would (wastefully and slightly
    # wrongly) draw Latin via the script subset at the bumped script size.
    if all(ord(c) <= 0x7F for c in msgstr):
        return "ascii", None

    if "{" not in msgid:
        return "plain", None

    # The translation must carry the same holes, or interpolation breaks.
    if _holes(msgid) != _holes(msgstr):
        return "unsupported", f"placeholder mismatch: msgid {_holes(msgid)} vs msgstr {_holes(msgstr)}"

    # Segmenting a hole that spans a line break is out of scope (the value would
    # straddle two stacked lines); rare, handled by the render layer if it ever
    # occurs.
    if "\n" in msgstr:
        return "unsupported", "multi-line template with placeholder"

    for m in HOLE_RE.finditer(msgstr):
        left = msgstr[m.start() - 1] if m.start() > 0 else None
        right = msgstr[m.end()] if m.end() < len(msgstr) else None
        if not (_shape_safe(left) and _shape_safe(right)):
            bad = left if not _shape_safe(left) else right
            return "unsupported", f"hole {m.group(0)} abuts script letter {bad!r} (not shape-safe)"

    return "segmented", None


def build_unit(msgid, msgstr, shape_line, line_breaks=None):
    """Build one shaping unit for (msgid, msgstr). `shape_line(text)` -> list of
    glyph dicts for a single line (no '\\n'); the caller binds font + script
    params. `line_breaks(text, glyphs)` -> glyph indices where the device may wrap
    that line (dictionary word boundaries); omit for no wrap data. Returns a dict
    (see module docstring for the per-kind shape)."""
    kind, reason = classify(msgid, msgstr)

    if kind == "ascii":
        return {"msgid": msgid, "kind": "ascii", "text": msgstr}

    if kind == "plain":
        # Each line carries its glyphs plus the indices where it may be wrapped.
        lines = []
        for line in msgstr.split("\n"):
            glyphs = shape_line(line)
            lines.append({"glyphs": glyphs,
                          "breaks": line_breaks(line, glyphs) if line_breaks else []})
        return {"msgid": msgid, "kind": "plain", "text": msgstr, "lines": lines}

    if kind == "segmented":
        parts = []
        for tag, val in split_segments(msgstr):
            if tag == "lit":
                parts.append({"lit": val, "glyphs": shape_line(val)})
            else:
                parts.append({"hole": val})
        return {"msgid": msgid, "kind": "segmented", "text": msgstr, "parts": parts}

    return {"msgid": msgid, "kind": "unsupported", "text": msgstr, "reason": reason}


def build_units(catalog, shape_line, line_breaks=None):
    """Build units for a whole catalog ({msgid: msgstr}). Returns (units, report)
    where report = {"plain": n, "segmented": n, "unsupported": [(msgid, reason)]}.
    Units with runs (plain/segmented) come first; callers should fail the build if
    `unsupported` is non-empty unless they explicitly accept the listed tail.
    `line_breaks` (optional) is threaded to build_unit for plain-line wrap marks."""
    units = []
    report = {"plain": 0, "segmented": 0, "ascii": 0, "unsupported": []}
    for msgid in sorted(catalog):
        msgstr = catalog[msgid]
        if not msgstr:
            continue
        unit = build_unit(msgid, msgstr, shape_line, line_breaks)
        units.append(unit)
        if unit["kind"] == "unsupported":
            report["unsupported"].append((msgid, unit["reason"]))
        else:
            report[unit["kind"]] += 1
    return units, report
