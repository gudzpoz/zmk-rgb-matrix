#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Build a static gallery page from a preview run.

Reads the manifest run-preview.sh writes (slug<TAB>display-name, both derived by
that script) and emits one page showing every GIF next to its display name. Used
by the GitHub Pages workflow, and handy locally to browse a run.

The page is deliberately plain: no scripts, no build step, one grid of figures.
"""

import argparse
import html
import sys
from pathlib import Path

PAGE = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{title}</title>
<style>
  :root {{ color-scheme: dark; }}
  body {{
    margin: 0 auto; padding: 2rem 1rem; max-width: 72rem;
    background: #111; color: #e8e8e8;
    font: 16px/1.5 system-ui, -apple-system, "Segoe UI", sans-serif;
  }}
  h1 {{ font-size: 1.6rem; margin: 0 0 .3rem; }}
  p.lede {{ color: #a8a8a8; margin: 0 0 2rem; }}
  ul {{ display: grid; gap: 1.5rem; margin: 0; padding: 0; list-style: none;
        grid-template-columns: repeat(auto-fill, minmax(320px, 1fr)); }}
  figure {{ margin: 0; background: #1b1b1b; border: 1px solid #2c2c2c;
            border-radius: 10px; padding: 1rem; }}
  figure img {{ width: 100%; height: auto; display: block;
                image-rendering: pixelated; border-radius: 4px; }}
  figcaption {{ margin-top: .6rem; font-weight: 600; }}
  figcaption a {{ color: inherit; text-decoration: none; }}
  figcaption a:hover {{ text-decoration: underline; }}
  code {{ color: #8ec07c; font-size: .85em; }}
</style>
</head>
<body>
<h1>{title}</h1>
<p class="lede">{lede}</p>
<ul>
{items}
</ul>
</body>
</html>
"""

ITEM = """  <li>
    <figure>
      <a href="{slug}.gif"><img src="{slug}.gif" alt="{name}" loading="lazy"></a>
      <figcaption><a href="{slug}.gif">{name}</a><br><code>{slug}.gif</code></figcaption>
    </figure>
  </li>"""


def read_manifest(path):
    entries = []
    for lineno, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) != 2 or not parts[0].strip():
            raise ValueError(f"{path}:{lineno}: expected 'slug<TAB>display-name'")
        entries.append((parts[0].strip(), parts[1].strip()))
    return entries


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--manifest",
        type=Path,
        required=True,
        help="previews.tsv written by run-preview.sh",
    )
    parser.add_argument("-o", "--output", type=Path, required=True, help="HTML to write")
    parser.add_argument("--title", default="RGB matrix effect previews")
    parser.add_argument(
        "--gif-dir",
        type=Path,
        default=None,
        help="directory holding the GIFs, checked so a broken manifest fails here "
        "rather than as a missing image on the published page",
    )
    args = parser.parse_args()

    try:
        entries = read_manifest(args.manifest)
    except (OSError, ValueError) as err:
        sys.exit(f"error: {err}")

    if not entries:
        sys.exit(f"error: {args.manifest} lists no previews")

    missing = []
    if args.gif_dir is not None:
        missing = [slug for slug, _ in entries if not (args.gif_dir / f"{slug}.gif").is_file()]
        if missing:
            sys.exit(f"error: {args.gif_dir} is missing {len(missing)} GIF(s): {', '.join(missing)}")

    items = "\n".join(
        ITEM.format(slug=html.escape(slug, quote=True), name=html.escape(name))
        for slug, name in entries
    )
    args.output.write_text(
        PAGE.format(
            title=html.escape(args.title),
            lede=f"{len(entries)} animations, rendered by the module's native_sim preview harness.",
            items=items,
        ),
        encoding="utf-8",
    )
    print(f"{args.output}: {len(entries)} previews")


if __name__ == "__main__":
    main()
