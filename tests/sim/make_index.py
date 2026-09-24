#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Build a static gallery page from a preview run.

Reads the manifest run-preview.sh writes (slug<TAB>display-name, both derived by
that script) and the fixture overlay that defines each effect, then emits one
page per effect family: every variant's animation, its devicetree node, and links
back to the repository. Used by the GitHub Pages workflow, and handy locally to
browse a run.

Every variant gets an `id` equal to its slug, and every family an `id` equal to
its compatible's suffix, so the README can deep-link straight to either.

The page is deliberately plain: no scripts, no build step.
"""

import argparse
import html
import re
import sys
from pathlib import Path
from string import Template

PAGE = Template(
    """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>$title</title>
<style>
  :root { color-scheme: dark; }
  body {
    margin: 0 auto; padding: 2rem 1rem; max-width: 76rem;
    background: #111; color: #e8e8e8;
    font: 16px/1.5 system-ui, -apple-system, "Segoe UI", sans-serif;
  }
  h1 { font-size: 1.7rem; margin: 0 0 .4rem; }
  p.lede { color: #a8a8a8; margin: 0 0 2.5rem; }
  p.lede a { color: #8ec07c; }
  section { margin: 0 0 3rem; }
  h2 { font-size: 1.2rem; margin: 0 0 .2rem; border-bottom: 1px solid #2c2c2c;
       padding-bottom: .4rem; }
  h2 a { color: inherit; text-decoration: none; }
  h2 a:hover { text-decoration: underline; }
  p.meta { color: #8a8a8a; font-size: .82rem; margin: .5rem 0 1.2rem; }
  p.meta a { color: #8ec07c; }
  ul { display: grid; gap: 1.5rem; margin: 0; padding: 0; list-style: none;
       grid-template-columns: repeat(auto-fill, minmax(22rem, 1fr)); }
  li { margin: 0; scroll-margin-top: 1rem; }
  figure { margin: 0; background: #1b1b1b; border: 1px solid #2c2c2c;
           border-radius: 10px; padding: 1rem; }
  figure img { width: 100%; height: auto; display: block;
               image-rendering: pixelated; border-radius: 4px; }
  figcaption { margin-top: .6rem; font-weight: 600; }
  figcaption a { color: inherit; text-decoration: none; }
  figcaption a:hover { text-decoration: underline; }
  figcaption code { font-weight: 400; }
  pre { margin: .8rem 0 0; padding: .7rem .8rem; overflow-x: auto;
        background: #121212; border: 1px solid #2a2a2a; border-radius: 6px;
        font-size: .78rem; line-height: 1.45; }
  code { color: #8ec07c; font-size: .85em; }
  pre code { color: #d8d8d8; font-size: 1em; }
</style>
</head>
<body>
<header>
  <h1>$title</h1>
  <p class="lede">$lede</p>
</header>
$sections
</body>
</html>
"""
)

FAMILY = Template(
    """<section id="$anchor">
  <h2><a href="#$anchor">$title</a></h2>
  <p class="meta"><code>$compatible</code>$source</p>
  <ul>
$items
  </ul>
</section>
"""
)

ITEM = Template(
    """    <li id="$slug">
      <figure>
        <a href="$slug.gif"><img src="$slug.gif" alt="$name" loading="lazy"></a>
        <figcaption><a href="#$slug">$name</a><br><code>$slug.gif</code></figcaption>
        <pre><code>$dt</code></pre>
      </figure>
    </li>"""
)

NODE_RE = re.compile(r"^\s*[A-Za-z_][A-Za-z0-9_]*\s*:\s*[A-Za-z_][A-Za-z0-9_]*\s*\{\s*$")
NAME_RE = re.compile(r'display-name\s*=\s*"([^"]+)"')
COMPAT_RE = re.compile(r'compatible\s*=\s*"([^"]+)"')
# The fixture's own `index` is meaningless to a reader (it just mirrors the
# order in tests/sim/config/native_sim.overlay), and a wrong one makes the build
# assert. The lede explains the rule instead.
INDEX_LINE_RE = re.compile(
    r"^[ \t]*index[ \t]*=[ \t]*<[^>]*>;[ \t]*(?:/\*.*\*/)?[ \t]*\n", re.M
)


def read_manifest(path):
    entries = []
    for lineno, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) != 2 or not parts[0].strip():
            raise ValueError(f"{path}:{lineno}: expected 'slug<TAB>display-name'")
        entries.append((parts[0].strip(), parts[1].strip()))
    if not entries:
        raise ValueError(f"{path}: lists no previews")
    return entries


def dedent(block):
    lines = block.splitlines()
    indents = [len(l) - len(l.lstrip()) for l in lines if l.strip()]
    cut = min(indents) if indents else 0
    return "\n".join(l[cut:] if l.strip() else "" for l in lines).strip("\n")


def read_effect_nodes(path):
    """Map display-name -> (compatible, node text, first line number).

    The overlay is our own file in a fixed style, so a brace-matching scan over
    the &kprgb children is enough; it fails loudly on a node without a
    display-name rather than publishing an entry with no definition.
    """
    lines = path.read_text(encoding="utf-8").splitlines()
    start = None
    for i, line in enumerate(lines):
        if re.match(r"\s*kprgb\s*:\s*kprgb\s*\{\s*$", line):
            start = i
            break
    if start is None:
        raise ValueError(f"{path}: no 'kprgb: kprgb {{' node found")

    nodes = {}
    depth = 0
    current = None
    block = []
    for i in range(start + 1, len(lines)):
        line = lines[i]
        if current is None:
            if depth == 0 and NODE_RE.match(line):
                current = i
                block = [line]
                depth = line.count("{") - line.count("}")
            continue

        block.append(line)
        depth += line.count("{") - line.count("}")
        if depth == 0:
            text = "\n".join(block)
            name = NAME_RE.search(text)
            compat = COMPAT_RE.search(text)
            if name is None or compat is None:
                raise ValueError(
                    f"{path}:{current + 1}: effect node lacks display-name or compatible"
                )
            key = name.group(1)
            if key in nodes:
                raise ValueError(f"{path}:{current + 1}: duplicate display-name '{key}'")
            nodes[key] = (compat.group(1), text, current + 1)
            current = None
            block = []
    if not nodes:
        raise ValueError(f"{path}: found no effect nodes under &kprgb")
    return nodes


def family_of(compatible, prefix):
    return compatible[len(prefix) :] if compatible.startswith(prefix) else compatible


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--overlay", type=Path, required=True)
    parser.add_argument("-o", "--output", type=Path, required=True)
    parser.add_argument("--title", default="ZMK RGB matrix effect previews")
    parser.add_argument(
        "--gif-dir",
        type=Path,
        default=None,
        help="directory holding the GIFs, checked so a broken manifest fails here "
        "rather than as a missing image on the published page",
    )
    parser.add_argument("--repo-url", default=None, help="e.g. https://github.com/o/r")
    parser.add_argument("--repo-ref", default="main")
    parser.add_argument(
        "--overlay-path",
        default="tests/sim/config/native_sim.overlay",
        help="overlay path within the repository, for the per-effect source link",
    )
    parser.add_argument(
        "--compatible-prefix",
        default="keypaw,rgb-matrix-",
        help="compatibles are grouped into families by this prefix",
    )
    args = parser.parse_args()

    try:
        entries = read_manifest(args.manifest)
        nodes = read_effect_nodes(args.overlay)
    except (OSError, ValueError) as err:
        sys.exit(f"error: {err}")

    missing = [name for _, name in entries if name not in nodes]
    if missing:
        sys.exit(f"error: {args.overlay} defines no node for: {', '.join(missing)}")

    if args.gif_dir is not None:
        absent = [s for s, _ in entries if not (args.gif_dir / f"{s}.gif").is_file()]
        if absent:
            sys.exit(f"error: {args.gif_dir} is missing {len(absent)} GIF(s): {', '.join(absent)}")

    # Group into families, keeping both families and their variants in manifest
    # order (which is the devicetree declaration order).
    families = {}
    for slug, name in entries:
        compatible, text, line = nodes[name]
        key = family_of(compatible, args.compatible_prefix)
        families.setdefault(key, {"variants": []})["variants"].append(
            {
                "slug": slug,
                "name": name,
                "dt": html.escape(INDEX_LINE_RE.sub("", dedent(text))),
                "line": line,
                "compatible": compatible,
            }
        )

    repo = args.repo_url.rstrip("/") if args.repo_url else None
    sections = []
    for key, family in families.items():
        title = family["variants"][0]["name"].split(" (")[0]
        source = ""
        if repo:
            source = (
                f' · <a href="{repo}/blob/{args.repo_ref}/{args.overlay_path}'
                f'#L{family["variants"][0]["line"]}">fixture node</a>'
            )
        items = "\n".join(
            ITEM.substitute(slug=v["slug"], name=html.escape(v["name"]), dt=v["dt"])
            for v in family["variants"]
        )
        sections.append(
            FAMILY.substitute(
                anchor=key.replace("_", "-"),
                title=html.escape(title),
                compatible=html.escape(family["variants"][0]["compatible"]),
                source=source,
                items=items,
            )
        )

    lede = f"{len(entries)} animations in {len(families)} effects, rendered by the module's native_sim preview harness."
    if repo:
        lede += f' <a href="{repo}">Source and documentation on GitHub</a>.'
    lede += (
        " Each node is meant to be copied into your own <code>&amp;kprgb</code> registry,"
        " where its <code>index</code> must equal its position among that node's children,"
        " so the snippets leave it out. Follow <em>fixture node</em> on an effect for a"
        " complete registry, indices and all."
    )
    args.output.write_text(
        PAGE.substitute(title=html.escape(args.title), lede=lede, sections="\n".join(sections)),
        encoding="utf-8",
    )
    print(f"{args.output}: {len(entries)} previews in {len(families)} effects")


if __name__ == "__main__":
    main()
