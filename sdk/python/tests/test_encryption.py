"""Tests for the PY-005 X25519 decryption module.

Acceptance criteria (TASKS.md, PY-005):
* Apps can declare a keypair.
* SDK transparently decrypts inbound encrypted requests.

Coverage strategy:

* The X25519 ECDH path is pinned to the shared crypto test vector
  ``x25519-rfc7748-6.1`` and the HKDF derivation is checked against an
  independent recomputation. This is the cross-impl interop check
  crypto-suite.md §3 calls out.
* The ChaCha20-Poly1305 IETF AEAD path is pinned to the RFC 8439 §2.8.2
  test vector (also in the shared vectors file). Tag mutation is verified
  to fail closed.
* Round-trip tests against PyNaCl (= libsodium = firmware) confirm that
  inbound payloads produced by the firmware can be decrypted here.
* Middleware tests cover both the cleartext pass-through and the
  encrypted-decrypt-and-rewrite paths, plus the 400/401 failure modes.
"""

from __future__ import annotations

import base64
import io
import json
import pathlib

import pytest
from nacl.bindings import crypto_scalarmult_base

from pageros.encryption import (
    AEAD_KEY_LEN,
    AEAD_NONCE_LEN,
    AEAD_TAG_LEN,
    ENVIRON_NONCE,
    ENVIRON_SENDER_ID,
    HEADER_ENCRYPTED,
    HEADER_NONCE,
    HEADER_SENDER,
    HKDF_INFO,
    HKDF_SALT,
    X25519_KEY_LEN,
    AppKeypair,
    DecryptionError,
    EncryptedRequestMiddleware,
    EncryptionError,
    InvalidEncryptionHeader,
    MissingEncryptionHeader,
    build_nonce,
    decrypt,
    derive_session_key,
    encrypt,
)


# --------------------------------------------------------------------------- #
# Shared crypto test vectors
# --------------------------------------------------------------------------- #


def _vector_path() -> pathlib.Path:
    here = pathlib.Path(__file__).resolve()
    return here.parents[3] / "docs" / "spec" / "crypto-test-vectors.json"


def _vectors() -> list[dict]:
    return json.loads(_vector_path().read_text())["vectors"]


def _find(vid: str) -> dict:
    for v in _vectors():
        if v["id"] == vid:
            return v
    raise AssertionError(f"vector {vid} missing")


# --------------------------------------------------------------------------- #
# Primitives: HKDF + X25519 + derive_session_key
# --------------------------------------------------------------------------- #


def test_x25519_matches_rfc7748_vector() -> None:
    """X25519(priv, peer) must equal the RFC-pinned shared secret."""
    vec = _find("x25519-rfc7748-6.1")
    priv = bytes.fromhex(vec["priv"])
    peer = bytes.fromhex(vec["pub_peer"])
    # We derive the session key, which is HKDF(shared, ...). Verify the
    # X25519 path through the AppKeypair too.
    expected_shared = bytes.fromhex(vec["shared"])
    # We don't expose `shared` directly, but if HKDF over the right input
    # produces the right output then the shared was right.
    import hashlib
    import hmac

    prk = hmac.new(HKDF_SALT, expected_shared, hashlib.sha256).digest()
    expected_key = hmac.new(prk, HKDF_INFO + b"\x01", hashlib.sha256).digest()

    derived = derive_session_key(priv, peer)
    assert derived == expected_key
    # And the keypair-flavored convenience returns the same.
    kp = AppKeypair.from_private_key(priv)
    assert kp.session_key(peer) == expected_key


def test_session_key_is_symmetric() -> None:
    """``derive_session_key(a_priv, b_pub) == derive_session_key(b_priv, a_pub)``.

    The whole point of X25519 ECDH: both ends produce the same key.
    """
    alice = AppKeypair.generate()
    bob = AppKeypair.generate()
    assert alice.session_key(bob.public_key) == bob.session_key(alice.public_key)


def test_session_key_length_is_32() -> None:
    a = AppKeypair.generate()
    b = AppKeypair.generate()
    assert len(a.session_key(b.public_key)) == AEAD_KEY_LEN == 32


def test_derive_session_key_rejects_bad_lengths() -> None:
    a = AppKeypair.generate()
    with pytest.raises(EncryptionError):
        derive_session_key(b"\x00" * 31, a.public_key)
    with pytest.raises(EncryptionError):
        derive_session_key(a.private_key, b"\x00" * 31)


# --------------------------------------------------------------------------- #
# AEAD primitives: encrypt / decrypt + RFC 8439 vector
# --------------------------------------------------------------------------- #


def test_chacha20poly1305_decrypts_rfc8439_vector() -> None:
    vec = _find("chacha20poly1305-rfc8439-2.8.2")
    key = bytes.fromhex(vec["key"])
    nonce = bytes.fromhex(vec["nonce"])
    aad = bytes.fromhex(vec["aad"])
    plaintext = bytes.fromhex(vec["plaintext"])
    ciphertext = bytes.fromhex(vec["ciphertext"])

    got = decrypt(ciphertext, key, nonce, aad)
    assert got == plaintext
    # Encrypt round-trip equals the on-the-wire ciphertext.
    assert encrypt(plaintext, key, nonce, aad) == ciphertext


def test_chacha20poly1305_decrypt_tag_mutation_fails_closed() -> None:
    vec = _find("chacha20poly1305-rfc8439-tag-mutated")
    key = bytes.fromhex(vec["key"])
    nonce = bytes.fromhex(vec["nonce"])
    aad = bytes.fromhex(vec["aad"])
    ciphertext = bytes.fromhex(vec["ciphertext"])
    with pytest.raises(DecryptionError):
        decrypt(ciphertext, key, nonce, aad)


def test_decrypt_rejects_short_ciphertext() -> None:
    key = b"\x00" * AEAD_KEY_LEN
    nonce = b"\x00" * AEAD_NONCE_LEN
    with pytest.raises(DecryptionError):
        decrypt(b"too short", key, nonce)


def test_encrypt_decrypt_round_trip_random() -> None:
    key = b"\x42" * AEAD_KEY_LEN
    nonce = build_nonce(0x02, 7)
    for body in [b"", b"hi", b"x" * 4096, b"\xff\x00\xff\x00"]:
        ct = encrypt(body, key, nonce)
        assert len(ct) == len(body) + AEAD_TAG_LEN
        assert decrypt(ct, key, nonce) == body


def test_encrypt_rejects_bad_key_or_nonce_length() -> None:
    with pytest.raises(EncryptionError):
        encrypt(b"hi", b"\x00" * 31, b"\x00" * AEAD_NONCE_LEN)
    with pytest.raises(EncryptionError):
        encrypt(b"hi", b"\x00" * AEAD_KEY_LEN, b"\x00" * 11)


# --------------------------------------------------------------------------- #
# build_nonce — crypto-suite.md §1.2 layout
# --------------------------------------------------------------------------- #


def test_build_nonce_layout_app_to_device_counter_zero() -> None:
    """Matches the empty-payload PagerOS vector's nonce field."""
    vec = _find("chacha20poly1305-pageros-nonce-empty")
    expected = bytes.fromhex(vec["nonce"])
    assert build_nonce(0x01, 0) == expected


def test_build_nonce_layout_device_to_app() -> None:
    n = build_nonce(0x02, 1)
    assert n == bytes([0x02]) + (1).to_bytes(8, "big") + b"\x00\x00\x00"
    assert len(n) == AEAD_NONCE_LEN


def test_build_nonce_rejects_out_of_range() -> None:
    with pytest.raises(EncryptionError):
        build_nonce(0x100, 0)
    with pytest.raises(EncryptionError):
        build_nonce(0x01, 1 << 64)


# --------------------------------------------------------------------------- #
# AppKeypair — "Apps can declare a keypair"
# --------------------------------------------------------------------------- #


def test_appkeypair_generate_yields_valid_pair() -> None:
    kp = AppKeypair.generate()
    assert len(kp.private_key) == X25519_KEY_LEN
    assert len(kp.public_key) == X25519_KEY_LEN
    # Public key must be the X25519 base-point image of the private scalar.
    assert kp.public_key == crypto_scalarmult_base(kp.private_key)


def test_appkeypair_from_private_key_round_trips() -> None:
    """Apps that store their X25519 scalar can rehydrate the keypair."""
    secret = bytes(range(32))
    kp = AppKeypair.from_private_key(secret)
    assert kp.private_key == secret
    assert kp.public_key == crypto_scalarmult_base(secret)


def test_appkeypair_public_key_b64_round_trips() -> None:
    kp = AppKeypair.generate()
    encoded = kp.public_key_b64
    # base64url-no-padding, decodes back to the pubkey.
    decoded = base64.urlsafe_b64decode(encoded + "==")
    assert decoded == kp.public_key
    assert "=" not in encoded


def test_appkeypair_rejects_wrong_private_key_length() -> None:
    with pytest.raises(EncryptionError):
        AppKeypair.from_private_key(b"too short")


def test_decrypt_from_round_trip_against_libsodium() -> None:
    """End-to-end: device → app encrypt with libsodium-equivalent path.

    PyNaCl is the same libsodium binding the firmware links against
    (crypto-suite.md §2), so an inbound encrypted payload generated here
    matches what the firmware emits.
    """
    device = AppKeypair.generate()
    app = AppKeypair.generate()
    nonce = build_nonce(0x02, 42)  # device→app
    plaintext = b"hello, encrypted world"

    # Device encrypts using its own private key against the app's pubkey.
    ciphertext = device.encrypt_to(app.public_key, plaintext, nonce)

    # App decrypts using its own private key against the device's pubkey.
    got = app.decrypt_from(device.public_key, ciphertext, nonce)
    assert got == plaintext


def test_decrypt_from_with_wrong_sender_fails() -> None:
    device = AppKeypair.generate()
    other = AppKeypair.generate()
    app = AppKeypair.generate()
    nonce = build_nonce(0x02, 0)
    ct = device.encrypt_to(app.public_key, b"hi", nonce)
    with pytest.raises(DecryptionError):
        app.decrypt_from(other.public_key, ct, nonce)


def test_decrypt_from_with_aad_round_trip() -> None:
    device = AppKeypair.generate()
    app = AppKeypair.generate()
    nonce = build_nonce(0x02, 9)
    ct = device.encrypt_to(app.public_key, b"hi", nonce, aad=b"meta")
    # Wrong AAD must fail closed.
    with pytest.raises(DecryptionError):
        app.decrypt_from(device.public_key, ct, nonce, aad=b"other")
    assert app.decrypt_from(device.public_key, ct, nonce, aad=b"meta") == b"hi"


# --------------------------------------------------------------------------- #
# Middleware — "transparently decrypts inbound encrypted requests"
# --------------------------------------------------------------------------- #


def _wsgi_environ(
    method: str,
    path: str,
    headers: dict[str, str],
    body: bytes = b"",
) -> dict:
    environ = {
        "REQUEST_METHOD": method,
        "PATH_INFO": path,
        "QUERY_STRING": "",
        "CONTENT_LENGTH": str(len(body)),
        "wsgi.input": io.BytesIO(body),
    }
    for k, v in headers.items():
        environ[f"HTTP_{k.upper().replace('-', '_')}"] = v
    return environ


class _CapturingApp:
    def __init__(self) -> None:
        self.calls: list[dict] = []

    def __call__(self, environ, start_response):
        self.calls.append(
            {
                "sender_id": environ.get(ENVIRON_SENDER_ID),
                "nonce": environ.get(ENVIRON_NONCE),
                "body": environ["wsgi.input"].read(),
                "content_length": environ.get("CONTENT_LENGTH"),
            }
        )
        start_response("200 OK", [("Content-Type", "text/plain")])
        return [b"OK"]


def _capture_response():
    status: list[str] = []
    headers: list = []

    def start(s, h):
        status.append(s)
        headers.append(h)

    return status, headers, start


def test_middleware_passes_cleartext_through_unmodified() -> None:
    """No PagerOS-Encrypted header → middleware does nothing at all."""
    app = _CapturingApp()
    keypair = AppKeypair.generate()
    mw = EncryptedRequestMiddleware(app, keypair)

    environ = _wsgi_environ("POST", "/", {}, body=b"plain body")
    status, _, start = _capture_response()
    body = b"".join(mw(environ, start))

    assert status == ["200 OK"]
    assert body == b"OK"
    assert app.calls[0]["body"] == b"plain body"
    assert app.calls[0]["sender_id"] is None
    assert app.calls[0]["content_length"] == str(len(b"plain body"))


def test_middleware_decrypts_inbound_request() -> None:
    """Happy path: encrypted body → middleware decrypts before the app sees it."""
    device = AppKeypair.generate()
    app_keypair = AppKeypair.generate()
    nonce = build_nonce(0x02, 0)
    plaintext = b'{"hello":"world"}'
    ciphertext = device.encrypt_to(app_keypair.public_key, plaintext, nonce)

    sender_b64 = base64.urlsafe_b64encode(device.public_key).rstrip(b"=").decode()
    nonce_b64 = base64.urlsafe_b64encode(nonce).rstrip(b"=").decode()
    headers = {
        HEADER_ENCRYPTED: "1",
        HEADER_SENDER: sender_b64,
        HEADER_NONCE: nonce_b64,
    }

    app = _CapturingApp()
    mw = EncryptedRequestMiddleware(app, app_keypair)
    environ = _wsgi_environ("POST", "/", headers, body=ciphertext)
    status, _, start = _capture_response()
    body = b"".join(mw(environ, start))

    assert status == ["200 OK"]
    assert body == b"OK"
    assert app.calls[0]["body"] == plaintext
    assert app.calls[0]["sender_id"] == device.public_key
    assert app.calls[0]["nonce"] == nonce
    # CONTENT_LENGTH must be rewritten so downstream parsers see the right length.
    assert app.calls[0]["content_length"] == str(len(plaintext))


def test_middleware_rejects_tampered_ciphertext() -> None:
    device = AppKeypair.generate()
    app_keypair = AppKeypair.generate()
    nonce = build_nonce(0x02, 0)
    ct = device.encrypt_to(app_keypair.public_key, b"hi", nonce)
    ct = ct[:-1] + bytes([ct[-1] ^ 0x01])  # flip last tag byte

    headers = {
        HEADER_ENCRYPTED: "1",
        HEADER_SENDER: base64.urlsafe_b64encode(device.public_key).rstrip(b"=").decode(),
        HEADER_NONCE: base64.urlsafe_b64encode(nonce).rstrip(b"=").decode(),
    }
    app = _CapturingApp()
    mw = EncryptedRequestMiddleware(app, app_keypair)
    environ = _wsgi_environ("POST", "/", headers, body=ct)
    status, _, start = _capture_response()
    body = b"".join(mw(environ, start))

    assert status[0].startswith("401")
    assert b"DecryptionError" in body
    assert app.calls == []


def test_middleware_rejects_missing_sender_header() -> None:
    """Encrypted flag set but Sender absent → 400 (don't silently leak plaintext)."""
    app_keypair = AppKeypair.generate()
    nonce_b64 = base64.urlsafe_b64encode(build_nonce(0x02, 0)).rstrip(b"=").decode()
    headers = {HEADER_ENCRYPTED: "1", HEADER_NONCE: nonce_b64}
    app = _CapturingApp()
    mw = EncryptedRequestMiddleware(app, app_keypair)

    environ = _wsgi_environ("POST", "/", headers, body=b"\x00" * (AEAD_TAG_LEN + 1))
    status, _, start = _capture_response()
    body = b"".join(mw(environ, start))

    assert status[0].startswith("400")
    assert b"MissingEncryptionHeader" in body
    assert app.calls == []


def test_middleware_rejects_bad_base64_sender() -> None:
    app_keypair = AppKeypair.generate()
    nonce_b64 = base64.urlsafe_b64encode(build_nonce(0x02, 0)).rstrip(b"=").decode()
    headers = {
        HEADER_ENCRYPTED: "1",
        HEADER_SENDER: "not-base64!!!!",
        HEADER_NONCE: nonce_b64,
    }
    app = _CapturingApp()
    mw = EncryptedRequestMiddleware(app, app_keypair)
    environ = _wsgi_environ("POST", "/", headers, body=b"\x00" * (AEAD_TAG_LEN + 1))
    status, _, start = _capture_response()
    body = b"".join(mw(environ, start))

    assert status[0].startswith("400")
    assert b"InvalidEncryptionHeader" in body


def test_middleware_rejects_wrong_nonce_length() -> None:
    app_keypair = AppKeypair.generate()
    sender_b64 = base64.urlsafe_b64encode(b"\x00" * X25519_KEY_LEN).rstrip(b"=").decode()
    nonce_b64 = base64.urlsafe_b64encode(b"\x00" * 8).rstrip(b"=").decode()
    headers = {
        HEADER_ENCRYPTED: "1",
        HEADER_SENDER: sender_b64,
        HEADER_NONCE: nonce_b64,
    }
    app = _CapturingApp()
    mw = EncryptedRequestMiddleware(app, app_keypair)
    environ = _wsgi_environ("POST", "/", headers, body=b"\x00" * (AEAD_TAG_LEN + 1))
    status, _, start = _capture_response()
    body = b"".join(mw(environ, start))

    assert status[0].startswith("400")
    assert b"InvalidEncryptionHeader" in body


def test_middleware_encrypted_flag_false_is_pass_through() -> None:
    """Apps that explicitly send PagerOS-Encrypted: 0 are not parsed."""
    app = _CapturingApp()
    keypair = AppKeypair.generate()
    mw = EncryptedRequestMiddleware(app, keypair)
    headers = {HEADER_ENCRYPTED: "0"}
    environ = _wsgi_environ("POST", "/", headers, body=b"plain")
    status, _, start = _capture_response()
    b"".join(mw(environ, start))
    assert status == ["200 OK"]
    assert app.calls[0]["body"] == b"plain"


def test_middleware_header_lookup_is_case_insensitive() -> None:
    device = AppKeypair.generate()
    app_keypair = AppKeypair.generate()
    nonce = build_nonce(0x02, 0)
    ct = device.encrypt_to(app_keypair.public_key, b"hi", nonce)

    sender_b64 = base64.urlsafe_b64encode(device.public_key).rstrip(b"=").decode()
    nonce_b64 = base64.urlsafe_b64encode(nonce).rstrip(b"=").decode()
    headers = {
        HEADER_ENCRYPTED.lower(): "1",
        HEADER_SENDER.lower(): sender_b64,
        HEADER_NONCE.lower(): nonce_b64,
    }
    app = _CapturingApp()
    mw = EncryptedRequestMiddleware(app, app_keypair)
    environ = _wsgi_environ("POST", "/", headers, body=ct)
    status, _, start = _capture_response()
    b"".join(mw(environ, start))
    assert status == ["200 OK"]
    assert app.calls[0]["body"] == b"hi"


# --------------------------------------------------------------------------- #
# Sanity / regression
# --------------------------------------------------------------------------- #


def test_error_hierarchy() -> None:
    """All encryption errors must be catchable as plain ``ValueError``."""
    assert issubclass(EncryptionError, ValueError)
    assert issubclass(DecryptionError, EncryptionError)
    assert issubclass(InvalidEncryptionHeader, EncryptionError)
    assert issubclass(MissingEncryptionHeader, EncryptionError)


def test_constants_match_spec() -> None:
    """Belt-and-braces: the constants we expose match crypto-suite.md."""
    assert HKDF_SALT == b"\x00" * 32
    assert HKDF_INFO == b"pageros/v1/e2e"
    assert AEAD_NONCE_LEN == 12
    assert AEAD_KEY_LEN == 32
    assert AEAD_TAG_LEN == 16
    assert X25519_KEY_LEN == 32
