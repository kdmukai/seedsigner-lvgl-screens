#!/usr/bin/env python3
"""Compare before/after screenshot sets and generate an HTML diff report.

Designed to run in CI after screenshot_gen produces a new set of screenshots,
comparing them against the baseline from gh-pages.
"""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(8192), b""):
            h.update(chunk)
    return h.hexdigest()


def png_path_for(manifest_dir: Path, entry: dict) -> Path:
    """Return the .png path for a manifest entry.

    Animated entries have path ending in .gif, but the static .png is always
    generated alongside it.
    """
    rel = entry["path"]
    if rel.endswith(".gif"):
        rel = rel[:-4] + ".png"
    return manifest_dir / rel


def load_manifest(directory: Path) -> dict:
    """Load manifest.json from directory, returning {} if missing."""
    manifest_path = directory / "manifest.json"
    if not manifest_path.exists():
        return {}
    with open(manifest_path) as f:
        return json.load(f)


def find_imagemagick() -> str | None:
    """Return the ImageMagick compare binary name, or None."""
    for candidate in ("magick", "convert"):
        if shutil.which(candidate):
            return candidate
    return None


def run_compare(before_png: Path, after_png: Path, diff_png: Path, im_bin: str) -> int:
    """Run ImageMagick compare, returning the pixel difference count.

    magick compare exits 0 if identical, 1 if different (normal), 2 on error.
    """
    if im_bin == "magick":
        cmd = [im_bin, "compare", "-metric", "AE", str(before_png), str(after_png), str(diff_png)]
    else:
        cmd = ["compare", "-metric", "AE", str(before_png), str(after_png), str(diff_png)]

    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode == 2:
        print(f"  WARNING: ImageMagick compare error: {result.stderr.strip()}", file=sys.stderr)
        return -1

    # AE metric is printed to stderr
    try:
        return int(float(result.stderr.strip()))
    except ValueError:
        return -1


def generate_html(
    pr_number: int,
    timestamp: str,
    changed: list[dict],
    new: list[dict],
    removed: list[dict],
    unchanged_count: int,
    width: int,
    height: int,
) -> str:
    total = len(changed) + len(new) + len(removed) + unchanged_count
    summary_parts = []
    if changed:
        summary_parts.append(f"{len(changed)} changed")
    if new:
        summary_parts.append(f"{len(new)} new")
    if removed:
        summary_parts.append(f"{len(removed)} removed")
    if unchanged_count:
        summary_parts.append(f"{unchanged_count} unchanged")
    summary = ", ".join(summary_parts) if summary_parts else "no screenshots"

    def html_escape(s: str) -> str:
        return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace('"', "&quot;")

    sections = []

    if changed:
        items = []
        for c in changed:
            display = html_escape(c["display_name"])
            # Show GIF in report for visual review if available, PNG for diff
            after_display = c.get("after_display_path", c["after_path"])
            items.append(f"""<div class="diff-item">
<h3>{display}</h3>
<div class="triple">
<figure><figcaption>Before</figcaption><img loading="lazy" src="{c['before_path']}" alt="before" width="{width}" height="{height}"></figure>
<figure><figcaption>Diff ({c['pixel_diff']} px)</figcaption><img loading="lazy" src="{c['diff_path']}" alt="diff" width="{width}" height="{height}"></figure>
<figure><figcaption>After</figcaption><img loading="lazy" src="{after_display}" alt="after" width="{width}" height="{height}"></figure>
</div></div>""")
        sections.append(f'<section><h2>Changed ({len(changed)})</h2>{"".join(items)}</section>')

    if new:
        items = []
        for n in new:
            display = html_escape(n["display_name"])
            img_path = n.get("display_path", n["path"])
            items.append(f"""<figure class="badge-new">
<figcaption>{display} <span class="badge green">NEW</span></figcaption>
<img loading="lazy" src="{img_path}" alt="{display}" width="{width}" height="{height}">
</figure>""")
        sections.append(f'<section><h2>New ({len(new)})</h2><div class="grid">{"".join(items)}</div></section>')

    if removed:
        items = []
        for r in removed:
            display = html_escape(r["display_name"])
            items.append(f"""<figure class="badge-removed">
<figcaption>{display} <span class="badge red">REMOVED</span></figcaption>
<img loading="lazy" src="{r['path']}" alt="{display}" width="{width}" height="{height}">
</figure>""")
        sections.append(f'<section><h2>Removed ({len(removed)})</h2><div class="grid">{"".join(items)}</div></section>')

    if unchanged_count and not changed and not new and not removed:
        sections.append(f"<section><p>All {unchanged_count} screenshots are identical to baseline.</p></section>")
    elif unchanged_count:
        sections.append(f"<section><p>{unchanged_count} screenshot(s) unchanged (not shown).</p></section>")

    return f"""<!doctype html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Screenshot Diff — PR #{pr_number}</title>
<style>
body{{font-family:system-ui,sans-serif;margin:16px;background:#111;color:#eee}}
h1{{margin-bottom:4px}}
.meta{{color:#bbb;margin-bottom:16px}}
h2{{border-bottom:1px solid #333;padding-bottom:6px}}
.grid{{display:grid;grid-template-columns:repeat(auto-fit,minmax(520px,1fr));gap:14px;align-items:start}}
.triple{{display:grid;grid-template-columns:1fr 1fr 1fr;gap:10px}}
.diff-item{{background:#1b1b1b;border:1px solid #333;border-radius:8px;padding:14px;margin-bottom:14px}}
.diff-item h3{{margin:0 0 10px 0;font-size:18px}}
figure{{margin:0;background:#1b1b1b;border:1px solid #333;border-radius:8px;padding:10px;overflow:auto}}
.diff-item figure{{border:none;padding:0;background:none}}
img{{display:block;width:auto;max-width:100%;height:auto;background:#000;border-radius:6px;margin:0 auto;image-rendering:auto}}
figcaption{{margin:0 0 8px 0;font-size:16px;font-weight:700;color:#f2f2f2;text-align:center}}
.badge{{display:inline-block;padding:2px 8px;border-radius:4px;font-size:12px;font-weight:700;vertical-align:middle}}
.green{{background:#1a7f37;color:#fff}}
.red{{background:#cf222e;color:#fff}}
</style>
</head><body>
<h1>Screenshot Diff &mdash; PR #{pr_number}</h1>
<p class="meta">Generated: {html_escape(timestamp)} &bull; {total} total &bull; {summary}</p>
{"".join(sections)}
</body></html>
"""


def main():
    parser = argparse.ArgumentParser(description="Compare before/after screenshots")
    parser.add_argument("--before-dir", required=True, help="Baseline screenshots directory")
    parser.add_argument("--after-dir", required=True, help="New screenshots directory")
    parser.add_argument("--out-dir", required=True, help="Output report directory")
    parser.add_argument("--pr-number", type=int, required=True, help="PR number")
    args = parser.parse_args()

    before_dir = Path(args.before_dir)
    after_dir = Path(args.after_dir)
    out_dir = Path(args.out_dir)
    pr_number = args.pr_number

    out_dir.mkdir(parents=True, exist_ok=True)

    before_manifest = load_manifest(before_dir)
    after_manifest = load_manifest(after_dir)

    before_by_name = {}
    if before_manifest:
        for entry in before_manifest.get("screenshots", []):
            before_by_name[entry["name"]] = entry

    after_by_name = {}
    if after_manifest:
        for entry in after_manifest.get("screenshots", []):
            after_by_name[entry["name"]] = entry

    all_names = set(before_by_name.keys()) | set(after_by_name.keys())

    width = after_manifest.get("width", before_manifest.get("width", 480))
    height = after_manifest.get("height", before_manifest.get("height", 320))

    im_bin = find_imagemagick()
    if not im_bin:
        print("WARNING: ImageMagick not found — diff images will not be generated", file=sys.stderr)

    # Prepare output subdirs
    before_out = out_dir / "before" / "img"
    after_out = out_dir / "after" / "img"
    diff_out = out_dir / "diff"
    before_out.mkdir(parents=True, exist_ok=True)
    after_out.mkdir(parents=True, exist_ok=True)
    diff_out.mkdir(parents=True, exist_ok=True)

    changed = []
    new_items = []
    removed_items = []
    unchanged_count = 0

    for name in sorted(all_names):
        in_before = name in before_by_name
        in_after = name in after_by_name

        if in_after and not in_before:
            # New screenshot
            after_entry = after_by_name[name]
            after_src = after_dir / after_entry["path"]
            after_dst = after_out / os.path.basename(after_entry["path"])
            if after_src.exists():
                shutil.copy2(after_src, after_dst)
            # Also copy the png if the display path is a gif
            png_src = png_path_for(after_dir, after_entry)
            if after_entry["path"].endswith(".gif") and png_src.exists():
                png_dst = after_out / os.path.basename(png_src)
                shutil.copy2(png_src, png_dst)

            display_path = f"after/img/{os.path.basename(after_entry['path'])}"
            new_items.append({
                "name": name,
                "display_name": after_entry.get("display_name", name),
                "path": f"after/img/{os.path.basename(png_path_for(after_dir, after_entry))}",
                "display_path": display_path,
            })
            print(f"  NEW: {name}")

        elif in_before and not in_after:
            # Removed screenshot
            before_entry = before_by_name[name]
            before_src = before_dir / before_entry["path"]
            # For removed, try PNG first, then the original path
            png_src = png_path_for(before_dir, before_entry)
            src = png_src if png_src.exists() else before_src
            dst = before_out / (name + ".png")
            if src.exists():
                shutil.copy2(src, dst)
            removed_items.append({
                "name": name,
                "display_name": before_entry.get("display_name", name),
                "path": f"before/img/{name}.png",
            })
            print(f"  REMOVED: {name}")

        else:
            # Both exist — compare PNG hashes
            before_entry = before_by_name[name]
            after_entry = after_by_name[name]

            before_png = png_path_for(before_dir, before_entry)
            after_png = png_path_for(after_dir, after_entry)

            if not before_png.exists() or not after_png.exists():
                print(f"  SKIP (missing PNG): {name}", file=sys.stderr)
                continue

            before_hash = sha256_file(before_png)
            after_hash = sha256_file(after_png)

            if before_hash == after_hash:
                unchanged_count += 1
                continue

            # Changed — copy images and generate diff
            b_dst = before_out / (name + ".png")
            a_dst = after_out / (name + ".png")
            shutil.copy2(before_png, b_dst)
            shutil.copy2(after_png, a_dst)

            # Copy GIF for display if available
            after_gif = after_dir / after_entry["path"]
            after_display_path = f"after/img/{name}.png"
            if after_entry["path"].endswith(".gif") and after_gif.exists():
                gif_dst = after_out / (name + ".gif")
                shutil.copy2(after_gif, gif_dst)
                after_display_path = f"after/img/{name}.gif"

            pixel_diff = 0
            diff_path = f"diff/{name}.png"
            d_dst = diff_out / (name + ".png")

            if im_bin:
                pixel_diff = run_compare(before_png, after_png, d_dst, im_bin)
                if pixel_diff < 0:
                    pixel_diff = -1  # error marker

            changed.append({
                "name": name,
                "display_name": after_entry.get("display_name", name),
                "before_path": f"before/img/{name}.png",
                "after_path": f"after/img/{name}.png",
                "after_display_path": after_display_path,
                "diff_path": diff_path,
                "pixel_diff": pixel_diff,
            })
            print(f"  CHANGED: {name} ({pixel_diff} px diff)")

    # Write compare_result.json
    result = {
        "pr_number": pr_number,
        "timestamp": datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%SZ"),
        "summary": {
            "total": len(changed) + len(new_items) + len(removed_items) + unchanged_count,
            "changed": len(changed),
            "new": len(new_items),
            "removed": len(removed_items),
            "unchanged": unchanged_count,
        },
        "changed": [{"name": c["name"], "pixel_diff": c["pixel_diff"]} for c in changed],
        "new": [{"name": n["name"]} for n in new_items],
        "removed": [{"name": r["name"]} for r in removed_items],
    }

    result_path = out_dir / "compare_result.json"
    with open(result_path, "w") as f:
        json.dump(result, f, indent=2)
        f.write("\n")

    # Generate HTML report
    html = generate_html(
        pr_number=pr_number,
        timestamp=result["timestamp"],
        changed=changed,
        new=new_items,
        removed=removed_items,
        unchanged_count=unchanged_count,
        width=width,
        height=height,
    )

    html_path = out_dir / "index.html"
    with open(html_path, "w") as f:
        f.write(html)

    print(f"\nReport: {html_path}")
    print(f"Result: {result_path}")
    print(f"Summary: {json.dumps(result['summary'])}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
