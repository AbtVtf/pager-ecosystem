"""Dev-mode marketplace launcher.

Stands up the FastAPI registry with the DNS-TXT challenge disabled and
pre-seeds a few demo manifests so the on-device STORE browser has
something to show without needing a real publish flow. Use only for
local development against the pager firmware.

  python3 -m uvicorn dev_serve:app --host 0.0.0.0 --port 8000

The seed list intentionally covers the three trust tags (verified,
featured, plain) so MKT-009 surfacing can be eyeballed too.

In addition to the canonical three demos, every ``examples/*/manifest.yaml``
under the repo is auto-loaded and re-pointed at ``http://<LAN_IP>:<port>/``
so the on-device STORE can fetch them off the dev box. Override the LAN
hostname with the ``PAGEROS_DEV_LAN_IP`` env var; ports come from
``APP_PORTS`` below (must match ``examples/launch_all.sh``).
"""

from __future__ import annotations

import os
import socket
from pathlib import Path

import httpx
from fastapi import HTTPException, Request
from starlette.responses import Response as StarletteResponse

from pageros_marketplace.manifest import Maintainer, Manifest, load_manifest
from pageros_marketplace.registry import Registry, create_app

_DEMOS = [
    {
        "manifest": Manifest(
            id="notes.demo.test",
            name="Notes",
            description="Tiny notepad — one of the canonical PagerOS demos.",
            icon="https://demo.invalid/notes.png",
            url="https://notes.demo.test/",
            permissions=[],
            lora_compatible=True,
            multi_device=False,
            categories=["productivity"],
            maintainer=Maintainer(name="PagerOS demo", contact="demo@pageros.test"),
            version=1,
        ),
        "tags": ["featured"],
    },
    {
        "manifest": Manifest(
            id="weather.demo.test",
            name="Weather",
            description="Three-day forecast over LoRa; works offline at the exit node.",
            icon="https://demo.invalid/weather.png",
            url="https://weather.demo.test/",
            permissions=["location"],
            lora_compatible=True,
            multi_device=False,
            categories=["utilities"],
            donate_url="https://example.com/tip",
            maintainer=Maintainer(name="PagerOS demo", contact="demo@pageros.test"),
            version=1,
        ),
        "tags": ["verified"],
    },
    {
        "manifest": Manifest(
            id="chat.demo.test",
            name="Mesh Chat",
            description="Group chat across nearby pagers, no internet needed.",
            icon="https://demo.invalid/chat.png",
            url="https://chat.demo.test/",
            permissions=["groups", "notifications"],
            lora_compatible=True,
            multi_device=True,
            categories=["social"],
            maintainer=Maintainer(name="PagerOS demo", contact="demo@pageros.test"),
            version=1,
        ),
        "tags": [],
    },
]


# Maps the on-disk examples/<dir>/ to the localhost port used by
# examples/launch_all.sh. Keep both in sync.
APP_PORTS: dict[str, int] = {
    "news": 8011,
    "email": 8012,
    "maps": 8013,
    "dm": 8014,
    "drive": 8015,
    "ssh": 8016,
    "askai": 8017,
    "stocks": 8018,
    "wiki": 8019,
    "translate": 8020,
}

# A few apps deserve a moderation tag on the dev store so the
# verified/featured surfaces aren't empty.
APP_TAGS: dict[str, list[str]] = {
    "news": ["featured"],
    "maps": ["verified"],
    "dm": ["verified"],
    "askai": ["featured"],
}


def _lan_ip() -> str:
    """Best-effort LAN address the pager can route to."""
    override = os.environ.get("PAGEROS_DEV_LAN_IP")
    if override:
        return override.strip()
    # Trick: a UDP socket "connect" picks a route without sending traffic.
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.connect(("8.8.8.8", 80))
            return s.getsockname()[0]
        finally:
            s.close()
    except OSError:
        return "127.0.0.1"


def _examples_root() -> Path:
    return Path(__file__).resolve().parents[2] / "examples"


def _seed_examples(reg: Registry, lan_ip: str) -> None:
    """Register every examples/*/manifest.yaml under the dev LAN address."""
    root = _examples_root()
    if not root.is_dir():
        return
    for child in sorted(root.iterdir()):
        if not child.is_dir():
            continue
        manifest_path = child / "manifest.yaml"
        if not manifest_path.exists():
            continue
        try:
            manifest = load_manifest(manifest_path)
        except Exception:
            # Skip malformed manifests rather than fail the whole boot —
            # dev_serve must stay resilient while contributors iterate.
            continue
        port = APP_PORTS.get(child.name)
        if port is not None:
            # Re-point the canonical URL at the local app server so the
            # device can actually reach it.
            manifest = manifest.model_copy(update={"url": f"http://{lan_ip}:{port}/"})
        try:
            reg.register(manifest)
        except Exception:
            # If the id collides with a builtin demo, prefer the builtin.
            continue
        for tag in APP_TAGS.get(child.name, []):
            reg.set_tags(manifest.id, [tag])


def _build_registry() -> Registry:
    reg = Registry()
    for d in _DEMOS:
        entry = reg.register(d["manifest"])
        for tag in d["tags"]:
            reg.set_tags(entry.manifest.id, [tag])
    _seed_examples(reg, _lan_ip())
    return reg


# Module-level `app` so `uvicorn dev_serve:app` works.
app = create_app(registry=_build_registry(), challenge_required=False)


# --------------------------------------------------------------------------- #
# Dev-only sideload proxy
#
# The on-device sideloader (firmware/components/sideload/src/sideload.c) saves
# whatever URL it was passed as `<app_dir>/.source_url`, and the launcher then
# GETs that URL to fetch the home Frame. Until the firmware learns to use the
# manifest's `url` field instead of the install URL, we proxy every path under
# `/apps/<id>/...` to the actual app server so launches return a real Frame.
# --------------------------------------------------------------------------- #

_proxy_client = httpx.Client(timeout=15.0, follow_redirects=False)

# Map full manifest id → short examples/ dir name, derived from APP_PORTS.
_SHORT_BY_ID: dict[str, str] = {}
for _short_name in APP_PORTS:
    _SHORT_BY_ID[f"{_short_name}.pageros.app"] = _short_name


def _resolve_app_port(app_id: str) -> int | None:
    short = _SHORT_BY_ID.get(app_id)
    if short is None:
        # Fallback: first dot-segment.
        short = app_id.split(".", 1)[0]
    return APP_PORTS.get(short)


_HOP_BY_HOP = {
    "host", "content-length", "connection", "keep-alive",
    "transfer-encoding", "upgrade", "proxy-authenticate",
    "proxy-authorization", "te", "trailers",
}


async def _proxy_to_app(app_id: str, request: Request, rest: str = ""):
    port = _resolve_app_port(app_id)
    if port is None:
        raise HTTPException(404, f"unknown app: {app_id}")
    qs = request.url.query
    target = f"http://127.0.0.1:{port}/{rest}" + (f"?{qs}" if qs else "")
    body = await request.body()
    fwd_headers = {
        k: v for k, v in request.headers.items()
        if k.lower() not in _HOP_BY_HOP
    }
    try:
        upstream = _proxy_client.request(
            request.method, target,
            content=body if body else None,
            headers=fwd_headers,
        )
    except httpx.RequestError as exc:
        raise HTTPException(502, f"upstream error: {exc}") from exc
    resp_headers = {
        k: v for k, v in upstream.headers.items()
        if k.lower() not in _HOP_BY_HOP
    }
    return StarletteResponse(
        content=upstream.content,
        status_code=upstream.status_code,
        headers=resp_headers,
        media_type=upstream.headers.get("content-type"),
    )


# The home-Frame fetch hits `<source_url>/` with trailing slash, so register
# both the bare and the path-suffixed forms. FastAPI matches earlier routes
# first, so the explicit `/.pageros/manifest.cbor` and `GET /apps/{id}` routes
# inside create_app() still win.
app.add_api_route(
    "/apps/{app_id}/",
    _proxy_to_app,
    methods=["GET", "POST", "PUT", "DELETE"],
    include_in_schema=False,
)
app.add_api_route(
    "/apps/{app_id}/{rest:path}",
    _proxy_to_app,
    methods=["GET", "POST", "PUT", "DELETE"],
    include_in_schema=False,
)
