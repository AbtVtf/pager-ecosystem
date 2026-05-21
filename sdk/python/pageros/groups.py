"""Group helpers (PY-007): broadcast + inbound group-event registry.

This is the app-side counterpart to the Push Relay's group fan-out
handler in ``push-relay/internal/server/group.go``. The relay accepts
a single signed envelope and stores N per-recipient encrypted payloads
on the device queues with ``kind: "group_event"`` (SPEC §5.4.2 /
§6.6.6). The relay sees only opaque per-recipient blobs — the same
X25519+ChaCha20-Poly1305 end-to-end pattern as :mod:`pageros.push`
(SPEC §6.6.3, crypto-suite.md §1.1–§1.2).

This module owns the wire format for ``POST /group_push``:

.. code-block:: json

    {
      "recipients": [
        {"device_pubkey": "<b64url>", "payload_b64": "<b64url>"},
        ...
      ]
    }

Each ``payload_b64`` is the same opaque envelope produced by
:func:`pageros.push.build_push_body` — but bound to *that* recipient's
X25519 pubkey, with a per-(app, device) monotonic counter. The
``payload_b64`` field uses raw (un-padded) URL-safe base64 to match the
relay's canonical decoder.

Per-recipient results are returned verbatim from the relay so callers
can distinguish accepted enqueues from rate-limit denials or oversized
payloads on a per-device basis without re-deriving the relay's status
strings.
"""

from __future__ import annotations

import base64
import json
import socket
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from typing import Any, Callable, Iterable, Sequence

from pageros.push import (
    HEADER_APP,
    PushConfig,
    PushError,
    PushRejected,
    PushUnavailable,
    _b64url,
    _coerce_device_pubkey,
)
from pageros.signing import (
    HEADER_SIG,
    HEADER_TIMESTAMP,
    build_signing_input,
)

__all__ = [
    "GROUP_PUSH_PATH",
    "GROUP_PUSH_CONTENT_TYPE",
    "GROUP_RESULT_ACCEPTED",
    "GROUP_RESULT_BAD_DEVICE",
    "GROUP_RESULT_BAD_PAYLOAD",
    "GROUP_RESULT_PAYLOAD_EMPTY",
    "GROUP_RESULT_PAYLOAD_LARGE",
    "GROUP_RESULT_RATE_LIMITED",
    "GROUP_RESULT_STORAGE_ERROR",
    "GroupBroadcastError",
    "GroupBroadcastResult",
    "GroupRecipientResult",
    "build_group_push_body",
    "send_group_push",
]

GROUP_PUSH_PATH = "/group_push"
GROUP_PUSH_CONTENT_TYPE = "application/json"

_DEFAULT_TIMEOUT = 10.0

# Per-recipient status strings — pinned to the Go relay (group.go).
GROUP_RESULT_ACCEPTED = "accepted"
GROUP_RESULT_RATE_LIMITED = "rate_limited"
GROUP_RESULT_PAYLOAD_EMPTY = "payload_empty"
GROUP_RESULT_PAYLOAD_LARGE = "payload_too_large"
GROUP_RESULT_BAD_PAYLOAD = "invalid_payload"
GROUP_RESULT_BAD_DEVICE = "invalid_device_pubkey"
GROUP_RESULT_STORAGE_ERROR = "storage_error"


class GroupBroadcastError(PushError):
    """Raised when a broadcast cannot even be attempted.

    Distinct from :class:`pageros.push.PushRejected` / ``PushUnavailable``
    which still apply when the relay rejects the *whole* batch (auth,
    sig, banned app, body too big). This class is for client-side
    misconfiguration: no recipients, missing app id/key, …
    """


@dataclass(frozen=True)
class GroupRecipientResult:
    """One row of the per-recipient relay response.

    Mirrors the JSON shape returned by ``POST /group_push``. Either
    ``id`` + ``enqueued_at`` are populated (status == ``"accepted"``)
    or ``retry_after`` is populated (status == ``"rate_limited"``);
    other failure statuses leave both unset.
    """

    device_pubkey: str
    status: str
    id: str | None = None
    enqueued_at: int | None = None
    retry_after: int | None = None

    @property
    def accepted(self) -> bool:
        return self.status == GROUP_RESULT_ACCEPTED


@dataclass(frozen=True)
class GroupBroadcastResult:
    """Aggregate result for a single ``broadcast`` call."""

    results: tuple[GroupRecipientResult, ...] = field(default_factory=tuple)

    @property
    def accepted(self) -> tuple[GroupRecipientResult, ...]:
        return tuple(r for r in self.results if r.accepted)

    @property
    def rejected(self) -> tuple[GroupRecipientResult, ...]:
        return tuple(r for r in self.results if not r.accepted)

    @property
    def total(self) -> int:
        return len(self.results)

    def by_device(self, device_pubkey: str) -> GroupRecipientResult | None:
        for r in self.results:
            if r.device_pubkey == device_pubkey:
                return r
        return None


# --------------------------------------------------------------------------- #
# Body framing
# --------------------------------------------------------------------------- #


def build_group_push_body(
    recipients: Sequence[tuple[str, bytes]],
) -> bytes:
    """Build the JSON envelope for ``POST /group_push``.

    ``recipients`` is a sequence of ``(device_pubkey_b64url, opaque_payload_bytes)``
    pairs. Each opaque payload should be the output of
    :func:`pageros.push.build_push_body` for that recipient — the relay
    treats the bytes as a black box and never inspects them.
    """
    if not recipients:
        raise GroupBroadcastError("broadcast requires at least one recipient")
    body = {
        "recipients": [
            {
                "device_pubkey": dev_b64,
                "payload_b64": _b64url(payload),
            }
            for dev_b64, payload in recipients
        ]
    }
    return json.dumps(body, separators=(",", ":")).encode("utf-8")


# --------------------------------------------------------------------------- #
# HTTP
# --------------------------------------------------------------------------- #


def send_group_push(
    config: PushConfig,
    recipients: Sequence[tuple[str, bytes]],
    *,
    timeout: float | None = _DEFAULT_TIMEOUT,
    now: Callable[[], float] | None = None,
    opener: Callable[..., Any] | None = None,
) -> GroupBroadcastResult:
    """Sign and POST a group-event batch to the Push Relay.

    Auth + signature failures (4xx) raise :class:`PushRejected` — those
    failures reject the *whole* batch, matching the relay's behaviour.
    Per-recipient failures (rate limit, bad device key, oversized
    payload) are reported in the returned
    :class:`GroupBroadcastResult` and do not raise.
    """
    body = build_group_push_body(recipients)
    url = config.relay_url.rstrip("/") + GROUP_PUSH_PATH

    clock = now if now is not None else time.time
    ts = str(int(clock()))
    sig_input = build_signing_input("POST", GROUP_PUSH_PATH, ts, body)
    sig = config.signing_key.sign(sig_input).signature

    headers = {
        HEADER_APP: config.app_id,
        HEADER_SIG: _b64url(sig),
        HEADER_TIMESTAMP: ts,
        "Content-Type": GROUP_PUSH_CONTENT_TYPE,
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
            f"push relay returned HTTP {status}",
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

    if not isinstance(parsed, dict):
        raise PushUnavailable("group push response is not a JSON object")
    raw_results = parsed.get("results")
    if not isinstance(raw_results, list):
        raise PushUnavailable("group push response missing 'results' array")

    results: list[GroupRecipientResult] = []
    for row in raw_results:
        if not isinstance(row, dict):
            raise PushUnavailable("group push 'results' row is not a JSON object")
        dev = row.get("device_pubkey")
        st = row.get("status")
        if not isinstance(dev, str) or not isinstance(st, str):
            raise PushUnavailable(
                "group push row missing 'device_pubkey' or 'status'"
            )
        nid = row.get("id")
        enq = row.get("enqueued_at")
        retry = row.get("retry_after")
        results.append(
            GroupRecipientResult(
                device_pubkey=dev,
                status=st,
                id=nid if isinstance(nid, str) and nid else None,
                enqueued_at=int(enq) if isinstance(enq, (int, float)) else None,
                retry_after=int(retry) if isinstance(retry, (int, float)) else None,
            )
        )
    return GroupBroadcastResult(results=tuple(results))


# --------------------------------------------------------------------------- #
# Member-key normalisation
# --------------------------------------------------------------------------- #


def normalize_device_pubkey(device_id: bytes | str) -> tuple[bytes, str]:
    """Return ``(raw_32B, b64url_path_form)`` for any of the accepted forms.

    Thin wrapper over :func:`pageros.push._coerce_device_pubkey` so the
    group registry and the broadcast helper share one validator without
    poking at a private symbol from another module.
    """
    return _coerce_device_pubkey(device_id)
