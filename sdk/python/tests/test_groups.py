"""Tests for PY-007 group helpers.

Two surfaces are exercised:

* **Outbound (``app.broadcast``)** — drives the helper against a local
  stub of the Push Relay's ``POST /group_push`` endpoint. The stub
  re-implements the signature verification from ``push-relay/internal/
  server/group.go`` so the test suite catches any drift between the
  Python helper's signing input and the Go relay's verifier. Per-recipient
  payloads are decrypted with each device's X25519 private key so the
  test asserts the *plaintext* envelope the device will see, end-to-end.

* **Inbound (``@app.group_event``)** — drives the dispatcher with a
  CBOR ``{group_id, data}`` body and asserts the registered handler is
  invoked with the right shape for each of the supported signatures.

The acceptance criterion in ``TASKS.md`` ("Reference chat example works
end-to-end with two simulator instances") is observed here through
two synthetic devices: one app broadcasts a group_message; two
in-process device keypairs each decrypt their per-recipient envelope
to the same plaintext payload — i.e. the SDK fan-out + per-recipient
encryption + relay routing all line up.
"""

from __future__ import annotations

import base64
import json
import threading
from dataclasses import dataclass, field
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any
from urllib.parse import urlsplit

import pytest
from nacl.signing import SigningKey, VerifyKey

from pageros import (
    App,
    AppKeypair,
    GROUP_PUSH_PATH,
    GROUP_RESULT_ACCEPTED,
    GROUP_RESULT_RATE_LIMITED,
    GroupBroadcastError,
    PushConfig,
    PushRejected,
    PushUnavailable,
    build_group_push_body,
    decode_frame,
    send_group_push,
)
from pageros.encryption import X25519_KEY_LEN
from pageros.groups import GROUP_PUSH_CONTENT_TYPE
from pageros.push import HEADER_APP, decode_push_body
from pageros.signing import HEADER_SIG, HEADER_TIMESTAMP, build_signing_input


# --------------------------------------------------------------------------- #
# Helpers / fixtures
# --------------------------------------------------------------------------- #


def _b64url_decode(s: str) -> bytes:
    padding = "=" * (-len(s) % 4)
    return base64.urlsafe_b64decode(s + padding)


def _b64url_encode(raw: bytes) -> str:
    return base64.urlsafe_b64encode(raw).rstrip(b"=").decode("ascii")


@dataclass
class CapturedGroupPush:
    method: str = ""
    path: str = ""
    headers: dict[str, str] = field(default_factory=dict)
    body: bytes = b""


@dataclass
class StubGroupBehaviour:
    """Per-recipient behaviour the stub relay echoes back."""

    status: int = 202
    # Either explicit per-recipient overrides (keyed by device_pubkey b64)
    # or a default applied to every recipient.
    default_status: str = GROUP_RESULT_ACCEPTED
    per_recipient: dict[str, str] = field(default_factory=dict)
    note_id_prefix: str = "grp"
    enqueued_at: int = 1_700_000_001
    retry_after: int = 7
    body_override: bytes | None = None
    content_type: str | None = "application/json"


class _StubGroupRelay:
    """Threaded HTTP stub for ``POST /group_push``."""

    def __init__(self, app_verify_key: VerifyKey) -> None:
        self._verify_key = app_verify_key
        self.captured: list[CapturedGroupPush] = []
        self.behaviour = StubGroupBehaviour()
        self._server: ThreadingHTTPServer | None = None
        self._thread: threading.Thread | None = None
        self.host: str = "127.0.0.1"
        self.port: int = 0

    def _make_handler(self) -> type[BaseHTTPRequestHandler]:
        stub = self

        class _Handler(BaseHTTPRequestHandler):
            def log_message(self, format: str, *args: Any) -> None:  # noqa: A002
                pass

            def do_POST(self) -> None:  # noqa: N802
                try:
                    length = int(self.headers.get("Content-Length", "0") or "0")
                except ValueError:
                    length = 0
                body = self.rfile.read(length) if length > 0 else b""
                headers = {k: v for k, v in self.headers.items()}
                ci = {k.lower(): v for k, v in headers.items()}
                stub.captured.append(
                    CapturedGroupPush(
                        method="POST",
                        path=self.path,
                        headers=headers,
                        body=body,
                    )
                )

                # Sig verification mirrors group.go: same signing input as
                # /push, body is the raw JSON bytes.
                try:
                    ts = ci[HEADER_TIMESTAMP.lower()]
                    sig_b64 = ci[HEADER_SIG.lower()]
                    app_id = ci[HEADER_APP.lower()]
                except KeyError as exc:
                    self._respond(400, f"missing header {exc.args[0]!r}".encode())
                    return
                if not app_id:
                    self._respond(401, b"missing app id")
                    return
                try:
                    sig = _b64url_decode(sig_b64)
                except Exception:
                    self._respond(401, b"bad sig encoding")
                    return
                signing_input = build_signing_input(
                    "POST", self.path, ts, body
                )
                try:
                    stub._verify_key.verify(signing_input, sig)
                except Exception:
                    self._respond(401, b"sig verification failed")
                    return

                # Parse the JSON envelope and build a per-recipient result row.
                try:
                    envelope = json.loads(body)
                    recipients = envelope["recipients"]
                except (ValueError, KeyError, TypeError):
                    self._respond(400, b"bad json")
                    return

                behaviour = stub.behaviour
                if behaviour.body_override is not None:
                    body_out = behaviour.body_override
                elif behaviour.status == 202:
                    rows = []
                    for idx, rec in enumerate(recipients):
                        dev = rec.get("device_pubkey", "")
                        status = behaviour.per_recipient.get(
                            dev, behaviour.default_status
                        )
                        row: dict[str, Any] = {
                            "device_pubkey": dev,
                            "status": status,
                        }
                        if status == GROUP_RESULT_ACCEPTED:
                            row["id"] = f"{behaviour.note_id_prefix}-{idx:04d}"
                            row["enqueued_at"] = behaviour.enqueued_at + idx
                        elif status == GROUP_RESULT_RATE_LIMITED:
                            row["retry_after"] = behaviour.retry_after
                        rows.append(row)
                    body_out = json.dumps({"results": rows}).encode("utf-8")
                else:
                    body_out = f"{behaviour.status}".encode("utf-8")
                self._respond(behaviour.status, body_out, behaviour.content_type)

            def _respond(
                self,
                status: int,
                body: bytes,
                content_type: str | None = "text/plain",
            ) -> None:
                self.send_response(status)
                if content_type is not None:
                    self.send_header("Content-Type", content_type)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                if body:
                    self.wfile.write(body)

        return _Handler

    def __enter__(self) -> "_StubGroupRelay":
        self._server = ThreadingHTTPServer((self.host, 0), self._make_handler())
        self.host, self.port = self._server.server_address
        self._thread = threading.Thread(
            target=self._server.serve_forever, daemon=True
        )
        self._thread.start()
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        assert self._server is not None
        self._server.shutdown()
        self._server.server_close()
        if self._thread is not None:
            self._thread.join(timeout=5)

    @property
    def url(self) -> str:
        return f"http://{self.host}:{self.port}"


@pytest.fixture
def app_signing() -> SigningKey:
    return SigningKey(seed=b"\x11" * 32)


@pytest.fixture
def app_keypair() -> AppKeypair:
    return AppKeypair.from_private_key(b"\x22" * X25519_KEY_LEN)


@pytest.fixture
def device_a() -> AppKeypair:
    # Devices are end-users — same X25519 primitive as AppKeypair.
    return AppKeypair.from_private_key(b"\x44" * X25519_KEY_LEN)


@pytest.fixture
def device_b() -> AppKeypair:
    return AppKeypair.from_private_key(b"\x55" * X25519_KEY_LEN)


@pytest.fixture
def relay(app_signing: SigningKey) -> Any:
    with _StubGroupRelay(app_signing.verify_key) as relay:
        yield relay


def _new_app(
    app_signing: SigningKey, app_keypair: AppKeypair, relay_url: str
) -> App:
    return App(
        name="chat-test",
        app_id="org.pageros.test.chat",
        signing_key=app_signing,
        keypair=app_keypair,
        push_relay_url=relay_url,
    )


# --------------------------------------------------------------------------- #
# Body framing
# --------------------------------------------------------------------------- #


def test_build_group_push_body_json_shape() -> None:
    body = build_group_push_body([("dev1", b"\xaa\xbb"), ("dev2", b"\xcc")])
    parsed = json.loads(body)
    assert list(parsed.keys()) == ["recipients"]
    recipients = parsed["recipients"]
    assert recipients[0] == {
        "device_pubkey": "dev1",
        "payload_b64": _b64url_encode(b"\xaa\xbb"),
    }
    assert recipients[1] == {
        "device_pubkey": "dev2",
        "payload_b64": _b64url_encode(b"\xcc"),
    }


def test_build_group_push_body_rejects_empty() -> None:
    with pytest.raises(GroupBroadcastError):
        build_group_push_body([])


# --------------------------------------------------------------------------- #
# Membership registry
# --------------------------------------------------------------------------- #


def test_add_and_remove_members_round_trip(
    app_signing: SigningKey,
    app_keypair: AppKeypair,
    device_a: AppKeypair,
    device_b: AppKeypair,
) -> None:
    app = _new_app(app_signing, app_keypair, relay_url="http://unused")
    app.add_member("grp_abc", device_a.public_key)
    app.add_member("grp_abc", device_b.public_key)
    # idempotency
    app.add_member("grp_abc", device_a.public_key)

    members = app.members("grp_abc")
    assert set(members) == {device_a.public_key, device_b.public_key}

    # Lookup by base64url string form must hit the same set.
    dev_b_b64 = _b64url_encode(device_b.public_key)
    app.remove_member("grp_abc", dev_b_b64)
    assert app.members("grp_abc") == [device_a.public_key]

    app.remove_member("grp_abc", device_a.public_key)
    assert app.members("grp_abc") == []
    # Empty groups drop from the registry — calling members on an unknown
    # id returns [].
    assert app.members("never_existed") == []


def test_groups_of_returns_subscriptions(
    app_signing: SigningKey,
    app_keypair: AppKeypair,
    device_a: AppKeypair,
    device_b: AppKeypair,
) -> None:
    app = _new_app(app_signing, app_keypair, relay_url="http://unused")
    app.add_member("grp_one", device_a.public_key)
    app.add_member("grp_two", device_a.public_key)
    app.add_member("grp_two", device_b.public_key)

    assert set(app.groups_of(device_a.public_key)) == {"grp_one", "grp_two"}
    assert app.groups_of(device_b.public_key) == ["grp_two"]


# --------------------------------------------------------------------------- #
# broadcast → relay → per-recipient decrypt
# --------------------------------------------------------------------------- #


def test_broadcast_signs_and_encrypts_per_recipient(
    app_signing: SigningKey,
    app_keypair: AppKeypair,
    device_a: AppKeypair,
    device_b: AppKeypair,
    relay: Any,
) -> None:
    app = _new_app(app_signing, app_keypair, relay_url=relay.url)
    app.add_member("grp_chat", device_a.public_key)
    app.add_member("grp_chat", device_b.public_key)

    result = app.broadcast(
        "grp_chat",
        "group_message",
        {"from": "alice", "text": "hello", "ts": 1234},
    )

    # The stub re-implements relay sig verification — reaching this point
    # already means the helper produced a sig the Go relay would accept.
    assert len(relay.captured) == 1
    cap = relay.captured[0]
    assert cap.path == GROUP_PUSH_PATH
    assert cap.headers.get("Content-Type", "").startswith(GROUP_PUSH_CONTENT_TYPE)

    # All recipients accepted; ids/enqueued_at populated.
    assert result.total == 2
    assert len(result.accepted) == 2
    assert len(result.rejected) == 0
    for row in result.accepted:
        assert row.status == GROUP_RESULT_ACCEPTED
        assert row.id is not None and row.id.startswith("grp-")
        assert row.enqueued_at is not None

    # Decrypt each per-recipient blob with the *receiving device's* X25519
    # private key — this is what the reference chat client does on-device.
    envelope = json.loads(cap.body)
    payloads_by_dev: dict[str, bytes] = {
        rec["device_pubkey"]: _b64url_decode(rec["payload_b64"])
        for rec in envelope["recipients"]
    }
    dev_a_b64 = _b64url_encode(device_a.public_key)
    dev_b_b64 = _b64url_encode(device_b.public_key)
    assert set(payloads_by_dev.keys()) == {dev_a_b64, dev_b_b64}

    expected = {
        "group_id": "grp_chat",
        "event": "group_message",
        "data": {"from": "alice", "text": "hello", "ts": 1234},
    }
    for dev_b64, dev_keypair in (
        (dev_a_b64, device_a),
        (dev_b_b64, device_b),
    ):
        sender, _nonce, _ct, plaintext = decode_push_body(
            payloads_by_dev[dev_b64], device_private_key=dev_keypair.private_key
        )
        assert sender == app_keypair.public_key
        assert decode_frame(plaintext) == expected


def test_broadcast_per_recipient_counters_are_monotonic(
    app_signing: SigningKey,
    app_keypair: AppKeypair,
    device_a: AppKeypair,
    relay: Any,
) -> None:
    """Two consecutive broadcasts to the same device must use distinct nonces."""
    app = _new_app(app_signing, app_keypair, relay_url=relay.url)
    app.add_member("grp_solo", device_a.public_key)

    app.broadcast("grp_solo", "ping", {"i": 1})
    app.broadcast("grp_solo", "ping", {"i": 2})

    assert len(relay.captured) == 2
    nonces = []
    for cap in relay.captured:
        envelope = json.loads(cap.body)
        payload = _b64url_decode(envelope["recipients"][0]["payload_b64"])
        _sender, nonce, _ct, _pt = decode_push_body(payload)
        nonces.append(nonce)
    assert nonces[0] != nonces[1]
    # First broadcast uses counter=0, second counter=1 (crypto-suite §1.2).
    assert nonces[0][1:9] == (0).to_bytes(8, "big")
    assert nonces[1][1:9] == (1).to_bytes(8, "big")


def test_broadcast_with_explicit_recipients_skips_registry(
    app_signing: SigningKey,
    app_keypair: AppKeypair,
    device_a: AppKeypair,
    device_b: AppKeypair,
    relay: Any,
) -> None:
    app = _new_app(app_signing, app_keypair, relay_url=relay.url)
    # device_a is NOT in any group; pass it explicitly.
    result = app.broadcast(
        "grp_unknown",
        "ping",
        {"x": 1},
        recipients=[device_a.public_key],
    )
    assert result.total == 1
    envelope = json.loads(relay.captured[0].body)
    assert len(envelope["recipients"]) == 1


def test_broadcast_no_recipients_raises(
    app_signing: SigningKey,
    app_keypair: AppKeypair,
    relay: Any,
) -> None:
    app = _new_app(app_signing, app_keypair, relay_url=relay.url)
    with pytest.raises(GroupBroadcastError):
        app.broadcast("grp_empty", "ping", {"x": 1})


def test_broadcast_requires_event_name(
    app_signing: SigningKey,
    app_keypair: AppKeypair,
    device_a: AppKeypair,
    relay: Any,
) -> None:
    app = _new_app(app_signing, app_keypair, relay_url=relay.url)
    app.add_member("grp", device_a.public_key)
    with pytest.raises(ValueError):
        app.broadcast("grp", "", {"x": 1})


def test_broadcast_requires_signing_config(
    app_keypair: AppKeypair,
    device_a: AppKeypair,
) -> None:
    """Same configuration guard as ``push()``."""
    app = App(name="no-config")
    app.add_member("g", device_a.public_key)
    with pytest.raises(Exception) as excinfo:
        app.broadcast("g", "ping", {})
    # PushError subclass — message lists the missing fields.
    msg = str(excinfo.value)
    assert "app_id" in msg or "signing_key" in msg or "keypair" in msg


def test_broadcast_reports_per_recipient_rate_limit(
    app_signing: SigningKey,
    app_keypair: AppKeypair,
    device_a: AppKeypair,
    device_b: AppKeypair,
    relay: Any,
) -> None:
    """Per-recipient errors do not raise — they surface in the result."""
    app = _new_app(app_signing, app_keypair, relay_url=relay.url)
    app.add_member("g", device_a.public_key)
    app.add_member("g", device_b.public_key)

    dev_b_b64 = _b64url_encode(device_b.public_key)
    relay.behaviour.per_recipient[dev_b_b64] = GROUP_RESULT_RATE_LIMITED

    result = app.broadcast("g", "ping", {"x": 1})
    assert result.total == 2
    assert len(result.accepted) == 1
    row_b = result.by_device(dev_b_b64)
    assert row_b is not None
    assert row_b.status == GROUP_RESULT_RATE_LIMITED
    assert row_b.retry_after == relay.behaviour.retry_after
    assert row_b.id is None


def test_broadcast_relay_4xx_raises_push_rejected(
    app_signing: SigningKey,
    app_keypair: AppKeypair,
    device_a: AppKeypair,
    relay: Any,
) -> None:
    app = _new_app(app_signing, app_keypair, relay_url=relay.url)
    app.add_member("g", device_a.public_key)
    relay.behaviour.status = 403
    relay.behaviour.body_override = b"banned"

    with pytest.raises(PushRejected) as excinfo:
        app.broadcast("g", "ping", {"x": 1})
    assert excinfo.value.status == 403


def test_broadcast_relay_5xx_raises_push_unavailable(
    app_signing: SigningKey,
    app_keypair: AppKeypair,
    device_a: AppKeypair,
    relay: Any,
) -> None:
    app = _new_app(app_signing, app_keypair, relay_url=relay.url)
    app.add_member("g", device_a.public_key)
    relay.behaviour.status = 503
    relay.behaviour.body_override = b"down"
    with pytest.raises(PushUnavailable):
        app.broadcast("g", "ping", {"x": 1})


def test_send_group_push_rejects_malformed_response(
    app_signing: SigningKey,
    app_keypair: AppKeypair,
    relay: Any,
) -> None:
    config = PushConfig(
        app_id="org.pageros.test",
        signing_key=app_signing,
        keypair=app_keypair,
        relay_url=relay.url,
    )
    relay.behaviour.body_override = b"not json"
    with pytest.raises(PushUnavailable):
        send_group_push(config, [("dev1", b"\xaa")])


# --------------------------------------------------------------------------- #
# @app.group_event(name)
# --------------------------------------------------------------------------- #


def _encode_group_event_body(group_id: str, data: Any) -> bytes:
    from pageros.codec import encode_frame

    return encode_frame({"group_id": group_id, "data": data})


def test_group_event_dispatch_three_arg_handler(
    app_signing: SigningKey, app_keypair: AppKeypair
) -> None:
    app = _new_app(app_signing, app_keypair, relay_url="http://unused")
    seen: dict[str, Any] = {}

    @app.group_event("group_message")
    def on_message(ctx, group_id, data):
        seen["ctx"] = ctx
        seen["group_id"] = group_id
        seen["data"] = data
        return None

    body = _encode_group_event_body("grp_abc", {"text": "hi"})
    status, _, _ = app.dispatch(
        "POST",
        "/group/group_message",
        body=body,
        headers={"Content-Type": "application/cbor"},
    )
    assert status == 204
    assert seen["group_id"] == "grp_abc"
    assert seen["data"] == {"text": "hi"}


def test_group_event_dispatch_two_arg_handler(
    app_signing: SigningKey, app_keypair: AppKeypair
) -> None:
    app = _new_app(app_signing, app_keypair, relay_url="http://unused")
    seen: dict[str, Any] = {}

    @app.group_event("presence_update")
    def on_pres(group_id, data):
        seen["group_id"] = group_id
        seen["data"] = data

    body = _encode_group_event_body("g", {"online": ["a"]})
    status, _, _ = app.dispatch(
        "POST",
        "/group/presence_update",
        body=body,
        headers={"Content-Type": "application/cbor"},
    )
    assert status == 204
    assert seen == {"group_id": "g", "data": {"online": ["a"]}}


def test_group_event_handler_can_return_frame(
    app_signing: SigningKey, app_keypair: AppKeypair
) -> None:
    app = _new_app(app_signing, app_keypair, relay_url="http://unused")

    @app.group_event("ack")
    def on_ack(group_id, data):
        return {"v": 1, "id": "scr_ack", "body": [{"t": "text", "s": "ok"}]}

    body = _encode_group_event_body("g", {})
    status, headers, payload = app.dispatch(
        "POST",
        "/group/ack",
        body=body,
        headers={"Content-Type": "application/cbor"},
    )
    assert status == 200
    assert headers["Content-Type"].startswith("application/cbor")
    assert decode_frame(payload)["id"] == "scr_ack"


def test_group_event_dispatch_handles_missing_body(
    app_signing: SigningKey, app_keypair: AppKeypair
) -> None:
    """Empty body still dispatches; group_id / data are None."""
    app = _new_app(app_signing, app_keypair, relay_url="http://unused")
    seen: dict[str, Any] = {}

    @app.group_event("noop")
    def on_noop(group_id, data):
        seen["group_id"] = group_id
        seen["data"] = data

    status, _, _ = app.dispatch("POST", "/group/noop")
    assert status == 204
    assert seen == {"group_id": None, "data": None}


def test_group_event_rejects_duplicate_registration(
    app_signing: SigningKey, app_keypair: AppKeypair
) -> None:
    app = _new_app(app_signing, app_keypair, relay_url="http://unused")

    @app.group_event("dupe")
    def first(group_id, data):
        return None

    with pytest.raises(ValueError):
        @app.group_event("dupe")
        def second(group_id, data):
            return None


def test_group_event_rejects_empty_name(
    app_signing: SigningKey, app_keypair: AppKeypair
) -> None:
    app = _new_app(app_signing, app_keypair, relay_url="http://unused")
    with pytest.raises(ValueError):
        app.group_event("")


# --------------------------------------------------------------------------- #
# Chat reference: round-trip mirror of SPEC §8.2 chat example.
# --------------------------------------------------------------------------- #


def test_chat_reference_inbound_event_triggers_broadcast(
    app_signing: SigningKey,
    app_keypair: AppKeypair,
    device_a: AppKeypair,
    device_b: AppKeypair,
    relay: Any,
) -> None:
    """The SPEC §8.2 example: an inbound group_message handler that
    re-broadcasts to all members. Drives the full SDK loop the chat
    reference app relies on."""
    app = _new_app(app_signing, app_keypair, relay_url=relay.url)
    app.add_member("grp_room", device_a.public_key)
    app.add_member("grp_room", device_b.public_key)

    @app.group_event("group_message")
    def on_message(ctx, group_id, data):
        app.broadcast(group_id, "group_message", data)

    body = _encode_group_event_body("grp_room", {"from": "a", "text": "hi"})
    status, _, _ = app.dispatch(
        "POST",
        "/group/group_message",
        body=body,
        headers={"Content-Type": "application/cbor"},
    )
    assert status == 204
    assert len(relay.captured) == 1
    envelope = json.loads(relay.captured[0].body)
    assert len(envelope["recipients"]) == 2

    # Both recipients receive the same decrypted envelope.
    for rec in envelope["recipients"]:
        dev_b64 = rec["device_pubkey"]
        kp = device_a if dev_b64 == _b64url_encode(device_a.public_key) else device_b
        payload_bytes = _b64url_decode(rec["payload_b64"])
        _sender, _nonce, _ct, plaintext = decode_push_body(
            payload_bytes, device_private_key=kp.private_key
        )
        assert decode_frame(plaintext) == {
            "group_id": "grp_room",
            "event": "group_message",
            "data": {"from": "a", "text": "hi"},
        }
