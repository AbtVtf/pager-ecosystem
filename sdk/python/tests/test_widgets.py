"""Tests for the PY-003 widget builders.

Two layers of coverage:

* **Per-widget shape & validation** — every widget builder produces
  the canonical dict shape from ``SPEC.md`` §5.3 and rejects invalid
  inputs (bad enum, missing required field, out-of-range numbers).
* **End-to-end via the dispatcher** — a handler returning a
  :class:`Screen` flows through :meth:`App.dispatch` and round-trips
  through canonical CBOR via :mod:`pageros.codec`. This is the
  acceptance criterion in ``TASKS.md`` ("produce valid CBOR via
  PY-002") observed end-to-end.

Where applicable the tests pin to the corresponding canonical hex
from ``protocol/test-vectors/ui/`` — proves the builder output is not
just structurally valid but byte-exact against the cross-language
PROTO-003 vectors (the same bytes the conformance runner and firmware
renderer accept).
"""

from __future__ import annotations

import pytest

from pageros import (
    App,
    Button,
    Chat,
    ChatCompose,
    ChatMessage,
    EVENT_TAGS,
    Form,
    Image,
    Input,
    List,
    ListItem,
    Map,
    MapMarker,
    Notification,
    PresenceList,
    PresenceMember,
    Screen,
    Text,
    WIDGET_TAGS,
    Widget,
    decode_frame,
    encode_frame,
    to_frame_dict,
)


# --------------------------------------------------------------------------- #
# Per-widget shape
# --------------------------------------------------------------------------- #


def test_text_default_style_omitted() -> None:
    """Producers may omit the default style (`body`) per SPEC §5.3.1
    note; the minimal-text canonical vector relies on this."""
    assert Text("hi").to_dict() == {"t": "text", "s": "hi"}


def test_text_non_default_style_emitted() -> None:
    assert Text("Title", style="heading").to_dict() == {
        "t": "text",
        "s": "Title",
        "style": "heading",
    }


def test_text_validates_style_enum() -> None:
    with pytest.raises(ValueError):
        Text("hi", style="huge")


def test_text_rejects_empty_string() -> None:
    with pytest.raises(ValueError):
        Text("")


def test_list_with_items() -> None:
    w = List(items=[ListItem("A", href="/a"), ListItem("B", sub="x")])
    assert w.to_dict() == {
        "t": "list",
        "items": [
            {"label": "A", "href": "/a"},
            {"label": "B", "sub": "x"},
        ],
    }


def test_list_item_method_normalised() -> None:
    assert ListItem("A", href="/a", method="post").to_dict()["method"] == "POST"


def test_list_item_invalid_method() -> None:
    with pytest.raises(ValueError):
        ListItem("A", method="DELETE")


def test_list_rejects_non_listitem_non_dict() -> None:
    with pytest.raises(TypeError):
        List(items=[42]).to_dict()


def test_input_emits_required_fields() -> None:
    w = Input(name="title", label="Title", max=80)
    d = w.to_dict()
    assert d["t"] == "input"
    assert d["name"] == "title"
    assert d["type"] == "text"
    assert d["label"] == "Title"
    assert d["max"] == 80
    assert "value" not in d  # None → omitted


def test_input_value_empty_string_emitted() -> None:
    # `""` is a meaningful value distinct from None (pre-filled empty).
    assert Input(name="t", value="").to_dict()["value"] == ""


def test_input_rejects_bad_type() -> None:
    with pytest.raises(ValueError):
        Input(name="t", type="textarea")


def test_input_rejects_non_positive_max() -> None:
    with pytest.raises(ValueError):
        Input(name="t", max=0)


def test_form_default_method() -> None:
    w = Form(action="/save", fields=[Input(name="x")])
    d = w.to_dict()
    assert d == {
        "t": "form",
        "action": "/save",
        "method": "POST",
        "fields": [{"t": "input", "name": "x", "type": "text"}],
    }


def test_form_with_submit_and_text_label() -> None:
    w = Form(
        action="/save",
        submit="Save",
        fields=[Text("Note details", style="heading"), Input(name="x")],
    )
    d = w.to_dict()
    assert d["submit"] == "Save"
    assert d["fields"][0] == {
        "t": "text",
        "s": "Note details",
        "style": "heading",
    }


def test_button_default_method_get() -> None:
    d = Button(label="Open", href="/o").to_dict()
    assert d == {"t": "button", "label": "Open", "href": "/o", "method": "GET"}


def test_button_with_confirm() -> None:
    d = Button(
        label="Delete", href="/d", method="post", confirm="Sure?"
    ).to_dict()
    assert d["method"] == "POST"
    assert d["confirm"] == "Sure?"


def test_image_requires_img_prefix() -> None:
    with pytest.raises(ValueError):
        Image(src="https://example.com/x.png")


def test_image_omits_optional_fields() -> None:
    assert Image(src="img:abc").to_dict() == {"t": "image", "src": "img:abc"}


def test_image_with_dimensions_and_alt() -> None:
    d = Image(src="img:abc", w=96, h=96, alt="logo").to_dict()
    assert d == {
        "t": "image",
        "src": "img:abc",
        "w": 96,
        "h": 96,
        "alt": "logo",
    }


def test_map_minimal() -> None:
    d = Map(lat=45.5, lon=-73.6).to_dict()
    assert d == {"t": "map", "lat": 45.5, "lon": -73.6}


def test_map_with_zoom_and_markers() -> None:
    d = Map(
        lat=45.5,
        lon=-73.6,
        zoom=14,
        markers=[
            MapMarker(lat=45.51, lon=-73.61, label="A"),
            MapMarker(lat=45.49, lon=-73.59),
        ],
    ).to_dict()
    assert d["zoom"] == 14
    assert d["markers"] == [
        {"lat": 45.51, "lon": -73.61, "label": "A"},
        {"lat": 45.49, "lon": -73.59},
    ]


def test_map_rejects_out_of_range_lat_lon() -> None:
    with pytest.raises(ValueError):
        Map(lat=91, lon=0)
    with pytest.raises(ValueError):
        Map(lat=0, lon=-181)


def test_map_rejects_zoom_out_of_range() -> None:
    with pytest.raises(ValueError):
        Map(lat=0, lon=0, zoom=20)


def test_notification_default_level_omitted() -> None:
    # Matches widget_notification_info_string canonical vector.
    assert Notification("Saved.").to_dict() == {
        "t": "notification",
        "s": "Saved.",
    }


def test_notification_warn_level() -> None:
    d = Notification("Disk full", level="warn").to_dict()
    assert d == {"t": "notification", "s": "Disk full", "level": "warn"}


def test_notification_rejects_bad_level() -> None:
    with pytest.raises(ValueError):
        Notification("x", level="critical")


def test_presence_list_shape() -> None:
    d = PresenceList(
        "grp_abc",
        members=[
            PresenceMember("pk_alice", "alice", True),
            PresenceMember("pk_bob", "bob", False),
        ],
    ).to_dict()
    assert d["t"] == "presence_list"
    assert d["group_id"] == "grp_abc"
    assert d["members"] == [
        {"id": "pk_alice", "name": "alice", "online": True},
        {"id": "pk_bob", "name": "bob", "online": False},
    ]


def test_presence_member_rejects_non_bool_online() -> None:
    with pytest.raises(TypeError):
        PresenceMember("pk", "name", 1)  # type: ignore[arg-type]


def test_chat_with_compose() -> None:
    d = Chat(
        "grp_abc",
        messages=[
            ChatMessage(from_="alice", s="hi", ts=1716200000),
            ChatMessage(from_="bob", s="yo", ts=1716200012),
        ],
        compose=ChatCompose(name="msg", submit="/send"),
    ).to_dict()
    assert d["t"] == "chat"
    assert d["messages"][0] == {"from": "alice", "s": "hi", "ts": 1716200000}
    assert d["compose"] == {"name": "msg", "submit": "/send"}


def test_chat_message_rejects_negative_ts() -> None:
    with pytest.raises(ValueError):
        ChatMessage(from_="a", s="x", ts=-1)


# --------------------------------------------------------------------------- #
# Screen frame builder
# --------------------------------------------------------------------------- #


def test_screen_minimal_emits_v_and_body() -> None:
    d = Screen(id="scr_a", body=[Text("hi")]).to_dict()
    assert d == {
        "v": 1,
        "id": "scr_a",
        "body": [{"t": "text", "s": "hi"}],
    }


def test_screen_with_title_ttl_actions() -> None:
    d = Screen(
        id="scr_a",
        title="Home",
        ttl=60,
        body=[Text("hi")],
        actions=[{"label": "New", "key": "n", "href": "/new"}],
    ).to_dict()
    assert d["title"] == "Home"
    assert d["ttl"] == 60
    assert d["actions"] == [{"label": "New", "key": "n", "href": "/new"}]


def test_screen_ttl_zero_emitted_explicitly() -> None:
    """`ttl: 0` disables caching (SPEC §5.5) and must survive on the wire."""
    d = Screen(id="scr_a", ttl=0, body=[Text("hi")]).to_dict()
    assert d["ttl"] == 0


def test_screen_subscribe_groups() -> None:
    d = Screen(
        id="scr_a",
        body=[PresenceList("g", members=[])],
        subscribe_groups=["g"],
    ).to_dict()
    assert d["subscribe_groups"] == ["g"]


def test_screen_subscribe_accepts_numeric_event_tags() -> None:
    d = Screen(
        id="scr_a",
        body=[Text("hi")],
        subscribe=[EVENT_TAGS["nfc_scan"], EVENT_TAGS["location"]],
    ).to_dict()
    assert d["subscribe"] == [1, 2]


def test_screen_rejects_empty_id() -> None:
    with pytest.raises(ValueError):
        Screen(id="", body=[])


def test_screen_rejects_negative_ttl() -> None:
    with pytest.raises(ValueError):
        Screen(id="scr_a", ttl=-1)


# --------------------------------------------------------------------------- #
# Numeric tag form
# --------------------------------------------------------------------------- #


def test_widget_to_dict_numeric_emits_int_tag() -> None:
    assert Text("hi").to_dict(numeric=True) == {"t": 1, "s": "hi"}
    assert List(items=[]).to_dict(numeric=True)["t"] == 2
    assert Chat("g").to_dict(numeric=True)["t"] == 10


def test_screen_numeric_propagates_to_children() -> None:
    s = Screen(
        id="scr_a",
        body=[
            Text("hi"),
            Form(action="/x", fields=[Input(name="y")]),
        ],
    )
    d = s.to_dict(numeric=True)
    assert d["body"][0]["t"] == WIDGET_TAGS["text"]
    assert d["body"][1]["t"] == WIDGET_TAGS["form"]
    assert d["body"][1]["fields"][0]["t"] == WIDGET_TAGS["input"]


def test_numeric_form_is_smaller_on_wire() -> None:
    """Numeric tags pack into 1 CBOR byte; the string form is 5+ bytes."""
    s = Screen(id="scr_a", body=[Text("x"), List(items=[])])
    string_bytes = encode_frame(s.to_dict())
    numeric_bytes = encode_frame(s.to_dict(numeric=True))
    assert len(numeric_bytes) < len(string_bytes)


# --------------------------------------------------------------------------- #
# Canonical-vector parity (subset)
# --------------------------------------------------------------------------- #


@pytest.mark.parametrize(
    "name,screen,expected_hex",
    [
        (
            "text_minimal_string",
            Screen(id="scr_text_min", body=[Text("Hello, world")]),
            "a36176016269646c7363725f746578745f6d696e64626f647981a261736c48656c6c6f2c20776f726c6461746474657874",
        ),
        (
            "button_confirm_string",
            Screen(
                id="scr_btn_confirm",
                body=[
                    Button(
                        label="Delete",
                        href="/delete/42",
                        method="POST",
                        confirm="Are you sure?",
                    )
                ],
            ),
            "a36176016269646f7363725f62746e5f636f6e6669726d64626f647981a5617466627574746f6e64687265666a2f64656c6574652f3432656c6162656c6644656c657465666d6574686f6464504f535467636f6e6669726d6d41726520796f7520737572653f",
        ),
        (
            "image_full_string",
            Screen(
                id="scr_image_full",
                body=[
                    Image(
                        src="img:abababababababababababababababababababababababababababababababab",
                        w=96,
                        h=96,
                        alt="logo",
                    )
                ],
            ),
            "a36176016269646e7363725f696d6167655f66756c6c64626f647981a561681860617465696d6167656177186063616c74646c6f676f637372637844696d673a61626162616261626162616261626162616261626162616261626162616261626162616261626162616261626162616261626162616261626162616261626162",
        ),
        (
            "list_basic_string",
            Screen(
                id="scr_list_basic",
                body=[
                    List(
                        items=[
                            ListItem("Item 1", href="/item/1", sub="extra info"),
                            ListItem("Item 2", href="/item/2", method="POST"),
                        ]
                    )
                ],
            ),
            "a36176016269646e7363725f6c6973745f626173696364626f647981a26174646c697374656974656d7382a3637375626a657874726120696e666f6468726566672f6974656d2f31656c6162656c664974656d2031a36468726566672f6974656d2f32656c6162656c664974656d2032666d6574686f6464504f5354",
        ),
        (
            "notification_info_string",
            Screen(id="scr_notif_info", body=[Notification("Saved.")]),
            "a36176016269646e7363725f6e6f7469665f696e666f64626f647981a261736653617665642e61746c6e6f74696669636174696f6e",
        ),
        (
            "presence_list_string",
            Screen(
                id="scr_presence",
                body=[
                    PresenceList(
                        "grp_abc",
                        members=[
                            PresenceMember("pk_alice", "alice", True),
                            PresenceMember("pk_bob", "bob", False),
                        ],
                    )
                ],
                subscribe_groups=["grp_abc"],
            ),
            "a46176016269646c7363725f70726573656e636564626f647981a361746d70726573656e63655f6c697374676d656d6265727382a362696468706b5f616c696365646e616d6565616c696365666f6e6c696e65f5a362696466706b5f626f62646e616d6563626f62666f6e6c696e65f46867726f75705f6964676772705f616263707375627363726962655f67726f75707381676772705f616263",
        ),
        (
            "chat_with_compose_string",
            Screen(
                id="scr_chat",
                body=[
                    Chat(
                        "grp_abc",
                        messages=[
                            ChatMessage(from_="alice", s="hi", ts=1716200000),
                            ChatMessage(from_="bob", s="yo", ts=1716200012),
                        ],
                        compose=ChatCompose(name="msg", submit="/send"),
                    )
                ],
                subscribe_groups=["grp_abc"],
            ),
            "a4617601626964687363725f6368617464626f647981a46174646368617467636f6d706f7365a2646e616d65636d7367667375626d6974652f73656e646867726f75705f6964676772705f616263686d6573736167657382a361736268696274731a664b22406466726f6d65616c696365a3617362796f6274731a664b224c6466726f6d63626f62707375627363726962655f67726f75707381676772705f616263",
        ),
        (
            "map_with_markers_string",
            Screen(
                id="scr_map_markers",
                body=[
                    Map(
                        lat=45.5,
                        lon=-73.6,
                        zoom=14,
                        markers=[
                            MapMarker(lat=45.51, lon=-73.61, label="A"),
                            MapMarker(lat=45.49, lon=-73.59),
                        ],
                    )
                ],
            ),
            "a36176016269646f7363725f6d61705f6d61726b65727364626f647981a56174636d6170636c6174fa42360000636c6f6efbc052666666666666647a6f6f6d0e676d61726b65727382a3636c6174fb4046c147ae147ae1636c6f6efbc052670a3d70a3d7656c6162656c6141a2636c6174fb4046beb851eb851f636c6f6efbc05265c28f5c28f6",
        ),
    ],
)
def test_canonical_vector_byte_exact(
    name: str, screen: Screen, expected_hex: str
) -> None:
    """Byte-exact match against PROTO-003 canonical hex vectors."""
    got = encode_frame(screen.to_dict()).hex()
    assert got == expected_hex, f"{name} mismatch"


# --------------------------------------------------------------------------- #
# to_frame_dict helper
# --------------------------------------------------------------------------- #


def test_to_frame_dict_flattens_nested_widgets() -> None:
    nested = {
        "outer": Text("hello"),
        "list": [Button(label="A", href="/a"), 42, "x"],
    }
    out = to_frame_dict(nested)
    assert out == {
        "outer": {"t": "text", "s": "hello"},
        "list": [
            {"t": "button", "label": "A", "href": "/a", "method": "GET"},
            42,
            "x",
        ],
    }


def test_to_frame_dict_passes_through_scalars() -> None:
    assert to_frame_dict(42) == 42
    assert to_frame_dict("hi") == "hi"
    assert to_frame_dict(None) is None
    assert to_frame_dict(b"abc") == b"abc"


# --------------------------------------------------------------------------- #
# End-to-end via App dispatch
# --------------------------------------------------------------------------- #


def test_handler_returning_screen_emits_cbor() -> None:
    app = App(name="t")

    @app.screen("/")
    def home():
        return Screen(
            id="scr_home",
            title="Home",
            body=[Text("Welcome"), Button(label="Open", href="/open")],
        )

    status, headers, body = app.dispatch("GET", "/")
    assert status == 200
    assert headers["Content-Type"].startswith("application/cbor")
    decoded = decode_frame(body)
    assert decoded["id"] == "scr_home"
    assert decoded["title"] == "Home"
    assert decoded["body"][0] == {"t": "text", "s": "Welcome"}
    assert decoded["body"][1]["t"] == "button"
    assert decoded["body"][1]["href"] == "/open"


def test_handler_returning_widget_directly() -> None:
    """A handler may return a widget without wrapping it in Screen for
    quick responses (e.g. error notifications)."""
    app = App(name="t")

    @app.handler("/notify")
    def notify():
        return Notification("Saved", level="info")

    status, _, body = app.dispatch("POST", "/notify")
    assert status == 200
    assert decode_frame(body) == {"t": "notification", "s": "Saved"}


def test_handler_returning_plain_dict_still_works() -> None:
    """Existing plain-dict handlers must keep working — widgets are
    additive, not a breaking change."""
    app = App(name="t")

    @app.screen("/legacy")
    def legacy():
        return {
            "v": 1,
            "id": "scr_legacy",
            "body": [{"t": "text", "s": "old"}],
        }

    status, _, body = app.dispatch("GET", "/legacy")
    assert status == 200
    assert decode_frame(body)["id"] == "scr_legacy"


def test_widget_subclass_check() -> None:
    """Every widget builder is a :class:`Widget` so callers can do
    duck-typed checks (`isinstance(x, Widget)`)."""
    for w in (
        Text("x"),
        List(items=[]),
        Input(name="x"),
        Form(action="/x"),
        Button(label="x", href="/x"),
        Image(src="img:abc"),
        Map(lat=0, lon=0),
        Notification("x"),
        PresenceList("g"),
        Chat("g"),
    ):
        assert isinstance(w, Widget)
