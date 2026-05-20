//! Frame, widget, and event models + CBOR decoder for PROTO-003 vectors.
//!
//! The CBOR layer accepts both canonical tag forms (string and numeric)
//! defined in `protocol/tag-registry.md`. Decoding is permissive in line
//! with the spec's forward-compatibility rules: unknown widgets are
//! preserved as [`Widget::Unsupported`], unknown top-level fields are
//! ignored, and unknown major versions raise [`RenderError::UnknownVersion`].
//!
//! The decoder is std-only because `ciborium`'s reader needs `std::io`.
//! A future no_std variant will wrap `ciborium-io::Read` for the firmware.

use alloc::format;
use alloc::string::{String, ToString};
use alloc::vec::Vec;
use core::fmt;

use ciborium::value::{Integer, Value};

/// Decoded Frame ready for rendering.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Frame {
    /// Protocol version (`v`). v1 is the only renderable version today.
    pub version: u32,
    /// Server-assigned screen id (`id`), if any.
    pub id: Option<String>,
    /// Cache TTL in seconds (`ttl`).
    pub ttl: Option<u32>,
    /// Top-bar title (`title`).
    pub title: Option<String>,
    /// Ordered list of body widgets.
    pub body: Vec<Widget>,
    /// Top-bar actions (`actions`). Not rendered as buttons; shown in the
    /// title bar with a soft-key hint.
    pub actions: Vec<Action>,
    /// Subscription list (`subscribe`). Not visible; recorded for debug.
    pub subscribe: Vec<String>,
    /// Body of an error frame (`error`), as defined in spec §7.4.1. Renders
    /// full-screen with the matching palette.
    pub error: Option<ErrorBody>,
}

/// Top-bar action (`actions[]`).
#[derive(Clone, Debug, PartialEq)]
pub struct Action {
    pub label: String,
    pub key: Option<String>,
    pub href: Option<String>,
    pub method: Option<String>,
}

/// Locally-built or server-provided error frame body.
#[derive(Clone, Debug, PartialEq)]
pub struct ErrorBody {
    pub id: Option<String>,
    pub style: Option<String>,
    pub title: Option<String>,
    pub message: Option<String>,
    pub status: Option<u32>,
}

/// `style` field on text widgets. `Error` is renderer-only (spec §7.4.1):
/// servers MUST NOT emit it; the local renderer applies it to text widgets
/// inside a locally-built error frame so the user sees red without an HTTP
/// error body.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Default)]
pub enum Style {
    #[default]
    Body,
    Heading,
    Dim,
    Mono,
    Error,
}

impl Style {
    fn parse(s: &str) -> Self {
        match s {
            "heading" => Style::Heading,
            "dim" => Style::Dim,
            "mono" => Style::Mono,
            "error" => Style::Error,
            _ => Style::Body,
        }
    }
}

/// All v1 widget shapes from `SPEC.md` §5.3, plus the unsupported fallback.
#[derive(Clone, Debug, PartialEq)]
pub enum Widget {
    /// Plain text. `style` controls colour + weight.
    Text {
        s: String,
        style: Style,
    },
    List {
        items: Vec<MenuItem>,
    },
    Input {
        name: Option<String>,
        label: Option<String>,
        input_type: Option<String>,
        value: Option<String>,
        max: Option<u32>,
    },
    Form {
        action: Option<String>,
        method: Option<String>,
        fields: Vec<Widget>,
        submit: Option<String>,
    },
    Button {
        label: Option<String>,
        href: Option<String>,
        method: Option<String>,
        confirm: Option<String>,
    },
    Image {
        src: Option<String>,
        w: Option<u32>,
        h: Option<u32>,
        alt: Option<String>,
    },
    Map {
        lat: Option<f64>,
        lon: Option<f64>,
        zoom: Option<u32>,
        marker_count: usize,
    },
    Notification {
        level: String,
        s: String,
    },
    PresenceList {
        group_id: Option<String>,
        members: Vec<Member>,
    },
    Chat {
        group_id: Option<String>,
        messages: Vec<ChatMessage>,
        compose_submit: Option<String>,
    },
    /// Forward-compat placeholder: any unknown widget tag.
    Unsupported {
        tag_repr: String,
    },
}

/// `list[]` entry.
#[derive(Clone, Debug, PartialEq)]
pub struct MenuItem {
    pub label: Option<String>,
    pub href: Option<String>,
    pub sub: Option<String>,
}

/// `presence_list.members[]` entry.
#[derive(Clone, Debug, PartialEq)]
pub struct Member {
    pub id: Option<String>,
    pub name: Option<String>,
    pub online: bool,
}

/// `chat.messages[]` entry.
#[derive(Clone, Debug, PartialEq)]
pub struct ChatMessage {
    pub from: Option<String>,
    pub ts: Option<u64>,
    pub s: Option<String>,
}

/// Decoder errors. The renderer surfaces these as the
/// `[invalid frame: <reason>]` placeholder; the conformance runner consumes
/// them as the `expect_error` class for negative test vectors.
#[derive(Debug)]
pub enum RenderError {
    /// CBOR was structurally invalid.
    Cbor(String),
    /// Top-level value was not a CBOR map (Frame must be a map per spec §5.2).
    TopLevelNotMap,
    /// Major version was not v1 (`v != 1`).
    UnknownVersion(u32),
}

impl fmt::Display for RenderError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            RenderError::Cbor(msg) => write!(f, "cbor: {msg}"),
            RenderError::TopLevelNotMap => f.write_str("top-level value is not a map"),
            RenderError::UnknownVersion(v) => write!(f, "unknown protocol version v={v}"),
        }
    }
}

#[cfg(feature = "std")]
impl std::error::Error for RenderError {}

/// Decode a Frame from canonical-CBOR bytes (PROTO-003 vector form).
#[cfg(feature = "std")]
pub fn decode_frame_cbor(bytes: &[u8]) -> Result<Frame, RenderError> {
    let value: Value = ciborium::de::from_reader(bytes)
        .map_err(|e| RenderError::Cbor(format!("{e}")))?;
    decode_frame_value(&value)
}

/// Decode a Frame from an already-parsed CBOR value.
pub fn decode_frame_value(value: &Value) -> Result<Frame, RenderError> {
    let map = match value {
        Value::Map(m) => m,
        _ => return Err(RenderError::TopLevelNotMap),
    };
    let mut frame = Frame::default();
    let mut saw_version = false;
    for (k, v) in map {
        let key = match str_key(k, "v") {
            Some(k) => k,
            None => continue,
        };
        match key.as_str() {
            "v" => {
                saw_version = true;
                frame.version = value_as_u32(v).unwrap_or(0);
                if frame.version != 1 {
                    return Err(RenderError::UnknownVersion(frame.version));
                }
            }
            "id" => frame.id = value_as_string(v),
            "ttl" => frame.ttl = value_as_u32(v),
            "title" => frame.title = value_as_string(v),
            "body" => {
                if let Value::Array(items) = v {
                    frame.body = items.iter().map(decode_widget).collect();
                }
            }
            "actions" => {
                if let Value::Array(items) = v {
                    frame.actions = items.iter().filter_map(decode_action).collect();
                }
            }
            "subscribe" => {
                if let Value::Array(items) = v {
                    frame.subscribe = items.iter().filter_map(value_as_string).collect();
                }
            }
            "error" => {
                frame.error = decode_error_body(v);
            }
            _ => { /* Unknown top-level field — ignored per §5.7. */ }
        }
    }
    if !saw_version {
        // Frames produced by some PROTO-003 vectors elide `v` for events; treat
        // as v1 by default so we render rather than reject.
        frame.version = 1;
    }
    Ok(frame)
}

fn str_key(k: &Value, _hint: &str) -> Option<String> {
    match k {
        Value::Text(s) => Some(s.clone()),
        Value::Integer(i) => Some(format!("{}", to_i128(*i))),
        _ => None,
    }
}

fn value_as_string(v: &Value) -> Option<String> {
    match v {
        Value::Text(s) => Some(s.clone()),
        _ => None,
    }
}

fn value_as_u32(v: &Value) -> Option<u32> {
    match v {
        Value::Integer(i) => u32::try_from(to_i128(*i)).ok(),
        _ => None,
    }
}

fn value_as_u64(v: &Value) -> Option<u64> {
    match v {
        Value::Integer(i) => u64::try_from(to_i128(*i)).ok(),
        _ => None,
    }
}

fn value_as_bool(v: &Value) -> Option<bool> {
    match v {
        Value::Bool(b) => Some(*b),
        _ => None,
    }
}

fn value_as_f64(v: &Value) -> Option<f64> {
    match v {
        Value::Float(f) => Some(*f),
        Value::Integer(i) => Some(to_i128(*i) as f64),
        _ => None,
    }
}

fn to_i128(i: Integer) -> i128 {
    i.into()
}

fn decode_action(v: &Value) -> Option<Action> {
    let map = match v {
        Value::Map(m) => m,
        _ => return None,
    };
    let mut action = Action {
        label: String::new(),
        key: None,
        href: None,
        method: None,
    };
    for (k, vv) in map {
        let key = str_key(k, "label")?;
        match key.as_str() {
            "label" => action.label = value_as_string(vv).unwrap_or_default(),
            "key" => action.key = value_as_string(vv),
            "href" => action.href = value_as_string(vv),
            "method" => action.method = value_as_string(vv),
            _ => {}
        }
    }
    Some(action)
}

fn decode_error_body(v: &Value) -> Option<ErrorBody> {
    let map = match v {
        Value::Map(m) => m,
        _ => return None,
    };
    let mut body = ErrorBody {
        id: None,
        style: None,
        title: None,
        message: None,
        status: None,
    };
    for (k, vv) in map {
        let key = match str_key(k, "id") {
            Some(k) => k,
            None => continue,
        };
        match key.as_str() {
            "id" => body.id = value_as_string(vv),
            "style" => body.style = value_as_string(vv),
            "title" => body.title = value_as_string(vv),
            "message" | "s" | "body" => body.message = value_as_string(vv),
            "status" => body.status = value_as_u32(vv),
            _ => {}
        }
    }
    Some(body)
}

/// Decode a widget value. Unknown tags fall through to
/// [`Widget::Unsupported`] with the tag repr captured for the placeholder.
pub fn decode_widget(v: &Value) -> Widget {
    let map = match v {
        Value::Map(m) => m,
        _ => {
            return Widget::Unsupported {
                tag_repr: "non-map".to_string(),
            }
        }
    };

    // First pass: identify the tag.
    let mut tag: Option<WidgetTag> = None;
    for (k, vv) in map {
        let key = match str_key(k, "t") {
            Some(k) => k,
            None => continue,
        };
        if key == "t" {
            tag = Some(WidgetTag::parse(vv));
            break;
        }
    }
    let tag = match tag {
        Some(t) => t,
        None => {
            return Widget::Unsupported {
                tag_repr: "no-t".to_string(),
            }
        }
    };

    match tag {
        WidgetTag::Text => {
            let mut s = String::new();
            let mut style = Style::Body;
            for (k, vv) in map {
                if let Some(key) = str_key(k, "") {
                    match key.as_str() {
                        "s" => s = value_as_string(vv).unwrap_or_default(),
                        "style" => {
                            style = value_as_string(vv)
                                .map(|s| Style::parse(&s))
                                .unwrap_or_default()
                        }
                        _ => {}
                    }
                }
            }
            Widget::Text { s, style }
        }
        WidgetTag::List => {
            let mut items = Vec::new();
            for (k, vv) in map {
                if let Some(key) = str_key(k, "") {
                    if key == "items" {
                        if let Value::Array(arr) = vv {
                            for entry in arr {
                                items.push(decode_menu_item(entry));
                            }
                        }
                    }
                }
            }
            Widget::List { items }
        }
        WidgetTag::Input => Widget::Input {
            name: field_string(map, "name"),
            label: field_string(map, "label"),
            input_type: field_string(map, "type"),
            value: field_string(map, "value"),
            max: field_u32(map, "max"),
        },
        WidgetTag::Form => {
            let mut fields = Vec::new();
            if let Some(Value::Array(arr)) = field_value(map, "fields") {
                for entry in arr {
                    fields.push(decode_widget(entry));
                }
            }
            Widget::Form {
                action: field_string(map, "action"),
                method: field_string(map, "method"),
                fields,
                submit: field_string(map, "submit"),
            }
        }
        WidgetTag::Button => Widget::Button {
            label: field_string(map, "label"),
            href: field_string(map, "href"),
            method: field_string(map, "method"),
            confirm: field_string(map, "confirm"),
        },
        WidgetTag::Image => Widget::Image {
            src: field_string(map, "src"),
            w: field_u32(map, "w"),
            h: field_u32(map, "h"),
            alt: field_string(map, "alt"),
        },
        WidgetTag::Map => {
            let marker_count = match field_value(map, "markers") {
                Some(Value::Array(a)) => a.len(),
                _ => 0,
            };
            Widget::Map {
                lat: field_f64(map, "lat"),
                lon: field_f64(map, "lon"),
                zoom: field_u32(map, "zoom"),
                marker_count,
            }
        }
        WidgetTag::Notification => Widget::Notification {
            level: field_string(map, "level").unwrap_or_else(|| "info".to_string()),
            s: field_string(map, "s").unwrap_or_default(),
        },
        WidgetTag::PresenceList => {
            let mut members = Vec::new();
            if let Some(Value::Array(arr)) = field_value(map, "members") {
                for entry in arr {
                    members.push(decode_member(entry));
                }
            }
            Widget::PresenceList {
                group_id: field_string(map, "group_id"),
                members,
            }
        }
        WidgetTag::Chat => {
            let mut messages = Vec::new();
            if let Some(Value::Array(arr)) = field_value(map, "messages") {
                for entry in arr {
                    messages.push(decode_chat_message(entry));
                }
            }
            let compose_submit = match field_value(map, "compose") {
                Some(Value::Map(cmap)) => {
                    let mut submit = None;
                    for (k, vv) in cmap {
                        if let Some(key) = str_key(k, "") {
                            if key == "submit" {
                                submit = value_as_string(vv);
                            }
                        }
                    }
                    submit
                }
                _ => None,
            };
            Widget::Chat {
                group_id: field_string(map, "group_id"),
                messages,
                compose_submit,
            }
        }
        WidgetTag::Unknown(repr) => Widget::Unsupported { tag_repr: repr },
    }
}

fn decode_menu_item(v: &Value) -> MenuItem {
    let mut item = MenuItem {
        label: None,
        href: None,
        sub: None,
    };
    if let Value::Map(m) = v {
        for (k, vv) in m {
            if let Some(key) = str_key(k, "") {
                match key.as_str() {
                    "label" => item.label = value_as_string(vv),
                    "href" => item.href = value_as_string(vv),
                    "sub" => item.sub = value_as_string(vv),
                    _ => {}
                }
            }
        }
    }
    item
}

fn decode_member(v: &Value) -> Member {
    let mut m = Member {
        id: None,
        name: None,
        online: false,
    };
    if let Value::Map(map) = v {
        for (k, vv) in map {
            if let Some(key) = str_key(k, "") {
                match key.as_str() {
                    "id" => m.id = value_as_string(vv),
                    "name" => m.name = value_as_string(vv),
                    "online" => m.online = value_as_bool(vv).unwrap_or(false),
                    _ => {}
                }
            }
        }
    }
    m
}

fn decode_chat_message(v: &Value) -> ChatMessage {
    let mut msg = ChatMessage {
        from: None,
        ts: None,
        s: None,
    };
    if let Value::Map(m) = v {
        for (k, vv) in m {
            if let Some(key) = str_key(k, "") {
                match key.as_str() {
                    "from" => msg.from = value_as_string(vv),
                    "ts" => msg.ts = value_as_u64(vv),
                    "s" => msg.s = value_as_string(vv),
                    _ => {}
                }
            }
        }
    }
    msg
}

fn field_value<'a>(map: &'a Vec<(Value, Value)>, name: &str) -> Option<&'a Value> {
    for (k, v) in map {
        if let Value::Text(s) = k {
            if s == name {
                return Some(v);
            }
        }
    }
    None
}

fn field_string(map: &Vec<(Value, Value)>, name: &str) -> Option<String> {
    field_value(map, name).and_then(value_as_string)
}

fn field_u32(map: &Vec<(Value, Value)>, name: &str) -> Option<u32> {
    field_value(map, name).and_then(value_as_u32)
}

fn field_f64(map: &Vec<(Value, Value)>, name: &str) -> Option<f64> {
    field_value(map, name).and_then(value_as_f64)
}

/// Widget tag identity after normalising string/numeric forms.
#[derive(Clone, Debug, PartialEq, Eq)]
enum WidgetTag {
    Text,
    List,
    Input,
    Form,
    Button,
    Image,
    Map,
    Notification,
    PresenceList,
    Chat,
    /// Unknown widget; preserves its textual repr for `[unsupported: …]`.
    Unknown(String),
}

impl WidgetTag {
    fn parse(v: &Value) -> Self {
        match v {
            Value::Text(s) => match s.as_str() {
                "text" => WidgetTag::Text,
                "list" => WidgetTag::List,
                "input" => WidgetTag::Input,
                "form" => WidgetTag::Form,
                "button" => WidgetTag::Button,
                "image" => WidgetTag::Image,
                "map" => WidgetTag::Map,
                "notification" => WidgetTag::Notification,
                "presence_list" => WidgetTag::PresenceList,
                "chat" => WidgetTag::Chat,
                _ => WidgetTag::Unknown(s.clone()),
            },
            Value::Integer(i) => {
                let n = to_i128(*i);
                match n {
                    1 => WidgetTag::Text,
                    2 => WidgetTag::List,
                    3 => WidgetTag::Input,
                    4 => WidgetTag::Form,
                    5 => WidgetTag::Button,
                    6 => WidgetTag::Image,
                    7 => WidgetTag::Map,
                    8 => WidgetTag::Notification,
                    9 => WidgetTag::PresenceList,
                    10 => WidgetTag::Chat,
                    _ => WidgetTag::Unknown(format!("{n}")),
                }
            }
            _ => WidgetTag::Unknown("?".to_string()),
        }
    }
}
