#!/usr/bin/env python3
"""Static site generator for spec.pageros.org (DOCS-003).

Renders SPEC.md plus the normative protocol/* documents and docs/spec/* into a
small static HTML site with sidebar navigation. Output goes to ``_site/``.

Single dependency: ``markdown`` (>=3.5). Install with::

    pip install -r requirements.txt

Usage::

    python3 build.py            # build into _site/
    python3 build.py --serve    # build, then serve _site/ on :8000
"""
from __future__ import annotations

import argparse
import html
import http.server
import os
import re
import shutil
import socketserver
import sys
from dataclasses import dataclass
from pathlib import Path

try:
    import markdown
except ImportError:  # pragma: no cover
    print(
        "error: the 'markdown' package is required.\n"
        "       run: pip install -r requirements.txt",
        file=sys.stderr,
    )
    raise SystemExit(2)


SITE_DIR = Path(__file__).resolve().parent
REPO_ROOT = SITE_DIR.parents[1]
OUT_DIR = SITE_DIR / "_site"
TEMPLATE_PATH = SITE_DIR / "template.html"
STYLES_PATH = SITE_DIR / "styles.css"

SITE_TITLE = "PagerOS Spec"
SITE_TAGLINE = "Normative specifications for PagerOS v0.2"


@dataclass(frozen=True)
class Page:
    """A single rendered page in the spec site."""

    slug: str  # url path (no extension), e.g. "protocol"
    title: str  # short nav label
    long_title: str  # page <h1> / <title>
    source: Path  # absolute path to source .md (relative to REPO_ROOT after resolve)
    section: str  # nav section label
    summary: str  # one-line description shown on the home page


# Order here determines sidebar order.
PAGES: list[Page] = [
    Page(
        slug="index",
        title="Overview",
        long_title="PagerOS Spec",
        source=SITE_DIR / "index.md",
        section="Start",
        summary="Landing page for the spec site.",
    ),
    Page(
        slug="spec",
        title="SPEC.md (full system)",
        long_title="PagerOS — Technical Specification",
        source=REPO_ROOT / "SPEC.md",
        section="System",
        summary="The full v0.2 system specification. Single source of truth for "
        "scope, architecture, and non-protocol concerns.",
    ),
    Page(
        slug="protocol",
        title="UI protocol (PROTO-002)",
        long_title="PagerOS UI Protocol — v1.0",
        source=REPO_ROOT / "protocol" / "spec.md",
        section="Protocol",
        summary="Normative UI protocol: Frames, widgets, events, wire format.",
    ),
    Page(
        slug="tag-registry",
        title="CBOR tag registry (PROTO-001)",
        long_title="PagerOS CBOR Tag Registry",
        source=REPO_ROOT / "protocol" / "tag-registry.md",
        section="Protocol",
        summary="Canonical widget and event identifiers used on the wire.",
    ),
    Page(
        slug="exit-node-advertise",
        title="Exit-node advertise (LORA-005)",
        long_title="Exit Node Advertise",
        source=REPO_ROOT / "protocol" / "exit-node-advertise.md",
        section="Protocol",
        summary="LoRa envelope `0x04` payload format used by exit-node "
        "discovery.",
    ),
    Page(
        slug="crypto-suite",
        title="Crypto suite (SEC-001)",
        long_title="PagerOS Crypto Suite Selection",
        source=REPO_ROOT / "docs" / "spec" / "crypto-suite.md",
        section="Security",
        summary="Library choices for Ed25519, X25519, and ChaCha20-Poly1305 "
        "plus the shared test-vector set.",
    ),
]


def load_template() -> str:
    return TEMPLATE_PATH.read_text(encoding="utf-8")


def md_to_html(text: str) -> str:
    """Render markdown to HTML with the extensions we use across all docs."""
    md = markdown.Markdown(
        extensions=[
            "fenced_code",
            "tables",
            "toc",
            "sane_lists",
            "admonition",
            "footnotes",
        ],
        extension_configs={
            "toc": {"toc_depth": "2-4", "permalink": False},
        },
        output_format="html5",
    )
    return md.convert(text)


def build_sidebar(active_slug: str) -> str:
    """Render the sidebar as <nav> HTML, grouped by section."""
    sections: dict[str, list[Page]] = {}
    for page in PAGES:
        sections.setdefault(page.section, []).append(page)

    out: list[str] = ['<nav class="sidebar" aria-label="Spec sections">']
    for section, pages in sections.items():
        out.append(f'  <h3 class="sidebar-section">{html.escape(section)}</h3>')
        out.append('  <ul class="sidebar-list">')
        for page in pages:
            href = "index.html" if page.slug == "index" else f"{page.slug}.html"
            current = ' aria-current="page"' if page.slug == active_slug else ""
            cls = " active" if page.slug == active_slug else ""
            out.append(
                f'    <li><a class="sidebar-link{cls}" href="{href}"{current}>'
                f"{html.escape(page.title)}</a></li>"
            )
        out.append("  </ul>")
    out.append("</nav>")
    return "\n".join(out)


def make_index_md() -> str:
    """The home page is generated; sidebar navigation does the rest."""
    rows = [
        "# PagerOS Spec",
        "",
        "Normative specifications for PagerOS v0.2.",
        "",
        "> This site renders the source-of-truth markdown documents committed to",
        "> the [`pager-ecosystem`](https://github.com/paperclipai/pager-ecosystem)",
        "> repository. On any disagreement between this site and the markdown",
        "> sources, the markdown wins.",
        "",
        "## Contents",
        "",
    ]
    seen: set[str] = set()
    for page in PAGES:
        if page.slug == "index" or page.section in seen:
            continue
        seen.add(page.section)
        rows.append(f"### {page.section}")
        rows.append("")
        for sibling in PAGES:
            if sibling.slug == "index" or sibling.section != page.section:
                continue
            href = f"{sibling.slug}.html"
            rows.append(f"- **[{sibling.title}]({href})** — {sibling.summary}")
        rows.append("")

    rows.extend(
        [
            "## How this site is built",
            "",
            "A small Python script (`docs/spec-site/build.py`) reads the",
            "markdown documents in place and renders them with a sidebar",
            "navigation. No content is duplicated — the build pulls from the",
            "live repository copies of `SPEC.md`, `protocol/*.md`, and",
            "`docs/spec/*.md`.",
            "",
            "See [`docs/spec-site/README.md`](https://github.com/paperclipai/"
            "pager-ecosystem/blob/main/docs/spec-site/README.md) for build and",
            "deploy instructions.",
        ]
    )
    return "\n".join(rows)


def rewrite_internal_links(body_html: str) -> str:
    """Map links to source paths into the rendered site's url space.

    The source markdown files cross-link with paths like
    ``protocol/spec.md`` or ``../../SPEC.md``. Map those to the rendered
    ``foo.html`` page when we have one.
    """
    source_to_slug: dict[str, str] = {}
    for page in PAGES:
        if page.slug == "index":
            continue
        rel = page.source.resolve().relative_to(REPO_ROOT)
        source_to_slug[str(rel)] = page.slug

    def replacement(match: re.Match[str]) -> str:
        href = match.group(1)
        # Strip any "../"+ prefix; we only care about the repo-relative tail.
        bare = re.sub(r"^(?:\.\./)+", "", href)
        bare = bare.split("#", 1)[0].split("?", 1)[0]
        if bare in source_to_slug:
            anchor = ""
            if "#" in href:
                anchor = "#" + href.split("#", 1)[1]
            return f'href="{source_to_slug[bare]}.html{anchor}"'
        # Markdown files we don't host fall back to the GitHub copy.
        if bare.endswith(".md") and not bare.startswith("http"):
            return (
                f'href="https://github.com/paperclipai/pager-ecosystem/blob/'
                f'main/{bare}"'
            )
        return match.group(0)

    return re.sub(r'href="([^"]+)"', replacement, body_html)


def render_page(page: Page, template: str) -> str:
    if page.slug == "index":
        source_text = make_index_md()
        source_label = "(generated)"
    else:
        source_text = page.source.read_text(encoding="utf-8")
        source_label = str(page.source.resolve().relative_to(REPO_ROOT))

    body_html = md_to_html(source_text)
    body_html = rewrite_internal_links(body_html)
    sidebar_html = build_sidebar(page.slug)

    source_url = (
        f"https://github.com/paperclipai/pager-ecosystem/blob/main/{source_label}"
        if source_label != "(generated)"
        else "https://github.com/paperclipai/pager-ecosystem/blob/main/docs/"
        "spec-site/build.py"
    )

    return (
        template.replace("{{TITLE}}", html.escape(page.long_title))
        .replace("{{SITE_TITLE}}", html.escape(SITE_TITLE))
        .replace("{{SITE_TAGLINE}}", html.escape(SITE_TAGLINE))
        .replace("{{SIDEBAR}}", sidebar_html)
        .replace("{{BODY}}", body_html)
        .replace("{{SOURCE_LABEL}}", html.escape(source_label))
        .replace("{{SOURCE_URL}}", source_url)
    )


def build() -> None:
    if OUT_DIR.exists():
        shutil.rmtree(OUT_DIR)
    OUT_DIR.mkdir(parents=True)

    template = load_template()

    for page in PAGES:
        rendered = render_page(page, template)
        out_path = OUT_DIR / f"{page.slug}.html"
        out_path.write_text(rendered, encoding="utf-8")

    shutil.copy2(STYLES_PATH, OUT_DIR / "styles.css")
    print(f"built {len(PAGES)} pages into {OUT_DIR.relative_to(REPO_ROOT)}/")


def serve(port: int) -> None:
    os.chdir(OUT_DIR)
    handler = http.server.SimpleHTTPRequestHandler
    with socketserver.TCPServer(("", port), handler) as httpd:
        print(f"serving _site/ at http://127.0.0.1:{port}/ — Ctrl-C to stop")
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nstopped.")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--serve",
        action="store_true",
        help="build, then serve _site/ on the chosen port",
    )
    parser.add_argument("--port", type=int, default=8000)
    args = parser.parse_args(argv)

    build()
    if args.serve:
        serve(args.port)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
