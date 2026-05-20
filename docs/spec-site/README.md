# Spec docs site (`spec.pageros.org`)

This directory is the source for the PagerOS spec docs site at
`spec.pageros.org`. The site renders `SPEC.md` and the normative
`protocol/*.md` documents (plus `docs/spec/crypto-suite.md`) as a small
static HTML site with sidebar navigation.

## Layout

| File               | What it is                                                         |
| ------------------ | ------------------------------------------------------------------ |
| `build.py`         | Static site generator. Reads markdown in-place, writes `_site/`.   |
| `template.html`    | Page shell. `{{...}}` placeholders filled by `build.py`.           |
| `styles.css`       | Stylesheet. Matches `docs/landing/styles.css` aesthetic.           |
| `index.md`         | Generated at build time (do not commit a hand-written copy).       |
| `requirements.txt` | One dependency: `markdown`.                                        |

## Build

The site has one Python dependency (`markdown`). Install in a venv to keep
your system Python clean:

```sh
cd docs/spec-site
python3 -m venv .venv
. .venv/bin/activate
pip install -r requirements.txt
python3 build.py            # writes _site/
```

To preview locally:

```sh
python3 build.py --serve    # http://127.0.0.1:8000/
# (override port with --port 9000)
```

The generator does **not** copy or duplicate the source markdown — it reads
`SPEC.md` and `protocol/*.md` straight from the repo, so the rendered site
is always in sync with the committed sources.

## Adding a page

To add another doc to the site, edit `PAGES` in `build.py`:

```py
Page(
    slug="my-page",
    title="My Page (in sidebar)",
    long_title="Full title shown as <h1>",
    source=REPO_ROOT / "path" / "to" / "source.md",
    section="Protocol",   # nav grouping
    summary="One-line description for the index page.",
)
```

Rebuild and the page will appear in the sidebar.

## Acceptance checklist (DOCS-003)

- [x] `SPEC.md` rendered with nav (`spec.html`).
- [x] `protocol/spec.md` rendered with nav (`protocol.html`).
- [x] Companion normative docs rendered: `protocol/tag-registry.md`,
      `protocol/exit-node-advertise.md`, `docs/spec/crypto-suite.md`.
- [x] Static output — any web host can serve it (no runtime JS required).
- [x] Single source of truth preserved: no duplicate copies of `SPEC.md`
      or `protocol/*.md` in this directory.

## Deployment

The output of `python3 build.py` (the `_site/` directory) is the deployable
artifact. The actual hosting (DNS, TLS, CDN) is owned by ops and not
configured in this repo. The site is plain static HTML/CSS — GitHub Pages,
Netlify, Cloudflare Pages, or a tiny nginx are all fine.

Suggested CI step (not yet wired): on push to `main` that touches
`SPEC.md`, `protocol/**`, `docs/spec/**`, or `docs/spec-site/**`, run
`python3 docs/spec-site/build.py` and upload `docs/spec-site/_site/` to
`spec.pageros.org`.

## Why not mkdocs / docsify / vitepress?

The landing page (`docs/landing/`) is plain HTML with no build step. We
match that aesthetic and keep the surface small:

- No JavaScript runtime — the rendered HTML is self-contained.
- One Python dependency, no Node tooling.
- No theme to keep in lockstep with upstream.
- Easy to read end-to-end (`build.py` is ~200 lines).

If the site needs richer features later (search, versioning, automatic
toc-on-page), swap in mkdocs-material or vitepress and reuse the same
source markdown — `build.py` doesn't lock anything in.
