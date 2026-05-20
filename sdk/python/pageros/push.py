"""``app.push(device_id, payload)`` helper (PY-006).

This is the app-side counterpart to the Push Relay handlers in
``push-relay/internal/server/push.go``. It:

1. Canonical-CBOR-encodes the caller's ``payload``.
2. Encrypts it end-to-end against the destination device's X25519
   public key (``X25519+ChaCha20-Poly1305`` per SPEC §6.6.3 /
   ``docs/spec/crypto-suite.md`` §1.1–§1.2).
3. Wraps the encrypted blob in a small CBOR envelope carrying the
   sender's X25519 pubkey and the AEAD nonce — the device needs both
   to decrypt, and the relay sees only these opaque bytes.
4. Signs the resulting HTTPS request with the app's Ed25519 key per
   SPEC §9.2 (same signing input the device uses).
5. POSTs to ``<relay>/push/<device_pubkey>`` and surfaces the relay's
   acknowledgement (HTTP 202 ``{id, enqueued_at}``) or raises a typed
   error for any other response.

The push body wire format is::

    {
      "from":  <app X25519 pubkey, 32 bytes>,
      "nonce": <12-byte AEAD nonce per crypto-suite.md §1.2>,
      "ct":    <ChaCha20-Poly1305 ciphertext including 16-byte tag>,
    }

This mirrors the LoRa inner-envelope idiom from SPEC §6.2.2 (which also
keys on a "from" pubkey + nonce + body) so device-side decoders can
share the same primitives across transports.

Nonces follow crypto-suite.md §1.2 strictly: ``sender_id_byte=0x01``
(app→device) ``|| u64_be(counter) || 0x000000``. Random 96-bit nonces
are explicitly forbidden by the spec, so the caller is responsible for
supplying a monotonic per-(app, device) counter and persisting it
across reboots. The :class:`pageros.App` integration wraps this with a
sensible in-process default for one-shot scripts.
"""

from __future__ import annotations

import base64
import json
import socket
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from typing import Any, Callable

from nacl.signing import SigningKey

from pageros.codec import CborDecodeError, CborEncodeError, decode_frame, encode_frame
from pageros.encryption import (
    AEAD_NONCE_LEN,
    AppKeypair,
    EncryptionError,
    X25519_KEY_LEN,
    build_nonce,
    decrypt,
    derive_session_key,
)
from pageros.signing import (
    HEADER_SIG,
    HEADER_TIMESTAMP,
    build_signing_input,
)

__all__ = [
    "DEFAULT_PUSH_RELAY_URL",
    "HEADER_APP",
    "PUSH_CONTENT_TYPE",
    "PushConfig",
    "PushError",
    "PushRejected",
    "PushResult",
    "PushUnavailable",
    "build_push_body",
    "decode_push_body",
    "send_push",
]

DEFAULT_PUSH_RELAY_URL = "https://push.pageros.org"
HEADER_APP = "PagerOS-App"
PUSH_CONTENT_TYPE = "application/cbor"

_DEFAULT_TIMEOUT = 10.0


# --------------------------------------------------------------------------- #
# Errors
# --------------------------------------------------------------------------- #


class PushError(RuntimeError):
    """Base class for push-helper failures."""


class PushRejected(PushError):
    """The relay returned a 4xx response.

    Carries the HTTP status and the raw response body so callers can
    distinguish auth (401), unknown-app (403), rate-limit (429), etc.
    """

    def __init__(self, status: int, body: bytes = b"") -> None:
        message = _status_text(status)
        super().__init__(f"push relay rejected: HTTP {status} {message}")
        self.status = status
        self.body = body


class PushUnavailable(PushError):
    """The relay was unreachable or returned a 5xx response.

    The caller may retry — at-least-once delivery semantics in SPEC §6.6.5
    permit duplicate enqueues, and the relay assigns the notification id.
    """

    def __init__(
        self,
        message: str,
        *,
        status: int | None = None,
        cause: BaseException | None = None,
    ) -> None:
        super().__init__(message)
        self.status = status
        if cause is not None:
            self.__cause__ = cause


# --------------------------------------------------------------------------- #
# Public value types
# --------------------------------------------------------------------------- #


@dataclass(frozen=True)
class PushResult:
    """Successful relay ack (HTTP 202)."""

    id: str
    """Server-assigned notification id used by the device for dedupe."""

    enqueued_at: int
    """Unix-seconds the relay enqueued the notification at."""


@dataclass(frozen=True)
class PushConfig:
    """Static configuration the helper needs on every push call.

    All fields are required:

    * ``app_id`` — the reverse-DNS id under which the app's Ed25519 pubkey
      is registered in the marketplace (matches ``PagerOS-App`` on the
      wire and the regex enforced by ``push-relay/internal/server/push.go``).
    * ``signing_key`` — the app's Ed25519 ``SigningKey``; used to sign the
      HTTPS request per SPEC §9.2.
    * ``keypair`` — the app's X25519 :class:`pageros.AppKeypair`; used for
      payload encryption against the destination device pubkey.
    * ``relay_url`` — base URL of the Push Relay; defaults to the
      project-operated relay at ``push.pageros.org``.
    """

    app_id: str
    signing_key: SigningKey
    keypair: AppKeypair
    relay_url: str = DEFAULT_PUSH_RELAY_URL


# --------------------------------------------------------------------------- #
# Body framing
# --------------------------------------------------------------------------- #


def _b64url(raw: bytes) -> str:
    return base64.urlsafe_b64encode(raw).rstrip(b"=").decode("ascii")


def _coerce_device_pubkey(device_id: bytes | str) -> tuple[bytes, str]:
    """Normalise the caller's ``device_id`` to (raw 32B, base64url path-form)."""
    if isinstance(device_id, str):
        padding = "=" * (-len(device_id) % 4)
        try:
            raw = base64.urlsafe_b64decode(device_id + padding)
        except (ValueError, base64.binascii.Error) as exc:  # type: ignore[attr-defined]
            raise ValueError(f"device_id: not valid base64url ({exc})") from exc
    elif isinstance(device_id, (bytes, bytearray, memoryview)):
        raw = bytes(device_id)
    else:
        raise TypeError(
            "device_id must be 32 raw key bytes or a base64url string"
        )
    if len(raw) != X25519_KEY_LEN:
        raise ValueError(
            f"device_id must decode to {X25519_KEY_LEN} bytes, got {len(raw)}"
        )
    return raw, _b64url(raw)


def _encode_payload(payload: Any) -> bytes:
    """Encode an SDK-level payload to the bytes that get encrypted.

    ``bytes``-likes pass through unchanged so callers can carry an
    already-CBOR-encoded inner frame (e.g. a group-event envelope built by
    PY-007). Everything else flows through canonical CBOR.
    """
    if isinstance(payload, (bytes, bytearray, memoryview)):
        return bytes(payload)
    try:
        return encode_frame(payload)
    except CborEncodeError as exc:
        raise PushError(f"payload not CBOR-encodable: {exc}") from exc


def build_push_body(
    keypair: AppKeypair,
    device_pubkey: bytes,
    payload: Any,
    *,
    counter: int,
) -> bytes:
    """Build the opaque encrypted body for ``POST /push/<device>``.

    See the module docstring for the on-the-wire envelope shape. The
    relay treats the result as opaque bytes (SPEC §6.6.3); only the
    receiving device's X25519 private key can recover the plaintext.

    ``counter`` MUST be unique per (app keypair, device pubkey) pair to
    avoid AEAD nonce reuse (crypto-suite.md §1.2).
    """
    nonce = build_nonce(0x01, counter)
    plaintext = _encode_payload(payload)
    try:
        ciphertext = keypair.encrypt_to(device_pubkey, plaintext, nonce)
    except EncryptionError as exc:
        raise PushError(f"failed to encrypt push payload: {exc}") from exc
    envelope = {
        "from": keypair.public_key,
        "nonce": nonce,
        "ct": ciphertext,
    }
    return encode_frame(envelope)


def decode_push_body(
    body: bytes,
    *,
    device_private_key: bytes | None = None,
) -> tuple[bytes, bytes, bytes, bytes]:
    """Decode an opaque push body produced by :func:`build_push_body`.

    Returns ``(sender_pubkey, nonce, ciphertext, plaintext)``. When
    ``device_private_key`` is supplied, ``plaintext`` is the decrypted
    bytes; otherwise it is ``b""`` and the caller can decrypt at its own
    layer. Mainly used by tests and by any device-side reference code.
    """
    try:
        envelope = decode_frame(body)
    except CborDecodeError as exc:
        raise PushError(f"push body is not valid CBOR: {exc}") from exc
    if not isinstance(envelope, dict):
        raise PushError("push body is not a CBOR map")
    try:
        sender = envelope["from"]
        nonce = envelope["nonce"]
        ct = envelope["ct"]
    except KeyError as exc:
        raise PushError(f"push body missing field: {exc.args[0]!r}") from exc
    if not isinstance(sender, (bytes, bytearray)) or len(sender) != X25519_KEY_LEN:
        raise PushError("push body 'from' must be a 32-byte pubkey")
    if not isinstance(nonce, (bytes, bytearray)) or len(nonce) != AEAD_NONCE_LEN:
        raise PushError(
            f"push body 'nonce' must be {AEAD_NONCE_LEN} bytes"
        )
    if not isinstance(ct, (bytes, bytearray)):
        raise PushError("push body 'ct' must be bytes")

    sender_b = bytes(sender)
    nonce_b = bytes(nonce)
    ct_b = bytes(ct)
    plaintext = b""
    if device_private_key is not None:
        key = derive_session_key(device_private_key, sender_b)
        plaintext = decrypt(ct_b, key, nonce_b)
    return sender_b, nonce_b, ct_b, plaintext


# --------------------------------------------------------------------------- #
# HTTP
# --------------------------------------------------------------------------- #


def send_push(
    config: PushConfig,
    device_id: bytes | str,
    payload: Any,
    *,
    counter: int,
    timeout: float | None = _DEFAULT_TIMEOUT,
    now: Callable[[], float] | None = None,
    opener: Callable[..., Any] | None = None,
) -> PushResult:
    """Encrypt, sign, and POST a notification to the Push Relay.

    ``counter`` must be unique per (app, device) pair (crypto-suite.md
    §1.2). ``timeout`` is forwarded to :func:`urllib.request.urlopen`.

    ``opener`` is an injection point for tests / advanced HTTP
    configurations — a callable with the same ``(request, timeout=...)``
    signature as :func:`urllib.request.urlopen`.
    """
    device_raw, device_b64 = _coerce_device_pubkey(device_id)
    body = build_push_body(
        config.keypair, device_raw, payload, counter=counter
    )

    path = f"/push/{device_b64}"
    url = config.relay_url.rstrip("/") + path

    clock = now if now is not None else time.time
    ts = str(int(clock()))
    sig_input = build_signing_input("POST", path, ts, body)
    sig = config.signing_key.sign(sig_input).signature

    headers = {
        HEADER_APP: config.app_id,
        HEADER_SIG: _b64url(sig),
        HEADER_TIMESTAMP: ts,
        "Content-Type": PUSH_CONTENT_TYPE,
        "Content-Length": str(len(body)),
    }
    req = urllib.request.Request(url, data=body, method="POST", headers=headers)

    fetch = opener if opener is not None else urllib.request.urlopen
    try:
        with fetch(req, timeout=timeout) as resp:
            status = getattr(resp, "status", None) or resp.getcode()
            resp_body = resp.read()
    except urllib.error.HTTPError as exc:
        status = exc.code
        try:
            resp_body = exc.read()
        except Exception:  # pragma: no cover - paranoid
            resp_body = b""
        if 400 <= status < 500:
            raise PushRejected(status, resp_body) from exc
        raise PushUnavailable(
            f"push relay returned HTTP {status} {_status_text(status)}",
            status=status,
            cause=exc,
        ) from exc
    except (urllib.error.URLError, socket.error, OSError) as exc:
        raise PushUnavailable(
            f"push relay unreachable: {exc}", cause=exc
        ) from exc

    if status != 202:
        raise PushUnavailable(
            f"push relay returned unexpected status {status}",
            status=status,
        )

    try:
        parsed = json.loads(resp_body or b"{}")
    except ValueError as exc:
        raise PushUnavailable(
            f"push relay returned malformed JSON: {exc}", cause=exc
        ) from exc

    note_id = parsed.get("id") if isinstance(parsed, dict) else None
    enqueued = parsed.get("enqueued_at") if isinstance(parsed, dict) else None
    if not isinstance(note_id, str) or not note_id:
        raise PushUnavailable("push relay response missing 'id'")
    if not isinstance(enqueued, (int, float)):
        raise PushUnavailable("push relay response missing 'enqueued_at'")
    return PushResult(id=note_id, enqueued_at=int(enqueued))


# --------------------------------------------------------------------------- #
# Helpers
# --------------------------------------------------------------------------- #


_STATUS_TEXT = {
    400: "bad request",
    401: "unauthorized",
    403: "forbidden",
    404: "not found",
    405: "method not allowed",
    413: "payload too large",
    429: "rate limited",
    500: "internal server error",
    502: "bad gateway",
    503: "service unavailable",
    504: "gateway timeout",
}


def _status_text(status: int) -> str:
    return _STATUS_TEXT.get(status, "error")
