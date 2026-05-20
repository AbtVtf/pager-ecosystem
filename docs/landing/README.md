# Landing page (`pageros.org`)

This directory is the source for the PagerOS project landing page at
`pageros.org`.

## Layout

| File | What it is |
|---|---|
| `index.html` | The page itself. Plain HTML; no build step. |
| `styles.css` | Stylesheet. Hand-written, ~150 lines. |
| `demo.svg` | Animated SVG of the device UI, used as the hero "demo gif". |

## Local preview

Open `index.html` in any browser, or:

```sh
python3 -m http.server -d docs/landing 8000
# then open http://127.0.0.1:8000/
```

Everything is relative-pathed so it also works when served from a
sub-path during preview.

## Why an animated SVG and not a GIF?

The project is pre-alpha. The simulator (`simulator/`) and firmware
shell (`firmware/`) aren't far enough along to produce a real captured
demo. The SVG cycles through three representative screens (home
launcher, Notes list, group chat) at the device's native 480×222
resolution so visitors get an honest sense of the form factor.

Replace `demo.svg` with a recorded `demo.mp4` (and a `<video>` tag) or
`demo.gif` (and an `<img>` tag) once one exists. The hero markup in
`index.html` is intentionally trivial — just swap the asset and adjust
width/height.

The SVG also respects `prefers-reduced-motion`: motion-sensitive
visitors see only the home screen with no animation.

## Acceptance checklist (DOCS-012)

- [x] Tagline ("Apps for your pocket. Wi-Fi or LoRa.")
- [x] Demo (animated SVG placeholder for an eventual real GIF/MP4)
- [x] "Get a device" CTA → `#get-a-device` section
- [x] "Build an app" CTA → `#build-an-app` section
- [x] Link to docs (`docs.pageros.org`)
- [x] Link to spec (`docs.pageros.org/spec/`)

## Deployment

This landing page is the source for `pageros.org`. The actual hosting
(DNS, TLS, CDN) is owned by ops and not configured in this repo. The
content here is static and can be served by any HTTP server — GitHub
Pages, Netlify, Cloudflare Pages, a tiny nginx, etc.

If/when a doc-site build is wired up that also covers `docs.pageros.org`
(via `docs/user/`, `docs/dev/`, `docs/spec/`), it can be pointed at this
directory as the root site root.

## Links in the page

External targets used:

- `https://docs.pageros.org/...` — the docs site (not yet hosted).
- `https://github.com/paperclipai/pager-ecosystem` — repository.

All in-page CTAs are same-page anchors, so the page is self-contained
and works offline.
