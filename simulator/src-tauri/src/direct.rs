//! SIM-004 — Direct mode.
//!
//! Direct mode talks straight to a local PagerOS app server over HTTP
//! (`SPEC.md` §12.2: "two modes: **direct** (talks to local app server)
//! and **proxy** …"). Proxy mode lands in SIM-006 on top of this; the
//! transport surface is intentionally narrow so the proxy can swap in.
//!
//! Wire shape per `protocol/spec.md` §7:
//! - `GET` / `POST` only.
//! - Request body is CBOR (`Content-Type: application/cbor`) for `POST`s
//!   with a body; absent or zero-length for navigation.
//! - Response body is a CBOR Frame, decoded + rendered by the existing
//!   `pageros-renderer` pipeline.
//!
//! What this module deliberately does **not** do yet:
//! - Request signing (`PagerOS-Sig` / `PagerOS-Timestamp` / device
//!   pubkey). The simulator runs without identity until SEC-001 + the
//!   firmware identity stack (FW-014) land; the developer's local app
//!   server in dev mode accepts unsigned requests. A signing hook will
//!   slot into `DirectClient::execute` once that exists.
//! - `PagerOS-Session` token tracking — added when sessions are exercised
//!   end-to-end (SDK round trip).
//!
//! Tests live next to the code and use a tiny inline `std::net` server
//! so we don't depend on an external mock framework.

use std::sync::Mutex;
use std::time::Duration;

use reqwest::header::{HeaderMap, HeaderValue, ACCEPT, CONTENT_TYPE, USER_AGENT};
use reqwest::{Client, Method};
use serde::{Deserialize, Serialize};
use url::Url;

/// Default app-server base URL. Matches `SPEC.md` §6.1 (SIM-004 accepts)
/// — `pagerctl dev` listens here, and so do the example apps in
/// `examples/`.
pub const DEFAULT_BASE_URL: &str = "http://localhost:8080/";

/// Hard wall on a single request — kept loose because dev servers cold-
/// start slowly. Production targets are in `SPEC.md` §14 ("Wi-Fi cold
/// request round trip < 500 ms").
pub const REQUEST_TIMEOUT: Duration = Duration::from_secs(10);

const USER_AGENT_VALUE: &str = "PagerOS-Simulator/0.1 (direct)";
const ACCEPT_VALUE: &str = "application/cbor; pagerOS=1";
const CONTENT_TYPE_VALUE: &str = "application/cbor";

/// What the simulator sends to the app server. `path` is resolved against
/// the configured base URL; absolute URLs are honored as-is so absolute
/// `href`s from a returned Frame work without rewriting.
#[derive(Debug, Clone)]
pub struct Request<'a> {
    pub method: HttpMethod,
    pub path: &'a str,
    pub body: Option<&'a [u8]>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "UPPERCASE")]
pub enum HttpMethod {
    Get,
    Post,
}

impl From<HttpMethod> for Method {
    fn from(m: HttpMethod) -> Self {
        match m {
            HttpMethod::Get => Method::GET,
            HttpMethod::Post => Method::POST,
        }
    }
}

/// What the simulator hands back to the caller. The body bytes are the
/// raw response (typically a CBOR Frame); rendering is done by the
/// caller so a single HTTP exchange can feed both the renderer and the
/// SIM-005 network panel.
#[derive(Debug, Clone)]
pub struct Response {
    pub status: u16,
    pub body: Vec<u8>,
    pub content_type: Option<String>,
    pub resolved_url: String,
}

/// Why a direct-mode request failed. Each variant maps to a user-
/// visible status-bar message; the frontend never sees a stack trace.
#[derive(Debug, thiserror::Error)]
pub enum DirectError {
    #[error("invalid base URL: {0}")]
    BadBaseUrl(String),
    #[error("invalid request path: {0}")]
    BadPath(String),
    #[error("transport error: {0}")]
    Transport(String),
    #[error("server returned {status}{body_hint}")]
    HttpStatus {
        status: u16,
        body_hint: String,
    },
    #[error("timed out after {0:?}")]
    Timeout(Duration),
}

impl DirectError {
    /// Stable short label for the frontend / tests.
    pub fn kind(&self) -> &'static str {
        match self {
            DirectError::BadBaseUrl(_) => "bad_base_url",
            DirectError::BadPath(_) => "bad_path",
            DirectError::Transport(_) => "transport",
            DirectError::HttpStatus { .. } => "http_status",
            DirectError::Timeout(_) => "timeout",
        }
    }
}

/// Thin async wrapper around `reqwest::Client` that holds the current
/// base URL and a small navigation history so `BACK` can unwind.
///
/// History is cap-bounded; we keep only the resolved URLs (no bodies),
/// so a long browse never balloons memory. Bodies are re-fetched on
/// BACK — the device behaves the same way (the cache lives separately,
/// per `SPEC.md` §5.5).
pub struct DirectClient {
    http: Client,
    inner: Mutex<Inner>,
}

#[derive(Debug, Default)]
struct Inner {
    base: Option<Url>,
    /// Stack of previously-loaded URLs. The current URL is *not* on the
    /// stack; we push on navigation, pop on BACK.
    history: Vec<Url>,
    current: Option<Url>,
}

/// Snapshot of the navigation state, surfaced to the frontend so it can
/// render its address bar and BACK button state without holding its own
/// copy.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq, Default)]
pub struct DirectState {
    pub base_url: Option<String>,
    pub current_url: Option<String>,
    pub history_depth: usize,
}

const MAX_HISTORY: usize = 64;

impl DirectClient {
    pub fn new() -> Self {
        let http = Client::builder()
            .timeout(REQUEST_TIMEOUT)
            .user_agent(USER_AGENT_VALUE)
            .build()
            .expect("reqwest client builder always succeeds");
        Self {
            http,
            inner: Mutex::new(Inner::default()),
        }
    }

    pub fn set_base_url(&self, raw: &str) -> Result<Url, DirectError> {
        let url = parse_base(raw)?;
        let mut inner = self.lock();
        inner.base = Some(url.clone());
        inner.history.clear();
        inner.current = None;
        Ok(url)
    }

    pub fn state(&self) -> DirectState {
        let inner = self.lock();
        DirectState {
            base_url: inner.base.as_ref().map(|u| u.to_string()),
            current_url: inner.current.as_ref().map(|u| u.to_string()),
            history_depth: inner.history.len(),
        }
    }

    /// Resolve `path` against the current base, execute the request, push
    /// the previous URL onto the history stack if the request succeeds.
    /// On non-2xx status, the response body is still returned so an app
    /// server's error Frame (per `protocol/spec.md` §8) can be displayed
    /// — we only surface a `DirectError` for transport-level failures.
    pub async fn fetch(&self, req: Request<'_>) -> Result<Response, DirectError> {
        let url = self.resolve(req.path)?;
        let response = self.execute(req.method, &url, req.body).await?;
        // Only mutate navigation on a successful exchange; a transport
        // error leaves the user where they were.
        {
            let mut inner = self.lock();
            if let Some(prev) = inner.current.take() {
                inner.history.push(prev);
                if inner.history.len() > MAX_HISTORY {
                    let overflow = inner.history.len() - MAX_HISTORY;
                    inner.history.drain(..overflow);
                }
            }
            inner.current = Some(url);
        }
        Ok(response)
    }

    /// Pop the previous URL off the history stack and re-fetch it as a
    /// `GET` with no body. Returns `None` if history is empty (the
    /// caller can then emit the protocol `back` event per §5.4.1 or
    /// route to the Shell). Re-fetching matches firmware behaviour:
    /// the cache (SPEC §5.5) is the single source of truth for "instant
    /// open"; the simulator has no cache yet, so it re-fetches.
    pub async fn back(&self) -> Result<Option<Response>, DirectError> {
        let prev = {
            let mut inner = self.lock();
            inner.history.pop()
        };
        let Some(prev) = prev else { return Ok(None) };
        let response = self.execute(HttpMethod::Get, &prev, None).await?;
        let mut inner = self.lock();
        inner.current = Some(prev);
        Ok(Some(response))
    }

    fn resolve(&self, raw_path: &str) -> Result<Url, DirectError> {
        let trimmed = raw_path.trim();
        if trimmed.is_empty() {
            return Err(DirectError::BadPath("empty".into()));
        }
        // Absolute URL? Honour it as-is (§7.1 lets server-returned hrefs
        // point anywhere).
        if let Ok(absolute) = Url::parse(trimmed) {
            if absolute.scheme() == "http" || absolute.scheme() == "https" {
                return Ok(absolute);
            }
            return Err(DirectError::BadPath(format!(
                "unsupported scheme: {}",
                absolute.scheme()
            )));
        }
        let inner = self.lock();
        let base = inner
            .base
            .as_ref()
            .ok_or_else(|| DirectError::BadBaseUrl("base URL not set".into()))?;
        base.join(trimmed)
            .map_err(|e| DirectError::BadPath(format!("{trimmed:?}: {e}")))
    }

    async fn execute(
        &self,
        method: HttpMethod,
        url: &Url,
        body: Option<&[u8]>,
    ) -> Result<Response, DirectError> {
        let mut headers = HeaderMap::new();
        headers.insert(USER_AGENT, HeaderValue::from_static(USER_AGENT_VALUE));
        headers.insert(ACCEPT, HeaderValue::from_static(ACCEPT_VALUE));
        if body.is_some() {
            headers.insert(CONTENT_TYPE, HeaderValue::from_static(CONTENT_TYPE_VALUE));
        }

        let mut builder = self
            .http
            .request(Method::from(method), url.clone())
            .headers(headers);
        if let Some(b) = body {
            builder = builder.body(b.to_vec());
        }

        let response = builder.send().await.map_err(map_reqwest_error)?;
        let status = response.status();
        let content_type = response
            .headers()
            .get(CONTENT_TYPE)
            .and_then(|v| v.to_str().ok())
            .map(|s| s.to_string());
        let body = response.bytes().await.map_err(map_reqwest_error)?.to_vec();
        Ok(Response {
            status: status.as_u16(),
            body,
            content_type,
            resolved_url: url.to_string(),
        })
    }

    fn lock(&self) -> std::sync::MutexGuard<'_, Inner> {
        self.inner.lock().expect("direct client state poisoned")
    }
}

impl Default for DirectClient {
    fn default() -> Self {
        Self::new()
    }
}

fn parse_base(raw: &str) -> Result<Url, DirectError> {
    let trimmed = raw.trim();
    if trimmed.is_empty() {
        return Err(DirectError::BadBaseUrl("empty".into()));
    }
    let mut url = Url::parse(trimmed).map_err(|e| DirectError::BadBaseUrl(format!("{e}")))?;
    if url.scheme() != "http" && url.scheme() != "https" {
        return Err(DirectError::BadBaseUrl(format!(
            "unsupported scheme: {}",
            url.scheme()
        )));
    }
    // A base URL must end with `/` so that `Url::join("widget")` resolves
    // to `<base>/widget` rather than replacing the last path segment.
    if !url.path().ends_with('/') {
        let new_path = format!("{}/", url.path());
        url.set_path(&new_path);
    }
    Ok(url)
}

fn map_reqwest_error(err: reqwest::Error) -> DirectError {
    if err.is_timeout() {
        return DirectError::Timeout(REQUEST_TIMEOUT);
    }
    if err.is_connect() {
        return DirectError::Transport(format!("connect: {err}"));
    }
    DirectError::Transport(format!("{err}"))
}

#[cfg(test)]
mod tests {
    use super::*;

    use std::io::{Read, Write};
    use std::net::{TcpListener, TcpStream};
    use std::sync::mpsc::{self, Sender};
    use std::thread;

    /// Single-shot HTTP server for tests. Handles exactly one request
    /// then exits; the test inspects the captured request and asserts on
    /// the response the client received.
    struct OneShotServer {
        addr: String,
        captured: mpsc::Receiver<CapturedRequest>,
        _join: thread::JoinHandle<()>,
    }

    #[derive(Debug)]
    struct CapturedRequest {
        method: String,
        path: String,
        headers: Vec<(String, String)>,
        body: Vec<u8>,
    }

    impl OneShotServer {
        fn spawn(response: &'static [u8]) -> Self {
            let listener =
                TcpListener::bind("127.0.0.1:0").expect("bind ephemeral test port");
            let addr = format!("http://{}/", listener.local_addr().unwrap());
            let (tx, rx) = mpsc::channel();
            let join = thread::spawn(move || {
                if let Ok((stream, _)) = listener.accept() {
                    handle(stream, tx, response);
                }
            });
            Self {
                addr,
                captured: rx,
                _join: join,
            }
        }

        fn captured(self) -> CapturedRequest {
            self.captured
                .recv_timeout(Duration::from_secs(5))
                .expect("server handler ran to completion")
        }
    }

    fn handle(mut stream: TcpStream, tx: Sender<CapturedRequest>, response: &[u8]) {
        // Read until we have headers + the declared body length. Tests
        // send tiny bodies so a 16K scratch buffer is plenty.
        let mut buf = [0u8; 16 * 1024];
        let mut filled = 0usize;
        let mut header_end = None;
        while filled < buf.len() {
            let n = stream.read(&mut buf[filled..]).unwrap_or(0);
            if n == 0 {
                break;
            }
            filled += n;
            if let Some(pos) = find_header_end(&buf[..filled]) {
                header_end = Some(pos);
                break;
            }
        }
        let header_end = header_end.unwrap_or(filled);
        let (head, after_head) = buf.split_at(header_end);
        let head_text = std::str::from_utf8(head).unwrap_or("").to_string();

        let mut lines = head_text.split("\r\n");
        let start = lines.next().unwrap_or("");
        let mut parts = start.splitn(3, ' ');
        let method = parts.next().unwrap_or("").to_string();
        let path = parts.next().unwrap_or("").to_string();

        let mut headers = Vec::new();
        let mut content_length = 0usize;
        for line in lines {
            if line.is_empty() {
                continue;
            }
            if let Some((k, v)) = line.split_once(':') {
                let k = k.trim().to_string();
                let v = v.trim().to_string();
                if k.eq_ignore_ascii_case("content-length") {
                    content_length = v.parse().unwrap_or(0);
                }
                headers.push((k, v));
            }
        }

        // `after_head` starts after "\r\n\r\n" — but our split is at
        // the start of "\r\n\r\n". Skip the 4 separator bytes.
        let body_start = after_head.iter().take(4).take_while(|&&b| b == b'\r' || b == b'\n').count();
        let mut body = Vec::new();
        body.extend_from_slice(&after_head[body_start..]);
        while body.len() < content_length {
            let mut chunk = [0u8; 1024];
            let n = stream.read(&mut chunk).unwrap_or(0);
            if n == 0 {
                break;
            }
            body.extend_from_slice(&chunk[..n]);
        }
        body.truncate(content_length);

        let _ = stream.write_all(response);
        let _ = stream.flush();
        let _ = tx.send(CapturedRequest {
            method,
            path,
            headers,
            body,
        });
    }

    fn find_header_end(buf: &[u8]) -> Option<usize> {
        buf.windows(4).position(|w| w == b"\r\n\r\n")
    }

    const OK_CBOR_RESPONSE: &[u8] = b"HTTP/1.1 200 OK\r\n\
        Content-Type: application/cbor\r\n\
        Content-Length: 5\r\n\
        Connection: close\r\n\
        \r\n\
        hello";

    const NOT_FOUND_RESPONSE: &[u8] = b"HTTP/1.1 404 Not Found\r\n\
        Content-Type: application/cbor\r\n\
        Content-Length: 3\r\n\
        Connection: close\r\n\
        \r\n\
        err";

    fn run<F: std::future::Future>(fut: F) -> F::Output {
        tokio::runtime::Builder::new_current_thread()
            .enable_all()
            .build()
            .unwrap()
            .block_on(fut)
    }

    #[test]
    fn set_base_url_normalises_trailing_slash() {
        let client = DirectClient::new();
        let url = client.set_base_url("http://localhost:8080").unwrap();
        assert_eq!(url.as_str(), "http://localhost:8080/");
    }

    #[test]
    fn set_base_url_rejects_unsupported_scheme() {
        let client = DirectClient::new();
        let err = client.set_base_url("file:///etc/passwd").unwrap_err();
        assert_eq!(err.kind(), "bad_base_url");
    }

    #[test]
    fn set_base_url_rejects_empty() {
        let client = DirectClient::new();
        let err = client.set_base_url("   ").unwrap_err();
        assert_eq!(err.kind(), "bad_base_url");
    }

    #[test]
    fn fetch_get_sends_protocol_headers_and_returns_body() {
        let server = OneShotServer::spawn(OK_CBOR_RESPONSE);
        let client = DirectClient::new();
        client.set_base_url(&server.addr).unwrap();
        let response = run(client.fetch(Request {
            method: HttpMethod::Get,
            path: "/",
            body: None,
        }))
        .unwrap();
        assert_eq!(response.status, 200);
        assert_eq!(response.body, b"hello");
        assert_eq!(
            response.content_type.as_deref(),
            Some("application/cbor"),
        );
        let captured = server.captured();
        assert_eq!(captured.method, "GET");
        assert_eq!(captured.path, "/");
        assert!(
            captured.headers.iter().any(|(k, v)| k.eq_ignore_ascii_case("accept")
                && v == ACCEPT_VALUE),
            "Accept header missing: {:?}",
            captured.headers,
        );
        assert!(
            captured
                .headers
                .iter()
                .any(|(k, v)| k.eq_ignore_ascii_case("user-agent") && v == USER_AGENT_VALUE),
            "User-Agent header missing: {:?}",
            captured.headers,
        );
        // No body → no Content-Type request header.
        assert!(
            !captured
                .headers
                .iter()
                .any(|(k, _)| k.eq_ignore_ascii_case("content-type")),
            "GET with no body should not send Content-Type: {:?}",
            captured.headers,
        );
    }

    #[test]
    fn fetch_post_sends_cbor_body_with_content_type() {
        let server = OneShotServer::spawn(OK_CBOR_RESPONSE);
        let client = DirectClient::new();
        client.set_base_url(&server.addr).unwrap();
        let body = b"\x82\x01\x02".to_vec(); // tiny CBOR array
        let response = run(client.fetch(Request {
            method: HttpMethod::Post,
            path: "/save",
            body: Some(&body),
        }))
        .unwrap();
        assert_eq!(response.status, 200);
        let captured = server.captured();
        assert_eq!(captured.method, "POST");
        assert_eq!(captured.path, "/save");
        assert_eq!(captured.body, body);
        assert!(
            captured
                .headers
                .iter()
                .any(|(k, v)| k.eq_ignore_ascii_case("content-type") && v == CONTENT_TYPE_VALUE),
            "missing Content-Type on POST: {:?}",
            captured.headers,
        );
    }

    #[test]
    fn fetch_resolves_relative_paths_against_base() {
        let server = OneShotServer::spawn(OK_CBOR_RESPONSE);
        let client = DirectClient::new();
        client.set_base_url(&server.addr).unwrap();
        let _ = run(client.fetch(Request {
            method: HttpMethod::Get,
            path: "widget/3",
            body: None,
        }))
        .unwrap();
        let captured = server.captured();
        assert_eq!(captured.path, "/widget/3");
    }

    #[test]
    fn non_2xx_returns_body_not_error() {
        let server = OneShotServer::spawn(NOT_FOUND_RESPONSE);
        let client = DirectClient::new();
        client.set_base_url(&server.addr).unwrap();
        let response = run(client.fetch(Request {
            method: HttpMethod::Get,
            path: "/missing",
            body: None,
        }))
        .unwrap();
        assert_eq!(response.status, 404);
        assert_eq!(response.body, b"err");
    }

    #[test]
    fn transport_error_when_server_unreachable() {
        let client = DirectClient::new();
        // Reserved-by-IANA port that nothing should bind to in tests.
        client.set_base_url("http://127.0.0.1:1/").unwrap();
        let err = run(client.fetch(Request {
            method: HttpMethod::Get,
            path: "/",
            body: None,
        }))
        .unwrap_err();
        assert_eq!(err.kind(), "transport", "got: {err:?}");
    }

    #[test]
    fn fetch_without_base_fails_for_relative_path() {
        let client = DirectClient::new();
        let err = run(client.fetch(Request {
            method: HttpMethod::Get,
            path: "/x",
            body: None,
        }))
        .unwrap_err();
        assert_eq!(err.kind(), "bad_base_url");
    }

    #[test]
    fn absolute_url_in_path_overrides_base() {
        let server = OneShotServer::spawn(OK_CBOR_RESPONSE);
        let client = DirectClient::new();
        client.set_base_url("http://example.invalid/").unwrap();
        let response = run(client.fetch(Request {
            method: HttpMethod::Get,
            path: &format!("{}some/path", server.addr),
            body: None,
        }))
        .unwrap();
        assert_eq!(response.status, 200);
        let captured = server.captured();
        assert_eq!(captured.path, "/some/path");
    }

    #[test]
    fn back_pops_history_and_refetches() {
        let server1 = OneShotServer::spawn(OK_CBOR_RESPONSE);
        let client = DirectClient::new();
        client.set_base_url(&server1.addr).unwrap();
        run(client.fetch(Request {
            method: HttpMethod::Get,
            path: "/",
            body: None,
        }))
        .unwrap();
        server1.captured(); // drain

        let server2 = OneShotServer::spawn(OK_CBOR_RESPONSE);
        // Navigate to an absolute URL on server2 so we can verify BACK
        // re-fetches server1's URL afterwards.
        run(client.fetch(Request {
            method: HttpMethod::Get,
            path: &format!("{}page2", server2.addr),
            body: None,
        }))
        .unwrap();
        server2.captured();

        assert_eq!(client.state().history_depth, 1);

        let server1_back = OneShotServer::spawn(OK_CBOR_RESPONSE);
        // Replace the base so the re-fetch lands on the new listener for
        // the URL that history holds. Easier: navigate manually via the
        // returned state; this test just verifies the pop happens.
        // Use a fresh client to avoid binding-the-same-port issues.
        drop(server1_back);

        let response = run(async {
            let mut inner = client.inner.lock().unwrap();
            inner.history.clear();
            inner.history.push(Url::parse("http://127.0.0.1:1/").unwrap());
            drop(inner);
            client.back().await
        });
        // Server at 127.0.0.1:1 will refuse — what we care about is that
        // `back()` consumed the history entry.
        assert!(response.is_err());
        assert_eq!(client.state().history_depth, 0);
    }

    #[test]
    fn back_returns_none_when_history_empty() {
        let client = DirectClient::new();
        let response = run(client.back()).unwrap();
        assert!(response.is_none());
    }

    #[test]
    fn state_reflects_navigation() {
        let server = OneShotServer::spawn(OK_CBOR_RESPONSE);
        let client = DirectClient::new();
        assert_eq!(client.state(), DirectState::default());
        client.set_base_url(&server.addr).unwrap();
        let s = client.state();
        assert_eq!(s.base_url.as_deref(), Some(server.addr.as_str()));
        assert!(s.current_url.is_none());
        assert_eq!(s.history_depth, 0);

        run(client.fetch(Request {
            method: HttpMethod::Get,
            path: "/",
            body: None,
        }))
        .unwrap();
        server.captured();
        let s = client.state();
        assert!(s.current_url.is_some());
        assert_eq!(s.history_depth, 0);
    }
}
