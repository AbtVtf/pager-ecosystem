"""X25519 + ChaCha20-Poly1305 inbound decryption (PY-005).

Implements the device→app encrypted path described in SPEC §6.2.2 (LoRa
inner envelope) and §6.6.3 (Push Relay payloads), keyed to the concrete
choices fixed in ``docs/spec/crypto-suite.md`` (SEC-001):

* X25519 ECDH between the device pubkey and the app's published X25519
  pubkey produces a 32-byte raw shared secret.
* HKDF-SHA256 with a zero 32-byte salt and ``info = "pageros/v1/e2e"``
  derives a 32-byte ChaCha20-Poly1305 key.
* AEAD encryption is ChaCha20-Poly1305 *IETF* (12-byte nonce, 16-byte
  tag). Nonces are 12 bytes per crypto-suite.md §1.2; see below.

The module exposes:

* :class:`AppKeypair` — apps declare their X25519 keypair via
  ``AppKeypair.generate()`` or ``AppKeypair.from_private_key(bytes)``.
* :func:`derive_session_key` — primitive used by the SDK and shared with
  PY-006/PY-007 when those land.
* :func:`encrypt` / :func:`decrypt` — straight AEAD primitives.
* :func:`decrypt_from` — convenience that wires the keypair + sender
  pubkey through HKDF and the AEAD in one call.
* :class:`EncryptedRequestMiddleware` — WSGI middleware that
  transparently decrypts inbound requests when a paired
  ``PagerOS-Sender`` + ``PagerOS-Nonce`` + ``PagerOS-Encrypted: 1`` header
  block is present, replacing ``wsgi.input`` with the plaintext body for
  downstream handlers. Without those headers the middleware is a
  no-op pass-through.

The header layout is the SDK's contract with whatever LoRa Exit Node /
Push Relay code lands the encrypted payload onto the app server. The
inner envelope's payload-framing details (CBOR field names, Ed25519
``sig`` field, etc. — SPEC §6.2.2) are *inside* the plaintext and are
out of scope for this module.
"""

from __future__ import annotations

import base64
import hashlib
import hmac
import io
import os
from dataclasses import dataclass
from typing import Any, Callable, Iterable, Mapping

from nacl.bindings import (
    crypto_aead_chacha20poly1305_ietf_ABYTES,
    crypto_aead_chacha20poly1305_ietf_KEYBYTES,
    crypto_aead_chacha20poly1305_ietf_NPUBBYTES,
    crypto_aead_chacha20poly1305_ietf_decrypt,
    crypto_aead_chacha20poly1305_ietf_encrypt,
    crypto_scalarmult,
    crypto_scalarmult_base,
)
from nacl.exceptions import CryptoError

# --------------------------------------------------------------------------- #
# Constants — pinned to docs/spec/crypto-suite.md (SEC-001)
# --------------------------------------------------------------------------- #

X25519_KEY_LEN = 32
AEAD_KEY_LEN = crypto_aead_chacha20poly1305_ietf_KEYBYTES  # 32
AEAD_NONCE_LEN = crypto_aead_chacha20poly1305_ietf_NPUBBYTES  # 12
AEAD_TAG_LEN = crypto_aead_chacha20poly1305_ietf_ABYTES  # 16

HKDF_INFO = b"pageros/v1/e2e"
HKDF_SALT = b"\x00" * 32  # crypto-suite.md §1.1: "salt = '' (32 zero bytes)"

HEADER_ENCRYPTED = "PagerOS-Encrypted"
HEADER_SENDER = "PagerOS-Sender"
HEADER_NONCE = "PagerOS-Nonce"

ENVIRON_SENDER_ID = "pageros.sender_id"
ENVIRON_NONCE = "pageros.encryption_nonce"


# --------------------------------------------------------------------------- #
# Errors
# --------------------------------------------------------------------------- #


class EncryptionError(ValueError):
    """Base class for crypto failures in this module."""


class DecryptionError(EncryptionError):
    """AEAD authentication failed or ciphertext was malformed."""


class InvalidEncryptionHeader(EncryptionError):
    """A PagerOS-Sender / Nonce / Encrypted header was malformed."""


class MissingEncryptionHeader(EncryptionError):
    """The request was flagged encrypted but a required header was absent."""


# --------------------------------------------------------------------------- #
# Helpers
# --------------------------------------------------------------------------- #


def _b64url_decode(value: str, *, expected_len: int | None, field: str) -> bytes:
    if not isinstance(value, str):
        raise InvalidEncryptionHeader(f"{field}: not a string")
    padding = "=" * (-len(value) % 4)
    try:
        raw = base64.urlsafe_b64decode(value + padding)
    except (ValueError, base64.binascii.Error) as exc:  # type: ignore[attr-defined]
        raise InvalidEncryptionHeader(f"{field}: not valid base64url") from exc
    if expected_len is not None and len(raw) != expected_len:
        raise InvalidEncryptionHeader(
            f"{field}: expected {expected_len} bytes, got {len(raw)}"
        )
    return raw


def _b64url_encode(raw: bytes) -> str:
    return base64.urlsafe_b64encode(raw).rstrip(b"=").decode("ascii")


def _get_ci(headers: Mapping[str, str], key: str) -> str | None:
    if key in headers:
        return headers[key]
    lower = key.lower()
    for k, v in headers.items():
        if k.lower() == lower:
            return v
    return None


# --------------------------------------------------------------------------- #
# HKDF-SHA256 — small inline implementation to avoid a `cryptography` dep
# --------------------------------------------------------------------------- #


def _hkdf_sha256(ikm: bytes, salt: bytes, info: bytes, length: int) -> bytes:
    """Standard HKDF-SHA256 (RFC 5869). Tiny because we only ever need L=32."""
    if length > 32 * 255:
        raise ValueError("HKDF L too large")
    prk = hmac.new(salt, ikm, hashlib.sha256).digest()
    out = b""
    t = b""
    counter = 1
    while len(out) < length:
        t = hmac.new(prk, t + info + bytes([counter]), hashlib.sha256).digest()
        out += t
        counter += 1
    return out[:length]


# --------------------------------------------------------------------------- #
# Session-key derivation
# --------------------------------------------------------------------------- #


def derive_session_key(privkey: bytes, peer_pubkey: bytes) -> bytes:
    """Return the 32-byte AEAD key for a (privkey, peer_pubkey) pair.

    Follows ``crypto-suite.md`` §1.1 exactly:
    ``HKDF-SHA256(ikm=X25519(priv, peer), salt=0x00*32, info="pageros/v1/e2e", L=32)``.
    """
    if len(privkey) != X25519_KEY_LEN:
        raise EncryptionError(
            f"X25519 private key must be {X25519_KEY_LEN} bytes, got {len(privkey)}"
        )
    if len(peer_pubkey) != X25519_KEY_LEN:
        raise EncryptionError(
            f"X25519 peer pubkey must be {X25519_KEY_LEN} bytes, got {len(peer_pubkey)}"
        )
    shared = crypto_scalarmult(privkey, peer_pubkey)
    if shared == b"\x00" * X25519_KEY_LEN:
        # crypto-suite.md §1 footnote: reject all-zero shared secrets (contributory check).
        raise EncryptionError("X25519 yielded an all-zero shared secret")
    return _hkdf_sha256(shared, HKDF_SALT, HKDF_INFO, AEAD_KEY_LEN)


# --------------------------------------------------------------------------- #
# AEAD primitives
# --------------------------------------------------------------------------- #


def encrypt(
    plaintext: bytes, key: bytes, nonce: bytes, aad: bytes = b""
) -> bytes:
    """ChaCha20-Poly1305 IETF encrypt. Returns ciphertext || tag (16 bytes)."""
    if len(key) != AEAD_KEY_LEN:
        raise EncryptionError(
            f"AEAD key must be {AEAD_KEY_LEN} bytes, got {len(key)}"
        )
    if len(nonce) != AEAD_NONCE_LEN:
        raise EncryptionError(
            f"AEAD nonce must be {AEAD_NONCE_LEN} bytes, got {len(nonce)}"
        )
    return crypto_aead_chacha20poly1305_ietf_encrypt(plaintext, aad, nonce, key)


def decrypt(
    ciphertext: bytes, key: bytes, nonce: bytes, aad: bytes = b""
) -> bytes:
    """ChaCha20-Poly1305 IETF decrypt. Raises :class:`DecryptionError` on failure.

    The library returns the plaintext or nothing — no partial output is ever
    returned on auth failure (crypto-suite.md §3 explicitly requires this).
    """
    if len(key) != AEAD_KEY_LEN:
        raise EncryptionError(
            f"AEAD key must be {AEAD_KEY_LEN} bytes, got {len(key)}"
        )
    if len(nonce) != AEAD_NONCE_LEN:
        raise EncryptionError(
            f"AEAD nonce must be {AEAD_NONCE_LEN} bytes, got {len(nonce)}"
        )
    if len(ciphertext) < AEAD_TAG_LEN:
        raise DecryptionError(
            f"ciphertext shorter than {AEAD_TAG_LEN}-byte AEAD tag"
        )
    try:
        return crypto_aead_chacha20poly1305_ietf_decrypt(
            ciphertext, aad, nonce, key
        )
    except CryptoError as exc:
        raise DecryptionError("AEAD authentication failed") from exc


# --------------------------------------------------------------------------- #
# AppKeypair
# --------------------------------------------------------------------------- #


@dataclass(frozen=True)
class AppKeypair:
    """An app's declared X25519 keypair.

    Construct via :meth:`generate` for fresh keys or
    :meth:`from_private_key` to import an existing 32-byte scalar. The
    private key is held as a raw ``bytes`` field — apps that need to
    persist it should store ``keypair.private_key`` somewhere safe (e.g.
    a secrets manager or the OS keyring) and rebuild the keypair on
    boot.
    """

    private_key: bytes
    public_key: bytes

    def __post_init__(self) -> None:
        if len(self.private_key) != X25519_KEY_LEN:
            raise EncryptionError(
                f"X25519 private key must be {X25519_KEY_LEN} bytes, "
                f"got {len(self.private_key)}"
            )
        if len(self.public_key) != X25519_KEY_LEN:
            raise EncryptionError(
                f"X25519 public key must be {X25519_KEY_LEN} bytes, "
                f"got {len(self.public_key)}"
            )

    @classmethod
    def generate(cls) -> "AppKeypair":
        """Generate a fresh X25519 keypair from the OS RNG."""
        priv = os.urandom(X25519_KEY_LEN)
        return cls.from_private_key(priv)

    @classmethod
    def from_private_key(cls, private_key: bytes) -> "AppKeypair":
        """Import a 32-byte X25519 private scalar; derive the pubkey from it."""
        if len(private_key) != X25519_KEY_LEN:
            raise EncryptionError(
                f"X25519 private key must be {X25519_KEY_LEN} bytes, "
                f"got {len(private_key)}"
            )
        pub = crypto_scalarmult_base(private_key)
        return cls(private_key=private_key, public_key=pub)

    @property
    def public_key_b64(self) -> str:
        """The pubkey as base64url-no-padding (manifest-friendly format)."""
        return _b64url_encode(self.public_key)

    def session_key(self, peer_pubkey: bytes) -> bytes:
        """Derive the 32-byte AEAD key for a peer's X25519 pubkey."""
        return derive_session_key(self.private_key, peer_pubkey)

    def decrypt_from(
        self,
        peer_pubkey: bytes,
        ciphertext: bytes,
        nonce: bytes,
        aad: bytes = b"",
    ) -> bytes:
        """Derive the session key and AEAD-decrypt in one call."""
        key = self.session_key(peer_pubkey)
        return decrypt(ciphertext, key, nonce, aad)

    def encrypt_to(
        self,
        peer_pubkey: bytes,
        plaintext: bytes,
        nonce: bytes,
        aad: bytes = b"",
    ) -> bytes:
        """Companion to :meth:`decrypt_from` — used by tests and PY-006/PY-007."""
        key = self.session_key(peer_pubkey)
        return encrypt(plaintext, key, nonce, aad)


# --------------------------------------------------------------------------- #
# WSGI middleware — "transparently decrypts inbound encrypted requests"
# --------------------------------------------------------------------------- #


WSGIEnviron = dict[str, Any]
StartResponse = Callable[[str, list[tuple[str, str]]], Any]
WSGIApp = Callable[[WSGIEnviron, StartResponse], Iterable[bytes]]


def _wsgi_headers(environ: WSGIEnviron) -> dict[str, str]:
    out: dict[str, str] = {}
    for key, value in environ.items():
        if key.startswith("HTTP_"):
            header = key[5:].replace("_", "-").title()
            out[header] = value
    return out


def _read_body(environ: WSGIEnviron) -> bytes:
    stream = environ.get("wsgi.input")
    if stream is None:
        return b""
    try:
        length = int(environ.get("CONTENT_LENGTH") or 0)
    except ValueError:
        length = 0
    return stream.read(length) if length > 0 else b""


class EncryptedRequestMiddleware:
    """WSGI middleware that transparently decrypts inbound encrypted requests.

    When all three of ``PagerOS-Encrypted: 1``, ``PagerOS-Sender`` (32-byte
    X25519 sender pubkey, base64url) and ``PagerOS-Nonce`` (12-byte AEAD
    nonce, base64url) are present, the middleware:

    1. Reads the full request body (the ciphertext).
    2. Derives the session key from the app keypair + sender pubkey per
       crypto-suite.md §1.1.
    3. AEAD-decrypts the body. On failure, short-circuits with
       ``401 Unauthorized``.
    4. Replaces ``wsgi.input`` with the plaintext bytes, rewrites
       ``CONTENT_LENGTH``, and stashes the sender pubkey in
       ``environ["pageros.sender_id"]``.

    When ``PagerOS-Encrypted`` is absent or false-y, the middleware passes
    the request through untouched — apps that mix cleartext and encrypted
    traffic (e.g., dev environments) work without code changes.

    A request that says ``PagerOS-Encrypted: 1`` but is missing one of the
    auxiliary headers is rejected with ``400 Bad Request`` — better to
    fail loudly than silently treat it as cleartext.
    """

    def __init__(self, app: WSGIApp, keypair: AppKeypair) -> None:
        self.app = app
        self.keypair = keypair

    def __call__(
        self, environ: WSGIEnviron, start_response: StartResponse
    ) -> Iterable[bytes]:
        headers = _wsgi_headers(environ)
        flag = _get_ci(headers, HEADER_ENCRYPTED)
        if flag is None or flag.strip().lower() in {"", "0", "false", "no"}:
            return self.app(environ, start_response)

        sender_b64 = _get_ci(headers, HEADER_SENDER)
        nonce_b64 = _get_ci(headers, HEADER_NONCE)
        try:
            if sender_b64 is None:
                raise MissingEncryptionHeader(f"missing {HEADER_SENDER}")
            if nonce_b64 is None:
                raise MissingEncryptionHeader(f"missing {HEADER_NONCE}")
            sender = _b64url_decode(
                sender_b64, expected_len=X25519_KEY_LEN, field=HEADER_SENDER
            )
            nonce = _b64url_decode(
                nonce_b64, expected_len=AEAD_NONCE_LEN, field=HEADER_NONCE
            )
        except EncryptionError as exc:
            return _bad_request(start_response, exc)

        ciphertext = _read_body(environ)
        try:
            plaintext = self.keypair.decrypt_from(sender, ciphertext, nonce)
        except EncryptionError as exc:
            return _unauthorized(start_response, exc)

        environ["wsgi.input"] = io.BytesIO(plaintext)
        environ["CONTENT_LENGTH"] = str(len(plaintext))
        environ[ENVIRON_SENDER_ID] = sender
        environ[ENVIRON_NONCE] = nonce
        return self.app(environ, start_response)


def _unauthorized(
    start_response: StartResponse, error: EncryptionError
) -> list[bytes]:
    reason = type(error).__name__
    body = f"{reason}: {error}".encode("utf-8")
    start_response(
        "401 Unauthorized",
        [
            ("Content-Type", "text/plain; charset=utf-8"),
            ("Content-Length", str(len(body))),
        ],
    )
    return [body]


def _bad_request(
    start_response: StartResponse, error: EncryptionError
) -> list[bytes]:
    reason = type(error).__name__
    body = f"{reason}: {error}".encode("utf-8")
    start_response(
        "400 Bad Request",
        [
            ("Content-Type", "text/plain; charset=utf-8"),
            ("Content-Length", str(len(body))),
        ],
    )
    return [body]


# --------------------------------------------------------------------------- #
# Spec-pinned nonce helper (crypto-suite.md §1.2)
# --------------------------------------------------------------------------- #


def build_nonce(sender_id_byte: int, counter: int) -> bytes:
    """Build a 12-byte AEAD nonce per crypto-suite.md §1.2.

    Layout: ``sender_id_byte || u64_be(counter) || 3 zero bytes``.

    ``sender_id_byte``: ``0x01`` = app→device, ``0x02`` = device→app. Callers
    are responsible for choosing the right value for their direction.
    """
    if not 0 <= sender_id_byte <= 0xFF:
        raise EncryptionError(
            f"sender_id_byte must fit in a byte, got {sender_id_byte}"
        )
    if not 0 <= counter < (1 << 64):
        raise EncryptionError(f"counter must fit in u64, got {counter}")
    return bytes([sender_id_byte]) + counter.to_bytes(8, "big") + b"\x00\x00\x00"


__all__ = [
    "AEAD_KEY_LEN",
    "AEAD_NONCE_LEN",
    "AEAD_TAG_LEN",
    "AppKeypair",
    "DecryptionError",
    "ENVIRON_NONCE",
    "ENVIRON_SENDER_ID",
    "EncryptedRequestMiddleware",
    "EncryptionError",
    "HEADER_ENCRYPTED",
    "HEADER_NONCE",
    "HEADER_SENDER",
    "HKDF_INFO",
    "HKDF_SALT",
    "InvalidEncryptionHeader",
    "MissingEncryptionHeader",
    "X25519_KEY_LEN",
    "build_nonce",
    "decrypt",
    "derive_session_key",
    "encrypt",
]
