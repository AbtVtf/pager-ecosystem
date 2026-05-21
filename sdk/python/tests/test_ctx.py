"""Tests for the PY-008 ``ctx`` object.

Acceptance criterion (``TASKS.md``): "All fields populated per spec;
documented." The tests below pin three slices:

* **Field defaults** — instantiating ``Ctx()`` produces the empty-state
  shape SPEC §8.3 implies for an anonymous, just-arrived request.
* **Header parsing** — ``Ctx.from_headers()`` correctly extracts
  ``device_id`` (verified pubkey takes precedence over the raw header),
  ``transport``, ``granted``, ``location``, and ``groups`` from the
  SDK-level header conventions documented in :mod:`pageros.ctx`.
* **Dispatch integration** — handlers receive a ``Request`` whose
  ``ctx`` reflects the inbound request; supplying
  ``verified_device_id`` to :meth:`App.dispatch` flows through to
  ``ctx.device_id`` as base64url.
"""

from __future__ import annotations

import base64

import pytest

from pageros import (
    App,
    Ctx,
    HEADER_GRANTED,
    HEADER_GROUPS,
    HEADER_LOCATION,
    HEADER_TRANSPORT,
    Location,
    Request,
    TRANSPORT_LORA,
    TRANSPORT_WIFI,
    decode_frame,
)


def _b64url_nopad(raw: bytes) -> str:
    return base64.urlsafe_b64encode(raw).rstrip(b"=").decode("ascii")


# --------------------------------------------------------------------------- #
# Field defaults
# --------------------------------------------------------------------------- #


def test_ctx_default_shape() -> None:
    ctx = Ctx()
    assert ctx.device_id == ""
    assert ctx.session == {}
    assert ctx.transport == TRANSPORT_WIFI
    assert ctx.granted == ()
    assert ctx.location is None
    assert ctx.groups == ()


def test_ctx_session_default_is_fresh_dict() -> None:
    """Default ``session`` factory must not share state across instances."""
    a = Ctx()
    a.session["k"] = "v"
    b = Ctx()
    assert b.session == {}


def test_location_is_a_tuple() -> None:
    """SPEC §8.3 types ``location`` as ``(lat, lon, ts) or None`` — the
    NamedTuple form must keep that literal shape."""
    loc = Location(lat=37.5, lon=-122.0, ts=1_700_000_000)
    assert loc == (37.5, -122.0, 1_700_000_000)
    assert loc.lat == 37.5
    assert loc.lon == -122.0
    assert loc.ts == 1_700_000_000


# --------------------------------------------------------------------------- #
# from_headers — device_id
# --------------------------------------------------------------------------- #


def test_from_headers_uses_verified_device_id_over_raw_header() -> None:
    verified = b"\x11" * 32
    ctx = Ctx.from_headers(
        {"PagerOS-Device": "ATTACKER_FORGED_HEADER"},
        verified_device_id=verified,
    )
    assert ctx.device_id == _b64url_nopad(verified)


def test_from_headers_falls_back_to_raw_device_header() -> None:
    raw = _b64url_nopad(b"\x22" * 32)
    ctx = Ctx.from_headers({"PagerOS-Device": raw})
    assert ctx.device_id == raw


def test_from_headers_device_id_is_empty_when_no_source() -> None:
    ctx = Ctx.from_headers({})
    assert ctx.device_id == ""


def test_from_headers_is_case_insensitive() -> None:
    raw = _b64url_nopad(b"\x33" * 32)
    ctx = Ctx.from_headers(
        {"pageros-device": raw, "PAGEROS-TRANSPORT": "lora"}
    )
    assert ctx.device_id == raw
    assert ctx.transport == TRANSPORT_LORA


# --------------------------------------------------------------------------- #
# from_headers — transport
# --------------------------------------------------------------------------- #


def test_from_headers_transport_defaults_to_wifi() -> None:
    assert Ctx.from_headers({}).transport == TRANSPORT_WIFI


def test_from_headers_transport_lora() -> None:
    assert (
        Ctx.from_headers({HEADER_TRANSPORT: "lora"}).transport
        == TRANSPORT_LORA
    )


def test_from_headers_transport_unknown_value_degrades_to_wifi() -> None:
    """Unknown transports (e.g. a future ``ble``) must not crash older SDKs."""
    assert (
        Ctx.from_headers({HEADER_TRANSPORT: "ble"}).transport
        == TRANSPORT_WIFI
    )


def test_from_headers_transport_strips_and_lowercases() -> None:
    assert (
        Ctx.from_headers({HEADER_TRANSPORT: "  LoRa  "}).transport
        == TRANSPORT_LORA
    )


# --------------------------------------------------------------------------- #
# from_headers — granted / groups (comma-separated lists)
# --------------------------------------------------------------------------- #


def test_from_headers_granted_parses_csv() -> None:
    ctx = Ctx.from_headers({HEADER_GRANTED: "location, push,nfc"})
    assert ctx.granted == ("location", "push", "nfc")


def test_from_headers_granted_drops_empty_entries() -> None:
    ctx = Ctx.from_headers({HEADER_GRANTED: "location,,push,"})
    assert ctx.granted == ("location", "push")


def test_from_headers_groups_parses_csv() -> None:
    ctx = Ctx.from_headers({HEADER_GROUPS: "team-blue, team-red"})
    assert ctx.groups == ("team-blue", "team-red")


def test_from_headers_granted_and_groups_default_empty() -> None:
    ctx = Ctx.from_headers({})
    assert ctx.granted == ()
    assert ctx.groups == ()


# --------------------------------------------------------------------------- #
# from_headers — location
# --------------------------------------------------------------------------- #


def test_from_headers_location_parses_triplet() -> None:
    ctx = Ctx.from_headers(
        {HEADER_LOCATION: "37.7749,-122.4194,1700000000"}
    )
    assert ctx.location == Location(
        lat=37.7749, lon=-122.4194, ts=1_700_000_000
    )


def test_from_headers_location_missing_is_none() -> None:
    assert Ctx.from_headers({}).location is None


@pytest.mark.parametrize(
    "raw",
    [
        "37.7749,-122.4194",  # missing ts
        "a,b,c",              # non-numeric
        "91.0,0,1700000000",  # lat out of range
        "0,200,1700000000",   # lon out of range
        ",,",                 # empty parts
    ],
)
def test_from_headers_location_malformed_is_none(raw: str) -> None:
    assert Ctx.from_headers({HEADER_LOCATION: raw}).location is None


# --------------------------------------------------------------------------- #
# from_headers — session passthrough
# --------------------------------------------------------------------------- #


def test_from_headers_session_passthrough() -> None:
    shared = {"user": "alice"}
    ctx = Ctx.from_headers({}, session=shared)
    assert ctx.session is shared


# --------------------------------------------------------------------------- #
# Dispatch integration
# --------------------------------------------------------------------------- #


def test_dispatch_attaches_ctx_to_request() -> None:
    """A handler that pulls ``request.ctx`` sees the parsed values."""
    captured: dict[str, object] = {}

    app = App(name="t")

    @app.handler("/echo")
    def echo(req: Request):
        captured["ctx"] = req.ctx
        return None

    raw = _b64url_nopad(b"\x55" * 32)
    status, _, _ = app.dispatch(
        "POST",
        "/echo",
        headers={
            "PagerOS-Device": raw,
            HEADER_TRANSPORT: "lora",
            HEADER_GRANTED: "location",
            HEADER_GROUPS: "g1,g2",
            HEADER_LOCATION: "10.0,20.0,1700000000",
        },
    )
    assert status == 204

    ctx = captured["ctx"]
    assert isinstance(ctx, Ctx)
    assert ctx.device_id == raw
    assert ctx.transport == TRANSPORT_LORA
    assert ctx.granted == ("location",)
    assert ctx.groups == ("g1", "g2")
    assert ctx.location == Location(lat=10.0, lon=20.0, ts=1_700_000_000)


def test_dispatch_uses_verified_device_id_when_supplied() -> None:
    captured: dict[str, object] = {}

    app = App(name="t")

    @app.screen("/")
    def home(req: Request):
        captured["device_id"] = req.ctx.device_id
        return {"v": 1, "id": "s", "body": []}

    verified = b"\x77" * 32
    app.dispatch(
        "GET",
        "/",
        headers={"PagerOS-Device": "FORGED"},
        verified_device_id=verified,
    )

    assert captured["device_id"] == _b64url_nopad(verified)


def test_dispatch_session_threaded_into_ctx() -> None:
    captured: dict[str, object] = {}

    app = App(name="t")
    shared_session: dict[str, object] = {"hits": 0}

    @app.screen("/")
    def home(req: Request):
        captured["session"] = req.ctx.session
        req.ctx.session["hits"] = req.ctx.session.get("hits", 0) + 1
        return None

    app.dispatch("GET", "/", session=shared_session)
    app.dispatch("GET", "/", session=shared_session)

    assert captured["session"] is shared_session
    assert shared_session["hits"] == 2


def test_zero_arg_handler_still_works() -> None:
    """PY-008 must not break the PY-001 zero-arg handler contract."""
    app = App(name="t")

    @app.screen("/")
    def home():
        return {"v": 1, "id": "s", "body": []}

    status, headers, body = app.dispatch("GET", "/")
    assert status == 200
    assert decode_frame(body)["id"] == "s"
