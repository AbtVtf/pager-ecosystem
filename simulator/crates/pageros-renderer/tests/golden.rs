//! Golden-image conformance tests.
//!
//! Walks every `encode`-kind vector in `protocol/test-vectors/ui/index.json`,
//! decodes the CBOR via [`decode_frame_cbor`], renders to a 480×222
//! framebuffer, and compares the resulting RGB8 bytes against a checked-in
//! PNG in `testdata/goldens/`. The acceptance criterion from `TASKS.md`
//! (SIM-002) is `≤ 1% pixel diff` per vector; this test enforces that
//! threshold but treats 0% as the target.
//!
//! Set `RENDERER_REGEN=1` to overwrite goldens with freshly-rendered output.
//! Used when intentionally adjusting the reference renderer.

use std::collections::BTreeSet;
use std::fs;
use std::path::{Path, PathBuf};

use pageros_renderer::{decode_frame_cbor, png_export, render_frame, Framebuffer};

const MAX_DIFF_FRACTION: f64 = 0.01;

#[test]
fn golden_widget_vectors() {
    run_category("widget");
}

#[test]
fn golden_error_vectors() {
    run_category("error");
}

#[test]
fn golden_oversized_vectors() {
    run_category("oversized");
}

#[test]
fn golden_forward_compat_vectors() {
    run_category("forward_compat");
}

fn run_category(category: &str) {
    let repo_root = repo_root();
    let vectors_dir = repo_root.join("protocol/test-vectors/ui/vectors");
    let index_path = repo_root.join("protocol/test-vectors/ui/index.json");
    let goldens_dir = crate_root().join("testdata/goldens");
    fs::create_dir_all(&goldens_dir).expect("mkdir goldens");

    let index_raw = fs::read_to_string(&index_path).expect("read index.json");
    let regen = std::env::var("RENDERER_REGEN").is_ok();

    let mut failures: Vec<String> = Vec::new();
    let mut covered: usize = 0;

    for vector in iter_index_vectors(&index_raw) {
        if vector.category != category {
            continue;
        }
        if vector.kind != "encode" && vector.kind != "decode_only" {
            // Negative vectors render via the error placeholder; they don't
            // get golden images here.
            continue;
        }

        let cbor_path = vectors_dir.join(format!("{}.cbor", vector.name));
        let cbor = match fs::read(&cbor_path) {
            Ok(b) => b,
            Err(e) => {
                failures.push(format!("{}: read cbor: {e}", vector.name));
                continue;
            }
        };
        let frame = match decode_frame_cbor(&cbor) {
            Ok(f) => f,
            Err(e) => {
                if vector.kind == "decode_only" {
                    // Decode-only vectors with unknown major versions etc. are
                    // expected to surface as errors; render the placeholder
                    // instead of bailing.
                    let mut fb = Framebuffer::new();
                    pageros_renderer::renderer::render_error_placeholder(
                        &mut fb,
                        &format!("{e}"),
                    );
                    let png = png_export::encode_png(&fb);
                    if let Err(reason) = compare_or_regen(&goldens_dir, &vector.name, &png, regen) {
                        failures.push(format!("{}: {reason}", vector.name));
                    } else {
                        covered += 1;
                    }
                    continue;
                }
                failures.push(format!("{}: decode: {e}", vector.name));
                continue;
            }
        };
        let mut fb = Framebuffer::new();
        render_frame(&frame, &mut fb);
        let png = png_export::encode_png(&fb);
        match compare_or_regen(&goldens_dir, &vector.name, &png, regen) {
            Ok(()) => covered += 1,
            Err(reason) => failures.push(format!("{}: {reason}", vector.name)),
        }
    }

    assert!(
        covered > 0,
        "no vectors processed for category {category:?} — index miss?"
    );
    assert!(
        failures.is_empty(),
        "{} vectors failed in category {category:?}:\n  - {}",
        failures.len(),
        failures.join("\n  - ")
    );
}

fn compare_or_regen(
    goldens_dir: &Path,
    name: &str,
    fresh_png: &[u8],
    regen: bool,
) -> Result<(), String> {
    let golden_path = goldens_dir.join(format!("{name}.png"));
    if regen {
        fs::write(&golden_path, fresh_png).map_err(|e| format!("write golden: {e}"))?;
        return Ok(());
    }
    if !golden_path.exists() {
        return Err(format!(
            "no golden at {} — re-run with RENDERER_REGEN=1 to create it",
            golden_path.display()
        ));
    }
    let golden = fs::read(&golden_path).map_err(|e| format!("read golden: {e}"))?;
    let (gw, gh, gbytes) = png_export::decode_png(&golden).map_err(|e| format!("decode golden: {e}"))?;
    let (fw, fh, fbytes) =
        png_export::decode_png(fresh_png).map_err(|e| format!("decode fresh: {e}"))?;
    if (gw, gh) != (fw, fh) {
        return Err(format!(
            "size mismatch: golden {gw}x{gh}, fresh {fw}x{fh}"
        ));
    }
    let mut diff = 0u64;
    let mut iter = gbytes.chunks_exact(3).zip(fbytes.chunks_exact(3));
    let total = (gw as u64) * (gh as u64);
    while let Some((g, f)) = iter.next() {
        if g != f {
            diff += 1;
        }
    }
    let frac = diff as f64 / total as f64;
    if frac > MAX_DIFF_FRACTION {
        return Err(format!(
            "{} pixels differ ({:.3}%, threshold {:.0}%)",
            diff,
            frac * 100.0,
            MAX_DIFF_FRACTION * 100.0,
        ));
    }
    Ok(())
}

struct IndexVector {
    name: String,
    category: String,
    kind: String,
}

/// Tiny ad-hoc JSON walker. The `index.json` file is small (<10 KB) and has
/// a flat, well-known shape, so we extract the fields we need with a
/// minimal byte-level scan instead of pulling in `serde_json`.
fn iter_index_vectors(raw: &str) -> Vec<IndexVector> {
    let mut out = Vec::new();
    let mut i = 0;
    let bytes = raw.as_bytes();
    while i < bytes.len() {
        // Find the start of a vector object — the `"name":` field is unique
        // within each object (no nested `name`).
        if let Some(name_at) = find_next(bytes, i, b"\"name\":") {
            i = name_at + b"\"name\":".len();
            let name = take_string(bytes, &mut i);
            let category_at = match find_next(bytes, i, b"\"category\":") {
                Some(p) => p + b"\"category\":".len(),
                None => break,
            };
            i = category_at;
            let category = take_string(bytes, &mut i);
            let kind_at = match find_next(bytes, i, b"\"kind\":") {
                Some(p) => p + b"\"kind\":".len(),
                None => break,
            };
            i = kind_at;
            let kind = take_string(bytes, &mut i);
            out.push(IndexVector {
                name,
                category,
                kind,
            });
        } else {
            break;
        }
    }
    // De-duplicate (the index.json header has its own "categories" array;
    // the matcher above only triggers on `"name":` so we're already safe).
    let _seen: BTreeSet<String> = BTreeSet::new();
    out
}

fn find_next(bytes: &[u8], start: usize, needle: &[u8]) -> Option<usize> {
    bytes
        .windows(needle.len())
        .enumerate()
        .skip(start)
        .find_map(|(i, w)| if w == needle { Some(i) } else { None })
}

fn take_string(bytes: &[u8], i: &mut usize) -> String {
    while *i < bytes.len() && bytes[*i] != b'"' {
        *i += 1;
    }
    if *i >= bytes.len() {
        return String::new();
    }
    *i += 1; // open quote
    let start = *i;
    while *i < bytes.len() && bytes[*i] != b'"' {
        *i += 1;
    }
    let s = std::str::from_utf8(&bytes[start..*i]).unwrap_or("").to_string();
    if *i < bytes.len() {
        *i += 1;
    }
    s
}

fn crate_root() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
}

fn repo_root() -> PathBuf {
    // CARGO_MANIFEST_DIR = simulator/crates/pageros-renderer
    crate_root()
        .parent()
        .unwrap()
        .parent()
        .unwrap()
        .parent()
        .unwrap()
        .to_path_buf()
}
