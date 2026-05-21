//! SIM-005 — Network log.
//!
//! Every exchange the simulator's transport layer performs is appended
//! here so the network panel can show what went over the wire. The log
//! is transport-agnostic: `DirectClient` (SIM-004) records here today,
//! and proxy mode (SIM-006) will record the same shape with
//! `transport = "proxy"` so the same UI works for both.
//!
//! Storage is a bounded ring buffer kept entirely in memory — restart-
//! and-forget is the right semantics for a developer tool. Bodies are
//! capped at [`MAX_BODY_BYTES`] each so a runaway server can't balloon
//! the log; truncation is surfaced to the UI via [`Body::truncated`].
//!
//! The CBOR pretty-printer below is intentionally local. We don't want
//! to pull in another diagnostic-notation crate, and the existing
//! `pageros-renderer` decoder is Frame-shaped — it would reject any
//! non-Frame request body. The output is a multi-line, indented form
//! roughly matching RFC 8949 §8 (CBOR diagnostic notation) with byte-
//! string truncation and string escaping.
//!
//! Keep the on-the-wire serde shape stable: the frontend reads it
//! through Tauri commands, and any rename will break the UI.

use std::sync::Mutex;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use ciborium::value::{Integer, Value};
use serde::Serialize;

/// Max bytes recorded per request/response body. Larger payloads are
/// truncated at this length and flagged via `Body::truncated`. 64 KiB
/// is well above any Frame in the test-vector set (largest is ~3 KiB)
/// while keeping the log cheap.
pub const MAX_BODY_BYTES: usize = 64 * 1024;

/// Cap on retained exchanges. Each entry holds at most
/// `2 * MAX_BODY_BYTES + overhead` so 128 entries is ~16 MiB worst
/// case — fine for an interactive debug tool, far below any device.
pub const MAX_EXCHANGES: usize = 128;

/// Stable transport tag surfaced in the network panel's "transport"
/// column. Strings (not an enum) so SIM-006 / SIM-007 can add their own
/// labels without a coupling commit here.
pub type Transport = &'static str;

pub const TRANSPORT_DIRECT: Transport = "direct";

/// A single header captured for display. Multi-value headers are flattened
/// into separate entries in arrival order.
#[derive(Clone, Debug, Serialize, PartialEq, Eq)]
pub struct Header {
    pub name: String,
    pub value: String,
}

/// One side of an exchange's body — request OR response. `bytes_len` is
/// the *original* size so the UI can show "truncated 64 KiB of 200 KiB"
/// even though `bytes` is clipped.
#[derive(Clone, Debug, Serialize, PartialEq, Eq)]
pub struct Body {
    /// Raw bytes, capped at [`MAX_BODY_BYTES`].
    pub bytes: Vec<u8>,
    /// Length of the body *before* truncation.
    pub bytes_len: usize,
    /// `true` when `bytes.len() < bytes_len`.
    pub truncated: bool,
}

impl Body {
    pub fn empty() -> Self {
        Self { bytes: Vec::new(), bytes_len: 0, truncated: false }
    }

    pub fn from_slice(b: &[u8]) -> Self {
        let bytes_len = b.len();
        if bytes_len <= MAX_BODY_BYTES {
            Self { bytes: b.to_vec(), bytes_len, truncated: false }
        } else {
            Self { bytes: b[..MAX_BODY_BYTES].to_vec(), bytes_len, truncated: true }
        }
    }
}

/// Outcome surfaced to the UI. We split "successful exchange" from
/// "transport error" because the panel treats them very differently —
/// transport errors have no response body to decode, but we still want
/// to show the request that failed and the error string.
#[derive(Clone, Debug, Serialize, PartialEq, Eq)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum Outcome {
    /// HTTP exchange completed (any status). `status` is the HTTP code.
    Response {
        status: u16,
        response_headers: Vec<Header>,
        response_body: Body,
    },
    /// Transport-layer failure (DNS, connect, timeout, …). `kind` is the
    /// short label from `DirectError::kind()`; `message` is the human-
    /// readable rendering.
    Error { error_kind: String, message: String },
}

/// One captured exchange.
#[derive(Clone, Debug, Serialize, PartialEq, Eq)]
pub struct Exchange {
    /// Monotonic, per-process id. Reused as the dropdown key.
    pub id: u64,
    pub transport: String,
    pub method: String,
    pub url: String,
    /// `started_at` as ms since unix epoch — frontend formats it.
    pub started_at_ms: u128,
    /// Wall-clock latency from request send to response received / error.
    pub latency_ms: u64,
    pub request_headers: Vec<Header>,
    pub request_body: Body,
    pub outcome: Outcome,
}

impl Exchange {
    /// Short label for the dropdown list. Status first when present, then
    /// method, path-or-host, latency.
    pub fn label(&self) -> String {
        let status = match &self.outcome {
            Outcome::Response { status, .. } => format!("{status}"),
            Outcome::Error { .. } => "ERR".to_string(),
        };
        let path = url_path_or_full(&self.url);
        format!("{} {} {} ({}ms)", status, self.method, path, self.latency_ms)
    }
}

fn url_path_or_full(url: &str) -> String {
    // Strip scheme://host[:port] to keep the dropdown narrow. Falls
    // back to the full URL if parsing fails (we'd rather show too much
    // than nothing).
    match ::url::Url::parse(url) {
        Ok(u) => {
            let mut s = u.path().to_string();
            if let Some(q) = u.query() {
                s.push('?');
                s.push_str(q);
            }
            s
        }
        Err(_) => url.to_string(),
    }
}

/// Mutex-wrapped ring buffer. Cheap to clone via `Arc<NetworkLog>`; the
/// recorder takes `&self`.
#[derive(Default)]
pub struct NetworkLog {
    inner: Mutex<NetworkLogInner>,
}

#[derive(Default)]
struct NetworkLogInner {
    next_id: u64,
    exchanges: Vec<Exchange>,
}

impl NetworkLog {
    pub fn new() -> Self {
        Self::default()
    }

    /// Reserve an exchange id + capture the start moment. Pair with
    /// [`Self::finish_response`] / [`Self::finish_error`].
    pub fn begin(
        &self,
        transport: Transport,
        method: &str,
        url: &str,
        request_headers: Vec<Header>,
        request_body: Body,
    ) -> Pending {
        let mut inner = self.lock();
        inner.next_id += 1;
        let id = inner.next_id;
        let started_at_ms = now_ms();
        Pending {
            id,
            transport,
            method: method.to_string(),
            url: url.to_string(),
            started_at_ms,
            started_at: SystemTime::now(),
            request_headers,
            request_body,
        }
    }

    pub fn finish_response(
        &self,
        pending: Pending,
        status: u16,
        response_headers: Vec<Header>,
        response_body: Body,
    ) {
        let latency_ms = latency_ms_since(pending.started_at);
        let ex = Exchange {
            id: pending.id,
            transport: pending.transport.to_string(),
            method: pending.method,
            url: pending.url,
            started_at_ms: pending.started_at_ms,
            latency_ms,
            request_headers: pending.request_headers,
            request_body: pending.request_body,
            outcome: Outcome::Response { status, response_headers, response_body },
        };
        self.push(ex);
    }

    pub fn finish_error(&self, pending: Pending, error_kind: &str, message: &str) {
        let latency_ms = latency_ms_since(pending.started_at);
        let ex = Exchange {
            id: pending.id,
            transport: pending.transport.to_string(),
            method: pending.method,
            url: pending.url,
            started_at_ms: pending.started_at_ms,
            latency_ms,
            request_headers: pending.request_headers,
            request_body: pending.request_body,
            outcome: Outcome::Error {
                error_kind: error_kind.to_string(),
                message: message.to_string(),
            },
        };
        self.push(ex);
    }

    /// Most recent first. Used by the dropdown.
    pub fn snapshot(&self) -> Vec<Exchange> {
        let inner = self.lock();
        inner.exchanges.iter().rev().cloned().collect()
    }

    pub fn get(&self, id: u64) -> Option<Exchange> {
        let inner = self.lock();
        inner.exchanges.iter().find(|e| e.id == id).cloned()
    }

    pub fn clear(&self) {
        let mut inner = self.lock();
        inner.exchanges.clear();
    }

    fn push(&self, ex: Exchange) {
        let mut inner = self.lock();
        inner.exchanges.push(ex);
        if inner.exchanges.len() > MAX_EXCHANGES {
            let overflow = inner.exchanges.len() - MAX_EXCHANGES;
            inner.exchanges.drain(..overflow);
        }
    }

    fn lock(&self) -> std::sync::MutexGuard<'_, NetworkLogInner> {
        self.inner.lock().expect("network log poisoned")
    }
}

/// Handed back from `begin`, consumed by `finish_response` / `finish_error`.
/// Holds enough state that the recorder doesn't need to lock between
/// begin and finish, so a slow request doesn't block the snapshot reader.
pub struct Pending {
    id: u64,
    transport: Transport,
    method: String,
    url: String,
    started_at_ms: u128,
    started_at: SystemTime,
    request_headers: Vec<Header>,
    request_body: Body,
}

impl Pending {
    pub fn id(&self) -> u64 {
        self.id
    }
}

fn latency_ms_since(start: SystemTime) -> u64 {
    SystemTime::now()
        .duration_since(start)
        .unwrap_or(Duration::ZERO)
        .as_millis()
        .min(u128::from(u64::MAX)) as u64
}

fn now_ms() -> u128 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or(Duration::ZERO)
        .as_millis()
}

/// Pretty-print CBOR bytes as multi-line diagnostic notation. Returns
/// the printed text. On decode failure, returns a single-line error so
/// the panel always has *something* to show — the raw hex is rendered
/// alongside, so the user can still see what came in.
pub fn pretty_print_cbor(bytes: &[u8]) -> String {
    if bytes.is_empty() {
        return "(empty)".to_string();
    }
    let mut cursor = bytes;
    match ciborium::de::from_reader::<Value, _>(&mut cursor) {
        Ok(value) => {
            let mut out = String::new();
            write_value(&value, 0, &mut out);
            if !cursor.is_empty() {
                out.push_str(&format!(
                    "\n; trailing {} byte(s) of unparsed data",
                    cursor.len()
                ));
            }
            out
        }
        Err(e) => format!("; cbor decode error: {e}"),
    }
}

fn write_value(v: &Value, indent: usize, out: &mut String) {
    match v {
        Value::Null => out.push_str("null"),
        Value::Bool(b) => out.push_str(if *b { "true" } else { "false" }),
        Value::Integer(i) => out.push_str(&format_integer(i)),
        Value::Float(f) => out.push_str(&format!("{f}")),
        Value::Text(s) => out.push_str(&format!("{:?}", s)),
        Value::Bytes(b) => write_bytes(b, out),
        Value::Tag(tag, inner) => {
            out.push_str(&format!("{tag}("));
            write_value(inner, indent, out);
            out.push(')');
        }
        Value::Array(items) => write_array(items, indent, out),
        Value::Map(pairs) => write_map(pairs, indent, out),
        // Catch-all for forward-compatible Value variants in newer ciborium.
        _ => out.push_str("?"),
    }
}

fn format_integer(i: &Integer) -> String {
    // ciborium's `Integer` already converts losslessly to i128 (it can't
    // hold anything wider), so just use that.
    let n: i128 = (*i).into();
    n.to_string()
}

fn write_bytes(b: &[u8], out: &mut String) {
    const MAX_HEX: usize = 64;
    if b.len() <= MAX_HEX {
        out.push_str("h'");
        for byte in b {
            out.push_str(&format!("{:02x}", byte));
        }
        out.push('\'');
    } else {
        out.push_str("h'");
        for byte in &b[..MAX_HEX] {
            out.push_str(&format!("{:02x}", byte));
        }
        out.push_str(&format!("…' ; {} bytes total", b.len()));
    }
}

fn write_array(items: &[Value], indent: usize, out: &mut String) {
    if items.is_empty() {
        out.push_str("[]");
        return;
    }
    out.push('[');
    let inner_indent = indent + 2;
    for (i, item) in items.iter().enumerate() {
        out.push('\n');
        push_indent(inner_indent, out);
        write_value(item, inner_indent, out);
        if i + 1 < items.len() {
            out.push(',');
        }
    }
    out.push('\n');
    push_indent(indent, out);
    out.push(']');
}

fn write_map(pairs: &[(Value, Value)], indent: usize, out: &mut String) {
    if pairs.is_empty() {
        out.push_str("{}");
        return;
    }
    out.push('{');
    let inner_indent = indent + 2;
    for (i, (k, val)) in pairs.iter().enumerate() {
        out.push('\n');
        push_indent(inner_indent, out);
        write_value(k, inner_indent, out);
        out.push_str(": ");
        write_value(val, inner_indent, out);
        if i + 1 < pairs.len() {
            out.push(',');
        }
    }
    out.push('\n');
    push_indent(indent, out);
    out.push('}');
}

fn push_indent(n: usize, out: &mut String) {
    for _ in 0..n {
        out.push(' ');
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn h(name: &str, value: &str) -> Header {
        Header { name: name.into(), value: value.into() }
    }

    #[test]
    fn body_from_slice_records_full_payload_when_small() {
        let b = Body::from_slice(b"hello");
        assert_eq!(b.bytes, b"hello");
        assert_eq!(b.bytes_len, 5);
        assert!(!b.truncated);
    }

    #[test]
    fn body_from_slice_truncates_oversize_payload() {
        let big = vec![0xabu8; MAX_BODY_BYTES + 256];
        let b = Body::from_slice(&big);
        assert_eq!(b.bytes.len(), MAX_BODY_BYTES);
        assert_eq!(b.bytes_len, MAX_BODY_BYTES + 256);
        assert!(b.truncated);
    }

    #[test]
    fn log_records_response_and_assigns_ids() {
        let log = NetworkLog::new();
        let p1 = log.begin(
            TRANSPORT_DIRECT,
            "GET",
            "http://example.test/",
            vec![h("accept", "application/cbor")],
            Body::empty(),
        );
        let p2 = log.begin(
            TRANSPORT_DIRECT,
            "POST",
            "http://example.test/save",
            vec![h("content-type", "application/cbor")],
            Body::from_slice(b"\x82\x01\x02"),
        );
        log.finish_response(p1, 200, vec![h("content-type", "application/cbor")], Body::from_slice(b"hi"));
        log.finish_response(p2, 201, vec![], Body::from_slice(b"\x83\x01\x02\x03"));

        let snap = log.snapshot();
        assert_eq!(snap.len(), 2);
        // newest first
        assert_eq!(snap[0].method, "POST");
        assert_eq!(snap[1].method, "GET");
        assert!(snap[0].id > snap[1].id);
        let id = snap[1].id;
        assert_eq!(log.get(id).unwrap().method, "GET");
    }

    #[test]
    fn log_records_transport_error() {
        let log = NetworkLog::new();
        let p = log.begin(TRANSPORT_DIRECT, "GET", "http://x/", vec![], Body::empty());
        log.finish_error(p, "transport", "connect: refused");
        let snap = log.snapshot();
        assert_eq!(snap.len(), 1);
        match &snap[0].outcome {
            Outcome::Error { error_kind, message } => {
                assert_eq!(error_kind, "transport");
                assert!(message.contains("connect"));
            }
            other => panic!("expected error outcome, got {other:?}"),
        }
    }

    #[test]
    fn log_ring_buffer_caps_at_max_exchanges() {
        let log = NetworkLog::new();
        for _ in 0..(MAX_EXCHANGES + 25) {
            let p = log.begin(TRANSPORT_DIRECT, "GET", "http://x/", vec![], Body::empty());
            log.finish_response(p, 200, vec![], Body::empty());
        }
        let snap = log.snapshot();
        assert_eq!(snap.len(), MAX_EXCHANGES);
        // Newest first → first id in snapshot is the very last begin().
        assert_eq!(snap[0].id, (MAX_EXCHANGES + 25) as u64);
    }

    #[test]
    fn log_clear_empties_storage() {
        let log = NetworkLog::new();
        let p = log.begin(TRANSPORT_DIRECT, "GET", "http://x/", vec![], Body::empty());
        log.finish_response(p, 200, vec![], Body::empty());
        assert_eq!(log.snapshot().len(), 1);
        log.clear();
        assert!(log.snapshot().is_empty());
    }

    #[test]
    fn label_uses_path_when_url_parses() {
        let ex = Exchange {
            id: 1,
            transport: TRANSPORT_DIRECT.into(),
            method: "GET".into(),
            url: "http://localhost:8080/menu?x=1".into(),
            started_at_ms: 0,
            latency_ms: 12,
            request_headers: vec![],
            request_body: Body::empty(),
            outcome: Outcome::Response {
                status: 200,
                response_headers: vec![],
                response_body: Body::empty(),
            },
        };
        assert_eq!(ex.label(), "200 GET /menu?x=1 (12ms)");
    }

    #[test]
    fn pretty_print_empty_returns_marker() {
        assert_eq!(pretty_print_cbor(&[]), "(empty)");
    }

    #[test]
    fn pretty_print_simple_map() {
        // {"a": 1, "b": [2, 3]} encoded with ciborium for stability.
        let mut buf = Vec::new();
        let val = Value::Map(vec![
            (Value::Text("a".into()), Value::Integer(1.into())),
            (
                Value::Text("b".into()),
                Value::Array(vec![Value::Integer(2.into()), Value::Integer(3.into())]),
            ),
        ]);
        ciborium::ser::into_writer(&val, &mut buf).unwrap();
        let printed = pretty_print_cbor(&buf);
        assert!(printed.starts_with('{'));
        assert!(printed.contains("\"a\": 1"));
        assert!(printed.contains("\"b\": ["));
        assert!(printed.contains("2,"));
        assert!(printed.contains("3"));
    }

    #[test]
    fn pretty_print_bytes_truncates_long_runs() {
        let big = vec![0u8; 200];
        let val = Value::Bytes(big);
        let mut buf = Vec::new();
        ciborium::ser::into_writer(&val, &mut buf).unwrap();
        let printed = pretty_print_cbor(&buf);
        assert!(printed.contains("200 bytes total"));
    }

    #[test]
    fn pretty_print_reports_decode_error() {
        // 0x1f is "indefinite-length array" with no break → decode error.
        let printed = pretty_print_cbor(&[0x1f]);
        assert!(printed.starts_with("; cbor decode error"));
    }

    #[test]
    fn pretty_print_flags_trailing_bytes() {
        // Encode `1` then append `2` so a single Value decode leaves a trailer.
        let mut buf = Vec::new();
        ciborium::ser::into_writer(&Value::Integer(1.into()), &mut buf).unwrap();
        buf.push(0x02);
        let printed = pretty_print_cbor(&buf);
        assert!(printed.contains("trailing 1 byte"));
    }
}
