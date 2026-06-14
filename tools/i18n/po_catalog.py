"""
Self-contained gettext .po reader for the SeedSigner i18n build tooling.

Just enough of the .po grammar for our needs (no Babel dependency): singular
msgid/msgstr pairs with multi-line quoted continuations, plus msgstr[0] for
plural entries (mapped onto the singular msgid). Shared by build_fontpacks.py
(glyph corpus) and gen_localized_scenarios.py (msgid->msgstr lookup).
"""


def _unescape(s):
    return (s.replace('\\n', '\n').replace('\\t', '\t').replace('\\r', '\r')
             .replace('\\"', '"').replace('\\\\', '\\'))


def _qval(line):
    """Extract the quoted payload from a `directive "..."` line."""
    a, b = line.find('"'), line.rfind('"')
    return _unescape(line[a + 1:b]) if a != -1 and b > a else ""


def parse_entries(po_path):
    """Yield (msgid, msgstr) tuples. For plural entries, msgstr is msgstr[0].
    The header entry (empty msgid) is included; callers filter as needed."""
    entries = []
    msgid, msgstr = [], []
    target = None  # "id" | "str" | None (inside an ignored field e.g. msgstr[1])
    have = False

    def flush():
        nonlocal have
        if have:
            entries.append(("".join(msgid), "".join(msgstr)))

    with open(po_path, encoding="utf-8") as f:
        for raw in f:
            s = raw.strip()
            if s == "" or s.startswith("#"):
                continue
            if s.startswith("msgid_plural"):
                target = None  # keep singular msgid as the key; ignore plural id text
            elif s.startswith("msgid"):
                flush()
                msgid, msgstr = [_qval(s)], []
                target = "id"
                have = True
            elif s.startswith("msgstr[0]"):
                target = "str"
                msgstr.append(_qval(s))
            elif s.startswith("msgstr["):
                target = None  # ignore other plural forms
            elif s.startswith("msgstr"):
                target = "str"
                msgstr.append(_qval(s))
            elif s.startswith('"'):
                if target == "id":
                    msgid.append(_qval(s))
                elif target == "str":
                    msgstr.append(_qval(s))
        flush()
    return entries


def parse_catalog(po_path):
    """Return {msgid: msgstr} for entries with a non-empty msgid AND msgstr.
    (Empty msgstr = untranslated; excluded so lookups fall back to English.)"""
    return {mid: mstr for mid, mstr in parse_entries(po_path) if mid and mstr}


def corpus_chars(po_path, ascii_only=False):
    """Unique glyph set used across a locale's translations (msgstr values).

    By default ASCII (<= 0x7F) is excluded: the baked-in OpenSans floor covers
    it, and for CJK-primary chains the OpenSans fallback renders embedded English
    at the English size, so the script subset only needs non-Latin glyphs."""
    cps = set()
    for _mid, mstr in parse_entries(po_path):
        for ch in mstr:
            if ascii_only or ord(ch) > 0x7F:
                cps.add(ch)
    return "".join(sorted(cps))
