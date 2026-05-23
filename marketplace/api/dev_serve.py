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
