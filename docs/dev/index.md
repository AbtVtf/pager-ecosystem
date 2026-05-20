# Developer Getting Started

This page walks you from an empty directory to a published PagerOS app. Budget: **~30 minutes of reading**, plus whatever time it takes to type along.

If you only have ten minutes, read [The 60-second tour](#the-60-second-tour) and [Scaffold and run a hello-world app](#scaffold-and-run-a-hello-world-app).

## What you'll build

A minimal app called **hello** that serves a single screen with the text *"Hello from hello!"* over the PagerOS UI protocol, runs locally, renders in the simulator, and is published to the Marketplace under your own DNS-verified app id.

## Prerequisites

- **Python 3.10+** with `venv` and `pip`. (JavaScript support is tracked in JS-001 — until it ships, the Python SDK is the only supported language.)
- **`pagerctl`** — the PagerOS CLI. See [Install pagerctl](#install-pagerctl).
- **A domain you control**, for the manifest `id` and DNS TXT challenge at publish time. Any subdomain works (e.g. `hello.example.com`).
- *Optional:* the **simulator** (`simulator/`) for previewing without a real device. Building it requires Rust + Tauri toolchains — see `simulator/RELEASING.md`.

You do **not** need a physical pager to develop or to publish. A device only matters when you want to test on real hardware (`pagerctl flash`, `pagerctl link`).

## The 60-second tour

A PagerOS app is an ordinary HTTPS server that returns **Frames** — CBOR-encoded UI documents — and accepts events from devices. There is no proprietary runtime on the server; you can write one by hand in any language. The official SDKs (`sdk/python/`, JS pending) just hide the wire format behind decorators.

```python
from pageros import App

app = App(name="hello")


@app.screen("/")
def home():
    return {
        "v": 1,
        "id": "scr_home",
        "body": [{"t": "text", "s": "Hello, PagerOS!"}],
    }


if __name__ == "__main__":
    app.run()
```

A device or simulator sends `GET /` with `Accept: application/cbor; pagerOS=1`. Your handler returns a dict; the SDK CBOR-encodes it and the device renders it. That's the whole protocol — `SPEC.md` §5 has the widget catalog, §7 has the request envelope.

## Install pagerctl

Pick one:

| Platform           | Command                                                                                                  |
| ------------------ | -------------------------------------------------------------------------------------------------------- |
| macOS / Linux      | `brew install pageros/tap/pagerctl`                                                                      |
| Debian / Ubuntu    | Download `pagerctl_<version>_linux_amd64.deb` and `sudo apt install ./pagerctl_*.deb`                    |
| Fedora / RHEL      | `sudo dnf install ./pagerctl-<version>.x86_64.rpm`                                                       |
| Alpine             | `sudo apk add --allow-untrusted ./pagerctl_<version>_linux_amd64.apk`                                    |
| Windows            | `scoop bucket add pageros https://github.com/pageros/scoop-bucket && scoop install pagerctl`             |
| From source        | `cd cli && go build -o pagerctl ./cmd/pagerctl`                                                          |

Verify:

```sh
pagerctl version
```

Run `pagerctl help` to see every subcommand. The ones used in this guide are `init` and `publish`. `dev`, `simulate`, and `link` are listed but not yet implemented (CLI-002/003/006); the workarounds appear inline below.

## Scaffold and run a hello-world app

```sh
pagerctl init python --name hello
cd hello
```

You now have:

```
hello/
├── app.py            # Your code. Edit this.
├── manifest.yaml     # Marketplace metadata. Edit before publishing.
├── requirements.txt  # `pageros>=0.0.1`
├── README.md
└── .gitignore
```

Open `app.py` and read it — it's ~15 lines. The `App` class, the `@app.screen("/")` decorator, the Frame dict, and `app.run()`. That's the surface.

Create a virtualenv, install the SDK, run:

```sh
python -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt
python app.py --host 127.0.0.1 --port 8000
```

The server logs `pageros app 'hello' listening on http://127.0.0.1:8000`. Curl it the way a device would:

```sh
curl -H 'Accept: application/cbor; pagerOS=1' http://127.0.0.1:8000/ | xxd | head
```

You should see a short binary blob — that's the CBOR-encoded Frame. If you want a text-eyeball-able version while you're learning, set the SDK's logger to debug:

```sh
PYTHONUNBUFFERED=1 python -c "import logging; logging.basicConfig(level=logging.DEBUG); import app"
```

### Auto-reload during development

Once you start changing `app.py` frequently you'll want auto-reload. The Python SDK ships a dev server that does exactly that:

```sh
python -m pageros.devserver --app app.py --host 127.0.0.1 --port 8000
```

This is the same entry point `pagerctl dev` (CLI-002) will wrap once it lands. The two flags are `--app` (path to your `app.py`) and the usual `--host` / `--port`.

## View it in the simulator

The simulator is a small Tauri app under `simulator/`. From the repo root:

```sh
cd simulator
cargo tauri dev
```

In the simulator's URL bar, point it at `http://127.0.0.1:8000/` (the dev server you started above). You should see the 480×222 pager screen rendering your text widget. Keyboard input is mapped per `simulator/KEYMAP.md` — ENTER activates, Escape is BACK, arrow keys turn the encoder.

Once `pagerctl simulate <url>` (CLI-003) ships, `pagerctl simulate http://127.0.0.1:8000/` will replace this two-step.

## Add a second screen

PagerOS apps are link-driven: a widget's `href` becomes a request, and the response is the next screen. Try replacing `home()` with a list that links to a detail screen:

```python
from pageros import App

app = App(name="hello")


@app.screen("/")
def home():
    return {
        "v": 1,
        "id": "scr_home",
        "body": [{"t": "text", "s": "Hello, PagerOS!", "style": "heading"},
                 {"t": "list", "items": [
                     {"label": "What is this?", "href": "/about"},
                 ]}],
    }


@app.screen("/about")
def about():
    return {
        "v": 1,
        "id": "scr_about",
        "body": [{"t": "text", "s": "An app served from your laptop."}],
    }


if __name__ == "__main__":
    app.run()
```

Reload the page in the simulator. Use the encoder (Arrow Down) to highlight the list item, then ENTER to navigate to `/about`. Escape (BACK) returns. That round-trip — request, Frame, render, event, request — is the entire UI protocol.

The full widget catalog (`text`, `list`, `input`, `form`, `button`, `image`, `map`, `notification`, `presence_list`, `chat`) lives in `SPEC.md` §5.3. Each widget has a typed CBOR shape; the SDK does not validate it for you — invalid Frames render as `[unsupported: t]` placeholders.

## Accepting form input

A `form` widget POSTs back to the action URL. Define a POST handler with `@app.handler(path, method="POST")`:

```python
@app.screen("/new")
def new():
    return {
        "v": 1, "id": "scr_new",
        "body": [{"t": "form", "action": "/save", "method": "POST",
                  "fields": [{"t": "input", "name": "title", "label": "Title"}],
                  "submit": "Save"}],
    }


@app.handler("/save", method="POST")
def save(req):
    title = req.body.get("title", "")
    # Persist `title` here. The Python SDK doesn't ship storage — pick SQLite,
    # a file, or a key-value store of your choice. See `examples/notes/` (DOCS-006)
    # for a reference persistence layer.
    return {"v": 1, "id": "scr_saved",
            "body": [{"t": "notification", "level": "info", "s": f"Saved: {title}"}]}
```

`req.body` is the CBOR-decoded request body (a dict of form fields). `req.query`, `req.headers`, and `req.method` are also available — see the `Request` dataclass in `sdk/python/pageros/app.py`.

## Prepare your manifest

Open `manifest.yaml`. The scaffold gave you a placeholder; edit it before publishing.

```yaml
id: hello.example.com          # Reverse-DNS app id. MUST be a domain you control.
name: hello
description: A hello-world PagerOS app.
icon: https://example.com/hello/icon.png
url: https://example.com/hello/
categories: [utilities]
maintainer:
  name: Your Name
  contact: you@example.com
version: 1
```

Fields that matter for `pagerctl publish`:

- **`id`** — reverse-DNS string (`alpha.example.com`, `org.example.weather`). The Marketplace will challenge the DNS host of `url` (see below), so `id` and the host of `url` should match the domain you can edit DNS for.
- **`url`** — the **deployed** HTTPS URL where your app's root will live. Not your dev server.
- **`icon`** — public PNG/SVG, ≤ 16 KiB, square. The Marketplace fetches it once.
- **`version`** — integer. Bump it every time you re-publish.

Optional fields the SDK and Marketplace know about:

- **`lora_compatible: true`** — promise that every encoded Frame fits in 200 B (one LoRa packet). The SDK will log a warning at runtime when a Frame exceeds the budget. See `sdk/python/pageros/lora_budget.py`.
- **`multi_device: true`** — your app supports shared sessions and uses `presence_list` / `chat` widgets (SPEC §5.4.2).
- **`permissions: [push, gps, nfc, ...]`** — capabilities the device will prompt the user for. Omit if your app only renders screens.
- **`donate_url`** — surfaced as a "Tip developer" action on the Marketplace listing.

## Deploy

`pagerctl publish` does **not** deploy your app. It only registers a manifest. You're responsible for putting `app.py` somewhere reachable at the `url` in your manifest, with valid TLS.

Anything that can host a Python WSGI/HTTP service works: Fly.io, Render, Railway, a $5 VPS with nginx + systemd, AWS Lambda function URLs, etc. The SDK uses `http.server.ThreadingHTTPServer` under the hood, so you can also wrap `app.run()` in any of the usual production runners (gunicorn, uvicorn-with-WSGI-shim, etc.) — `App` exposes the routing dispatch directly, so adapting it to another server framework is a 20-line file.

Once your app responds to `GET /` at `https://hello.example.com/` with a CBOR Frame, move on.

## Publish

Run from your project directory (the one with `manifest.yaml`):

```sh
pagerctl publish
```

What happens:

1. **Local validation.** The CLI parses `manifest.yaml`, checks the reverse-DNS pattern of `id`, normalizes the URL, and rejects obvious mistakes before touching the network.
2. **DNS TXT challenge.** The Marketplace returns a `_pageros-challenge.<host>` TXT record and a value. The CLI prints it:
   ```
   _pageros-challenge.hello.example.com.  IN  TXT  "v=1; t=…"
   ```
3. **You publish the TXT record** in your DNS provider's control panel and press Enter. (Use `--skip-prompt --verify-delay 60s` in CI when the record is provisioned automatically.)
4. **Verification.** The Marketplace resolves the TXT record from authoritative DNS. On success it returns a one-time `verified` token.
5. **Registration.** The CLI POSTs the manifest plus the token to the Marketplace. You get back a listing URL like `https://market.pageros.org/apps/hello.example.com`.

If verification fails, the most common causes are TTL (wait one full TTL after publishing the record) and provider quirks where the TXT key is `_pageros-challenge.hello` instead of fully qualified — read the message; the CLI passes the Marketplace's error body through verbatim.

Re-publishing (a new icon, a bumped `version`, a new description) re-runs the whole flow. The DNS challenge is one-shot — you'll be asked to publish a fresh TXT record each time.

## What "done" looks like

- `pagerctl init python` produced a runnable project.
- `python app.py` (or `python -m pageros.devserver --app app.py`) served a Frame to curl.
- The simulator rendered your screen and round-tripped a list/link click.
- A `manifest.yaml` describes your real `url` and `id`.
- `pagerctl publish` returned a listing URL.

You now have a Marketplace-listed PagerOS app. Devices that install it will request Frames directly from your `url` — there's nothing between them and you except DNS and TLS.

## Where to go next

- **Real-world examples.** `examples/` in this repo: `hello` (this guide), `notes` (forms + persistence, DOCS-006), `weather` (image + remote API, DOCS-007), `gps-tracker` (location event, DOCS-008), `nfc-counter` (NFC scan event, DOCS-009), `chat` (multi-device, DOCS-010), `push-reminder` (push notifications, DOCS-011).
- **Widget catalog.** `SPEC.md` §5.3 — every widget and its CBOR shape.
- **Event model.** `SPEC.md` §5.4 — what `Encoder`, `Back`, `Activate`, GPS, NFC, and group events look like.
- **Caching & TTL.** `SPEC.md` §5.5 — the `cache_ms` field that lets your app survive the device losing Wi-Fi.
- **Push notifications.** `sdk/python/pageros/push.py` and `App.push(...)` — send a notification to a known device via the Push Relay. The `push-reminder` example is the canonical reference.
- **LoRa budget.** `sdk/python/README.md` and `pageros.lora_budget` — keep Frames ≤ 200 B if you want the app to work off Wi-Fi.
- **End-to-end protocol detail.** `SPEC.md` §7 (request envelope) and `protocol/` (wire format).

## Troubleshooting

**`pagerctl init js`** fails with *"JS scaffold not yet implemented (waiting on JS-001)."* That's expected; use `pagerctl init python` until the JS SDK lands.

**`pagerctl dev` / `simulate` / `link`** print *"not implemented yet"*. They are tracked as CLI-002, CLI-003, and CLI-006. The workarounds — `python -m pageros.devserver` and pointing the simulator at the dev server manually — are above.

**`python app.py` exits immediately** with `ImportError: pageros`. You forgot to activate the venv, or `pip install -r requirements.txt` failed silently. Re-run `source .venv/bin/activate && pip install -r requirements.txt`.

**Simulator shows `[unsupported: t]`** for a widget. Your Frame has a `t` value that isn't in the widget catalog — check spelling against `SPEC.md` §5.3.

**`pagerctl publish` returns `challenge did not enter verified state`.** Your TXT record either isn't propagated yet or isn't on the exact host the Marketplace asks for. Verify with `dig +short TXT _pageros-challenge.<host>` from a public resolver (`@1.1.1.1`), wait one TTL, and retry.

**`pagerctl publish` returns a validation error.** The Marketplace and the CLI share an error format; the field that failed is in the output. Common ones: `id` is not a valid reverse-DNS string; `url` is HTTP instead of HTTPS; `icon` exceeds the size limit.

If you hit something that isn't covered here, open an issue against the repo with the exact CLI output — the project tracks dev-experience gaps as DOCS-* tasks.
