"""Per-request handler context (PY-008).

SPEC §8.3 defines a ``ctx`` object that handlers can read to learn who
issued the current request and under what conditions. This module
implements that object in the Python SDK, keeping field names and
types byte-for-byte aligned with the spec table:

==============  =====================================  ==========================
Field           Type                                   Source
==============  =====================================  ==========================
device_id       ``str`` — base64url device pubkey      ``PagerOS-Device`` (after
                                                       signature verification)
session         ``dict[str, Any]``                     SDK-managed; opt-in helper
transport       ``"wifi" | "lora"``                    ``PagerOS-Transport``
granted         ``tuple[str, ...]``                    ``PagerOS-Granted`` (CSV)
location        :class:`Location` ``| None``           ``PagerOS-Location``
                (NamedTuple: lat, lon, ts)
groups          ``tuple[str, ...]``                    ``PagerOS-Groups`` (CSV)
==============  =====================================  ==========================

The transport / granted / location / groups headers are SDK-level
conventions that v1 firmware emits on every request (and exit nodes
preserve through LoRa rehydration). They are intentionally text rather
than CBOR — they ride alongside the HTTP envelope without touching the
body, which keeps the LoRa inner envelope (§6.2.2) untouched.

``device_id`` is the authoritative identity field. When a request
arrives through :class:`pageros.SignatureVerifierMiddleware`, the verified
32-byte pubkey lives in ``environ["pageros.device_id"]`` and
:meth:`Ctx.from_headers` accepts it via the ``verified_device_id``
keyword so handlers never see an unverified value. The bare
``PagerOS-Device`` header is used only when no middleware result is
available (e.g. in test rigs that bypass verification on purpose).
"""

from __future__ import annotations

import base64
from dataclasses import dataclass, field
from typing import Any, Mapping, NamedTuple

__all__ = [
    "Ctx",
    "HEADER_GRANTED",
    "HEADER_GROUPS",
    "HEADER_LOCATION",
    "HEADER_TRANSPORT",
    "Location",
    "TRANSPORT_LORA",
    "TRANSPORT_WIFI",
    "TRANSPORTS",
]


# --------------------------------------------------------------------------- #
# Constants
# --------------------------------------------------------------------------- #

HEADER_TRANSPORT = "PagerOS-Transport"
HEADER_GRANTED = "PagerOS-Granted"
HEADER_LOCATION = "PagerOS-Location"
HEADER_GROUPS = "PagerOS-Groups"

# SPEC §8.3: ``transport`` is enumerated as ``wifi`` or ``lora``. Modelled as
# string constants rather than an ``enum.Enum`` so the field carries the
# string straight through serialisation and comparison (``ctx.transport ==
# "wifi"``), matching the spec table exactly.
TRANSPORT_WIFI = "wifi"
TRANSPORT_LORA = "lora"
TRANSPORTS = frozenset({TRANSPORT_WIFI, TRANSPORT_LORA})


# --------------------------------------------------------------------------- #
# Location
# --------------------------------------------------------------------------- #


class Location(NamedTuple):
    """A device GPS fix.

    NamedTuple keeps the (lat, lon, ts) tuple-literal form from SPEC §8.3
    (``"(lat, lon, ts) or None"``) while also giving handler authors
    named field access (``ctx.location.lat``).
    """

    lat: float
    lon: float
    ts: int


# --------------------------------------------------------------------------- #
# Ctx
# --------------------------------------------------------------------------- #


@dataclass
class Ctx:
    """Per-request context surfaced to handlers (SPEC §8.3).

    Field defaults model the "absent" state explicitly so an app that
    inspects ``ctx`` always sees the same shape regardless of which
    headers were supplied:

    - ``device_id`` defaults to ``""`` (unverified / anonymous request).
    - ``session`` defaults to an empty mutable ``dict`` so the opt-in
      session helper can write into it without the app first checking
      for ``None``.
    - ``transport`` defaults to :data:`TRANSPORT_WIFI` because a request
      hitting the SDK's stdlib HTTP server arrived over an IP transport
      by definition; LoRa-rehydrated requests must opt in by setting
      :data:`HEADER_TRANSPORT`.
    - ``granted`` and ``groups`` default to ``()`` (apps that don't
      declare permissions or use multi-device sessions get the empty
      iterables they expect).
    - ``location`` defaults to ``None``.

    Construct via :meth:`from_headers` for the common HTTP-request path,
    or instantiate directly (e.g. in tests, dev fixtures, or hand-rolled
    transports).
    """

    device_id: str = ""
    session: dict[str, Any] = field(default_factory=dict)
    transport: str = TRANSPORT_WIFI
    granted: tuple[str, ...] = ()
    location: Location | None = None
    groups: tuple[str, ...] = ()

    @classmethod
    def from_headers(
        cls,
        headers: Mapping[str, str],
        *,
        verified_device_id: bytes | None = None,
        session: dict[str, Any] | None = None,
    ) -> "Ctx":
        """Build a :class:`Ctx` from HTTP request headers.

        ``verified_device_id`` — the 32-byte device pubkey the SDK has
        already verified (e.g. via :class:`pageros.SignatureVerifierMiddleware`).
        When supplied, this is used as the canonical ``device_id``; the
        ``PagerOS-Device`` header is ignored entirely so a forged header
        cannot override a verified identity. When ``None``, the helper
        falls back to ``PagerOS-Device`` for cases where the SDK runs
        without the verifier (test fixtures, dev mode).

        ``session`` is forwarded through as the ``session`` field; pass an
        existing dict to share state across handler invocations within a
        request lifecycle. Defaults to a fresh empty dict.

        Malformed values (bad base64 in ``PagerOS-Device``, non-numeric
        location parts, an unknown ``PagerOS-Transport``) are tolerated by
        falling back to the empty-state default — handlers should not
        crash because a device sent a stale header.
        """
        device_id = _device_id_from(verified_device_id, headers)
        transport = _transport_from(headers)
        granted = _csv_from(headers, HEADER_GRANTED)
        groups = _csv_from(headers, HEADER_GROUPS)
        location = _location_from(headers)
        return cls(
            device_id=device_id,
            session=session if session is not None else {},
            transport=transport,
            granted=granted,
            location=location,
            groups=groups,
        )


# --------------------------------------------------------------------------- #
# Header parsing helpers
# --------------------------------------------------------------------------- #


def _get_ci(headers: Mapping[str, str], key: str) -> str | None:
    """Case-insensitive header lookup."""
    if key in headers:
        return headers[key]
    lower = key.lower()
    for k, v in headers.items():
        if k.lower() == lower:
            return v
    return None


def _b64url_encode_nopad(raw: bytes) -> str:
    return base64.urlsafe_b64encode(raw).rstrip(b"=").decode("ascii")


def _device_id_from(
    verified: bytes | None, headers: Mapping[str, str]
) -> str:
    """Return the canonical ``device_id`` string.

    The verified pubkey takes precedence — if the signature middleware
    has run, the only safe identifier is the bytes it returned. We
    re-encode as base64url-no-padding (the wire form used in
    ``PagerOS-Device`` and in push relay URL paths) so handlers see a
    single representation regardless of where the identity came from.
    """
    if verified is not None:
        return _b64url_encode_nopad(verified)
    raw = _get_ci(headers, "PagerOS-Device")
    if not raw:
        return ""
    return raw


def _transport_from(headers: Mapping[str, str]) -> str:
    value = _get_ci(headers, HEADER_TRANSPORT)
    if value is None:
        return TRANSPORT_WIFI
    normalised = value.strip().lower()
    if normalised in TRANSPORTS:
        return normalised
    # Unknown transport names degrade to ``wifi`` rather than raising,
    # so a forward-compatible value (e.g. a future "ble" transport) from
    # a newer device doesn't break the handler on older SDKs.
    return TRANSPORT_WIFI


def _csv_from(
    headers: Mapping[str, str], header_name: str
) -> tuple[str, ...]:
    value = _get_ci(headers, header_name)
    if value is None:
        return ()
    parts = [p.strip() for p in value.split(",")]
    return tuple(p for p in parts if p)


def _location_from(headers: Mapping[str, str]) -> Location | None:
    """Parse the ``PagerOS-Location`` header as ``"lat,lon,ts"``.

    Returns ``None`` for missing or malformed values so handlers can
    treat "no fix" and "bad fix" identically — the device is free to
    omit the header when it has nothing to share.
    """
    value = _get_ci(headers, HEADER_LOCATION)
    if value is None:
        return None
    parts = [p.strip() for p in value.split(",")]
    if len(parts) != 3:
        return None
    try:
        lat = float(parts[0])
        lon = float(parts[1])
        ts = int(parts[2])
    except ValueError:
        return None
    # Silently clamp obviously-invalid fixes rather than raise. SPEC §8.3
    # gives no validation contract, but firmware GPS code can momentarily
    # emit out-of-range values; a None here lets the app render "no fix"
    # cleanly instead of trusting noise.
    if not (-90.0 <= lat <= 90.0) or not (-180.0 <= lon <= 180.0):
        return None
    return Location(lat=lat, lon=lon, ts=ts)
