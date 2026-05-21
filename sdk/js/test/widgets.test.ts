// Tests for the JS-003 widget builders.
//
// Two layers of coverage:
//
// * **Per-widget shape & validation** — every builder produces the
//   canonical dict shape from SPEC.md §5.3 and rejects invalid inputs.
// * **Canonical-vector parity** — a subset of builders is checked
//   byte-exact against the cross-language PROTO-003 hex vectors.
//   Proves the builder output is structurally valid AND identical to
//   the bytes the firmware renderer + conformance runner accept.

import { test } from "node:test";
import { strict as assert } from "node:assert";

import {
  Button,
  Chat,
  ChatCompose,
  ChatMessage,
  encodeFrame,
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
  toFrameDict,
  Widget,
  WIDGET_TAGS,
  bytesToHex,
} from "../src/index.js";

// ---------------------------------------------------------------------------
// Per-widget shape
// ---------------------------------------------------------------------------

test("Text: default style is omitted on the wire", () => {
  // SPEC §5.3.1 — `body` is the default style; the minimal-text
  // canonical vector relies on producers omitting it.
  assert.deepEqual(new Text({ s: "hi" }).toDict(), { t: "text", s: "hi" });
});

test("Text: non-default style is emitted", () => {
  assert.deepEqual(new Text({ s: "Title", style: "heading" }).toDict(), {
    t: "text",
    s: "Title",
    style: "heading",
  });
});

test("Text: invalid style throws", () => {
  assert.throws(() => new Text({ s: "hi", style: "huge" as never }));
});

test("Text: empty string throws", () => {
  assert.throws(() => new Text({ s: "" }));
});

test("List: items render through ListItem.toDict", () => {
  const w = new List({
    items: [new ListItem({ label: "A", href: "/a" }), new ListItem({ label: "B", sub: "x" })],
  });
  assert.deepEqual(w.toDict(), {
    t: "list",
    items: [
      { label: "A", href: "/a" },
      { label: "B", sub: "x" },
    ],
  });
});

test("List: empty items still emits the items array", () => {
  assert.deepEqual(new List().toDict(), { t: "list", items: [] });
});

test("ListItem: method is uppercased on output", () => {
  const d = new ListItem({ label: "A", href: "/a", method: "post" }).toDict();
  assert.equal(d["method"], "POST");
});

test("ListItem: rejects non-GET/POST method", () => {
  assert.throws(() => new ListItem({ label: "A", method: "DELETE" }));
});

test("List: rejects non-ListItem non-object entry", () => {
  // @ts-expect-error — intentional bad input
  assert.throws(() => new List({ items: [42] }).toDict());
});

test("Input: emits required fields with type default", () => {
  const d = new Input({ name: "title", label: "Title", max: 80 }).toDict();
  assert.equal(d["t"], "input");
  assert.equal(d["name"], "title");
  assert.equal(d["type"], "text");
  assert.equal(d["label"], "Title");
  assert.equal(d["max"], 80);
  assert.equal("value" in d, false);
});

test("Input: empty-string value is preserved (distinct from omitted)", () => {
  const d = new Input({ name: "t", value: "" }).toDict();
  assert.equal(d["value"], "");
});

test("Input: invalid type throws", () => {
  assert.throws(() => new Input({ name: "t", type: "textarea" as never }));
});

test("Input: max must be a positive integer", () => {
  assert.throws(() => new Input({ name: "t", max: 0 }));
  assert.throws(() => new Input({ name: "t", max: 1.5 }));
});

test("Form: default method is POST and submit is omitted", () => {
  const d = new Form({ action: "/save", fields: [new Input({ name: "x" })] }).toDict();
  assert.deepEqual(d, {
    t: "form",
    action: "/save",
    method: "POST",
    fields: [{ t: "input", name: "x", type: "text" }],
  });
});

test("Form: with submit label and mixed field types", () => {
  const d = new Form({
    action: "/save",
    submit: "Save",
    fields: [new Text({ s: "Note details", style: "heading" }), new Input({ name: "x" })],
  }).toDict();
  assert.equal(d["submit"], "Save");
  const fields = d["fields"] as Array<Record<string, unknown>>;
  assert.deepEqual(fields[0], { t: "text", s: "Note details", style: "heading" });
});

test("Form: rejects bad method", () => {
  assert.throws(() => new Form({ action: "/save", method: "PATCH" }));
});

test("Button: default method is GET", () => {
  const d = new Button({ label: "Open", href: "/o" }).toDict();
  assert.deepEqual(d, { t: "button", label: "Open", href: "/o", method: "GET" });
});

test("Button: confirm + POST", () => {
  const d = new Button({
    label: "Delete",
    href: "/d",
    method: "post",
    confirm: "Sure?",
  }).toDict();
  assert.equal(d["method"], "POST");
  assert.equal(d["confirm"], "Sure?");
});

test("Image: rejects non-`img:` src", () => {
  assert.throws(() => new Image({ src: "https://example.com/x.png" }));
});

test("Image: minimal form", () => {
  assert.deepEqual(new Image({ src: "img:abc" }).toDict(), {
    t: "image",
    src: "img:abc",
  });
});

test("Image: full form with dims + alt", () => {
  const d = new Image({ src: "img:abc", w: 96, h: 96, alt: "logo" }).toDict();
  assert.deepEqual(d, {
    t: "image",
    src: "img:abc",
    w: 96,
    h: 96,
    alt: "logo",
  });
});

test("Map: minimal form", () => {
  const d = new Map({ lat: 45.5, lon: -73.6 }).toDict();
  assert.deepEqual(d, { t: "map", lat: 45.5, lon: -73.6 });
});

test("Map: with zoom + markers", () => {
  const d = new Map({
    lat: 45.5,
    lon: -73.6,
    zoom: 14,
    markers: [
      new MapMarker({ lat: 45.51, lon: -73.61, label: "A" }),
      new MapMarker({ lat: 45.49, lon: -73.59 }),
    ],
  }).toDict();
  assert.equal(d["zoom"], 14);
  assert.deepEqual(d["markers"], [
    { lat: 45.51, lon: -73.61, label: "A" },
    { lat: 45.49, lon: -73.59 },
  ]);
});

test("Map: rejects out-of-range lat/lon", () => {
  assert.throws(() => new Map({ lat: 91, lon: 0 }));
  assert.throws(() => new Map({ lat: 0, lon: -181 }));
});

test("Map: rejects zoom out of OSM range", () => {
  assert.throws(() => new Map({ lat: 0, lon: 0, zoom: 20 }));
  assert.throws(() => new Map({ lat: 0, lon: 0, zoom: -1 }));
});

test("Notification: default `info` level is omitted", () => {
  // Matches widget_notification_info_string canonical vector.
  assert.deepEqual(new Notification({ s: "Saved." }).toDict(), {
    t: "notification",
    s: "Saved.",
  });
});

test("Notification: warn level emitted explicitly", () => {
  const d = new Notification({ s: "Disk full", level: "warn" }).toDict();
  assert.deepEqual(d, { t: "notification", s: "Disk full", level: "warn" });
});

test("Notification: rejects bad level", () => {
  assert.throws(() => new Notification({ s: "x", level: "critical" as never }));
});

test("PresenceList: shape matches SPEC §5.3.9", () => {
  const d = new PresenceList({
    groupId: "grp_abc",
    members: [
      new PresenceMember({ id: "pk_alice", name: "alice", online: true }),
      new PresenceMember({ id: "pk_bob", name: "bob", online: false }),
    ],
  }).toDict();
  assert.equal(d["t"], "presence_list");
  assert.equal(d["group_id"], "grp_abc");
  assert.deepEqual(d["members"], [
    { id: "pk_alice", name: "alice", online: true },
    { id: "pk_bob", name: "bob", online: false },
  ]);
});

test("PresenceMember: rejects non-boolean online", () => {
  // @ts-expect-error — runtime check beyond the type system
  assert.throws(() => new PresenceMember({ id: "pk", name: "n", online: 1 }));
});

test("Chat: with composer", () => {
  const d = new Chat({
    groupId: "grp_abc",
    messages: [
      new ChatMessage({ from: "alice", s: "hi", ts: 1716200000 }),
      new ChatMessage({ from: "bob", s: "yo", ts: 1716200012 }),
    ],
    compose: new ChatCompose({ name: "msg", submit: "/send" }),
  }).toDict();
  assert.equal(d["t"], "chat");
  const messages = d["messages"] as Array<Record<string, unknown>>;
  assert.deepEqual(messages[0], { from: "alice", s: "hi", ts: 1716200000 });
  assert.deepEqual(d["compose"], { name: "msg", submit: "/send" });
});

test("ChatMessage: rejects negative ts", () => {
  assert.throws(() => new ChatMessage({ from: "a", s: "x", ts: -1 }));
});

// ---------------------------------------------------------------------------
// Screen frame builder
// ---------------------------------------------------------------------------

test("Screen: minimal frame emits only v/id/body", () => {
  const d = new Screen({ id: "scr_a", body: [new Text({ s: "hi" })] }).toDict();
  assert.deepEqual(d, {
    v: 1,
    id: "scr_a",
    body: [{ t: "text", s: "hi" }],
  });
});

test("Screen: title + ttl + actions are forwarded verbatim", () => {
  const d = new Screen({
    id: "scr_a",
    title: "Home",
    ttl: 60,
    body: [new Text({ s: "hi" })],
    actions: [{ label: "New", key: "n", href: "/new" }],
  }).toDict();
  assert.equal(d["title"], "Home");
  assert.equal(d["ttl"], 60);
  assert.deepEqual(d["actions"], [{ label: "New", key: "n", href: "/new" }]);
});

test("Screen: ttl=0 disables cache and must survive on the wire", () => {
  // SPEC §5.5 — ttl:0 means "do not cache"; the value is meaningful.
  const d = new Screen({ id: "scr_a", ttl: 0, body: [new Text({ s: "hi" })] }).toDict();
  assert.equal(d["ttl"], 0);
});

test("Screen: subscribeGroups maps to wire name `subscribe_groups`", () => {
  const d = new Screen({
    id: "scr_a",
    body: [new PresenceList({ groupId: "g" })],
    subscribeGroups: ["g"],
  }).toDict();
  assert.deepEqual(d["subscribe_groups"], ["g"]);
});

test("Screen: subscribe accepts numeric event tags", () => {
  const d = new Screen({
    id: "scr_a",
    body: [new Text({ s: "hi" })],
    subscribe: [EVENT_TAGS.nfc_scan, EVENT_TAGS.location],
  }).toDict();
  assert.deepEqual(d["subscribe"], [1, 2]);
});

test("Screen: rejects empty id", () => {
  assert.throws(() => new Screen({ id: "" }));
});

test("Screen: rejects negative ttl", () => {
  assert.throws(() => new Screen({ id: "scr_a", ttl: -1 }));
});

// ---------------------------------------------------------------------------
// Numeric tag form
// ---------------------------------------------------------------------------

test("toDict(numeric:true) emits integer widget tags", () => {
  assert.deepEqual(new Text({ s: "hi" }).toDict({ numeric: true }), { t: 1, s: "hi" });
  assert.equal((new List().toDict({ numeric: true }) as Record<string, unknown>)["t"], 2);
  assert.equal(
    (new Chat({ groupId: "g" }).toDict({ numeric: true }) as Record<string, unknown>)["t"],
    10,
  );
});

test("Screen.toDict(numeric:true) propagates to nested widgets", () => {
  const s = new Screen({
    id: "scr_a",
    body: [new Text({ s: "hi" }), new Form({ action: "/x", fields: [new Input({ name: "y" })] })],
  });
  const d = s.toDict({ numeric: true });
  const body = d["body"] as Array<Record<string, unknown>>;
  assert.equal(body[0]?.["t"], WIDGET_TAGS.text);
  assert.equal(body[1]?.["t"], WIDGET_TAGS.form);
  const formFields = body[1]?.["fields"] as Array<Record<string, unknown>>;
  assert.equal(formFields[0]?.["t"], WIDGET_TAGS.input);
});

test("numeric tags produce smaller CBOR than string tags", () => {
  // Numeric tags pack into 1 CBOR byte; the string form is 5+ bytes.
  const s = new Screen({ id: "scr_a", body: [new Text({ s: "x" }), new List()] });
  const stringBytes = encodeFrame(s.toDict());
  const numericBytes = encodeFrame(s.toDict({ numeric: true }));
  assert.ok(numericBytes.length < stringBytes.length);
});

// ---------------------------------------------------------------------------
// toFrameDict helper
// ---------------------------------------------------------------------------

test("toFrameDict: flattens nested Widget instances", () => {
  const nested = {
    outer: new Text({ s: "hello" }),
    list: [new Button({ label: "A", href: "/a" }), 42, "x"],
  };
  assert.deepEqual(toFrameDict(nested), {
    outer: { t: "text", s: "hello" },
    list: [
      { t: "button", label: "A", href: "/a", method: "GET" },
      42,
      "x",
    ],
  });
});

test("toFrameDict: passes scalars and Uint8Array through unchanged", () => {
  assert.equal(toFrameDict(42), 42);
  assert.equal(toFrameDict("hi"), "hi");
  assert.equal(toFrameDict(null), null);
  const bytes = new Uint8Array([1, 2, 3]);
  assert.equal(toFrameDict(bytes), bytes);
});

// ---------------------------------------------------------------------------
// Widget base type
// ---------------------------------------------------------------------------

test("Widget is the shared abstract base for every builder", () => {
  assert.ok(new Text({ s: "x" }) instanceof Widget);
  assert.ok(new Button({ label: "x", href: "/x" }) instanceof Widget);
  assert.ok(new Chat({ groupId: "g" }) instanceof Widget);
});

test("Widget instances are frozen against accidental mutation", () => {
  const t = new Text({ s: "hi" });
  assert.throws(() => {
    (t as unknown as { s: string }).s = "bye";
  });
});

// ---------------------------------------------------------------------------
// Canonical-vector parity (subset)
// ---------------------------------------------------------------------------
//
// Byte-exact match against the PROTO-003 canonical hex vectors. Same
// inputs as the Python parity suite; if these pass we know the JS SDK
// produces the same on-the-wire bytes as the firmware renderer and
// conformance runner expect.

const vectorCases: Array<{ name: string; screen: Screen; expected: string }> = [
  {
    name: "text_minimal_string",
    screen: new Screen({ id: "scr_text_min", body: [new Text({ s: "Hello, world" })] }),
    expected:
      "a36176016269646c7363725f746578745f6d696e64626f647981a261736c48656c6c6f2c20776f726c6461746474657874",
  },
  {
    name: "button_confirm_string",
    screen: new Screen({
      id: "scr_btn_confirm",
      body: [
        new Button({
          label: "Delete",
          href: "/delete/42",
          method: "POST",
          confirm: "Are you sure?",
        }),
      ],
    }),
    expected:
      "a36176016269646f7363725f62746e5f636f6e6669726d64626f647981a5617466627574746f6e64687265666a2f64656c6574652f3432656c6162656c6644656c657465666d6574686f6464504f535467636f6e6669726d6d41726520796f7520737572653f",
  },
  {
    name: "image_full_string",
    screen: new Screen({
      id: "scr_image_full",
      body: [
        new Image({
          src: "img:abababababababababababababababababababababababababababababababab",
          w: 96,
          h: 96,
          alt: "logo",
        }),
      ],
    }),
    expected:
      "a36176016269646e7363725f696d6167655f66756c6c64626f647981a561681860617465696d6167656177186063616c74646c6f676f637372637844696d673a61626162616261626162616261626162616261626162616261626162616261626162616261626162616261626162616261626162616261626162616261626162",
  },
  {
    name: "list_basic_string",
    screen: new Screen({
      id: "scr_list_basic",
      body: [
        new List({
          items: [
            new ListItem({ label: "Item 1", href: "/item/1", sub: "extra info" }),
            new ListItem({ label: "Item 2", href: "/item/2", method: "POST" }),
          ],
        }),
      ],
    }),
    expected:
      "a36176016269646e7363725f6c6973745f626173696364626f647981a26174646c697374656974656d7382a3637375626a657874726120696e666f6468726566672f6974656d2f31656c6162656c664974656d2031a36468726566672f6974656d2f32656c6162656c664974656d2032666d6574686f6464504f5354",
  },
  {
    name: "notification_info_string",
    screen: new Screen({ id: "scr_notif_info", body: [new Notification({ s: "Saved." })] }),
    expected:
      "a36176016269646e7363725f6e6f7469665f696e666f64626f647981a261736653617665642e61746c6e6f74696669636174696f6e",
  },
  {
    name: "presence_list_string",
    screen: new Screen({
      id: "scr_presence",
      body: [
        new PresenceList({
          groupId: "grp_abc",
          members: [
            new PresenceMember({ id: "pk_alice", name: "alice", online: true }),
            new PresenceMember({ id: "pk_bob", name: "bob", online: false }),
          ],
        }),
      ],
      subscribeGroups: ["grp_abc"],
    }),
    expected:
      "a46176016269646c7363725f70726573656e636564626f647981a361746d70726573656e63655f6c697374676d656d6265727382a362696468706b5f616c696365646e616d6565616c696365666f6e6c696e65f5a362696466706b5f626f62646e616d6563626f62666f6e6c696e65f46867726f75705f6964676772705f616263707375627363726962655f67726f75707381676772705f616263",
  },
  {
    name: "chat_with_compose_string",
    screen: new Screen({
      id: "scr_chat",
      body: [
        new Chat({
          groupId: "grp_abc",
          messages: [
            new ChatMessage({ from: "alice", s: "hi", ts: 1716200000 }),
            new ChatMessage({ from: "bob", s: "yo", ts: 1716200012 }),
          ],
          compose: new ChatCompose({ name: "msg", submit: "/send" }),
        }),
      ],
      subscribeGroups: ["grp_abc"],
    }),
    expected:
      "a4617601626964687363725f6368617464626f647981a46174646368617467636f6d706f7365a2646e616d65636d7367667375626d6974652f73656e646867726f75705f6964676772705f616263686d6573736167657382a361736268696274731a664b22406466726f6d65616c696365a3617362796f6274731a664b224c6466726f6d63626f62707375627363726962655f67726f75707381676772705f616263",
  },
  {
    name: "map_with_markers_string",
    screen: new Screen({
      id: "scr_map_markers",
      body: [
        new Map({
          lat: 45.5,
          lon: -73.6,
          zoom: 14,
          markers: [
            new MapMarker({ lat: 45.51, lon: -73.61, label: "A" }),
            new MapMarker({ lat: 45.49, lon: -73.59 }),
          ],
        }),
      ],
    }),
    expected:
      "a36176016269646f7363725f6d61705f6d61726b65727364626f647981a56174636d6170636c6174fa42360000636c6f6efbc052666666666666647a6f6f6d0e676d61726b65727382a3636c6174fb4046c147ae147ae1636c6f6efbc052670a3d70a3d7656c6162656c6141a2636c6174fb4046beb851eb851f636c6f6efbc05265c28f5c28f6",
  },
];

for (const { name, screen, expected } of vectorCases) {
  test(`canonical vector parity: ${name}`, () => {
    const got = bytesToHex(encodeFrame(screen.toDict()));
    assert.equal(got, expected, `${name} mismatch`);
  });
}
