"""Tests for the PY-006 ``app.push(device_id, payload)`` helper.

The acceptance criterion in ``TASKS.md`` is:

    Signs and encrypts payload, POSTs to Push Relay, returns 2xx or
    raises with relay error.

The tests below cover the three slices that need to be right:

* **Body framing** — ``build_push_body`` / ``decode_push_body`` round-trip
  through canonical CBOR and ChaCha20-Poly1305, and the nonce layout
  matches ``crypto-suite.md`` §1.2 (sender_id=0x01, u64 counter).
* **HTTP behaviour** — drive ``app.push()`` against a local stub of the
  Push Relay running on a real socket. The stub re-implements the
  signature verification from ``push-relay/internal/server/push.go``
  (Ed25519 over ``method || url || timestamp || sha256(body)``) so we
  catch any divergence in the signing input layout between the Go relay
  and the Python helper.
* **Error mapping** — 401/403/413/429 → :class:`PushRejected`; 503 and
  connection failures → :class:`PushUnavailable`; missing config →
  :class:`PushError` with a clear message.
"""

from __future__ import annotations

import base64
import hashlib
import json
import socket
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
    PushConfig,
    PushError,
    PushRejected,
    PushResult,
    PushUnavailable,
    build_push_body,
    decode_push_body,
    send_push,
)
from pageros.codec import decode_frame, encode_frame
from pageros.encryption import AEAD_NONCE_LEN, X25519_KEY_LEN, build_nonce
from pageros.push import HEADER_APP, PUSH_CONTENT_TYPE
from pageros.signing import HEADER_SIG, HEADER_TIMESTAMP, build_signing_input


# --------------------------------------------------------------------------- #
# Helpers
# --------------------------------------------------------------------------- #


def _b64url_decode(s: str) -> bytes:
    padding = "=" * (-len(s) % 4)
    return base64.urlsafe_b64decode(s + padding)


def _b64url_encode(raw: bytes) -> str:
    return base64.urlsafe_b64encode(raw).rstrip(b"=").decode("ascii")


@dataclass
class CapturedPush:
    """Everything the stub relay observed on the last POST."""

    method: str = ""
    path: str = ""
    headers: dict[str, str] = field(default_factory=dict)
    body: bytes = b""
    device_path: str = ""


@dataclass
class StubBehaviour:
    """How the stub relay should respond to the next request."""

    status: int = 202
    note_id: str = "deadbeef0000000000000000deadbeef"
    enqueued_at: int = 1_700_000_000
    body_override: bytes | None = None
    content_type: str | None = "application/json"
    delay_seconds: float = 0.0


class _StubPushRelay:
    """Threaded HTTP stub that mimics ``POST /push/{device_pubkey}``.

    The stub re-implements the Go relay's signature check so the test
    suite catches any drift between the helper's signing input and the
    relay's verifier. The app pubkey is configured up front (the real
    relay reads it from the marketplace registry — that path is covered
    by push-relay tests).
    """

    def __init__(self, app_verify_key: VerifyKey) -> None:
        self._verify_key = app_verify_key
        self.captured: list[CapturedPush] = []
        self.behaviour = StubBehaviour()
        self._server: ThreadingHTTPServer | None = None
        self._thread: threading.Thread | None = None
        self.host: str = "127.0.0.1"
        self.port: int = 0

    # The handler class is built dynamically so it can close over `self`
    # without leaking state across multiple stub instances in a single test
    # run.
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
                # HTTP header names are case-insensitive (RFC 7230 §3.2); the
                # captured record preserves the raw casing, but verification
                # works off a lowercased lookup map so it does not care
                # whether urllib title-cases or lowercases on the wire.
                headers = {k: v for k, v in self.headers.items()}
                ci = {k.lower(): v for k, v in headers.items()}

                split = urlsplit(self.path)
                device_path = split.path.removeprefix("/push/")

                stub.captured.append(
                    CapturedPush(
                        method="POST",
                        path=self.path,
                        headers=headers,
                        body=body,
                        device_path=device_path,
                    )
                )

                # Verify the SPEC §9.2 signature so we catch any signing-input
                # divergence between the helper and the Go relay.
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

                behaviour = stub.behaviour
                if behaviour.delay_seconds > 0:
                    import time as _t

                    _t.sleep(behaviour.delay_seconds)

                if behaviour.body_override is not None:
                    body_out = behaviour.body_override
                elif behaviour.status == 202:
                    body_out = json.dumps(
                        {
                            "id": behaviour.note_id,
                            "enqueued_at": behaviour.enqueued_at,
                        }
                    ).encode("utf-8")
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

    def __enter__(self) -> "_StubPushRelay":
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
    # Deterministic seed = nicer failure logs across re-runs.
    return SigningKey(seed=b"\x11" * 32)


@pytest.fixture
def app_keypair() -> AppKeypair:
    return AppKeypair.from_private_key(b"\x22" * X25519_KEY_LEN)


@pytest.fixture
def device_keypair() -> AppKeypair:
    return AppKeypair.from_private_key(b"\x33" * X25519_KEY_LEN)


@pytest.fixture
def relay(app_signing: SigningKey) -> Any:
    with _StubPushRelay(app_signing.verify_key) as relay:
        yield relay


# --------------------------------------------------------------------------- #
# Body framing
# --------------------------------------------------------------------------- #


def test_build_push_body_roundtrip(
    app_keypair: AppKeypair, device_keypair: AppKeypair
) -> None:
    payload = {"title": "Reminder", "body": "Buy milk"}
    body = build_push_body(
        app_keypair, device_keypair.public_key, payload, counter=0
    )

    sender, nonce, ciphertext, plaintext = decode_push_body(
        body, device_private_key=device_keypair.private_key
    )
    assert sender == app_keypair.public_key
    assert nonce == build_nonce(0x01, 0)
    assert len(nonce) == AEAD_NONCE_LEN
    assert decode_frame(plaintext) == payload
    # ``ct`` includes the 16-byte Poly1305 tag.
    assert len(ciphertext) == len(encode_frame(payload)) + 16


def test_build_push_body_accepts_raw_bytes_payload(
    app_keypair: AppKeypair, device_keypair: AppKeypair
) -> None:
    raw = b"already-encoded-inner-envelope"
    body = build_push_body(
        app_keypair, device_keypair.public_key, raw, counter=7
    )
    sender, nonce, _, plaintext = decode_push_body(
        body, device_private_key=device_keypair.private_key
    )
    assert sender == app_keypair.public_key
    assert nonce == build_nonce(0x01, 7)
    assert plaintext == raw


def test_build_push_body_uses_counter_nonce(
    app_keypair: AppKeypair, device_keypair: AppKeypair
) -> None:
    """crypto-suite.md §1.2: nonce = 0x01 || u64_be(counter) || 000000."""
    body = build_push_body(
        app_keypair, device_keypair.public_key, {"x": 1}, counter=42
    )
    _, nonce, _, _ = decode_push_body(body)
    assert nonce[0] == 0x01
    assert nonce[1:9] == (42).to_bytes(8, "big")
    assert nonce[9:] == b"\x00\x00\x00"


def test_decode_push_body_rejects_garbage() -> None:
    with pytest.raises(PushError):
        decode_push_body(b"\xff\xff not-cbor")


def test_decode_push_body_rejects_missing_fields(
    app_keypair: AppKeypair,
) -> None:
    half = encode_frame({"from": app_keypair.public_key, "nonce": b"\x00" * AEAD_NONCE_LEN})
    with pytest.raises(PushError):
        decode_push_body(half)


def test_decode_push_body_rejects_wrong_lengths(
    app_keypair: AppKeypair,
) -> None:
    broken = encode_frame(
        {"from": b"\x00" * 16, "nonce": b"\x00" * AEAD_NONCE_LEN, "ct": b"x"}
    )
    with pytest.raises(PushError):
        decode_push_body(broken)


# --------------------------------------------------------------------------- #
# HTTP — happy path
# --------------------------------------------------------------------------- #


def test_send_push_happy_path(
    app_signing: SigningKey,
    app_keypair: AppKeypair,
    device_keypair: AppKeypair,
    relay: _StubPushRelay,
) -> None:
    config = PushConfig(
        app_id="notes.mafu.dev",
        signing_key=app_signing,
        keypair=app_keypair,
        relay_url=relay.url,
    )
    relay.behaviour = StubBehaviour(
        status=202, note_id="abc123", enqueued_at=1_700_000_001
    )

    result = send_push(
        config,
        device_keypair.public_key,
        {"title": "Hi", "body": "Hello!"},
        counter=0,
        now=lambda: 1_700_000_000,
    )

    assert isinstance(result, PushResult)
    assert result.id == "abc123"
    assert result.enqueued_at == 1_700_000_001

    # The stub got exactly one request with the right shape.
    assert len(relay.captured) == 1
    captured = relay.captured[0]
    assert captured.method == "POST"
    captured_ci = {k.lower(): v for k, v in captured.headers.items()}
    assert captured_ci[HEADER_APP.lower()] == "notes.mafu.dev"
    assert captured_ci["content-type"] == PUSH_CONTENT_TYPE
    assert captured_ci[HEADER_TIMESTAMP.lower()] == "1700000000"
    # Path = /push/<base64url(device pubkey)>
    expected_path = "/push/" + _b64url_encode(device_keypair.public_key)
    assert captured.path == expected_path
    assert captured.device_path == _b64url_encode(device_keypair.public_key)

    # Body decrypts back to the original payload.
    _, _, _, plaintext = decode_push_body(
        captured.body, device_private_key=device_keypair.private_key
    )
    assert decode_frame(plaintext) == {"title": "Hi", "body": "Hello!"}


def test_send_push_accepts_b64_device_id(
    app_signing: SigningKey,
    app_keypair: AppKeypair,
    device_keypair: AppKeypair,
    relay: _StubPushRelay,
) -> None:
    config = PushConfig(
        app_id="notes.mafu.dev",
        signing_key=app_signing,
        keypair=app_keypair,
        relay_url=relay.url,
    )
    device_b64 = _b64url_encode(device_keypair.public_key)
    result = send_push(
        config, device_b64, b"payload-bytes", counter=1, now=lambda: 1_700_000_000
    )
    assert isinstance(result, PushResult)
    captured = relay.captured[-1]
    assert captured.device_path == device_b64


def test_send_push_rejects_bad_device_id(
    app_signing: SigningKey, app_keypair: AppKeypair
) -> None:
    config = PushConfig(
        app_id="x.example.dev",
        signing_key=app_signing,
        keypair=app_keypair,
        relay_url="http://127.0.0.1:1",  # unused — should fail first
    )
    with pytest.raises(ValueError, match="must decode to 32 bytes"):
        send_push(config, b"too-short", b"x", counter=0)
    with pytest.raises(ValueError, match="must decode to 32 bytes"):
        send_push(config, "QUJD", b"x", counter=0)  # decodes to 3 bytes
    with pytest.raises(TypeError):
        send_push(config, 12345, b"x", counter=0)  # type: ignore[arg-type]


def test_send_push_signature_includes_full_body(
    app_signing: SigningKey,
    app_keypair: AppKeypair,
    device_keypair: AppKeypair,
    relay: _StubPushRelay,
) -> None:
    """If the signing input drifts (e.g. the helper hashes the
    *plaintext* instead of the wire body), the stub's verify would
    fail. This test pins that the helper signs the same bytes the
    relay will sha256."""
    config = PushConfig(
        app_id="x.example.dev",
        signing_key=app_signing,
        keypair=app_keypair,
        relay_url=relay.url,
    )
    result = send_push(
        config,
        device_keypair.public_key,
        {"hello": "world"},
        counter=5,
        now=lambda: 1_700_000_000,
    )
    assert isinstance(result, PushResult)
    captured = relay.captured[-1]
    body_hash = hashlib.sha256(captured.body).digest()
    signing_input = (
        b"POST"
        + captured.path.encode("utf-8")
        + b"1700000000"
        + body_hash
    )
    captured_ci = {k.lower(): v for k, v in captured.headers.items()}
    sig = _b64url_decode(captured_ci[HEADER_SIG.lower()])
    app_signing.verify_key.verify(signing_input, sig)


# --------------------------------------------------------------------------- #
# HTTP — error mapping
# --------------------------------------------------------------------------- #


@pytest.mark.parametrize("status", [400, 401, 403, 404, 413, 429])
def test_send_push_4xx_raises_rejected(
    status: int,
    app_signing: SigningKey,
    app_keypair: AppKeypair,
    device_keypair: AppKeypair,
    relay: _StubPushRelay,
) -> None:
    relay.behaviour = StubBehaviour(
        status=status, body_override=b"detail"
    )
    config = PushConfig(
        app_id="x.example.dev",
        signing_key=app_signing,
        keypair=app_keypair,
        relay_url=relay.url,
    )
    with pytest.raises(PushRejected) as excinfo:
        send_push(
            config,
            device_keypair.public_key,
            b"x",
            counter=0,
            now=lambda: 1_700_000_000,
        )
    assert excinfo.value.status == status
    assert excinfo.value.body == b"detail"


@pytest.mark.parametrize("status", [500, 502, 503, 504])
def test_send_push_5xx_raises_unavailable(
    status: int,
    app_signing: SigningKey,
    app_keypair: AppKeypair,
    device_keypair: AppKeypair,
    relay: _StubPushRelay,
) -> None:
    relay.behaviour = StubBehaviour(status=status)
    config = PushConfig(
        app_id="x.example.dev",
        signing_key=app_signing,
        keypair=app_keypair,
        relay_url=relay.url,
    )
    with pytest.raises(PushUnavailable) as excinfo:
        send_push(
            config,
            device_keypair.public_key,
            b"x",
            counter=0,
            now=lambda: 1_700_000_000,
        )
    assert excinfo.value.status == status


def test_send_push_connection_refused_raises_unavailable(
    app_signing: SigningKey,
    app_keypair: AppKeypair,
    device_keypair: AppKeypair,
) -> None:
    # Bind a socket just to grab a free port, then close so the next
    # connect attempt refuses.
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    config = PushConfig(
        app_id="x.example.dev",
        signing_key=app_signing,
        keypair=app_keypair,
        relay_url=f"http://127.0.0.1:{port}",
    )
    with pytest.raises(PushUnavailable):
        send_push(
            config,
            device_keypair.public_key,
            b"x",
            counter=0,
            now=lambda: 1_700_000_000,
            timeout=1.0,
        )


def test_send_push_unexpected_2xx_raises_unavailable(
    app_signing: SigningKey,
    app_keypair: AppKeypair,
    device_keypair: AppKeypair,
    relay: _StubPushRelay,
) -> None:
    relay.behaviour = StubBehaviour(status=200, body_override=b"{}")
    config = PushConfig(
        app_id="x.example.dev",
        signing_key=app_signing,
        keypair=app_keypair,
        relay_url=relay.url,
    )
    with pytest.raises(PushUnavailable):
        send_push(
            config,
            device_keypair.public_key,
            b"x",
            counter=0,
            now=lambda: 1_700_000_000,
        )


def test_send_push_malformed_json_raises_unavailable(
    app_signing: SigningKey,
    app_keypair: AppKeypair,
    device_keypair: AppKeypair,
    relay: _StubPushRelay,
) -> None:
    relay.behaviour = StubBehaviour(status=202, body_override=b"{not-json")
    config = PushConfig(
        app_id="x.example.dev",
        signing_key=app_signing,
        keypair=app_keypair,
        relay_url=relay.url,
    )
    with pytest.raises(PushUnavailable):
        send_push(
            config,
            device_keypair.public_key,
            b"x",
            counter=0,
            now=lambda: 1_700_000_000,
        )


# --------------------------------------------------------------------------- #
# App integration
# --------------------------------------------------------------------------- #


def test_app_push_missing_config_raises_clear_error() -> None:
    app = App(name="empty")
    with pytest.raises(PushError, match="app_id, signing_key, keypair"):
        app.push(b"\x33" * X25519_KEY_LEN, {"x": 1})


def test_app_push_auto_increments_counter(
    app_signing: SigningKey,
    app_keypair: AppKeypair,
    device_keypair: AppKeypair,
    relay: _StubPushRelay,
) -> None:
    app = App(
        name="notes",
        app_id="notes.mafu.dev",
        signing_key=app_signing,
        keypair=app_keypair,
        push_relay_url=relay.url,
    )
    relay.behaviour = StubBehaviour(status=202, note_id="n1")
    app.push(device_keypair.public_key, {"i": 0})
    relay.behaviour = StubBehaviour(status=202, note_id="n2")
    app.push(device_keypair.public_key, {"i": 1})

    nonces = [
        decode_push_body(c.body)[1] for c in relay.captured
    ]
    assert nonces[0] == build_nonce(0x01, 0)
    assert nonces[1] == build_nonce(0x01, 1)


def test_app_push_returns_relay_result(
    app_signing: SigningKey,
    app_keypair: AppKeypair,
    device_keypair: AppKeypair,
    relay: _StubPushRelay,
) -> None:
    app = App(
        name="notes",
        app_id="notes.mafu.dev",
        signing_key=app_signing,
        keypair=app_keypair,
        push_relay_url=relay.url,
    )
    relay.behaviour = StubBehaviour(
        status=202, note_id="result-id", enqueued_at=1_700_000_010
    )
    result = app.push(device_keypair.public_key, {"v": 1, "title": "hi"})
    assert result == PushResult(id="result-id", enqueued_at=1_700_000_010)


def test_app_push_passes_relay_errors_through(
    app_signing: SigningKey,
    app_keypair: AppKeypair,
    device_keypair: AppKeypair,
    relay: _StubPushRelay,
) -> None:
    app = App(
        name="notes",
        app_id="notes.mafu.dev",
        signing_key=app_signing,
        keypair=app_keypair,
        push_relay_url=relay.url,
    )
    relay.behaviour = StubBehaviour(status=429, body_override=b"slow down")
    with pytest.raises(PushRejected) as excinfo:
        app.push(device_keypair.public_key, {"x": 1})
    assert excinfo.value.status == 429
