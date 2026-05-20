#!/usr/bin/env python3
# Generator for PagerOS UI-protocol conformance vectors (PROTO-003).
#
# Self-contained: no external dependencies. Emits canonical, definite-length
# CBOR per RFC 8949 §4.2 (smallest encoding, lexicographic map key order over
# CBOR-encoded keys). The resulting bytes are byte-exact reproducible from
# the JSON descriptor for every vector tagged `encoding: canonical_rfc8949`.
#
# Output layout (relative to this script):
#   index.json               machine-readable list of every vector
#   vectors/<name>.json      JSON descriptor (input + expected metadata)
#   vectors/<name>.cbor      the canonical CBOR bytes
#
# Run from anywhere:  python3 protocol/test-vectors/ui/generate.py

from __future__ import annotations

import json
import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Any

# ---------------------------------------------------------------------------
# Canonical CBOR encoder (RFC 8949 §4.2)
# ---------------------------------------------------------------------------

MT_UINT = 0
MT_NINT = 1
MT_BSTR = 2
MT_TSTR = 3
MT_ARRAY = 4
MT_MAP = 5
MT_TAG = 6
MT_SIMPLE = 7


def _head(mt: int, val: int) -> bytes:
    assert 0 <= mt <= 7
    assert val >= 0
    first = mt << 5
    if val < 24:
        return bytes([first | val])
    if val < 0x100:
        return bytes([first | 24, val])
    if val < 0x10000:
        return bytes([first | 25]) + val.to_bytes(2, "big")
    if val < 0x100000000:
        return bytes([first | 26]) + val.to_bytes(4, "big")
    return bytes([first | 27]) + val.to_bytes(8, "big")


class CborTaggedInt(int):
    """Marker: encode this int as the *t* field of a widget using the numeric
    form (a plain CBOR unsigned int). Identical to a plain int on the wire,
    but in the JSON descriptor we render it as `{"$num": N}` so a reader can
    tell that the value is intentionally a number and not a misspelled tag."""


def encode(v: Any) -> bytes:
    # JSON descriptors carry binary payloads as {"$bytes": "<hex>"} markers
    # so the descriptor stays JSON-serializable. Expand them here.
    if isinstance(v, dict) and set(v.keys()) == {"$bytes"} and isinstance(v["$bytes"], str):
        return encode(bytes.fromhex(v["$bytes"]))
    if v is None:
        return bytes([0xF6])
    if v is True:
        return bytes([0xF5])
    if v is False:
        return bytes([0xF4])
    if isinstance(v, bool):  # pragma: no cover (handled above)
        raise AssertionError
    if isinstance(v, int):
        if v >= 0:
            return _head(MT_UINT, v)
        return _head(MT_NINT, -1 - v)
    if isinstance(v, float):
        # RFC 8949 §4.2.2: prefer the shortest representation that round-trips.
        # For our vectors we only emit floats for lat/lon and known-float
        # payloads, where 32-bit accuracy is sufficient.
        f32 = struct.unpack(">f", struct.pack(">f", v))[0]
        if f32 == v:
            return bytes([0xFA]) + struct.pack(">f", v)
        return bytes([0xFB]) + struct.pack(">d", v)
    if isinstance(v, bytes):
        return _head(MT_BSTR, len(v)) + v
    if isinstance(v, str):
        b = v.encode("utf-8")
        return _head(MT_TSTR, len(b)) + b
    if isinstance(v, list):
        out = _head(MT_ARRAY, len(v))
        for item in v:
            out += encode(item)
        return out
    if isinstance(v, dict):
        # Canonical key sort: encode each key, sort by encoded bytes
        # (lexicographic), then concatenate. RFC 8949 §4.2.1.
        encoded_pairs = [(encode(k), encode(val)) for k, val in v.items()]
        encoded_pairs.sort(key=lambda kv: kv[0])
        out = _head(MT_MAP, len(v))
        for k_bytes, v_bytes in encoded_pairs:
            out += k_bytes + v_bytes
        return out
    raise TypeError(f"unsupported value for CBOR encoding: {type(v).__name__}")


# ---------------------------------------------------------------------------
# Vector dataclass
# ---------------------------------------------------------------------------


@dataclass
class Vector:
    name: str
    category: str
    description: str
    kind: str = "encode"  # encode | decode_only | negative
    tag_form: str | None = None  # "string" | "numeric" | "mixed" | None
    input: Any = None  # logical JSON the SDK would encode (None if decode-only)
    input_cbor_hex: str | None = None  # for decode-only / negative
    expected_decoded: Any = None  # what a decoder MUST yield (decode_only)
    expect_error: str | None = None  # negative: error class / category
    expected_cbor_hex: str | None = None  # populated by build()
    expected_size_bytes: int | None = None
    encoding: str = "canonical_rfc8949"
    notes: str = ""

    def build(self, cbor_bytes: bytes) -> None:
        self.expected_cbor_hex = cbor_bytes.hex()
        self.expected_size_bytes = len(cbor_bytes)

    def to_descriptor(self) -> dict:
        d: dict[str, Any] = {
            "name": self.name,
            "category": self.category,
            "description": self.description,
            "kind": self.kind,
            "encoding": self.encoding,
        }
        if self.tag_form is not None:
            d["tag_form"] = self.tag_form
        if self.input is not None:
            d["input"] = self.input
        if self.input_cbor_hex is not None:
            d["input_cbor_hex"] = self.input_cbor_hex
        if self.expected_decoded is not None:
            d["expected_decoded"] = self.expected_decoded
        if self.expect_error is not None:
            d["expect_error"] = self.expect_error
        if self.expected_cbor_hex is not None:
            d["expected_cbor_hex"] = self.expected_cbor_hex
        if self.expected_size_bytes is not None:
            d["expected_size_bytes"] = self.expected_size_bytes
        if self.notes:
            d["notes"] = self.notes
        return d


# ---------------------------------------------------------------------------
# Widget tag map (mirrors protocol/tag-registry.md §3)
# ---------------------------------------------------------------------------

WIDGET_NUM = {
    "text": 1,
    "list": 2,
    "input": 3,
    "form": 4,
    "button": 5,
    "image": 6,
    "map": 7,
    "notification": 8,
    "presence_list": 9,
    "chat": 10,
}

EVENT_NUM = {
    "nfc_scan": 1,
    "location": 2,
    "back": 3,
    "tick": 4,
    "notification_action": 5,
    "member_joined": 16,
    "member_left": 17,
    "presence_update": 18,
    "group_message": 19,
}


def numeric_form(frame: dict) -> dict:
    """Return a deep copy of `frame` with every widget `t` value replaced by
    its numeric tag (recursing into `body`, `fields`, etc.). Mixed/unknown
    tags are left untouched so callers can spike them in directly."""
    out = json.loads(json.dumps(frame))

    def walk(node: Any) -> Any:
        if isinstance(node, dict):
            if "t" in node and isinstance(node["t"], str) and node["t"] in WIDGET_NUM:
                node["t"] = WIDGET_NUM[node["t"]]
            for k, v in list(node.items()):
                node[k] = walk(v)
            return node
        if isinstance(node, list):
            return [walk(x) for x in node]
        return node

    return walk(out)


# ---------------------------------------------------------------------------
# Vector definitions
# ---------------------------------------------------------------------------


def widget_vectors() -> list[Vector]:
    out: list[Vector] = []

    def both(name: str, desc: str, frame: dict, notes: str = "") -> None:
        out.append(
            Vector(
                name=f"widget_{name}_string",
                category="widget",
                description=desc + " (string tags)",
                tag_form="string",
                input=frame,
                notes=notes,
            )
        )
        out.append(
            Vector(
                name=f"widget_{name}_numeric",
                category="widget",
                description=desc + " (numeric tags)",
                tag_form="numeric",
                input=numeric_form(frame),
                notes=notes,
            )
        )

    # 5.1 text
    both(
        "text_minimal",
        "Minimal text widget",
        {
            "v": 1,
            "id": "scr_text_min",
            "body": [{"t": "text", "s": "Hello, world"}],
        },
    )
    both(
        "text_styled",
        "Text widget with every defined style value, one per body entry",
        {
            "v": 1,
            "id": "scr_text_styles",
            "title": "Styles",
            "body": [
                {"t": "text", "s": "Heading line", "style": "heading"},
                {"t": "text", "s": "Body line", "style": "body"},
                {"t": "text", "s": "Dim line", "style": "dim"},
                {"t": "text", "s": "Mono line", "style": "mono"},
            ],
        },
        notes="Covers spec §5.1: style enum body|heading|dim|mono.",
    )

    # 5.2 list
    both(
        "list_empty",
        "Empty list widget — renders as dim 'empty' placeholder",
        {
            "v": 1,
            "id": "scr_list_empty",
            "body": [{"t": "list", "items": []}],
        },
    )
    both(
        "list_basic",
        "List with two items, one with `sub` and explicit POST method",
        {
            "v": 1,
            "id": "scr_list_basic",
            "body": [
                {
                    "t": "list",
                    "items": [
                        {
                            "label": "Item 1",
                            "href": "/item/1",
                            "sub": "extra info",
                        },
                        {
                            "label": "Item 2",
                            "href": "/item/2",
                            "method": "POST",
                        },
                    ],
                }
            ],
        },
    )

    # 5.3 input
    both(
        "input_text",
        "Standalone text input with max length",
        {
            "v": 1,
            "id": "scr_input_text",
            "body": [
                {
                    "t": "input",
                    "name": "title",
                    "label": "Title",
                    "type": "text",
                    "value": "",
                    "max": 80,
                }
            ],
        },
    )
    both(
        "input_number",
        "Number input — exercises non-default type and integer value",
        {
            "v": 1,
            "id": "scr_input_num",
            "body": [
                {
                    "t": "input",
                    "name": "qty",
                    "label": "Quantity",
                    "type": "number",
                    "value": "1",
                }
            ],
        },
    )

    # 5.4 form
    both(
        "form_simple",
        "Two-field form with default method/submit label",
        {
            "v": 1,
            "id": "scr_form_simple",
            "body": [
                {
                    "t": "form",
                    "action": "/save",
                    "fields": [
                        {"t": "input", "name": "title", "label": "Title"},
                        {"t": "input", "name": "body", "label": "Body"},
                    ],
                }
            ],
        },
    )
    both(
        "form_full",
        "Form with heading text between fields, explicit method and submit",
        {
            "v": 1,
            "id": "scr_form_full",
            "title": "Edit note",
            "body": [
                {
                    "t": "form",
                    "action": "/save",
                    "method": "POST",
                    "submit": "Save",
                    "fields": [
                        {"t": "text", "s": "Note details", "style": "heading"},
                        {
                            "t": "input",
                            "name": "title",
                            "label": "Title",
                            "max": 80,
                        },
                        {
                            "t": "input",
                            "name": "body",
                            "label": "Body",
                            "type": "text",
                        },
                    ],
                }
            ],
        },
    )

    # 5.5 button
    both(
        "button_basic",
        "Plain button with GET",
        {
            "v": 1,
            "id": "scr_btn",
            "body": [
                {"t": "button", "label": "Open", "href": "/open/42"}
            ],
        },
    )
    both(
        "button_confirm",
        "Destructive POST button with confirmation prompt",
        {
            "v": 1,
            "id": "scr_btn_confirm",
            "body": [
                {
                    "t": "button",
                    "label": "Delete",
                    "href": "/delete/42",
                    "method": "POST",
                    "confirm": "Are you sure?",
                }
            ],
        },
    )

    # 5.6 image
    both(
        "image_minimal",
        "Image with just `src`",
        {
            "v": 1,
            "id": "scr_image_min",
            "body": [
                {
                    "t": "image",
                    "src": "img:" + "7f3a" * 16,  # 32-byte hex sha256
                }
            ],
        },
    )
    both(
        "image_full",
        "Image with display dimensions and alt text",
        {
            "v": 1,
            "id": "scr_image_full",
            "body": [
                {
                    "t": "image",
                    "src": "img:" + "ab" * 32,
                    "w": 96,
                    "h": 96,
                    "alt": "logo",
                }
            ],
        },
    )

    # 5.7 map
    both(
        "map_minimal",
        "Map with center only",
        {
            "v": 1,
            "id": "scr_map_min",
            "body": [
                {"t": "map", "lat": 45.5, "lon": -73.6}
            ],
        },
    )
    both(
        "map_with_markers",
        "Map with explicit zoom and two markers",
        {
            "v": 1,
            "id": "scr_map_markers",
            "body": [
                {
                    "t": "map",
                    "lat": 45.5,
                    "lon": -73.6,
                    "zoom": 14,
                    "markers": [
                        {"lat": 45.51, "lon": -73.61, "label": "A"},
                        {"lat": 45.49, "lon": -73.59},
                    ],
                }
            ],
        },
    )

    # 5.8 notification
    both(
        "notification_info",
        "Info-level notification (default level)",
        {
            "v": 1,
            "id": "scr_notif_info",
            "body": [
                {"t": "notification", "s": "Saved."}
            ],
        },
    )
    both(
        "notification_error",
        "Error-level notification with explicit level",
        {
            "v": 1,
            "id": "scr_notif_error",
            "body": [
                {
                    "t": "notification",
                    "level": "error",
                    "s": "Out of stock.",
                }
            ],
        },
    )

    # 5.9 presence_list
    both(
        "presence_list",
        "Presence list with two members, mixed online state",
        {
            "v": 1,
            "id": "scr_presence",
            "subscribe_groups": ["grp_abc"],
            "body": [
                {
                    "t": "presence_list",
                    "group_id": "grp_abc",
                    "members": [
                        {"id": "pk_alice", "name": "alice", "online": True},
                        {"id": "pk_bob", "name": "bob", "online": False},
                    ],
                }
            ],
        },
    )

    # 5.10 chat
    both(
        "chat_with_compose",
        "Chat widget with two messages and an inline composer",
        {
            "v": 1,
            "id": "scr_chat",
            "subscribe_groups": ["grp_abc"],
            "body": [
                {
                    "t": "chat",
                    "group_id": "grp_abc",
                    "messages": [
                        {"from": "alice", "ts": 1716200000, "s": "hi"},
                        {"from": "bob", "ts": 1716200012, "s": "yo"},
                    ],
                    "compose": {"name": "msg", "submit": "/send"},
                }
            ],
        },
    )

    # 5.12 actions
    both(
        "actions_topbar",
        "Frame with two top-bar actions",
        {
            "v": 1,
            "id": "scr_actions",
            "title": "Notes",
            "body": [{"t": "text", "s": "Welcome"}],
            "actions": [
                {"label": "New", "key": "n", "href": "/new"},
                {
                    "label": "Refresh",
                    "key": "r",
                    "href": "/",
                    "method": "GET",
                },
            ],
        },
    )

    # Full-screen mixed example — many widgets in one frame, plus subscribe
    both(
        "frame_full_kitchen_sink",
        "End-to-end Frame: title, multi-widget body, actions, subscribe set",
        {
            "v": 1,
            "id": "scr_full",
            "ttl": 60,
            "title": "Home",
            "body": [
                {"t": "text", "s": "Welcome to PagerOS", "style": "heading"},
                {
                    "t": "list",
                    "items": [
                        {"label": "Notes", "href": "/notes"},
                        {"label": "Map", "href": "/map"},
                    ],
                },
                {"t": "button", "label": "Sync", "href": "/sync", "method": "POST"},
            ],
            "actions": [{"label": "Help", "key": "h", "href": "/help"}],
            "subscribe": ["nfc_scan", "location"],
            "meta": {"tick_seconds": 30},
        },
    )

    # §6.2.3 subscribe_groups Frame field — explicit empty array.
    # Spec MUST: encoders MAY omit an empty array; this vector pins the
    # on-wire shape for producers that choose to emit it explicitly so
    # decoders treat it as equivalent to absent.
    both(
        "frame_subscribe_groups_empty",
        "Frame with explicit empty subscribe_groups array (no active groups)",
        {
            "v": 1,
            "id": "scr_no_groups",
            "subscribe_groups": [],
            "body": [{"t": "text", "s": "no groups subscribed"}],
        },
        notes=(
            "Spec §6.2.3: encoders MAY omit an empty array. Decoders MUST "
            "treat absence and `[]` as equivalent (no subscriptions)."
        ),
    )

    # §6.2.3 subscribe_groups Frame field — two distinct groups, with
    # multi-device widgets in `body` bound to each. Exercises the
    # "multiple widgets bound to different group_id" assertion in §6.2.1.
    both(
        "frame_subscribe_groups_multi",
        "Frame subscribing to two groups with one chat + one presence_list",
        {
            "v": 1,
            "id": "scr_two_groups",
            "subscribe_groups": ["grp_abc", "grp_xyz"],
            "body": [
                {
                    "t": "chat",
                    "group_id": "grp_abc",
                    "messages": [
                        {"from": "alice", "ts": 1716200000, "s": "hi"},
                    ],
                    "compose": {"name": "msg", "submit": "/send"},
                },
                {
                    "t": "presence_list",
                    "group_id": "grp_xyz",
                    "members": [
                        {"id": "pk_alice", "online": True},
                        {"id": "pk_bob", "online": False},
                    ],
                },
            ],
        },
        notes=(
            "Spec §6.2.1: a Screen MAY contain multiple multi-device "
            "widgets bound to different `group_id`s. §6.2.3: "
            "`subscribe_groups` entries are tstr; the array has no "
            "numeric-tag form."
        ),
    )

    # §6.2.3 subscribe_groups Frame field — mixed with §6.1 `subscribe`.
    # Pins that the two arrays coexist independently in one Frame.
    both(
        "frame_subscribe_groups_with_subscribe",
        "Frame populating both subscribe and subscribe_groups simultaneously",
        {
            "v": 1,
            "id": "scr_mixed_subs",
            "subscribe": ["nfc_scan", "back"],
            "subscribe_groups": ["grp_abc"],
            "body": [
                {
                    "t": "chat",
                    "group_id": "grp_abc",
                    "messages": [],
                    "compose": {"name": "msg", "submit": "/send"},
                },
            ],
        },
        notes=(
            "Spec §6.2.3: `subscribe` (event-tag registry) and "
            "`subscribe_groups` (opaque group ids) are independent "
            "namespaces; a Frame MAY populate both."
        ),
    )

    return out


def event_vectors() -> list[Vector]:
    """Cover every device→app event payload shape (§6.1) and every server→
    device group event shape (§6.2). For each, both string and numeric
    `type` discriminators."""
    out: list[Vector] = []

    def pair(name: str, desc: str, type_str: str, payload: dict, notes: str = "") -> None:
        out.append(
            Vector(
                name=f"event_{name}_string",
                category="event",
                description=desc + " (string type)",
                tag_form="string",
                input={"type": type_str, "payload": payload},
                notes=notes,
            )
        )
        out.append(
            Vector(
                name=f"event_{name}_numeric",
                category="event",
                description=desc + " (numeric type)",
                tag_form="numeric",
                input={"type": EVENT_NUM[type_str], "payload": payload},
                notes=notes,
            )
        )

    # Device → app
    pair(
        "nfc_scan",
        "NFC scan event",
        "nfc_scan",
        {
            "uid": {"$bytes": "04a1b2c3d4e5f6"},
            "records": [{"type": "U", "uri": "https://example.com"}],
        },
    )
    pair(
        "location",
        "GPS fix update event",
        "location",
        {"lat": 45.5, "lon": -73.6, "accuracy": 12.0},
    )
    pair("back", "BACK key event (empty payload)", "back", {})
    pair("tick", "Periodic tick event (empty payload)", "tick", {})
    pair(
        "notification_action",
        "User taps a notification",
        "notification_action",
        {"id": "notif_42"},
    )

    # Server → device group events
    pair(
        "member_joined",
        "Group member joined event",
        "member_joined",
        {
            "group_id": "grp_abc",
            "member": {"id": "pk_carol", "name": "carol"},
        },
    )
    pair(
        "member_left",
        "Group member left event",
        "member_left",
        {"group_id": "grp_abc", "member_id": "pk_bob"},
    )
    pair(
        "presence_update",
        "Bulk presence update event",
        "presence_update",
        {
            "group_id": "grp_abc",
            "members": [
                {"id": "pk_alice", "online": True},
                {"id": "pk_bob", "online": False},
            ],
        },
    )
    pair(
        "group_message",
        "New chat message event",
        "group_message",
        {
            "group_id": "grp_abc",
            "from": "alice",
            "ts": 1716200030,
            "s": "back soon",
        },
    )

    return out


def error_vectors() -> list[Vector]:
    """Error response frames per spec §7.4 and the local-error shape §7.4.1.
    These are all valid Frames — the test runner verifies SDKs can build
    them and that decoders accept them."""
    out: list[Vector] = []

    def err(name: str, desc: str, frame: dict, notes: str = "") -> None:
        out.append(
            Vector(
                name=f"error_{name}",
                category="error",
                description=desc,
                tag_form="string",
                input=frame,
                notes=notes,
            )
        )

    err(
        "bad_request_400",
        "Server-provided body for HTTP 400 (Bad request)",
        {
            "v": 1,
            "id": "err_400",
            "body": [{"t": "text", "s": "Bad request"}],
        },
        notes="Server MAY return a single-widget Frame body on 4xx/5xx.",
    )
    err(
        "unauthorized_401",
        "Sign-in required frame after second 401",
        {
            "v": 1,
            "id": "err_401",
            "title": "Sign-in",
            "body": [{"t": "text", "s": "Sign-in required"}],
        },
    )
    err(
        "forbidden_403",
        "Forbidden frame with descriptive text",
        {
            "v": 1,
            "id": "err_403",
            "body": [{"t": "text", "s": "You do not have permission."}],
        },
    )
    err(
        "not_found_404",
        "Plain 404 response frame",
        {
            "v": 1,
            "id": "err_404",
            "body": [{"t": "text", "s": "Not found"}],
        },
    )
    err(
        "gone_410",
        "Gone response — evicts cache for this screen id",
        {
            "v": 1,
            "id": "err_410",
            "body": [{"t": "text", "s": "Removed"}],
        },
        notes="Device evicts cached Screens for this (app, screen_id) on 410.",
    )
    err(
        "rate_limited_429",
        "Rate-limited response with transient notification",
        {
            "v": 1,
            "id": "err_429",
            "body": [
                {
                    "t": "notification",
                    "level": "warn",
                    "s": "Slow down — try again shortly.",
                }
            ],
        },
    )
    err(
        "server_error_5xx",
        "Generic 5xx response body",
        {
            "v": 1,
            "id": "err_500",
            "body": [{"t": "text", "s": "Server error"}],
        },
    )
    err(
        "local_error_frame",
        "Locally-generated error frame (no usable server response), per §7.4.1",
        {
            "v": 1,
            "id": "err_local",
            "ttl": 0,
            "title": "Error",
            "body": [
                {
                    "t": "text",
                    "s": "Network unreachable",
                    "style": "error",
                }
            ],
        },
        notes=(
            "style: 'error' is renderer-only. Servers MUST NOT emit it; "
            "the local renderer uses it to flag a self-built error frame."
        ),
    )
    return out


def oversized_vectors() -> list[Vector]:
    """Frames that intentionally exceed the 200-byte LoRa budget — SDKs MUST
    surface a warning (PY-011 hook) while still producing valid CBOR. We
    cover one chat-heavy, one list-heavy, and one form-heavy oversize case."""
    out: list[Vector] = []

    long_messages = [
        {
            "from": "alice",
            "ts": 1716200000 + i,
            "s": "Lorem ipsum dolor sit amet, consectetur adipiscing.",
        }
        for i in range(8)
    ]
    out.append(
        Vector(
            name="oversized_chat",
            category="oversized",
            description="Chat frame with 8 ~50-byte messages — far over the 200B LoRa budget",
            tag_form="string",
            input={
                "v": 1,
                "id": "scr_over_chat",
                "subscribe_groups": ["grp_big"],
                "body": [
                    {
                        "t": "chat",
                        "group_id": "grp_big",
                        "messages": long_messages,
                    }
                ],
            },
            notes="SDK MUST emit a LoRa size warning (PY-011 hook) for this Frame.",
        )
    )

    out.append(
        Vector(
            name="oversized_list",
            category="oversized",
            description="List frame with 24 items — exceeds the 200B LoRa budget",
            tag_form="string",
            input={
                "v": 1,
                "id": "scr_over_list",
                "body": [
                    {
                        "t": "list",
                        "items": [
                            {
                                "label": f"Item {i:02d}",
                                "href": f"/item/{i}",
                                "sub": "secondary info line",
                            }
                            for i in range(24)
                        ],
                    }
                ],
            },
            notes="SDK MUST emit a LoRa size warning for this Frame.",
        )
    )

    out.append(
        Vector(
            name="oversized_form",
            category="oversized",
            description="Form with 12 fields plus inline heading text",
            tag_form="string",
            input={
                "v": 1,
                "id": "scr_over_form",
                "title": "Survey",
                "body": [
                    {
                        "t": "form",
                        "action": "/survey",
                        "method": "POST",
                        "submit": "Submit",
                        "fields": (
                            [
                                {
                                    "t": "text",
                                    "s": "Please answer all questions",
                                    "style": "heading",
                                }
                            ]
                            + [
                                {
                                    "t": "input",
                                    "name": f"q{i:02d}",
                                    "label": f"Question {i:02d}",
                                    "max": 60,
                                }
                                for i in range(12)
                            ]
                        ),
                    }
                ],
            },
            notes="SDK MUST emit a LoRa size warning for this Frame.",
        )
    )

    return out


def forward_compat_vectors() -> list[Vector]:
    """Forward-compat / unknown-widget / unknown-event coverage. Most of
    these are decode-only — they don't correspond to anything an SDK should
    emit, but every conformant decoder MUST handle them gracefully."""
    out: list[Vector] = []

    # A frame with a known text widget followed by an unknown widget at a
    # numeric tag that lies in the v1.x reserved single-byte slot range.
    frame = {
        "v": 1,
        "id": "fwd_v1x_unknown_widget",
        "body": [
            {"t": "text", "s": "Hello"},
            {"t": 11, "x": 1},  # numeric tag 11 — reserved v1.x
        ],
    }
    raw = encode(frame)
    out.append(
        Vector(
            name="forward_unknown_widget_v1x_numeric",
            category="forward_compat",
            description="Body contains an unknown numeric widget tag in the v1.x reserved range (11)",
            tag_form="mixed",
            kind="decode_only",
            input_cbor_hex=raw.hex(),
            expected_decoded={
                "v": 1,
                "id": "fwd_v1x_unknown_widget",
                "body": [
                    {"t": "text", "s": "Hello"},
                    {
                        "$render_placeholder": "[unsupported: 11]",
                        "raw_widget": {"t": 11, "x": 1},
                    },
                ],
            },
            notes="Renderer MUST render '[unsupported: 11]' and continue rendering the rest of body (spec §4.2).",
        )
    )

    # Pre-allocated v2 widget tag (24, "chart") — v1 device MUST treat as unknown.
    frame = {
        "v": 1,
        "id": "fwd_v2_chart",
        "body": [{"t": 24, "data": [1, 2, 3, 4, 5]}],
    }
    raw = encode(frame)
    out.append(
        Vector(
            name="forward_unknown_widget_v2_numeric",
            category="forward_compat",
            description="Body contains a pre-allocated v2 widget (tag 24, 'chart') — v1 MUST render as unsupported",
            tag_form="numeric",
            kind="decode_only",
            input_cbor_hex=raw.hex(),
            expected_decoded={
                "v": 1,
                "id": "fwd_v2_chart",
                "body": [
                    {
                        "$render_placeholder": "[unsupported: 24]",
                        "raw_widget": {"t": 24, "data": [1, 2, 3, 4, 5]},
                    }
                ],
            },
            notes="v1 servers MUST NOT emit tags 24-29 (spec §5.11); v1 renderers MUST treat them as unknown.",
        )
    )

    # On-device runtime range (64-127).
    frame = {
        "v": 1,
        "id": "fwd_runtime_range",
        "body": [{"t": 64, "blob": {"$bytes": "010203"}}],
    }
    raw = encode(frame)
    out.append(
        Vector(
            name="forward_unknown_widget_runtime_numeric",
            category="forward_compat",
            description="Widget tag 64 — reserved for on-device runtime (registry §5.1)",
            tag_form="numeric",
            kind="decode_only",
            input_cbor_hex=raw.hex(),
            expected_decoded={
                "v": 1,
                "id": "fwd_runtime_range",
                "body": [
                    {
                        "$render_placeholder": "[unsupported: 64]",
                        "raw_widget": {
                            "t": 64,
                            "blob": "$bytes:010203",
                        },
                    }
                ],
            },
            notes="Tags 64-127 are reserved for the v2 on-device runtime; v1 implementations MUST decode but treat as unknown.",
        )
    )

    # Vendor / experimental range (128-255).
    frame = {
        "v": 1,
        "id": "fwd_vendor",
        "body": [{"t": 200, "vendor": "acme"}],
    }
    raw = encode(frame)
    out.append(
        Vector(
            name="forward_unknown_widget_vendor_numeric",
            category="forward_compat",
            description="Widget tag 200 — vendor/experimental range",
            tag_form="numeric",
            kind="decode_only",
            input_cbor_hex=raw.hex(),
            expected_decoded={
                "v": 1,
                "id": "fwd_vendor",
                "body": [
                    {
                        "$render_placeholder": "[unsupported: 200]",
                        "raw_widget": {"t": 200, "vendor": "acme"},
                    }
                ],
            },
            notes="Tags 128-255 are vendor/experimental. Producers MUST NOT publish to the Marketplace; consumers MUST render as unsupported.",
        )
    )

    # String form unknown widget.
    frame = {
        "v": 1,
        "id": "fwd_string_unknown",
        "body": [
            {"t": "text", "s": "before"},
            {"t": "futurewidget", "data": "x"},
            {"t": "text", "s": "after"},
        ],
    }
    raw = encode(frame)
    out.append(
        Vector(
            name="forward_unknown_widget_string",
            category="forward_compat",
            description="Body contains an unknown string-form widget tag between two known widgets",
            tag_form="string",
            kind="decode_only",
            input_cbor_hex=raw.hex(),
            expected_decoded={
                "v": 1,
                "id": "fwd_string_unknown",
                "body": [
                    {"t": "text", "s": "before"},
                    {
                        "$render_placeholder": "[unsupported: futurewidget]",
                        "raw_widget": {"t": "futurewidget", "data": "x"},
                    },
                    {"t": "text", "s": "after"},
                ],
            },
            notes="Renderer MUST emit '[unsupported: futurewidget]' and continue.",
        )
    )

    # Unknown top-level Frame field.
    frame = {
        "v": 1,
        "id": "fwd_unknown_top_field",
        "title": "OK",
        "body": [{"t": "text", "s": "Hi"}],
        "unknown_future_field": {"hello": "world"},
    }
    raw = encode(frame)
    out.append(
        Vector(
            name="forward_unknown_top_level_field",
            category="forward_compat",
            description="Frame contains an unknown top-level field — MUST be silently ignored",
            tag_form="string",
            kind="decode_only",
            input_cbor_hex=raw.hex(),
            expected_decoded={
                "v": 1,
                "id": "fwd_unknown_top_field",
                "title": "OK",
                "body": [{"t": "text", "s": "Hi"}],
            },
            notes="Spec §9.2: renderers MUST silently ignore unknown top-level Frame fields.",
        )
    )

    # Unknown field inside a known widget.
    frame = {
        "v": 1,
        "id": "fwd_unknown_widget_field",
        "body": [
            {
                "t": "text",
                "s": "Greetings",
                "future_style": "rainbow",  # unknown widget-level field
            }
        ],
    }
    raw = encode(frame)
    out.append(
        Vector(
            name="forward_unknown_widget_field",
            category="forward_compat",
            description="A known widget carries an unknown field — MUST be silently ignored",
            tag_form="string",
            kind="decode_only",
            input_cbor_hex=raw.hex(),
            expected_decoded={
                "v": 1,
                "id": "fwd_unknown_widget_field",
                "body": [{"t": "text", "s": "Greetings"}],
            },
            notes="Spec §9.2: renderers MUST silently ignore unknown widget fields.",
        )
    )

    # Unknown event in a subscribe array, mixed with known events (numeric +
    # string forms).
    frame = {
        "v": 1,
        "id": "fwd_unknown_event_in_subscribe",
        "body": [{"t": "text", "s": "ok"}],
        "subscribe": ["nfc_scan", "future_event", 7, 2],
    }
    raw = encode(frame)
    out.append(
        Vector(
            name="forward_unknown_event_in_subscribe",
            category="forward_compat",
            description="Subscribe array mixes known and unknown event tags in both string and numeric form",
            tag_form="mixed",
            kind="decode_only",
            input_cbor_hex=raw.hex(),
            expected_decoded={
                "v": 1,
                "id": "fwd_unknown_event_in_subscribe",
                "body": [{"t": "text", "s": "ok"}],
                "subscribe_effective": ["nfc_scan", 2],
                "subscribe_dropped": ["future_event", 7],
            },
            notes=(
                "Spec §6.3: unknown event tags in subscribe MUST be silently "
                "ignored. Tag 7 is in the reserved 6-15 device-event range. "
                "After filtering, the device subscribes to nfc_scan + location (2)."
            ),
        )
    )

    # Unknown event payload arriving server→device.
    payload = {"type": "future_group_event", "payload": {"x": 1}}
    raw = encode(payload)
    out.append(
        Vector(
            name="forward_unknown_event_payload",
            category="forward_compat",
            description="Inbound server→device event with unknown type — device MUST drop",
            tag_form="string",
            kind="negative",
            input_cbor_hex=raw.hex(),
            expect_error="unknown_event_dropped",
            notes="Spec §6.3: inbound events with unknown type are dropped (not surfaced as user errors).",
        )
    )

    # Negative: unknown major protocol version — device MUST reject with the
    # local error frame (§7.4 v entry).
    frame = {
        "v": 99,
        "id": "scr_future",
        "body": [{"t": "text", "s": "from v99"}],
    }
    raw = encode(frame)
    out.append(
        Vector(
            name="negative_unknown_major_version",
            category="forward_compat",
            description="Frame declares an unknown major version — device MUST reject",
            tag_form="string",
            kind="negative",
            input_cbor_hex=raw.hex(),
            expect_error="unknown_major_version",
            notes="Spec §4.1.1 / §9.1: devices MUST reject unknown major versions with a graceful error frame.",
        )
    )

    # Negative: not a CBOR map at all (e.g., an array at top-level).
    raw = encode([1, 2, 3])
    out.append(
        Vector(
            name="negative_top_level_not_a_map",
            category="forward_compat",
            description="Top-level CBOR value is an array, not a map — invalid Frame",
            tag_form="string",
            kind="negative",
            input_cbor_hex=raw.hex(),
            expect_error="invalid_frame_shape",
            notes="A Frame MUST be a CBOR map (spec §4.1).",
        )
    )

    return out


# ---------------------------------------------------------------------------
# Build + write
# ---------------------------------------------------------------------------


def main() -> None:
    here = Path(__file__).resolve().parent
    out_dir = here / "vectors"
    out_dir.mkdir(exist_ok=True)
    # Clean any stale outputs so removed vectors do not linger.
    for p in out_dir.iterdir():
        if p.is_file() and p.suffix in (".json", ".cbor"):
            p.unlink()

    vectors: list[Vector] = []
    vectors += widget_vectors()
    vectors += event_vectors()
    vectors += error_vectors()
    vectors += oversized_vectors()
    vectors += forward_compat_vectors()

    for v in vectors:
        if v.kind == "encode":
            if v.input is None:
                raise ValueError(f"{v.name}: encode vector requires `input`")
            cbor_bytes = encode(v.input)
        elif v.kind in ("decode_only", "negative"):
            if v.input_cbor_hex is None:
                raise ValueError(f"{v.name}: {v.kind} vector requires `input_cbor_hex`")
            cbor_bytes = bytes.fromhex(v.input_cbor_hex)
        else:
            raise ValueError(f"{v.name}: unknown kind {v.kind}")
        v.build(cbor_bytes)
        # Write .cbor next to .json descriptor.
        (out_dir / f"{v.name}.cbor").write_bytes(cbor_bytes)
        (out_dir / f"{v.name}.json").write_text(
            json.dumps(v.to_descriptor(), indent=2, sort_keys=True) + "\n"
        )

    # Index.
    index = {
        "version": "1.0",
        "spec": "protocol/spec.md (PROTO-002), protocol/tag-registry.md (PROTO-001)",
        "encoding": "canonical_rfc8949",
        "generator": "protocol/test-vectors/ui/generate.py",
        "categories": sorted({v.category for v in vectors}),
        "kinds": sorted({v.kind for v in vectors}),
        "count": len(vectors),
        "vectors": [
            {
                "name": v.name,
                "category": v.category,
                "kind": v.kind,
                "tag_form": v.tag_form,
                "description": v.description,
                "size_bytes": v.expected_size_bytes,
                "files": {
                    "descriptor": f"vectors/{v.name}.json",
                    "cbor": f"vectors/{v.name}.cbor",
                },
            }
            for v in vectors
        ],
    }
    (here / "index.json").write_text(json.dumps(index, indent=2) + "\n")

    print(f"wrote {len(vectors)} vectors to {out_dir}")
    by_cat: dict[str, int] = {}
    for v in vectors:
        by_cat[v.category] = by_cat.get(v.category, 0) + 1
    for cat, n in sorted(by_cat.items()):
        print(f"  {cat:15s} {n}")


def verify() -> int:
    """Re-encode every encode-kind descriptor and confirm byte equality with
    the sibling .cbor file. Treat decode_only/negative vectors as opaque:
    only check that the descriptor's input_cbor_hex matches the .cbor file
    bytes (no re-encoding pass)."""
    here = Path(__file__).resolve().parent
    vec_dir = here / "vectors"
    if not vec_dir.is_dir():
        print(f"verify: no vectors dir at {vec_dir}")
        return 1
    failures: list[str] = []
    checked = 0
    for json_path in sorted(vec_dir.glob("*.json")):
        descriptor = json.loads(json_path.read_text())
        cbor_path = vec_dir / f"{descriptor['name']}.cbor"
        if not cbor_path.exists():
            failures.append(f"{descriptor['name']}: missing .cbor sibling")
            continue
        on_disk = cbor_path.read_bytes()
        if descriptor.get("expected_cbor_hex") != on_disk.hex():
            failures.append(
                f"{descriptor['name']}: descriptor hex != .cbor bytes"
            )
            continue
        if descriptor["kind"] == "encode":
            try:
                re_encoded = encode(descriptor["input"])
            except Exception as exc:  # noqa: BLE001
                failures.append(f"{descriptor['name']}: re-encode raised {exc}")
                continue
            if re_encoded != on_disk:
                failures.append(
                    f"{descriptor['name']}: re-encode mismatch "
                    f"({re_encoded.hex()} vs {on_disk.hex()})"
                )
                continue
        elif descriptor["kind"] in ("decode_only", "negative"):
            if descriptor.get("input_cbor_hex") != on_disk.hex():
                failures.append(
                    f"{descriptor['name']}: input_cbor_hex != .cbor bytes"
                )
                continue
        else:
            failures.append(
                f"{descriptor['name']}: unknown kind {descriptor['kind']}"
            )
            continue
        checked += 1
    if failures:
        print(f"verify: {len(failures)} failure(s) of {checked + len(failures)} vectors:")
        for f in failures:
            print(f"  - {f}")
        return 1
    print(f"verify: all {checked} vectors round-trip cleanly")
    return 0


if __name__ == "__main__":
    import sys

    if len(sys.argv) >= 2 and sys.argv[1] == "--verify":
        sys.exit(verify())
    main()
