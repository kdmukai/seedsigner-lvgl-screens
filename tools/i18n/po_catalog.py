"""
Self-contained gettext .po reader for the SeedSigner i18n build tooling.

Just enough of the .po grammar for our needs (no Babel dependency): msgid/msgstr
pairs with multi-line quoted continuations, plus EVERY plural form (msgstr[0],
msgstr[1], ...) of a plural entry. Shared by build_fontpacks.py (glyph corpus +
shaping pipeline) and gen_localized_scenarios.py (msgid->msgstr lookup).

One grammar walk (`parse_records`) captures all forms; the other readers compose
over it:
  - parse_entries / parse_catalog  -> the singular view (msgid -> msgstr[0]), for
    msgid->display lookups.
  - iter_translations              -> EVERY translated form (one pair per plural
    form), for the corpus + the complex-script run table, so a form the runtime
    can pick via ngettext (e.g. Hindi इनपुट AND इनपुट्स) is never dropped.
"""


def _unescape(s):
    return (s.replace('\\n', '\n').replace('\\t', '\t').replace('\\r', '\r')
             .replace('\\"', '"').replace('\\\\', '\\'))


def _qval(line):
    """Extract the quoted payload from a `directive "..."` line."""
    a, b = line.find('"'), line.rfind('"')
    return _unescape(line[a + 1:b]) if a != -1 and b > a else ""


def parse_records(po_path):
    """Yield (msgid, forms) for each entry, where `forms` is the list of
    translated strings in plural order: a one-element list [msgstr] for a singular
    entry, or [msgstr[0], msgstr[1], ...] for a plural entry. The header entry
    (empty msgid) is included; callers filter as needed.

    This is the single .po grammar walk; parse_entries / iter_translations compose
    over it. msgid_plural's English text is never captured (the singular msgid
    stays the catalog key); msgctxt is not handled (no contextual duplicates in
    our catalogs)."""
    records = []
    msgid = []
    forms = []      # forms[i] = the accumulated quoted chunks of msgstr[i]
    target = None   # "id" | int plural-form index | None (inside an ignored field)
    have = False

    def flush():
        nonlocal have
        if have:
            records.append(("".join(msgid), ["".join(chunks) for chunks in forms]))

    with open(po_path, encoding="utf-8") as f:
        for raw in f:
            s = raw.strip()
            if s == "" or s.startswith("#"):
                continue
            if s.startswith("msgid_plural"):
                target = None  # keep the singular msgid as the key; ignore plural id text
            elif s.startswith("msgid"):
                flush()
                msgid, forms = [_qval(s)], []
                target = "id"
                have = True
            elif s.startswith("msgstr["):
                # msgstr[N] — the Nth plural form. Index by the declared N so the
                # form order follows the file regardless of declaration order.
                n = int(s[s.index("[") + 1:s.index("]")])
                while len(forms) <= n:
                    forms.append([])
                forms[n].append(_qval(s))
                target = n
            elif s.startswith("msgstr"):
                # Singular msgstr == form 0.
                if not forms:
                    forms.append([])
                forms[0].append(_qval(s))
                target = 0
            elif s.startswith('"'):
                # A quoted continuation line appends to whatever field is open.
                if target == "id":
                    msgid.append(_qval(s))
                elif isinstance(target, int):
                    forms[target].append(_qval(s))
        flush()
    return records


def parse_entries(po_path):
    """Yield (msgid, msgstr) tuples — the SINGULAR view. For plural entries msgstr
    is msgstr[0] (mapped onto the singular msgid). The header entry (empty msgid)
    is included; callers filter as needed. Use iter_translations for ALL forms."""
    return [(mid, forms[0] if forms else "") for mid, forms in parse_records(po_path)]


def iter_translations(po_path):
    """Yield (msgid, msgstr) for EVERY non-empty translated form across the
    catalog — one pair per plural form, not just msgstr[0]. The corpus builders
    and the complex-script shaping pipeline use this so that every form a locale
    can display via ngettext (e.g. Hindi इनपुट AND इनपुट्स) gets its glyphs subset
    in and a run shaped. The header (empty msgid) and empty forms (untranslated ->
    English fallback) are skipped."""
    for msgid, forms in parse_records(po_path):
        if not msgid:
            continue
        for msgstr in forms:
            if msgstr:
                yield (msgid, msgstr)


def parse_catalog(po_path):
    """Return {msgid: msgstr[0]} for entries with a non-empty msgid AND msgstr.
    (Empty msgstr = untranslated; excluded so lookups fall back to English.)
    Singular view — plural forms beyond msgstr[0] are not in this map; see
    iter_translations."""
    return {mid: mstr for mid, mstr in parse_entries(po_path) if mid and mstr}


def corpus_chars(po_path, ascii_only=False):
    """Unique glyph set used across a locale's translations (every plural form).

    By default ASCII (<= 0x7F) is excluded: the baked-in OpenSans floor covers
    it, and for CJK-primary chains the OpenSans fallback renders embedded English
    at the English size, so the script subset only needs non-Latin glyphs."""
    cps = set()
    for _mid, mstr in iter_translations(po_path):
        for ch in mstr:
            if ascii_only or ord(ch) > 0x7F:
                cps.add(ch)
    return "".join(sorted(cps))
