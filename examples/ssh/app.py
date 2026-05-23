"""PagerOS SSH client example.

A tiny one-shot SSH client for the LILYGO T-LoRa pager. Lets you save a
short list of hosts, then run a single command on one of them and view
the (truncated) output on the 480x222 screen.

Hosts are persisted to ``ssh_hosts.json`` next to this file as a plain
JSON array of ``{nickname, host, port, user, password?, key_path?}``
records. Network work (paramiko) is fully wrapped in try/except so a bad
host or missing dependency renders an error Screen instead of crashing
the dev server.

Run with::

    python app.py --port 8016

SECURITY NOTE: This example uses ``AutoAddPolicy`` for unknown host
keys, which is only appropriate for a local dev demo. See README.
"""

from __future__ import annotations

import json
import threading
from pathlib import Path
from typing import Any

from pageros import (
    App,
    Button,
    Form,
    Input,
    List,
    ListItem,
    Notification,
    Screen,
    Text,
)

# --------------------------------------------------------------------------- #
# Constants — keep text short for the 480x222 pager screen.
# --------------------------------------------------------------------------- #

STORE_PATH = Path(__file__).with_name("ssh_hosts.json")
OUTPUT_CHAR_LIMIT = 2000
CONNECT_TIMEOUT = 5.0
COMMAND_TIMEOUT = 10.0
CMD_MAX = 200

_store_lock = threading.Lock()

app = App(name="ssh")


# --------------------------------------------------------------------------- #
# Host store (JSON file on disk, list of host dicts).
# --------------------------------------------------------------------------- #


def _load_hosts() -> list[dict[str, Any]]:
    if not STORE_PATH.exists():
        return []
    try:
        raw = json.loads(STORE_PATH.read_text("utf-8"))
    except (OSError, ValueError):
        return []
    if not isinstance(raw, list):
        return []
    return [h for h in raw if isinstance(h, dict) and h.get("nickname")]


def _save_hosts(hosts: list[dict[str, Any]]) -> None:
    STORE_PATH.write_text(
        json.dumps(hosts, indent=2, sort_keys=True), encoding="utf-8"
    )


def _find_host(nickname: str) -> dict[str, Any] | None:
    for h in _load_hosts():
        if h.get("nickname") == nickname:
            return h
    return None


def _first_q(request, key: str) -> str:
    vals = request.query.get(key) or []
    return (vals[0] if vals else "").strip()


def _trim_output(s: str, n: int = OUTPUT_CHAR_LIMIT) -> str:
    if not s:
        return ""
    return s if len(s) <= n else s[: n - 1] + "…"


# --------------------------------------------------------------------------- #
# Screen builders
# --------------------------------------------------------------------------- #


def _home_screen(flash: str | None = None) -> Screen:
    hosts = _load_hosts()
    body: list[Any] = [Text("Saved hosts", style="heading")]
    if flash:
        body.append(Notification(s=flash))
    if not hosts:
        body.append(Text("No hosts yet — add one to get started.", style="dim"))
    else:
        body.append(
            List(
                items=[
                    ListItem(
                        label=h["nickname"],
                        sub=f"{h.get('user', '?')}@{h.get('host', '?')}:{int(h.get('port', 22))}",
                        href=f"/host?n={h['nickname']}",
                    )
                    for h in hosts
                ]
            )
        )
    body.append(Button(label="Add host", href="/add", method="GET"))
    return Screen(id="ssh_home", title="SSH", body=body, ttl=0)


def _add_form_screen(values: dict[str, Any] | None = None,
                     flash: str | None = None) -> Screen:
    v = values or {}
    body: list[Any] = [Text("Add host", style="heading")]
    if flash:
        body.append(Notification(s=flash, level="warn"))
    body.append(
        Form(
            action="/add",
            method="POST",
            submit="Save",
            fields=[
                Input(name="nickname", label="Nickname",
                      value=v.get("nickname"), max=40),
                Input(name="host", label="Host", value=v.get("host"), max=120),
                Input(name="port", label="Port", type="number",
                      value=v.get("port", 22)),
                Input(name="user", label="User", value=v.get("user"), max=40),
                Input(name="password", label="Password (optional)",
                      type="password", value=v.get("password")),
                Input(name="key_path", label="Key path (optional)",
                      value=v.get("key_path"), max=200),
            ],
        )
    )
    body.append(Button(label="Back", href="/", method="GET"))
    return Screen(id="ssh_add", title="Add host", body=body, ttl=0)


def _host_screen(nickname: str, flash: str | None = None) -> Screen:
    host = _find_host(nickname)
    if host is None:
        return _missing_screen()
    body: list[Any] = [
        Text(host["nickname"], style="heading"),
        Text(f"{host.get('user', '?')}@{host.get('host', '?')}", style="dim"),
    ]
    if flash:
        body.append(Notification(s=flash))
    body.append(
        Form(
            action=f"/run?n={nickname}",
            method="POST",
            submit="Run",
            fields=[Input(name="cmd", label="Command", max=CMD_MAX)],
        )
    )
    body.append(
        Button(
            label="Delete host",
            href=f"/delete?n={nickname}",
            method="POST",
            confirm="Delete this host?",
        )
    )
    body.append(Button(label="Back", href="/", method="GET"))
    return Screen(id=f"ssh_host_{nickname}", title=nickname,
                  body=body, ttl=0)


def _missing_screen() -> Screen:
    return Screen(
        id="ssh_missing",
        title="SSH",
        body=[
            Notification(s="That host no longer exists.", level="warn"),
            Button(label="Back", href="/", method="GET"),
        ],
        ttl=0,
    )


def _error_screen(message: str) -> Screen:
    return Screen(
        id="ssh_error",
        title="SSH error",
        body=[
            Notification(s=_trim_output(message, 240), level="error"),
            Button(label="Back", href="/", method="GET"),
        ],
        ttl=0,
    )


# --------------------------------------------------------------------------- #
# SSH execution
# --------------------------------------------------------------------------- #


def _run_remote_command(host: dict[str, Any], cmd: str) -> tuple[str, str]:
    """Connect and execute ``cmd``. Returns (stdout, stderr).

    Raises a plain :class:`Exception` (or paramiko subclass) on failure;
    the caller wraps the call in try/except.
    """
    import paramiko  # local import so missing dep is caught upstream

    client = paramiko.SSHClient()
    client.load_system_host_keys()
    # AutoAddPolicy is acceptable for the dev demo; see README.
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())

    connect_kwargs: dict[str, Any] = {
        "hostname": host.get("host"),
        "port": int(host.get("port", 22)),
        "username": host.get("user"),
        "timeout": CONNECT_TIMEOUT,
        "banner_timeout": CONNECT_TIMEOUT,
        "auth_timeout": CONNECT_TIMEOUT,
        "allow_agent": True,
        "look_for_keys": True,
    }
    if host.get("password"):
        connect_kwargs["password"] = host["password"]
    if host.get("key_path"):
        connect_kwargs["key_filename"] = host["key_path"]

    try:
        client.connect(**connect_kwargs)
        _stdin, stdout, stderr = client.exec_command(
            cmd, timeout=COMMAND_TIMEOUT
        )
        out_bytes = stdout.read()
        err_bytes = stderr.read()
    finally:
        try:
            client.close()
        except Exception:
            pass

    return (
        out_bytes.decode("utf-8", errors="replace"),
        err_bytes.decode("utf-8", errors="replace"),
    )


# --------------------------------------------------------------------------- #
# Routes
# --------------------------------------------------------------------------- #


@app.screen("/")
def home(_request=None):
    return _home_screen()


@app.screen("/add")
def add_form(_request=None):
    return _add_form_screen()


@app.handler("/add", method="POST")
def add_post(request):
    body = request.body if isinstance(request.body, dict) else {}
    nickname = (body.get("nickname") or "").strip()
    hostname = (body.get("host") or "").strip()
    user = (body.get("user") or "").strip()
    raw_port = body.get("port", 22)
    try:
        port = int(raw_port) if raw_port not in (None, "") else 22
    except (TypeError, ValueError):
        port = 22
    if not nickname or not hostname or not user:
        return _add_form_screen(
            values=body,
            flash="Nickname, host, and user are required.",
        )
    new_host: dict[str, Any] = {
        "nickname": nickname,
        "host": hostname,
        "port": port,
        "user": user,
    }
    pw = (body.get("password") or "").strip()
    if pw:
        new_host["password"] = pw
    kp = (body.get("key_path") or "").strip()
    if kp:
        new_host["key_path"] = kp

    with _store_lock:
        hosts = _load_hosts()
        # Replace any prior entry with the same nickname.
        hosts = [h for h in hosts if h.get("nickname") != nickname]
        hosts.append(new_host)
        _save_hosts(hosts)
    return _home_screen(flash=f"Saved {nickname}.")


@app.screen("/host")
def host_screen(request):
    nickname = _first_q(request, "n")
    if not nickname:
        return _home_screen()
    return _host_screen(nickname)


@app.handler("/run", method="POST")
def run_command(request):
    nickname = _first_q(request, "n")
    if not nickname:
        return _home_screen()
    host = _find_host(nickname)
    if host is None:
        return _missing_screen()
    body = request.body if isinstance(request.body, dict) else {}
    cmd = (body.get("cmd") or "").strip()
    if not cmd:
        return _host_screen(nickname, flash="Enter a command first.")

    # Import + connect + exec are all wrapped here so any failure
    # (missing paramiko, DNS, auth, timeout, ...) becomes a friendly
    # error Screen rather than a 500.
    try:
        stdout, stderr = _run_remote_command(host, cmd)
    except ImportError:
        return _error_screen(
            "paramiko is not installed. Run: pip install paramiko"
        )
    except Exception as exc:  # noqa: BLE001 - dev demo, surface anything
        return _error_screen(f"{type(exc).__name__}: {exc}")

    body_widgets: list[Any] = [
        Text(f"$ {cmd}", style="heading"),
        Text(_trim_output(stdout) or "(no output)", style="mono"),
    ]
    if stderr:
        body_widgets.append(Text(_trim_output(stderr), style="dim"))
    body_widgets.append(
        Button(label="Run again", href=f"/host?n={nickname}", method="GET")
    )
    body_widgets.append(Button(label="Back", href="/", method="GET"))
    return Screen(
        id=f"ssh_run_{nickname}",
        title=nickname,
        body=body_widgets,
        ttl=0,
    )


@app.handler("/delete", method="POST")
def delete_host(request):
    nickname = _first_q(request, "n")
    if not nickname:
        return _home_screen()
    with _store_lock:
        hosts = _load_hosts()
        remaining = [h for h in hosts if h.get("nickname") != nickname]
        if len(remaining) == len(hosts):
            return _home_screen(flash="Host not found.")
        _save_hosts(remaining)
    return _home_screen(flash=f"Deleted {nickname}.")


if __name__ == "__main__":
    app.run()
