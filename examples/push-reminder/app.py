"""PagerOS push-reminder example (DOCS-011).

User sets a delay + a short body; the app schedules a push notification
that fires at the requested time and plays a tone on the device.

Demonstrates:

- the form → schedule → push flow end to end,
- the Push Relay client (``send_push``) wired with an ``AppKeypair``
  and Ed25519 signing key (SPEC §6.6, PY-006),
- the ``tone`` payload field that the firmware audio driver maps to a
  bundled PCM clip (SPEC §7.8, FW-032).

Reminders are kept in-memory only — restarting the app loses pending
reminders. A production app would persist them and re-arm on boot.
"""

from __future__ import annotations

import logging
import os
import threading
import time
from dataclasses import dataclass

from nacl.signing import SigningKey  # type: ignore[import-untyped]

from pageros import (
    App,
    AppKeypair,
    Button,
    Form,
    Input,
    List,
    ListItem,
    Notification,
    PushConfig,
    PushError,
    Screen,
    Text,
    send_push,
)


APP_ID = os.environ.get("PAGEROS_REMINDER_APP_ID", "push-reminder.example")
RELAY_URL = os.environ.get("PAGEROS_RELAY_URL", "https://push.pageros.org")


def _load_or_make_signing_key() -> SigningKey:
    raw = os.environ.get("PAGEROS_REMINDER_SIGNING_KEY")
    if raw:
        return SigningKey(bytes.fromhex(raw))
    return SigningKey.generate()


def _load_or_make_keypair() -> AppKeypair:
    raw = os.environ.get("PAGEROS_REMINDER_X25519_PRIVATE")
    if raw:
        return AppKeypair.from_private(bytes.fromhex(raw))
    return AppKeypair.generate()


_signing_key = _load_or_make_signing_key()
_app_keypair = _load_or_make_keypair()
_push_cfg = PushConfig(
    app_id=APP_ID,
    signing_key=_signing_key,
    keypair=_app_keypair,
    relay_url=RELAY_URL,
)
_log = logging.getLogger(__name__)


@dataclass
class Reminder:
    device_id: str
    fire_at: float
    body: str
    tone: str
    counter: int


_lock = threading.Lock()
_pending: dict[str, list[Reminder]] = {}
_counter = 0


def _send_now(reminder: Reminder) -> None:
    payload = {"title": "Reminder", "body": reminder.body, "tone": reminder.tone}
    try:
        res = send_push(_push_cfg, reminder.device_id, payload, counter=reminder.counter)
        _log.info("reminder dispatched: %s", res)
    except PushError as exc:
        _log.warning("reminder failed for %s: %s", reminder.device_id[:8], exc)


def _arm(reminder: Reminder) -> None:
    delay = max(0.0, reminder.fire_at - time.time())

    def _fire() -> None:
        _send_now(reminder)
        with _lock:
            lst = _pending.get(reminder.device_id, [])
            if reminder in lst:
                lst.remove(reminder)
                if not lst:
                    _pending.pop(reminder.device_id, None)

    timer = threading.Timer(delay, _fire)
    timer.daemon = True
    timer.start()


app = App(name="push-reminder")


def _home_screen(device_id: str, flash: tuple[str, str] | None = None) -> Screen:
    with _lock:
        mine = sorted(_pending.get(device_id, []), key=lambda r: r.fire_at)
    body = []
    if flash:
        body.append(Notification(s=flash[0], level=flash[1]))
    if not mine:
        body.append(Text(s="No reminders scheduled.", style="dim"))
    else:
        body.append(List(items=[
            ListItem(
                label=r.body,
                sub=f"in {int(r.fire_at - time.time())}s · tone={r.tone}",
            )
            for r in mine
        ]))
    body.append(Button(label="New reminder", href="/new"))
    return Screen(id="scr_home", title="Reminders", body=body)


def _new_form() -> Screen:
    return Screen(
        id="scr_new",
        title="New reminder",
        body=[
            Form(
                action="/schedule",
                method="POST",
                fields=[
                    Input(name="body", label="Message", max=80),
                    Input(name="delay_s", label="Fire in (seconds)", type="number", value="60"),
                    Input(name="tone", label="Tone", value="default", max=16),
                ],
                submit="Schedule",
            ),
        ],
    )


@app.screen("/")
def home(request):
    return _home_screen(request.ctx.device_id)


@app.screen("/new")
def new_form(request):
    return _new_form()


@app.handler("/schedule", method="POST")
def schedule(request):
    global _counter
    payload = request.body if isinstance(request.body, dict) else {}
    body = (payload.get("body") or "").strip()
    if not body:
        return Screen(
            id="scr_new", title="New reminder",
            body=[Notification(s="Body is required.", level="error"), _new_form().body[0]],
        )
    try:
        delay = max(0, int(payload.get("delay_s") or 0))
    except ValueError:
        delay = 60
    tone = (payload.get("tone") or "default").strip() or "default"
    with _lock:
        _counter += 1
        r = Reminder(
            device_id=request.ctx.device_id,
            fire_at=time.time() + delay,
            body=body[:80],
            tone=tone[:16],
            counter=_counter,
        )
        _pending.setdefault(request.ctx.device_id, []).append(r)
    _arm(r)
    return _home_screen(request.ctx.device_id, flash=(f"Scheduled for {delay}s", "info"))


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    app.run()
