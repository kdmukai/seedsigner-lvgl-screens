"""Pure-Python gettext `.po` -> `.mo` compiler for the SeedSigner i18n build.

No Babel / no `msgfmt` dependency (matching po_catalog.py's "parse .po directly"
stance): reads the `.po` and writes a standard little-endian GNU `.mo` that BOTH
consumers accept unchanged — CPython stdlib `gettext` on the Pi and the on-device
pure-Python reader (`seedsigner/compat/_mo.py`) on ESP32 / MicroPython.

Why a writer here at all: `build_fontpacks.py` already ships each locale's font +
endonym in `lang-packs/<locale>/`; co-locating the compiled catalog at
`lang-packs/<locale>/LC_MESSAGES/messages.mo` makes the pack the ONE self-contained,
distributable unit (script rendering + translations) that both platforms consume
identically. `LC_MESSAGES/messages.mo` is the gettext-standard path (stdlib gettext
on the Pi requires it), so the pack layout stays platform-neutral.

Correctness scope for gettext parity:
  * plural entries -> key `msgid\\x00msgid_plural`, value `msgstr[0]\\x00msgstr[1]…`
    (exactly what `ngettext` looks up + splits by the header's Plural-Forms rule);
  * `msgctxt` -> key `msgctxt\\x04msgid` (none in our catalogs today, but handled);
  * fuzzy + untranslated entries are dropped (msgfmt's default), EXCEPT the header
    entry (empty msgid), which is always kept — it carries the charset + Plural-Forms
    the readers parse.
Assumes UTF-8 `.po` (all SeedSigner catalogs are), which the `.mo` header declares.
"""

import os
import struct

from po_catalog import _qval  # shared quoting/continuation unescape rules


_MO_MAGIC = 0x950412DE


def parse_po(po_path):
    """Parse `po_path` into an ordered list of (key, value) unicode strings for the
    `.mo`, applying the gettext key/value packing + the fuzzy/untranslated filter.
    Entries are blank-line delimited (all our tool-generated catalogs are)."""
    entries = []

    state = {}

    def reset():
        state.clear()
        state.update(msgctxt=None, msgid=[], msgid_plural=[], msgstrs={},
                     fuzzy=False, target=None, have=False)

    def flush():
        if not state["have"]:
            return
        mid = "".join(state["msgid"])
        mplural = "".join(state["msgid_plural"])
        ctxt = state["msgctxt"]
        forms = ["".join(state["msgstrs"][i]) for i in sorted(state["msgstrs"])]
        is_header = (mid == "" and ctxt is None)
        if not is_header:
            if state["fuzzy"] or not any(forms):
                return  # msgfmt default: skip fuzzy + untranslated
        key = mid + "\x00" + mplural if mplural else mid
        if ctxt is not None:
            key = ctxt + "\x04" + key
        value = "\x00".join(forms) if mplural else (forms[0] if forms else "")
        entries.append((key, value))

    reset()
    with open(po_path, encoding="utf-8") as f:
        lines = f.readlines()
    for raw in lines + [""]:               # trailing "" flushes the final entry
        s = raw.strip()
        if s == "":
            flush()
            reset()
            continue
        if s.startswith("#"):
            if s.startswith("#,") and "fuzzy" in s:
                state["fuzzy"] = True
            continue
        if s.startswith("msgctxt"):
            state["msgctxt"] = _qval(s); state["target"] = "ctxt"; state["have"] = True
        elif s.startswith("msgid_plural"):
            state["msgid_plural"] = [_qval(s)]; state["target"] = "plural"
        elif s.startswith("msgid"):
            state["msgid"] = [_qval(s)]; state["target"] = "id"; state["have"] = True
        elif s.startswith("msgstr["):
            n = int(s[s.index("[") + 1:s.index("]")])
            state["msgstrs"].setdefault(n, []).append(_qval(s)); state["target"] = n
        elif s.startswith("msgstr"):
            state["msgstrs"].setdefault(0, []).append(_qval(s)); state["target"] = 0
        elif s.startswith('"'):
            t = state["target"]
            if t == "ctxt":
                state["msgctxt"] = (state["msgctxt"] or "") + _qval(s)
            elif t == "id":
                state["msgid"].append(_qval(s))
            elif t == "plural":
                state["msgid_plural"].append(_qval(s))
            elif isinstance(t, int):
                state["msgstrs"][t].append(_qval(s))
    return entries


def write_mo_bytes(entries):
    """Serialize (key, value) unicode pairs into standard little-endian GNU `.mo`
    bytes. Originals are sorted (the format's requirement); strings are UTF-8 and
    NUL-terminated in the data section (the terminator is excluded from the length)."""
    items = sorted(((k.encode("utf-8"), v.encode("utf-8")) for k, v in entries),
                   key=lambda kv: kv[0])
    n = len(items)
    key_table = 28                 # after the 7 * uint32 header
    val_table = key_table + n * 8
    data = val_table + n * 8

    blob = bytearray()
    key_descs = []
    for k, _v in items:
        key_descs.append((len(k), data + len(blob)))
        blob += k + b"\x00"
    val_descs = []
    for _k, v in items:
        val_descs.append((len(v), data + len(blob)))
        blob += v + b"\x00"

    out = bytearray()
    out += struct.pack("<IIIIIII", _MO_MAGIC, 0, n, key_table, val_table, 0, 0)
    for length, off in key_descs:
        out += struct.pack("<II", length, off)
    for length, off in val_descs:
        out += struct.pack("<II", length, off)
    out += blob
    return bytes(out)


def compile_po_to_mo(po_path, mo_path):
    """Compile `po_path` -> `mo_path` (creating parent dirs). Returns the bytes written."""
    data = write_mo_bytes(parse_po(po_path))
    os.makedirs(os.path.dirname(mo_path), exist_ok=True)
    with open(mo_path, "wb") as f:
        f.write(data)
    return len(data)


if __name__ == "__main__":
    import sys
    if len(sys.argv) != 3:
        sys.exit("usage: mo_compile.py <messages.po> <messages.mo>")
    n = compile_po_to_mo(sys.argv[1], sys.argv[2])
    print(f"wrote {sys.argv[2]} ({n} bytes)")
