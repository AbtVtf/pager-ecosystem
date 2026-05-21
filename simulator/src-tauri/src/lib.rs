// PagerOS Simulator — Rust core entrypoint (Tauri 2).
//
// SIM-001 stood up the 480x222 window. SIM-002 wires the shared
// `pageros-renderer` crate behind a `render_vector` Tauri command so the
// frontend can pick any PROTO-003 vector by name and display the resulting
// PNG. SIM-003 maps host-OS keyboard activity to the canonical device
// `InputEvent` set (QWERTY chars, encoder, ENTER, BACK) via `dispatch_input`
// — see `input.rs` and `KEYMAP.md`. SIM-004 (this file's `direct_*` commands
// + `direct.rs`) connects to a local PagerOS app server over plain HTTP and
// renders the returned CBOR Frame. Proxy mode (SIM-006) will reuse the same
// commands behind a swappable transport.

pub mod direct;
pub mod input;
pub mod network_log;

use std::fs;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::sync::Mutex;

use base64::Engine;
use pageros_renderer::{decode_frame_cbor, png_export, render_frame, Framebuffer};
use serde::Serialize;

use crate::direct::{DirectClient, DirectState, HttpMethod, Request as DirectRequest};
use crate::input::{map_host_key, HostKey, InputEvent};
use crate::network_log::{pretty_print_cbor, Exchange, NetworkLog};

#[derive(Serialize)]
pub struct RenderResult {
    /// Base64-encoded PNG bytes ready for `data:image/png;base64,…`.
    pub png_b64: String,
    /// Width / height in display pixels (always 480 × 222).
    pub width: u32,
    pub height: u32,
    /// Vector name echoed back so the frontend can show what was rendered.
    pub vector: String,
    /// Decode error message, if the CBOR didn't parse. The PNG still
    /// contains the placeholder render and is always present.
    pub decode_error: Option<String>,
}

#[derive(Serialize)]
pub struct VectorEntry {
    pub name: String,
    pub category: String,
    pub kind: String,
}

/// Render a PROTO-003 vector by name. Returns the encoded PNG as a base64
/// string + size metadata.
#[tauri::command]
fn render_vector(name: String) -> Result<RenderResult, String> {
    let cbor_path = vectors_dir().join(format!("{name}.cbor"));
    let cbor = fs::read(&cbor_path)
        .map_err(|e| format!("read {}: {e}", cbor_path.display()))?;
    Ok(render_to_result(name, &cbor))
}

/// Render raw CBOR bytes supplied by the frontend (hex-encoded). Lets you
/// paste a vector from the wire directly into the simulator without having
/// to drop the file on disk.
#[tauri::command]
fn render_cbor_hex(hex: String) -> Result<RenderResult, String> {
    let bytes = decode_hex(&hex).map_err(|e| format!("invalid hex: {e}"))?;
    Ok(render_to_result("(pasted)".into(), &bytes))
}

/// List every PROTO-003 vector the simulator can render, sorted by category
/// then name. Used to populate the frontend dropdown.
#[tauri::command]
fn list_vectors() -> Result<Vec<VectorEntry>, String> {
    let dir = vectors_dir();
    let mut out = Vec::new();
    let read = fs::read_dir(&dir).map_err(|e| format!("read_dir {}: {e}", dir.display()))?;
    for entry in read {
        let entry = entry.map_err(|e| format!("{e}"))?;
        let path = entry.path();
        if path.extension().and_then(|s| s.to_str()) != Some("json") {
            continue;
        }
        let stem = match path.file_stem().and_then(|s| s.to_str()) {
            Some(s) => s.to_string(),
            None => continue,
        };
        let raw = match fs::read_to_string(&path) {
            Ok(s) => s,
            Err(_) => continue,
        };
        out.push(VectorEntry {
            name: stem,
            category: scan_field(&raw, "\"category\":"),
            kind: scan_field(&raw, "\"kind\":"),
        });
    }
    out.sort_by(|a, b| a.category.cmp(&b.category).then(a.name.cmp(&b.name)));
    Ok(out)
}

fn render_cbor_or_placeholder(cbor: &[u8]) -> (Framebuffer, Option<String>) {
    let mut fb = Framebuffer::new();
    match decode_frame_cbor(cbor) {
        Ok(frame) => {
            render_frame(&frame, &mut fb);
            (fb, None)
        }
        Err(e) => {
            let msg = format!("{e}");
            pageros_renderer::renderer::render_error_placeholder(&mut fb, &msg);
            (fb, Some(msg))
        }
    }
}

fn vectors_dir() -> PathBuf {
    // Resolve `simulator/src-tauri` → repo_root → protocol/test-vectors/ui/vectors.
    // CARGO_MANIFEST_DIR is the build-time location of src-tauri; at runtime
    // we still want to support a relative layout in dev (`cargo run` from
    // src-tauri/) and a deployed layout where vectors live next to the
    // executable. Try both.
    let manifest_path = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    let dev_dir = manifest_path
        .parent() // simulator/
        .and_then(Path::parent) // repo root
        .map(|root| root.join("protocol/test-vectors/ui/vectors"));

    if let Some(p) = dev_dir {
        if p.exists() {
            return p;
        }
    }
    // Deployed fallback: next to the executable.
    if let Ok(exe) = std::env::current_exe() {
        if let Some(parent) = exe.parent() {
            let candidate = parent.join("vectors");
            if candidate.exists() {
                return candidate;
            }
        }
    }
    PathBuf::from("vectors")
}

fn decode_hex(s: &str) -> Result<Vec<u8>, String> {
    let cleaned: String = s.chars().filter(|c| !c.is_whitespace()).collect();
    if cleaned.len() % 2 != 0 {
        return Err("odd length".into());
    }
    let mut out = Vec::with_capacity(cleaned.len() / 2);
    let bytes = cleaned.as_bytes();
    for chunk in bytes.chunks(2) {
        let hi = nibble(chunk[0])?;
        let lo = nibble(chunk[1])?;
        out.push((hi << 4) | lo);
    }
    Ok(out)
}

fn nibble(b: u8) -> Result<u8, String> {
    match b {
        b'0'..=b'9' => Ok(b - b'0'),
        b'a'..=b'f' => Ok(b - b'a' + 10),
        b'A'..=b'F' => Ok(b - b'A' + 10),
        _ => Err(format!("bad hex char: {:?}", b as char)),
    }
}

fn scan_field(raw: &str, needle: &str) -> String {
    let pos = match raw.find(needle) {
        Some(p) => p + needle.len(),
        None => return String::new(),
    };
    let tail = &raw[pos..];
    let start = match tail.find('"') {
        Some(p) => p + 1,
        None => return String::new(),
    };
    let rest = &tail[start..];
    let end = match rest.find('"') {
        Some(p) => p,
        None => return String::new(),
    };
    rest[..end].to_string()
}

/// Recent device input events, oldest first. Capped at `INPUT_LOG_LIMIT` so
/// long debug sessions don't grow unbounded. SIM-003's status-bar UI reads
/// the tail; SIM-004's app dispatch will drain this in arrival order.
#[derive(Default)]
struct InputState {
    log: Vec<InputEvent>,
}

const INPUT_LOG_LIMIT: usize = 256;

/// Map a host keyboard activation to a device `InputEvent` (if any) and
/// append it to the recent-events log. Returns the emitted event so the
/// frontend can show feedback in the status bar — `None` means the host
/// key has no device equivalent and was passed through.
#[tauri::command]
fn dispatch_input(
    host: HostKey,
    state: tauri::State<'_, Mutex<InputState>>,
) -> Option<InputEvent> {
    let event = map_host_key(&host)?;
    let mut s = state.lock().expect("input state poisoned");
    s.log.push(event);
    if s.log.len() > INPUT_LOG_LIMIT {
        let overflow = s.log.len() - INPUT_LOG_LIMIT;
        s.log.drain(..overflow);
    }
    Some(event)
}

/// Recent device input events, oldest first. Used by tests and by the
/// debug overlay; SIM-005's network panel will replace this with structured
/// transport logging.
#[tauri::command]
fn recent_inputs(state: tauri::State<'_, Mutex<InputState>>) -> Vec<InputEvent> {
    state.lock().expect("input state poisoned").log.clone()
}

/// Encode a (possibly invalid) CBOR Frame to a PNG + decode status.
/// Shared by `render_vector` / `render_cbor_hex` / `direct_*` so every
/// response renders through one code path.
fn render_to_result(label: String, cbor: &[u8]) -> RenderResult {
    let (fb, decode_err) = render_cbor_or_placeholder(cbor);
    let png = png_export::encode_png(&fb);
    RenderResult {
        png_b64: base64::engine::general_purpose::STANDARD.encode(&png),
        width: pageros_renderer::DISPLAY_WIDTH,
        height: pageros_renderer::DISPLAY_HEIGHT,
        vector: label,
        decode_error: decode_err,
    }
}

/// Combined result of a direct-mode HTTP exchange + render. The PNG is
/// always populated (the renderer produces a placeholder on decode error
/// — see `render_cbor_or_placeholder`), and the network metadata is
/// surfaced separately so the SIM-005 panel can pick it up unchanged.
#[derive(Serialize)]
pub struct DirectRenderResult {
    #[serde(flatten)]
    pub render: RenderResult,
    pub status: u16,
    pub url: String,
    pub content_type: Option<String>,
    pub body_bytes: usize,
    /// Snapshot of navigation state after the request — frontend uses
    /// this to keep its address bar in sync without a second round trip.
    pub state: DirectState,
}

/// Set / replace the direct-mode base URL. Clears history because a base
/// switch invalidates any in-flight navigation.
#[tauri::command]
fn direct_set_base_url(
    url: String,
    client: tauri::State<'_, Arc<DirectClient>>,
) -> Result<DirectState, String> {
    client.set_base_url(&url).map_err(|e| e.to_string())?;
    Ok(client.state())
}

#[tauri::command]
fn direct_state(client: tauri::State<'_, Arc<DirectClient>>) -> DirectState {
    client.state()
}

/// Fetch `path` (GET or POST), render the response, return everything
/// the frontend needs to draw the screen + update the address bar.
#[tauri::command]
async fn direct_fetch(
    path: String,
    method: Option<HttpMethod>,
    body: Option<Vec<u8>>,
    client: tauri::State<'_, Arc<DirectClient>>,
) -> Result<DirectRenderResult, String> {
    let client = (*client).clone();
    let method = method.unwrap_or(HttpMethod::Get);
    let response = client
        .fetch(DirectRequest {
            method,
            path: &path,
            body: body.as_deref(),
        })
        .await
        .map_err(|e| e.to_string())?;
    Ok(DirectRenderResult {
        render: render_to_result(response.resolved_url.clone(), &response.body),
        status: response.status,
        body_bytes: response.body.len(),
        url: response.resolved_url,
        content_type: response.content_type,
        state: client.state(),
    })
}

/// Network-panel detail view. Mirrors a single captured exchange and
/// adds pretty-printed CBOR for whichever side actually carried CBOR
/// (request body for POSTs, response body when present). Hex preview
/// of the raw bytes is computed in the frontend so the wire shape
/// stays compact.
#[derive(Serialize)]
pub struct NetworkExchangeDetail {
    #[serde(flatten)]
    pub exchange: Exchange,
    /// Multi-line diagnostic notation for the request body, or `None`
    /// when the request had no body.
    pub request_decoded: Option<String>,
    /// Multi-line diagnostic notation for the response body, or `None`
    /// when the exchange ended in a transport error.
    pub response_decoded: Option<String>,
}

/// Most-recent-first list of captured exchanges. The panel uses this
/// to populate its dropdown; full bodies live in `network_exchange`.
#[tauri::command]
fn network_exchanges(log: tauri::State<'_, Arc<NetworkLog>>) -> Vec<Exchange> {
    log.snapshot()
}

#[tauri::command]
fn network_exchange(
    id: u64,
    log: tauri::State<'_, Arc<NetworkLog>>,
) -> Option<NetworkExchangeDetail> {
    let ex = log.get(id)?;
    let request_decoded = if ex.request_body.bytes.is_empty() {
        None
    } else {
        Some(pretty_print_cbor(&ex.request_body.bytes))
    };
    let response_decoded = match &ex.outcome {
        network_log::Outcome::Response { response_body, .. } => {
            Some(pretty_print_cbor(&response_body.bytes))
        }
        network_log::Outcome::Error { .. } => None,
    };
    Some(NetworkExchangeDetail { exchange: ex, request_decoded, response_decoded })
}

#[tauri::command]
fn network_clear(log: tauri::State<'_, Arc<NetworkLog>>) {
    log.clear();
}

/// Navigate one entry back in the direct-mode history stack. Returns
/// `None` if history is empty so the frontend can decide whether to
/// route to the Shell (per `SPEC.md` §5.6.2: BACK from app root → Shell).
#[tauri::command]
async fn direct_back(
    client: tauri::State<'_, Arc<DirectClient>>,
) -> Result<Option<DirectRenderResult>, String> {
    let client = (*client).clone();
    let response = client.back().await.map_err(|e| e.to_string())?;
    let Some(response) = response else { return Ok(None) };
    Ok(Some(DirectRenderResult {
        render: render_to_result(response.resolved_url.clone(), &response.body),
        status: response.status,
        body_bytes: response.body.len(),
        url: response.resolved_url,
        content_type: response.content_type,
        state: client.state(),
    }))
}

pub fn run() {
    let network_log = Arc::new(NetworkLog::new());
    let direct_client = Arc::new(DirectClient::with_log(Some(network_log.clone())));
    tauri::Builder::default()
        .manage(Mutex::new(InputState::default()))
        .manage(direct_client)
        .manage(network_log)
        .invoke_handler(tauri::generate_handler![
            render_vector,
            render_cbor_hex,
            list_vectors,
            dispatch_input,
            recent_inputs,
            direct_set_base_url,
            direct_state,
            direct_fetch,
            direct_back,
            network_exchanges,
            network_exchange,
            network_clear,
        ])
        .setup(|_app| Ok(()))
        .run(tauri::generate_context!())
        .expect("error while running PagerOS simulator");
}
