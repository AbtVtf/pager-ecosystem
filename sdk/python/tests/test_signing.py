"""Tests for the PY-004 request signature verification middleware.

Acceptance criteria (TASKS.md, PY-004):
* Rejects requests with missing/invalid ``PagerOS-Sig``.
* Populates ``ctx.device_id`` on success — exposed here as
  ``environ["pageros.device_id"]`` for the WSGI middleware and as the
  ``device_id`` attribute on :class:`VerifiedRequest` for direct callers.
* Tested against firmware-generated signatures.

The "firmware-generated" check is satisfied by signing with PyNaCl, which
is the same libsodium binding the firmware links against per
``docs/spec/crypto-suite.md`` (SEC-001). Both libraries produce
byte-identical Ed25519 output, so PyNaCl-produced signatures are
representative of what the firmware emits. We additionally pin the test
to the ``ed25519-pageros-sig-sample`` vector in the shared test-vector
file so any cross-impl regression in signing input assembly shows up
here first.
"""

from __future__ import annotations

import base64
import hashlib
import io
import json
import pathlib
import time

import pytest
from nacl.signing import SigningKey, VerifyKey

from pageros.signing import (
    DEFAULT_MAX_SKEW_SECONDS,
    ED25519_PUBKEY_LEN,
    ED25519_SIG_LEN,
    ENVIRON_DEVICE_ID,
    ENVIRON_TIMESTAMP,
    HEADER_DEVICE,
    HEADER_SIG,
    HEADER_TIMESTAMP,
    BadSignature,
    InvalidEncoding,
    MissingHeader,
    SignatureError,
    SignatureVerifierMiddleware,
    TimestampSkew,
    build_signing_input,
    compute_body_hash,
    sign_request,
    verify_request,
)


# --------------------------------------------------------------------------- #
# Fixtures
# --------------------------------------------------------------------------- #


# Seed pinned to the ed25519-pageros-sig-sample vector so we stay in lockstep
# with the firmware test inputs.
PAGEROS_SAMPLE_SEED_HEX = (
    "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60"
)
PAGEROS_SAMPLE_PUB_HEX = (
    "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a"
)


@pytest.fixture()
def signing_key() -> SigningKey:
    return SigningKey(bytes.fromhex(PAGEROS_SAMPLE_SEED_HEX))


@pytest.fixture()
def frozen_now() -> int:
    """A stable Unix timestamp inside the spec skew window."""
    return 1_700_000_000


# --------------------------------------------------------------------------- #
# build_signing_input — locked to the shared crypto test vector
# --------------------------------------------------------------------------- #


def _vector_path() -> pathlib.Path:
    """Path to docs/spec/crypto-test-vectors.json relative to repo root."""
    here = pathlib.Path(__file__).resolve()
    # tests/ → sdk/python/ → sdk/ → repo root
    return here.parents[3] / "docs" / "spec" / "crypto-test-vectors.json"


def _load_pageros_sig_vector() -> dict:
    data = json.loads(_vector_path().read_text())
    for vec in data["vectors"]:
        if vec.get("id") == "ed25519-pageros-sig-sample":
            return vec
    raise AssertionError("ed25519-pageros-sig-sample vector missing")


def test_signing_input_matches_shared_vector() -> None:
    """Byte layout must equal the `msg_assembly` from the shared vector.

    The vector's ``msg`` field is the concatenation written with a space
    between the UTF-8 part and the body-hash part (purely for readability —
    see ``msg_assembly``). We strip whitespace and compare the hex.
    """
    vec = _load_pageros_sig_vector()
    parts = vec["msg_components"]
    expected = bytes.fromhex(vec["msg"].replace(" ", ""))

    signing_input = build_signing_input(
        parts["method"],
        parts["url"],
        parts["timestamp"],
        body=b"{}",  # body_hash is sha256("{}"), see vector's body_hash_hex.
    )

    assert signing_input == expected
    # And the body-hash slice must equal the documented sha256("{}").
    body_hash = signing_input[-32:]
    assert body_hash.hex() == parts["body_hash_hex"]


def test_compute_body_hash_empty_and_typical() -> None:
    assert compute_body_hash(None).hex() == hashlib.sha256(b"").hexdigest()
    assert compute_body_hash(b"").hex() == hashlib.sha256(b"").hexdigest()
    assert (
        compute_body_hash(b"{}").hex()
        == hashlib.sha256(b"{}").hexdigest()
    )


# --------------------------------------------------------------------------- #
# verify_request — happy path and field handling
# --------------------------------------------------------------------------- #


def test_verify_request_round_trip_succeeds(
    signing_key: SigningKey, frozen_now: int
) -> None:
    headers = sign_request(
        "POST", "/v1/events", b"{}", signing_key, timestamp=frozen_now
    )
    result = verify_request(
        "POST",
        "/v1/events",
        headers,
        b"{}",
        now=lambda: float(frozen_now),
    )

    assert result.device_id == bytes(signing_key.verify_key)
    assert result.timestamp == frozen_now
    assert len(result.device_id) == ED25519_PUBKEY_LEN
    # Convenience b64 view returns base64url, no padding.
    assert "=" not in result.device_id_b64


def test_verify_request_uses_known_pubkey(
    signing_key: SigningKey, frozen_now: int
) -> None:
    """Verified device_id matches the spec-pinned pubkey."""
    headers = sign_request(
        "POST", "/v1/events", b"{}", signing_key, timestamp=frozen_now
    )
    result = verify_request(
        "POST", "/v1/events", headers, b"{}", now=lambda: float(frozen_now)
    )
    assert result.device_id.hex() == PAGEROS_SAMPLE_PUB_HEX


def test_verify_request_header_lookup_is_case_insensitive(
    signing_key: SigningKey, frozen_now: int
) -> None:
    headers = sign_request(
        "GET", "/", b"", signing_key, timestamp=frozen_now
    )
    munged = {k.lower(): v for k, v in headers.items()}
    result = verify_request(
        "GET", "/", munged, b"", now=lambda: float(frozen_now)
    )
    assert result.timestamp == frozen_now


def test_verify_request_empty_body(
    signing_key: SigningKey, frozen_now: int
) -> None:
    headers = sign_request("GET", "/", b"", signing_key, timestamp=frozen_now)
    # Calling with None for body must hash identically to b"".
    result = verify_request(
        "GET", "/", headers, None, now=lambda: float(frozen_now)
    )
    assert result.device_id == bytes(signing_key.verify_key)


# --------------------------------------------------------------------------- #
# verify_request — every failure mode in the acceptance criteria
# --------------------------------------------------------------------------- #


@pytest.mark.parametrize("missing", [HEADER_DEVICE, HEADER_SIG, HEADER_TIMESTAMP])
def test_verify_request_missing_header_rejected(
    signing_key: SigningKey, frozen_now: int, missing: str
) -> None:
    headers = sign_request(
        "POST", "/v1/events", b"{}", signing_key, timestamp=frozen_now
    )
    headers.pop(missing)
    with pytest.raises(MissingHeader, match=missing):
        verify_request(
            "POST",
            "/v1/events",
            headers,
            b"{}",
            now=lambda: float(frozen_now),
        )


def test_verify_request_bad_base64_rejected(
    signing_key: SigningKey, frozen_now: int
) -> None:
    headers = sign_request(
        "POST", "/v1/events", b"{}", signing_key, timestamp=frozen_now
    )
    headers[HEADER_SIG] = "not-base64!!!"
    with pytest.raises(InvalidEncoding):
        verify_request(
            "POST",
            "/v1/events",
            headers,
            b"{}",
            now=lambda: float(frozen_now),
        )


def test_verify_request_wrong_pubkey_length_rejected(
    signing_key: SigningKey, frozen_now: int
) -> None:
    headers = sign_request(
        "POST", "/v1/events", b"{}", signing_key, timestamp=frozen_now
    )
    headers[HEADER_DEVICE] = base64.urlsafe_b64encode(b"too-short").rstrip(b"=").decode()
    with pytest.raises(InvalidEncoding):
        verify_request(
            "POST",
            "/v1/events",
            headers,
            b"{}",
            now=lambda: float(frozen_now),
        )


def test_verify_request_non_integer_timestamp_rejected(
    signing_key: SigningKey, frozen_now: int
) -> None:
    headers = sign_request(
        "POST", "/v1/events", b"{}", signing_key, timestamp=frozen_now
    )
    headers[HEADER_TIMESTAMP] = "yesterday"
    with pytest.raises(InvalidEncoding):
        verify_request(
            "POST",
            "/v1/events",
            headers,
            b"{}",
            now=lambda: float(frozen_now),
        )


def test_verify_request_tampered_body_rejected(
    signing_key: SigningKey, frozen_now: int
) -> None:
    headers = sign_request(
        "POST", "/v1/events", b"{}", signing_key, timestamp=frozen_now
    )
    with pytest.raises(BadSignature):
        verify_request(
            "POST",
            "/v1/events",
            headers,
            b'{"tampered":true}',
            now=lambda: float(frozen_now),
        )


def test_verify_request_tampered_method_rejected(
    signing_key: SigningKey, frozen_now: int
) -> None:
    headers = sign_request(
        "POST", "/v1/events", b"{}", signing_key, timestamp=frozen_now
    )
    with pytest.raises(BadSignature):
        verify_request(
            "PUT", "/v1/events", headers, b"{}", now=lambda: float(frozen_now)
        )


def test_verify_request_tampered_url_rejected(
    signing_key: SigningKey, frozen_now: int
) -> None:
    headers = sign_request(
        "POST", "/v1/events", b"{}", signing_key, timestamp=frozen_now
    )
    with pytest.raises(BadSignature):
        verify_request(
            "POST",
            "/v1/elsewhere",
            headers,
            b"{}",
            now=lambda: float(frozen_now),
        )


def test_verify_request_wrong_key_rejected(frozen_now: int) -> None:
    attacker = SigningKey.generate()
    expected = SigningKey.generate()
    headers = sign_request(
        "POST", "/v1/events", b"{}", attacker, timestamp=frozen_now
    )
    # Replace the device header with the expected pubkey but keep attacker's sig.
    headers[HEADER_DEVICE] = base64.urlsafe_b64encode(
        bytes(expected.verify_key)
    ).rstrip(b"=").decode()
    with pytest.raises(BadSignature):
        verify_request(
            "POST",
            "/v1/events",
            headers,
            b"{}",
            now=lambda: float(frozen_now),
        )


def test_verify_request_old_timestamp_rejected(
    signing_key: SigningKey, frozen_now: int
) -> None:
    headers = sign_request(
        "POST", "/v1/events", b"{}", signing_key, timestamp=frozen_now
    )
    server_now = frozen_now + DEFAULT_MAX_SKEW_SECONDS + 1
    with pytest.raises(TimestampSkew):
        verify_request(
            "POST",
            "/v1/events",
            headers,
            b"{}",
            now=lambda: float(server_now),
        )


def test_verify_request_future_timestamp_rejected(
    signing_key: SigningKey, frozen_now: int
) -> None:
    headers = sign_request(
        "POST", "/v1/events", b"{}", signing_key, timestamp=frozen_now
    )
    server_now = frozen_now - DEFAULT_MAX_SKEW_SECONDS - 1
    with pytest.raises(TimestampSkew):
        verify_request(
            "POST",
            "/v1/events",
            headers,
            b"{}",
            now=lambda: float(server_now),
        )


def test_verify_request_skew_disabled_with_negative(
    signing_key: SigningKey, frozen_now: int
) -> None:
    """``max_skew_seconds=-1`` (or any negative) opts out of the skew check."""
    headers = sign_request(
        "POST", "/v1/events", b"{}", signing_key, timestamp=frozen_now
    )
    very_late = frozen_now + 86_400 * 30
    # Old API: we treat negative as "off"; a positive skew of 0 means only the
    # exact second is accepted. We want a way to disable the check entirely.
    result = verify_request(
        "POST",
        "/v1/events",
        headers,
        b"{}",
        max_skew_seconds=-1,
        now=lambda: float(very_late),
    )
    assert result.timestamp == frozen_now


# --------------------------------------------------------------------------- #
# Firmware-vector cross-check
# --------------------------------------------------------------------------- #


def test_signature_matches_known_pubkey_round_trip(
    signing_key: SigningKey, frozen_now: int
) -> None:
    """Sign with PyNaCl, verify with bare VerifyKey, then via verify_request.

    PyNaCl is the same libsodium binding the firmware links against
    (docs/spec/crypto-suite.md §2 — "binary-identical to firmware"), so a
    PyNaCl-produced signature is a stand-in for a firmware-produced
    signature. The double check guards against the middleware doing
    something silly with the signing input.
    """
    headers = sign_request(
        "POST", "/v1/events", b"{}", signing_key, timestamp=frozen_now
    )
    sig = base64.urlsafe_b64decode(headers[HEADER_SIG] + "==")
    msg = build_signing_input("POST", "/v1/events", frozen_now, b"{}")
    # Bare-libsodium verify path (what firmware does).
    VerifyKey(bytes(signing_key.verify_key)).verify(msg, sig)

    # And via the middleware-facing API.
    verify_request(
        "POST", "/v1/events", headers, b"{}", now=lambda: float(frozen_now)
    )


# --------------------------------------------------------------------------- #
# WSGI middleware
# --------------------------------------------------------------------------- #


def _wsgi_environ(
    method: str,
    path: str,
    headers: dict[str, str],
    body: bytes = b"",
    query: str = "",
) -> dict:
    environ = {
        "REQUEST_METHOD": method,
        "PATH_INFO": path,
        "QUERY_STRING": query,
        "CONTENT_LENGTH": str(len(body)),
        "wsgi.input": io.BytesIO(body),
    }
    for k, v in headers.items():
        environ[f"HTTP_{k.upper().replace('-', '_')}"] = v
    return environ


class _CapturingApp:
    """Records what the wrapped app saw — used by middleware tests."""

    def __init__(self) -> None:
        self.calls: list[dict] = []

    def __call__(self, environ, start_response):
        self.calls.append(
            {
                "device_id": environ.get(ENVIRON_DEVICE_ID),
                "timestamp": environ.get(ENVIRON_TIMESTAMP),
                "body": environ["wsgi.input"].read(),
            }
        )
        start_response("200 OK", [("Content-Type", "text/plain")])
        return [b"OK"]


def _capture_response() -> tuple[list, list, list]:
    status_box: list = []
    headers_box: list = []

    def start_response(status, headers):
        status_box.append(status)
        headers_box.append(headers)

    return status_box, headers_box, start_response


def test_middleware_accepts_signed_request(
    signing_key: SigningKey, frozen_now: int
) -> None:
    headers = sign_request(
        "POST", "/v1/events", b"{}", signing_key, timestamp=frozen_now
    )
    app = _CapturingApp()
    mw = SignatureVerifierMiddleware(app, now=lambda: float(frozen_now))

    environ = _wsgi_environ("POST", "/v1/events", headers, body=b"{}")
    status, _, start = _capture_response()
    body = b"".join(mw(environ, start))

    assert status == ["200 OK"]
    assert body == b"OK"
    assert len(app.calls) == 1
    # The wrapped app sees the verified device pubkey...
    assert app.calls[0]["device_id"] == bytes(signing_key.verify_key)
    assert app.calls[0]["timestamp"] == frozen_now
    # ...and the body is still readable downstream.
    assert app.calls[0]["body"] == b"{}"


def test_middleware_rejects_missing_signature(frozen_now: int) -> None:
    app = _CapturingApp()
    mw = SignatureVerifierMiddleware(app, now=lambda: float(frozen_now))

    environ = _wsgi_environ("GET", "/", {}, body=b"")
    status, _, start = _capture_response()
    body = b"".join(mw(environ, start))

    assert status[0].startswith("401")
    assert b"MissingHeader" in body
    assert app.calls == []  # the wrapped app must NOT see unauth'd requests


def test_middleware_rejects_tampered_body(
    signing_key: SigningKey, frozen_now: int
) -> None:
    headers = sign_request(
        "POST", "/v1/events", b"{}", signing_key, timestamp=frozen_now
    )
    app = _CapturingApp()
    mw = SignatureVerifierMiddleware(app, now=lambda: float(frozen_now))

    environ = _wsgi_environ(
        "POST", "/v1/events", headers, body=b'{"tampered":true}'
    )
    status, _, start = _capture_response()
    body = b"".join(mw(environ, start))

    assert status[0].startswith("401")
    assert b"BadSignature" in body
    assert app.calls == []


def test_middleware_rejects_skewed_timestamp(
    signing_key: SigningKey, frozen_now: int
) -> None:
    headers = sign_request(
        "POST", "/v1/events", b"{}", signing_key, timestamp=frozen_now
    )
    app = _CapturingApp()
    server_now = frozen_now + DEFAULT_MAX_SKEW_SECONDS + 1
    mw = SignatureVerifierMiddleware(app, now=lambda: float(server_now))

    environ = _wsgi_environ("POST", "/v1/events", headers, body=b"{}")
    status, _, start = _capture_response()
    body = b"".join(mw(environ, start))

    assert status[0].startswith("401")
    assert b"TimestampSkew" in body
    assert app.calls == []


def test_middleware_signs_url_with_query(
    signing_key: SigningKey, frozen_now: int
) -> None:
    """The signed URL must include the query string the client used."""
    headers = sign_request(
        "GET", "/search?q=hello", b"", signing_key, timestamp=frozen_now
    )
    app = _CapturingApp()
    mw = SignatureVerifierMiddleware(app, now=lambda: float(frozen_now))

    environ = _wsgi_environ("GET", "/search", headers, body=b"", query="q=hello")
    status, _, start = _capture_response()
    body = b"".join(mw(environ, start))

    assert status == ["200 OK"]
    assert app.calls[0]["device_id"] == bytes(signing_key.verify_key)


# --------------------------------------------------------------------------- #
# Sanity / regression
# --------------------------------------------------------------------------- #


def test_sign_request_includes_all_required_headers(
    signing_key: SigningKey,
) -> None:
    headers = sign_request("GET", "/", b"", signing_key)
    assert set(headers.keys()) == {HEADER_DEVICE, HEADER_SIG, HEADER_TIMESTAMP}
    # Sig length is fixed by Ed25519.
    raw_sig = base64.urlsafe_b64decode(headers[HEADER_SIG] + "==")
    assert len(raw_sig) == ED25519_SIG_LEN


def test_signature_error_is_value_error() -> None:
    """All sig errors must be catchable as ValueError, so plain ``except
    ValueError`` blocks in apps still see them."""
    assert issubclass(SignatureError, ValueError)
    assert issubclass(MissingHeader, SignatureError)
    assert issubclass(InvalidEncoding, SignatureError)
    assert issubclass(BadSignature, SignatureError)
    assert issubclass(TimestampSkew, SignatureError)


def test_default_skew_matches_spec() -> None:
    """Belt-and-braces: SPEC.md §9.2 calls out 5 minutes."""
    assert DEFAULT_MAX_SKEW_SECONDS == 5 * 60
