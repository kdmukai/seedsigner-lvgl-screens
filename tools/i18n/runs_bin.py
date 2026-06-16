#!/usr/bin/env python3
"""
runs_bin.py — serialize a complex-script pack's pre-shaped run table into the
compact on-device binary blob `runs.bin` (the production replacement for the
verbose `runs.json`; see docs/knowledge/complex-script-run-pipeline.md).

`runs.json` is human-readable but ~87 bytes/glyph — almost all repeated field
names + ASCII numbers, plus a `cluster` field the device never reads. On the
ESP32-S3 the acute cost is the transient nlohmann JSON DOM built while parsing
half a megabyte of text on a 512 KB-SRAM part. `runs.bin` is ~8 bytes/glyph
(~10x smaller) and the device walks it straight into its run table with no DOM.

The byte layout below is the AUTHORITATIVE format spec; the device parser
(components/seedsigner/glyph_runs.cpp::seedsigner_set_glyph_runs) mirrors it, the
same way shape_spike.cpp mirrored spike_shape.py's "SSR1" spike format. This is
that idiom evolved to the production run-table shape (multi-line entries with
`breaks`, and `segmented` {}-template parts).

------------------------------------------------------------------------------
FORMAT — magic "SSRB", version 1, little-endian; UTF-8 strings, u16-len-prefixed
------------------------------------------------------------------------------
  Header
    char[4] magic   = "SSRB"
    u16     version = 1
    u16     upem                      font design units / em
    u8      direction                 0 = ltr, 1 = rtl
    u8      reserved (0)
    u32     run_count                 plain + segmented entries (ascii/unsupported
                                       carry no run and are not emitted)
  Glyph record (8 bytes)              cluster + y_advance dropped (see below)
    u16     gid
    i16     x_advance                 font design units
    i16     x_offset
    i16     y_offset
  Run record
    u8      kind                      0 = plain, 1 = segmented

    kind == plain
      u16   text_len ; bytes          translated msgstr (device keys via ap_form)
      u16   line_count                one per '\\n'-split line
      per line:
        u16 glyph_count ; glyph[glyph_count]
        u16 break_count ; u16 break_index[break_count]   interior wrap marks

    kind == segmented
      u16   part_count
      per part:
        u8  is_hole
        is_hole == 0 (literal):
          u16 lit_len ; bytes
          u16 glyph_count ; glyph[glyph_count]
        is_hole == 1: (nothing — the device only needs "a value goes here")

Dropped vs runs.json, and why it's safe:
  - cluster   — informational; the device never reads it (only the offline word-
                break mapping used it, and that is already baked into `breaks`).
  - y_advance — always 0 for these horizontal scripts; asserted 0 here so the
                device can omit it. (Reintroduce via a version bump if a vertical
                script ever needs it.)
  - hole token string ("{}" / "{network}") — glyph_runs.cpp already ignores it;
                only the is_hole flag drives segmented matching.

Glyph ids fit u16 (subset glyph counts are < 2 k, already asserted < glyph_count
upstream). Advances/offsets fit i16 (design units at upem 1000). Both are asserted
here so an out-of-range value fails the build loudly rather than truncating.
"""

import struct

MAGIC = b"SSRB"
VERSION = 1

# Signed/unsigned 16-bit bounds for the loud range asserts below.
_I16_MIN, _I16_MAX = -32768, 32767
_U16_MAX = 65535


def _check_u16(value, what, locale):
    if not (0 <= value <= _U16_MAX):
        raise SystemExit(f"[{locale}] FATAL runs.bin: {what} = {value} exceeds u16")


def _check_i16(value, what, locale):
    if not (_I16_MIN <= value <= _I16_MAX):
        raise SystemExit(f"[{locale}] FATAL runs.bin: {what} = {value} exceeds i16")


def _pack_str(s):
    """u16 length + UTF-8 bytes (no NUL). The device reads exactly `len` bytes."""
    b = s.encode("utf-8")
    if len(b) > _U16_MAX:
        raise SystemExit(f"FATAL runs.bin: string of {len(b)} bytes exceeds u16 length")
    return struct.pack("<H", len(b)) + b


def _pack_glyphs(glyphs, locale):
    """A u16 count followed by one 8-byte record per glyph. Asserts y_advance == 0
    (dropped) and that gid/advance/offsets fit their fields."""
    _check_u16(len(glyphs), "glyph_count", locale)
    out = bytearray(struct.pack("<H", len(glyphs)))
    for g in glyphs:
        if g.get("y_advance", 0) != 0:
            raise SystemExit(f"[{locale}] FATAL runs.bin: non-zero y_advance "
                             f"{g['y_advance']} (format drops it); gid {g.get('gid')}")
        _check_u16(g["gid"], "gid", locale)
        _check_i16(g["x_advance"], "x_advance", locale)
        _check_i16(g["x_offset"], "x_offset", locale)
        _check_i16(g["y_offset"], "y_offset", locale)
        out += struct.pack("<Hhhh", g["gid"], g["x_advance"],
                           g["x_offset"], g["y_offset"])
    return bytes(out)


def serialize(units, upem, direction, locale="?"):
    """Serialize the `with_runs` units (kind in {plain, segmented}) to SSRB bytes.
    `units` is shape_inventory.build_units output filtered to runs; `upem` and
    `direction` ("ltr"/"rtl") come from the shaper. `locale` is used only in
    assert messages."""
    runs = [u for u in units if u["kind"] in ("plain", "segmented")]
    _check_u16(upem, "upem", locale)

    out = bytearray()
    out += MAGIC
    out += struct.pack("<HHBBI", VERSION, upem,
                       1 if direction == "rtl" else 0, 0, len(runs))

    for u in runs:
        if u["kind"] == "plain":
            out += struct.pack("<B", 0)
            out += _pack_str(u["text"])
            _check_u16(len(u["lines"]), "line_count", locale)
            out += struct.pack("<H", len(u["lines"]))
            for line in u["lines"]:
                out += _pack_glyphs(line["glyphs"], locale)
                breaks = line.get("breaks", [])
                _check_u16(len(breaks), "break_count", locale)
                out += struct.pack("<H", len(breaks))
                for b in breaks:
                    _check_u16(b, "break_index", locale)
                    out += struct.pack("<H", b)
        else:  # segmented
            out += struct.pack("<B", 1)
            _check_u16(len(u["parts"]), "part_count", locale)
            out += struct.pack("<H", len(u["parts"]))
            for p in u["parts"]:
                if "hole" in p:
                    out += struct.pack("<B", 1)
                else:
                    out += struct.pack("<B", 0)
                    out += _pack_str(p["lit"])
                    out += _pack_glyphs(p["glyphs"], locale)

    return bytes(out)


def write_runs_bin(path, units, upem, direction, locale="?"):
    """Serialize and write runs.bin. Returns the byte length written."""
    blob = serialize(units, upem, direction, locale)
    with open(path, "wb") as f:
        f.write(blob)
    return len(blob)
