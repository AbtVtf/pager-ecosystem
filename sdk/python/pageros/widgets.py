"""Idiomatic widget builders (PY-003).

Pythonic constructors for every v1 widget in ``SPEC.md`` §5.3 plus the
top-level :class:`Screen` Frame builder (§5.2). Every builder is a
frozen dataclass that validates its inputs at construction time and
emits a canonical Frame-shaped ``dict`` via :meth:`Widget.to_dict`.

Handlers may return either a :class:`Screen` (the typed path — best for
LSP help and refactor safety) or a plain ``dict`` (the escape hatch
used by tests and quick scripts). :class:`pageros.App` recursively
flattens widget instances before CBOR encoding so either form works at
every level of nesting — e.g. a ``Form`` may hold ``Input`` instances,
which may themselves be returned bare from a handler.

Tag encoding:

* Default ``to_dict()`` emits the **string** widget tag (``"t":
  "text"``). This is the canonical encoding for Wi-Fi transport
  (``protocol/tag-registry.md`` §2.1) and matches every string-form
  test vector under ``protocol/test-vectors/ui/``.
* ``to_dict(numeric=True)`` emits the **numeric** tag (``"t": 1``).
  LoRa-targeted apps should prefer this form — it shaves bytes per
  widget map (registry §2.3) and keeps Frames closer to the 200 B
  budget enforced by :func:`pageros.lora_budget.check_frame_size`.

Optional fields are omitted from the emitted dict when set to ``None``
or left at their dataclass default. This keeps the wire form minimal
and matches the canonical test-vector shape (e.g. a default-level
notification serialises *without* a ``level`` key).
"""

from __future__ import annotations

import abc
from dataclasses import dataclass, field
from typing import Any, Iterable, Sequence

__all__ = [
    "WIDGET_TAGS",
    "EVENT_TAGS",
    "Button",
    "Chat",
    "ChatCompose",
    "ChatMessage",
    "Form",
    "Image",
    "Input",
    "List",
    "ListItem",
    "Map",
    "MapMarker",
    "Notification",
    "PresenceList",
    "PresenceMember",
    "Screen",
    "Text",
    "Widget",
    "to_frame_dict",
]


# Widget tag registry (protocol/tag-registry.md §3). Pinned to v1.0 of
# the registry so the SDK and the conformance runner agree on numeric
# fallbacks for LoRa-bound Frames.
WIDGET_TAGS: dict[str, int] = {
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

# Event tag registry (§4). Kept here so callers building manual event
# payloads or `subscribe` arrays don't need to crack open the registry
# file at runtime.
EVENT_TAGS: dict[str, int] = {
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


# Validation enums — pinned to SPEC.md §5.3.
_TEXT_STYLES = ("body", "heading", "dim", "mono")
_INPUT_TYPES = ("text", "password", "number", "email")
_NOTIFICATION_LEVELS = ("info", "warn", "error")
_HTTP_METHODS = ("GET", "POST")

# Frame field bounds. ``ttl == 0`` is allowed and explicitly means "do
# not cache" per SPEC §5.5; we therefore only reject negative values.
_MAX_LATITUDE = 90.0
_MAX_LONGITUDE = 180.0


# --------------------------------------------------------------------------- #
# Widget base
# --------------------------------------------------------------------------- #


class Widget(abc.ABC):
    """Common base for every widget builder.

    Subclasses are frozen dataclasses; they override :meth:`_tag` (the
    string form of their widget id) and :meth:`_to_payload` (the dict
    body sans the ``t`` field). :meth:`to_dict` wraps the two with the
    string-or-numeric tag selection.
    """

    @abc.abstractmethod
    def _tag(self) -> str:  # pragma: no cover - abstract
        """Return the canonical string tag (e.g. ``"text"``)."""

    @abc.abstractmethod
    def _to_payload(self, *, numeric: bool) -> dict[str, Any]:  # pragma: no cover
        """Return the dict body excluding the ``t`` field."""

    def to_dict(self, *, numeric: bool = False) -> dict[str, Any]:
        tag: Any = WIDGET_TAGS[self._tag()] if numeric else self._tag()
        payload = self._to_payload(numeric=numeric)
        # Sort-stability: we always insert ``t`` first; the canonical
        # CBOR encoder sorts keys by encoded bytes anyway, so emit order
        # only matters for human-readable debug output of the dict.
        return {"t": tag, **payload}


# --------------------------------------------------------------------------- #
# Helpers
# --------------------------------------------------------------------------- #


def _validate_enum(value: str, allowed: Sequence[str], *, field_name: str) -> None:
    if value not in allowed:
        raise ValueError(
            f"{field_name} must be one of {sorted(allowed)!r}, got {value!r}"
        )


def _validate_non_empty(value: str, *, field_name: str) -> None:
    if not isinstance(value, str) or not value:
        raise ValueError(f"{field_name} must be a non-empty string")


def to_frame_dict(value: Any, *, numeric: bool = False) -> Any:
    """Recursively flatten widget instances into plain dicts.

    Plain ``dict`` / ``list`` / ``tuple`` containers are walked so
    widgets can appear at any depth inside a handler return value.
    Scalars (str/int/float/bool/bytes/None) pass through unchanged.
    The recursion stops at any other type — keeps the helper safe to
    apply to opaque payloads (e.g. CBOR-encoded sub-frames).
    """
    if isinstance(value, Widget):
        return to_frame_dict(value.to_dict(numeric=numeric), numeric=numeric)
    if isinstance(value, Screen):
        return value.to_dict(numeric=numeric)
    if isinstance(value, dict):
        return {k: to_frame_dict(v, numeric=numeric) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [to_frame_dict(v, numeric=numeric) for v in value]
    return value


# --------------------------------------------------------------------------- #
# Leaf widgets
# --------------------------------------------------------------------------- #


@dataclass(frozen=True)
class Text(Widget):
    """``text`` widget (SPEC §5.3.1)."""

    s: str
    style: str = "body"

    def __post_init__(self) -> None:
        _validate_non_empty(self.s, field_name="Text.s")
        _validate_enum(self.style, _TEXT_STYLES, field_name="Text.style")

    def _tag(self) -> str:
        return "text"

    def _to_payload(self, *, numeric: bool) -> dict[str, Any]:
        out: dict[str, Any] = {"s": self.s}
        if self.style != "body":
            # `body` is the default; omit on the wire to save bytes
            # (matches widget_text_minimal_string vectors).
            out["style"] = self.style
        return out


@dataclass(frozen=True)
class ListItem:
    """One row of a :class:`List` widget.

    Not a :class:`Widget` itself — list items have no ``t`` discriminator
    on the wire (their parent's ``items`` array dictates shape).
    """

    label: str
    href: str | None = None
    sub: str | None = None
    method: str | None = None

    def __post_init__(self) -> None:
        _validate_non_empty(self.label, field_name="ListItem.label")
        if self.method is not None:
            _validate_enum(
                self.method.upper(), _HTTP_METHODS, field_name="ListItem.method"
            )

    def to_dict(self, *, numeric: bool = False) -> dict[str, Any]:
        out: dict[str, Any] = {"label": self.label}
        if self.href is not None:
            out["href"] = self.href
        if self.sub is not None:
            out["sub"] = self.sub
        if self.method is not None:
            out["method"] = self.method.upper()
        return out


@dataclass(frozen=True)
class List(Widget):
    """``list`` widget (SPEC §5.3.2)."""

    items: Sequence[ListItem | dict[str, Any]] = field(default_factory=tuple)

    def __post_init__(self) -> None:
        # Coerce to tuple so the dataclass stays hashable and we keep
        # the same object identity for repeated ``to_dict`` calls.
        coerced = tuple(self.items)
        object.__setattr__(self, "items", coerced)

    def _tag(self) -> str:
        return "list"

    def _to_payload(self, *, numeric: bool) -> dict[str, Any]:
        items_out: list[dict[str, Any]] = []
        for item in self.items:
            if isinstance(item, ListItem):
                items_out.append(item.to_dict(numeric=numeric))
            elif isinstance(item, dict):
                items_out.append(item)
            else:
                raise TypeError(
                    "List.items entries must be ListItem or dict, "
                    f"got {type(item).__name__}"
                )
        return {"items": items_out}


@dataclass(frozen=True)
class Input(Widget):
    """``input`` widget (SPEC §5.3.3)."""

    name: str
    label: str | None = None
    type: str = "text"
    value: Any = None
    max: int | None = None

    def __post_init__(self) -> None:
        _validate_non_empty(self.name, field_name="Input.name")
        _validate_enum(self.type, _INPUT_TYPES, field_name="Input.type")
        if self.max is not None and (
            not isinstance(self.max, int) or self.max <= 0
        ):
            raise ValueError("Input.max must be a positive integer")

    def _tag(self) -> str:
        return "input"

    def _to_payload(self, *, numeric: bool) -> dict[str, Any]:
        out: dict[str, Any] = {"name": self.name, "type": self.type}
        if self.label is not None:
            out["label"] = self.label
        if self.value is not None:
            out["value"] = self.value
        if self.max is not None:
            out["max"] = self.max
        return out


@dataclass(frozen=True)
class Form(Widget):
    """``form`` widget (SPEC §5.3.4)."""

    action: str
    fields: Sequence[Widget | dict[str, Any]] = field(default_factory=tuple)
    method: str = "POST"
    submit: str | None = None

    def __post_init__(self) -> None:
        _validate_non_empty(self.action, field_name="Form.action")
        _validate_enum(
            self.method.upper(), _HTTP_METHODS, field_name="Form.method"
        )
        object.__setattr__(self, "method", self.method.upper())
        object.__setattr__(self, "fields", tuple(self.fields))

    def _tag(self) -> str:
        return "form"

    def _to_payload(self, *, numeric: bool) -> dict[str, Any]:
        fields_out: list[dict[str, Any]] = []
        for fld in self.fields:
            if isinstance(fld, Widget):
                fields_out.append(fld.to_dict(numeric=numeric))
            elif isinstance(fld, dict):
                fields_out.append(fld)
            else:
                raise TypeError(
                    "Form.fields entries must be Widget or dict, "
                    f"got {type(fld).__name__}"
                )
        out: dict[str, Any] = {
            "action": self.action,
            "method": self.method,
            "fields": fields_out,
        }
        if self.submit is not None:
            out["submit"] = self.submit
        return out


@dataclass(frozen=True)
class Button(Widget):
    """``button`` widget (SPEC §5.3.5)."""

    label: str
    href: str
    method: str = "GET"
    confirm: str | None = None

    def __post_init__(self) -> None:
        _validate_non_empty(self.label, field_name="Button.label")
        _validate_non_empty(self.href, field_name="Button.href")
        _validate_enum(
            self.method.upper(), _HTTP_METHODS, field_name="Button.method"
        )
        object.__setattr__(self, "method", self.method.upper())

    def _tag(self) -> str:
        return "button"

    def _to_payload(self, *, numeric: bool) -> dict[str, Any]:
        out: dict[str, Any] = {
            "label": self.label,
            "href": self.href,
            "method": self.method,
        }
        if self.confirm is not None:
            out["confirm"] = self.confirm
        return out


@dataclass(frozen=True)
class Image(Widget):
    """``image`` widget (SPEC §5.3.6)."""

    src: str
    w: int | None = None
    h: int | None = None
    alt: str | None = None

    def __post_init__(self) -> None:
        _validate_non_empty(self.src, field_name="Image.src")
        if not self.src.startswith("img:"):
            raise ValueError(
                "Image.src must be content-addressed: img:<sha256-prefix>"
            )
        for name, val in (("w", self.w), ("h", self.h)):
            if val is not None and (not isinstance(val, int) or val <= 0):
                raise ValueError(
                    f"Image.{name} must be a positive integer when set"
                )

    def _tag(self) -> str:
        return "image"

    def _to_payload(self, *, numeric: bool) -> dict[str, Any]:
        out: dict[str, Any] = {"src": self.src}
        if self.w is not None:
            out["w"] = self.w
        if self.h is not None:
            out["h"] = self.h
        if self.alt is not None:
            out["alt"] = self.alt
        return out


@dataclass(frozen=True)
class MapMarker:
    """One pin on a :class:`Map` widget. No ``t`` discriminator."""

    lat: float
    lon: float
    label: str | None = None

    def __post_init__(self) -> None:
        _check_lat_lon(self.lat, self.lon, owner="MapMarker")

    def to_dict(self, *, numeric: bool = False) -> dict[str, Any]:
        out: dict[str, Any] = {"lat": float(self.lat), "lon": float(self.lon)}
        if self.label is not None:
            out["label"] = self.label
        return out


@dataclass(frozen=True)
class Map(Widget):
    """``map`` widget (SPEC §5.3.7)."""

    lat: float
    lon: float
    zoom: int | None = None
    markers: Sequence[MapMarker | dict[str, Any]] = field(default_factory=tuple)

    def __post_init__(self) -> None:
        _check_lat_lon(self.lat, self.lon, owner="Map")
        if self.zoom is not None and (
            not isinstance(self.zoom, int) or not 0 <= self.zoom <= 19
        ):
            # OSM tile zoom range — matches §5.3.7 tile fetch decision.
            raise ValueError("Map.zoom must be an integer in [0, 19]")
        object.__setattr__(self, "markers", tuple(self.markers))

    def _tag(self) -> str:
        return "map"

    def _to_payload(self, *, numeric: bool) -> dict[str, Any]:
        out: dict[str, Any] = {"lat": float(self.lat), "lon": float(self.lon)}
        if self.zoom is not None:
            out["zoom"] = self.zoom
        if self.markers:
            markers_out: list[dict[str, Any]] = []
            for m in self.markers:
                if isinstance(m, MapMarker):
                    markers_out.append(m.to_dict(numeric=numeric))
                elif isinstance(m, dict):
                    markers_out.append(m)
                else:
                    raise TypeError(
                        "Map.markers entries must be MapMarker or dict, "
                        f"got {type(m).__name__}"
                    )
            out["markers"] = markers_out
        return out


@dataclass(frozen=True)
class Notification(Widget):
    """``notification`` widget (SPEC §5.3.8)."""

    s: str
    level: str = "info"

    def __post_init__(self) -> None:
        _validate_non_empty(self.s, field_name="Notification.s")
        _validate_enum(
            self.level, _NOTIFICATION_LEVELS, field_name="Notification.level"
        )

    def _tag(self) -> str:
        return "notification"

    def _to_payload(self, *, numeric: bool) -> dict[str, Any]:
        out: dict[str, Any] = {"s": self.s}
        if self.level != "info":
            # Default-level omitted to match widget_notification_info_string.
            out["level"] = self.level
        return out


@dataclass(frozen=True)
class PresenceMember:
    """One member entry inside a :class:`PresenceList`."""

    id: str
    name: str
    online: bool

    def __post_init__(self) -> None:
        _validate_non_empty(self.id, field_name="PresenceMember.id")
        _validate_non_empty(self.name, field_name="PresenceMember.name")
        if not isinstance(self.online, bool):
            raise TypeError("PresenceMember.online must be bool")

    def to_dict(self, *, numeric: bool = False) -> dict[str, Any]:
        return {"id": self.id, "name": self.name, "online": self.online}


@dataclass(frozen=True)
class PresenceList(Widget):
    """``presence_list`` widget (SPEC §5.3.9)."""

    group_id: str
    members: Sequence[PresenceMember | dict[str, Any]] = field(
        default_factory=tuple
    )

    def __post_init__(self) -> None:
        _validate_non_empty(self.group_id, field_name="PresenceList.group_id")
        object.__setattr__(self, "members", tuple(self.members))

    def _tag(self) -> str:
        return "presence_list"

    def _to_payload(self, *, numeric: bool) -> dict[str, Any]:
        members_out: list[dict[str, Any]] = []
        for m in self.members:
            if isinstance(m, PresenceMember):
                members_out.append(m.to_dict(numeric=numeric))
            elif isinstance(m, dict):
                members_out.append(m)
            else:
                raise TypeError(
                    "PresenceList.members entries must be PresenceMember or dict, "
                    f"got {type(m).__name__}"
                )
        return {"group_id": self.group_id, "members": members_out}


@dataclass(frozen=True)
class ChatMessage:
    """One message bubble inside a :class:`Chat` widget."""

    from_: str
    s: str
    ts: int

    def __post_init__(self) -> None:
        _validate_non_empty(self.from_, field_name="ChatMessage.from_")
        _validate_non_empty(self.s, field_name="ChatMessage.s")
        if not isinstance(self.ts, int) or self.ts < 0:
            raise ValueError("ChatMessage.ts must be a non-negative integer")

    def to_dict(self, *, numeric: bool = False) -> dict[str, Any]:
        # ``from`` collides with the Python keyword — the dataclass uses
        # ``from_`` and we rename on the wire.
        return {"from": self.from_, "s": self.s, "ts": self.ts}


@dataclass(frozen=True)
class ChatCompose:
    """The inline composer on a :class:`Chat` widget."""

    name: str
    submit: str

    def __post_init__(self) -> None:
        _validate_non_empty(self.name, field_name="ChatCompose.name")
        _validate_non_empty(self.submit, field_name="ChatCompose.submit")

    def to_dict(self, *, numeric: bool = False) -> dict[str, Any]:
        return {"name": self.name, "submit": self.submit}


@dataclass(frozen=True)
class Chat(Widget):
    """``chat`` widget (SPEC §5.3.10)."""

    group_id: str
    messages: Sequence[ChatMessage | dict[str, Any]] = field(default_factory=tuple)
    compose: ChatCompose | dict[str, Any] | None = None

    def __post_init__(self) -> None:
        _validate_non_empty(self.group_id, field_name="Chat.group_id")
        object.__setattr__(self, "messages", tuple(self.messages))

    def _tag(self) -> str:
        return "chat"

    def _to_payload(self, *, numeric: bool) -> dict[str, Any]:
        messages_out: list[dict[str, Any]] = []
        for m in self.messages:
            if isinstance(m, ChatMessage):
                messages_out.append(m.to_dict(numeric=numeric))
            elif isinstance(m, dict):
                messages_out.append(m)
            else:
                raise TypeError(
                    "Chat.messages entries must be ChatMessage or dict, "
                    f"got {type(m).__name__}"
                )
        out: dict[str, Any] = {
            "group_id": self.group_id,
            "messages": messages_out,
        }
        if self.compose is not None:
            if isinstance(self.compose, ChatCompose):
                out["compose"] = self.compose.to_dict(numeric=numeric)
            elif isinstance(self.compose, dict):
                out["compose"] = self.compose
            else:
                raise TypeError(
                    "Chat.compose must be ChatCompose or dict, "
                    f"got {type(self.compose).__name__}"
                )
        return out


# --------------------------------------------------------------------------- #
# Frame builder
# --------------------------------------------------------------------------- #


@dataclass(frozen=True)
class Screen:
    """Top-level Frame builder (SPEC §5.2).

    ``id`` is the only field a typical screen handler must set — the
    other fields default to omitted to match the canonical minimal
    Frame shape. Pass widgets directly into ``body``; pass action
    button dicts into ``actions``.

    ``subscribe`` and ``subscribe_groups`` declare the device-event
    subscriptions the device should hold for this screen
    (SPEC §5.4.1, §5.4.2). They accept either canonical string event
    names (``"nfc_scan"``, ``"group_message"``) or numeric tag ids
    (``1``, ``19``) — both forms are pinned by
    ``protocol/tag-registry.md`` §4 and the SDK forwards them as-is.
    """

    id: str
    body: Sequence[Widget | dict[str, Any]] = field(default_factory=tuple)
    title: str | None = None
    ttl: int | None = None
    actions: Sequence[dict[str, Any]] = field(default_factory=tuple)
    subscribe: Sequence[str | int] = field(default_factory=tuple)
    subscribe_groups: Sequence[str] = field(default_factory=tuple)
    version: int = 1

    def __post_init__(self) -> None:
        _validate_non_empty(self.id, field_name="Screen.id")
        if not isinstance(self.version, int) or self.version <= 0:
            raise ValueError("Screen.version must be a positive integer")
        if self.ttl is not None and (
            not isinstance(self.ttl, int) or self.ttl < 0
        ):
            raise ValueError("Screen.ttl must be a non-negative integer")
        object.__setattr__(self, "body", tuple(self.body))
        object.__setattr__(self, "actions", tuple(self.actions))
        object.__setattr__(self, "subscribe", tuple(self.subscribe))
        object.__setattr__(self, "subscribe_groups", tuple(self.subscribe_groups))

    def to_dict(self, *, numeric: bool = False) -> dict[str, Any]:
        body_out: list[Any] = [
            to_frame_dict(b, numeric=numeric) for b in self.body
        ]
        out: dict[str, Any] = {
            "v": self.version,
            "id": self.id,
            "body": body_out,
        }
        if self.title is not None:
            out["title"] = self.title
        if self.ttl is not None:
            out["ttl"] = self.ttl
        if self.actions:
            out["actions"] = [
                to_frame_dict(a, numeric=numeric) for a in self.actions
            ]
        if self.subscribe:
            out["subscribe"] = list(self.subscribe)
        if self.subscribe_groups:
            out["subscribe_groups"] = list(self.subscribe_groups)
        return out


# --------------------------------------------------------------------------- #
# Misc validation
# --------------------------------------------------------------------------- #


def _check_lat_lon(lat: float, lon: float, *, owner: str) -> None:
    if not isinstance(lat, (int, float)) or not -_MAX_LATITUDE <= lat <= _MAX_LATITUDE:
        raise ValueError(f"{owner}.lat must be in [-90, 90]")
    if not isinstance(lon, (int, float)) or not -_MAX_LONGITUDE <= lon <= _MAX_LONGITUDE:
        raise ValueError(f"{owner}.lon must be in [-180, 180]")
