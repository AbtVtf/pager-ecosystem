// PagerOS Simulator — Rust core entrypoint (Tauri 2).
//
// SIM-001 stood up the 480x222 window. SIM-002 wires the shared
// `pageros-renderer` crate behind a `render_vector` Tauri command so the
// frontend can pick any PROTO-003 vector by name and display the resulting
// PNG. The full Direct/Proxy network paths land later (SIM-004 / SIM-006).

use std::fs;
use std::path::{Path, PathBuf};

use base64::Engine;
use pageros_renderer::{decode_frame_cbor, png_export, render_frame, Framebuffer};
use serde::Serialize;

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
    let (fb, decode_err) = render_cbor_or_placeholder(&cbor);
    let png = png_export::encode_png(&fb);
    Ok(RenderResult {
        png_b64: base64::engine::general_purpose::STANDARD.encode(&png),
        width: pageros_renderer::DISPLAY_WIDTH,
        height: pageros_renderer::DISPLAY_HEIGHT,
        vector: name,
        decode_error: decode_err,
    })
}

/// Render raw CBOR bytes supplied by the frontend (hex-encoded). Lets you
/// paste a vector from the wire directly into the simulator without having
/// to drop the file on disk.
#[tauri::command]
fn render_cbor_hex(hex: String) -> Result<RenderResult, String> {
    let bytes = decode_hex(&hex).map_err(|e| format!("invalid hex: {e}"))?;
    let (fb, decode_err) = render_cbor_or_placeholder(&bytes);
    let png = png_export::encode_png(&fb);
    Ok(RenderResult {
        png_b64: base64::engine::general_purpose::STANDARD.encode(&png),
        width: pageros_renderer::DISPLAY_WIDTH,
        height: pageros_renderer::DISPLAY_HEIGHT,
        vector: "(pasted)".into(),
        decode_error: decode_err,
    })
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

pub fn run() {
    tauri::Builder::default()
        .invoke_handler(tauri::generate_handler![
            render_vector,
            render_cbor_hex,
            list_vectors
        ])
        .setup(|_app| Ok(()))
        .run(tauri::generate_context!())
        .expect("error while running PagerOS simulator");
}
