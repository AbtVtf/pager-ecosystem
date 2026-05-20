"""Request signature verification (PY-004).

Implements the `PagerOS-Sig` request authentication scheme defined in
SPEC.md §9.2:

    Every request to an App Server includes ``PagerOS-Sig``: an Ed25519
    signature over ``method || url || timestamp || body_hash``. Timestamp
    prevents replay (servers reject > 5 min skew).

Header layout used on the wire:

* ``PagerOS-Device``    – base64url (no padding) of the device Ed25519
                          public key (32 bytes).
* ``PagerOS-Sig``       – base64url (no padding) of the Ed25519 signature
                          (64 bytes).
* ``PagerOS-Timestamp`` – integer Unix seconds, as ASCII (matches the
                          test-vector layout in
                          ``docs/spec/crypto-test-vectors.json``).

The signing input is the byte concatenation, without separators::

    utf8(method) + utf8(url) + utf8(timestamp) + sha256(body)

where ``sha256(body)`` is the raw 32-byte digest of the request body (empty
body hashes the empty string). This matches the
``ed25519-pageros-sig-sample`` vector in the shared test-vector file.

This module deliberately stays framework-agnostic: it exposes the
low-level :func:`verify_request` and a thin WSGI middleware
(:class:`SignatureVerifierMiddleware`). Higher-level integration with the
PY-008 ``ctx`` object plugs in by reading the verified device pubkey from
the WSGI environ key ``"pageros.device_id"``.

Cryptography uses PyNaCl, per ``docs/spec/crypto-suite.md`` (SEC-001) — the
same libsodium binding the firmware links against, so signatures round-trip
byte-for-byte across implementations.
"""

from __future__ import annotations

import base64
import hashlib
import time
from dataclasses import dataclass
from typing import Any, Callable, Iterable, Mapping

from nacl.exceptions import BadSignatureError
from nacl.signing import SigningKey, VerifyKey

HEADER_DEVICE = "PagerOS-Device"
HEADER_SIG = "PagerOS-Sig"
HEADER_TIMESTAMP = "PagerOS-Timestamp"

DEFAULT_MAX_SKEW_SECONDS = 300  # SPEC §9.2: reject > 5 min skew.

ED25519_PUBKEY_LEN = 32
ED25519_SIG_LEN = 64


# --------------------------------------------------------------------------- #
# Errors
# --------------------------------------------------------------------------- #


class SignatureError(ValueError):
    """Base class for all signature-verification failures."""


class MissingHeader(SignatureError):
    """A required PagerOS-* header was missing from the request."""


class InvalidEncoding(SignatureError):
    """A PagerOS-* header could not be parsed (bad base64, bad length, ...)."""


class TimestampSkew(SignatureError):
    """The request timestamp is outside the allowed skew window."""


class BadSignature(SignatureError):
    """The signature did not verify against the device public key."""


# --------------------------------------------------------------------------- #
# Low-level helpers
# --------------------------------------------------------------------------- #


def _b64url_decode(value: str, *, expected_len: int, field: str) -> bytes:
    """Decode a base64url (no-padding) string and assert the byte length."""
    if not isinstance(value, str):
        raise InvalidEncoding(f"{field}: not a string")
    padding = "=" * (-len(value) % 4)
    try:
        raw = base64.urlsafe_b64decode(value + padding)
    except (ValueError, base64.binascii.Error) as exc:  # type: ignore[attr-defined]
        raise InvalidEncoding(f"{field}: not valid base64url") from exc
    if len(raw) != expected_len:
        raise InvalidEncoding(
            f"{field}: expected {expected_len} bytes, got {len(raw)}"
        )
    return raw


def _b64url_encode(raw: bytes) -> str:
    return base64.urlsafe_b64encode(raw).rstrip(b"=").decode("ascii")


def compute_body_hash(body: bytes | None) -> bytes:
    """Return the SHA-256 digest used in the signing input."""
    return hashlib.sha256(body or b"").digest()


def build_signing_input(
    method: str, url: str, timestamp: str | int, body: bytes | None
) -> bytes:
    """Assemble the bytes that get signed.

    Matches the ``msg_assembly`` rule of the
    ``ed25519-pageros-sig-sample`` test vector: concatenate the UTF-8
    encodings of ``method``, ``url`` and ``timestamp`` followed by the raw
    SHA-256 of the body. No separators.
    """
    ts = str(timestamp)
    return (
        method.encode("utf-8")
        + url.encode("utf-8")
        + ts.encode("utf-8")
        + compute_body_hash(body)
    )


def _get_case_insensitive(headers: Mapping[str, str], key: str) -> str | None:
    """Look up a header with case-insensitive comparison.

    The middleware is happy to accept any of ``PagerOS-Device``,
    ``pageros-device``, ``PAGEROS-DEVICE``, etc., because WSGI servers and
    HTTP clients normalize casing inconsistently.
    """
    if key in headers:
        return headers[key]
    lower = key.lower()
    for k, v in headers.items():
        if k.lower() == lower:
            return v
    return None


# --------------------------------------------------------------------------- #
# Verification
# --------------------------------------------------------------------------- #


@dataclass(frozen=True)
class VerifiedRequest:
    """Result of a successful signature check."""

    device_id: bytes
    """Raw 32-byte Ed25519 device public key (the PagerOS device identity)."""

    timestamp: int
    """The Unix-seconds timestamp embedded in the request."""

    @property
    def device_id_b64(self) -> str:
        """Convenience accessor for the base64url-encoded device id."""
        return _b64url_encode(self.device_id)


def verify_request(
    method: str,
    url: str,
    headers: Mapping[str, str],
    body: bytes | None = b"",
    *,
    max_skew_seconds: int = DEFAULT_MAX_SKEW_SECONDS,
    now: Callable[[], float] | None = None,
) -> VerifiedRequest:
    """Verify ``PagerOS-Sig`` against ``headers`` for the given request.

    Raises a :class:`SignatureError` subclass on any failure; on success
    returns a :class:`VerifiedRequest` carrying the verified device pubkey
    and timestamp.

    ``method`` and ``url`` MUST be the same byte-for-byte strings the
    client used when computing the signature. Callers integrating into a
    web framework should pass the raw request path-with-query (e.g.
    ``environ["RAW_URI"]`` for WSGI).
    """
    device_header = _get_case_insensitive(headers, HEADER_DEVICE)
    sig_header = _get_case_insensitive(headers, HEADER_SIG)
    ts_header = _get_case_insensitive(headers, HEADER_TIMESTAMP)

    if device_header is None:
        raise MissingHeader(f"missing {HEADER_DEVICE}")
    if sig_header is None:
        raise MissingHeader(f"missing {HEADER_SIG}")
    if ts_header is None:
        raise MissingHeader(f"missing {HEADER_TIMESTAMP}")

    pubkey = _b64url_decode(
        device_header, expected_len=ED25519_PUBKEY_LEN, field=HEADER_DEVICE
    )
    sig = _b64url_decode(
        sig_header, expected_len=ED25519_SIG_LEN, field=HEADER_SIG
    )

    try:
        ts_int = int(ts_header)
    except ValueError as exc:
        raise InvalidEncoding(
            f"{HEADER_TIMESTAMP}: expected integer Unix seconds, got {ts_header!r}"
        ) from exc

    clock = now if now is not None else time.time
    if max_skew_seconds is not None and max_skew_seconds >= 0:
        current = int(clock())
        if abs(current - ts_int) > max_skew_seconds:
            raise TimestampSkew(
                f"{HEADER_TIMESTAMP}: {ts_int} outside ±{max_skew_seconds}s "
                f"of server time {current}"
            )

    signing_input = build_signing_input(method, url, ts_header, body)
    try:
        VerifyKey(pubkey).verify(signing_input, sig)
    except BadSignatureError as exc:
        raise BadSignature("PagerOS-Sig did not verify") from exc

    return VerifiedRequest(device_id=pubkey, timestamp=ts_int)


# --------------------------------------------------------------------------- #
# Signing helper (clients, tests)
# --------------------------------------------------------------------------- #


def sign_request(
    method: str,
    url: str,
    body: bytes | None,
    signing_key: SigningKey,
    *,
    timestamp: int | None = None,
) -> dict[str, str]:
    """Produce ``PagerOS-*`` headers for the given request.

    Mainly intended for tests and for client code that talks to PagerOS
    app servers (the device firmware constructs its own signatures via
    libsodium). On the wire, the same construction is byte-identical.
    """
    if timestamp is None:
        timestamp = int(time.time())
    signing_input = build_signing_input(method, url, timestamp, body)
    sig = signing_key.sign(signing_input).signature
    pubkey = bytes(signing_key.verify_key)
    return {
        HEADER_DEVICE: _b64url_encode(pubkey),
        HEADER_SIG: _b64url_encode(sig),
        HEADER_TIMESTAMP: str(timestamp),
    }


# --------------------------------------------------------------------------- #
# WSGI middleware
# --------------------------------------------------------------------------- #


WSGIEnviron = dict[str, Any]
StartResponse = Callable[[str, list[tuple[str, str]]], Any]
WSGIApp = Callable[[WSGIEnviron, StartResponse], Iterable[bytes]]

ENVIRON_DEVICE_ID = "pageros.device_id"
"""WSGI environ key set on a successfully verified request (bytes pubkey)."""

ENVIRON_TIMESTAMP = "pageros.signed_timestamp"
"""WSGI environ key carrying the request's signed Unix timestamp (int)."""


def _wsgi_headers(environ: WSGIEnviron) -> dict[str, str]:
    """Pull HTTP headers out of a WSGI environ as a plain mapping."""
    out: dict[str, str] = {}
    for key, value in environ.items():
        if key.startswith("HTTP_"):
            header = key[5:].replace("_", "-").title()
            out[header] = value
    return out


def _wsgi_url(environ: WSGIEnviron) -> str:
    """Reconstruct the path-with-query a client signs over."""
    raw_uri = environ.get("RAW_URI") or environ.get("REQUEST_URI")
    if raw_uri:
        return raw_uri
    path = environ.get("PATH_INFO", "")
    query = environ.get("QUERY_STRING", "")
    return f"{path}?{query}" if query else path


def _read_body(environ: WSGIEnviron) -> bytes:
    """Read and re-buffer the request body so downstream apps can re-read it."""
    stream = environ.get("wsgi.input")
    if stream is None:
        return b""
    try:
        length = int(environ.get("CONTENT_LENGTH") or 0)
    except ValueError:
        length = 0
    body = stream.read(length) if length > 0 else b""
    # Put the bytes back so the wrapped app can still read them.
    import io

    environ["wsgi.input"] = io.BytesIO(body)
    return body


class SignatureVerifierMiddleware:
    """WSGI middleware that enforces PagerOS-Sig on every request.

    On success, the wrapped app sees the verified device pubkey in
    ``environ["pageros.device_id"]`` (bytes) and the signed timestamp in
    ``environ["pageros.signed_timestamp"]`` (int).

    On any failure (missing header, malformed encoding, bad signature, or
    timestamp skew), the middleware short-circuits with ``401
    Unauthorized`` and a plain-text body identifying the error class. The
    wrapped app is never called.
    """

    def __init__(
        self,
        app: WSGIApp,
        *,
        max_skew_seconds: int = DEFAULT_MAX_SKEW_SECONDS,
        now: Callable[[], float] | None = None,
    ) -> None:
        self.app = app
        self.max_skew_seconds = max_skew_seconds
        self.now = now

    def __call__(
        self, environ: WSGIEnviron, start_response: StartResponse
    ) -> Iterable[bytes]:
        method = environ.get("REQUEST_METHOD", "GET")
        url = _wsgi_url(environ)
        headers = _wsgi_headers(environ)
        body = _read_body(environ)

        try:
            verified = verify_request(
                method,
                url,
                headers,
                body,
                max_skew_seconds=self.max_skew_seconds,
                now=self.now,
            )
        except SignatureError as exc:
            return _unauthorized(start_response, exc)

        environ[ENVIRON_DEVICE_ID] = verified.device_id
        environ[ENVIRON_TIMESTAMP] = verified.timestamp
        return self.app(environ, start_response)


def _unauthorized(
    start_response: StartResponse, error: SignatureError
) -> list[bytes]:
    reason = type(error).__name__
    body = f"{reason}: {error}".encode("utf-8")
    start_response(
        "401 Unauthorized",
        [
            ("Content-Type", "text/plain; charset=utf-8"),
            ("Content-Length", str(len(body))),
            ("WWW-Authenticate", 'PagerOS-Sig realm="pageros"'),
        ],
    )
    return [body]


__all__ = [
    "BadSignature",
    "DEFAULT_MAX_SKEW_SECONDS",
    "ED25519_PUBKEY_LEN",
    "ED25519_SIG_LEN",
    "ENVIRON_DEVICE_ID",
    "ENVIRON_TIMESTAMP",
    "HEADER_DEVICE",
    "HEADER_SIG",
    "HEADER_TIMESTAMP",
    "InvalidEncoding",
    "MissingHeader",
    "SignatureError",
    "SignatureVerifierMiddleware",
    "TimestampSkew",
    "VerifiedRequest",
    "build_signing_input",
    "compute_body_hash",
    "sign_request",
    "verify_request",
]
