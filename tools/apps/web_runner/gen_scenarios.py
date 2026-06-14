#!/usr/bin/env python3
"""Generate an Emscripten --pre-js file embedding the scenario catalog.

Reads tools/scenarios/scenarios.json and expands each screen's base `context` plus its
`variations` (RFC 7396 merge-patch, the same semantics nlohmann::merge_patch and
runner_core use) into concrete per-variation contexts, grouped by screen and in
file order. The output sets `window.SS_SCENARIOS` so shell.html can populate the
screen + scenario selectors with no runtime fetch — which keeps the final
single-file bundle runnable straight from file://.

Output shape:
  window.SS_SCENARIOS = {
    "button_list_screen": [
      { "name": "(default)",   "context": { ... } },
      { "name": "scroll_many", "context": { ... merged ... } },
    ],
    ...
  };

Usage: gen_scenarios.py <scenarios.json> <out.js>
"""

import json
import sys


def merge_patch(target, patch):
    """RFC 7396 JSON Merge Patch (null removes a key)."""
    if isinstance(patch, dict):
        if not isinstance(target, dict):
            target = {}
        else:
            target = dict(target)
        for key, value in patch.items():
            if value is None:
                target.pop(key, None)
            else:
                target[key] = merge_patch(target.get(key), value)
        return target
    return patch


def slugify(name):
    out = []
    for ch in name:
        if ch.isalnum() or ch == "_":
            out.append(ch)
        elif ch in (" ", "-"):
            out.append("_")
    return "".join(out) or "variation"


def build_catalog(root):
    catalog = {}
    for screen_name, screen_def in root.items():
        if not isinstance(screen_def, dict):
            raise ValueError(f"screen '{screen_name}' is not an object")

        base_ctx = screen_def.get("context", {})
        if not isinstance(base_ctx, dict):
            raise ValueError(f"screen '{screen_name}' context is not an object")

        entries = [{"name": "(default)", "context": base_ctx}]

        for var in screen_def.get("variations", []):
            if not isinstance(var, dict):
                raise ValueError(f"variation in '{screen_name}' is not an object")
            var_name = var.get("name") or "variation"
            merged = merge_patch(base_ctx, var.get("context", {}))
            entries.append({"name": slugify(var_name), "context": merged})

        catalog[screen_name] = entries
    return catalog


def main(argv):
    if len(argv) != 3:
        sys.stderr.write("usage: gen_scenarios.py <scenarios.json> <out.js>\n")
        return 2

    with open(argv[1], "r", encoding="utf-8") as f:
        root = json.load(f)

    catalog = build_catalog(root)

    body = json.dumps(catalog, indent=2)
    with open(argv[2], "w", encoding="utf-8") as f:
        f.write("// AUTO-GENERATED from tools/scenarios/scenarios.json by gen_scenarios.py — do not edit.\n")
        f.write("var SS_SCENARIOS = ")
        f.write(body)
        f.write(";\n")
        f.write("if (typeof window !== 'undefined') { window.SS_SCENARIOS = SS_SCENARIOS; }\n")

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
