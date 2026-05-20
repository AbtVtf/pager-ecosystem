//! `render-vector` — render any PROTO-003 vector to a PNG on disk.
//!
//! Used from the Tauri simulator as a fallback CLI when developers want to
//! reproduce a single rendered Frame outside of the GUI, and by the golden
//! test runner in `--regen` mode to refresh checked-in goldens.
//!
//! Usage:
//!
//! ```text
//! render-vector path/to/vector.cbor out.png
//! ```

use std::path::PathBuf;
use std::process::ExitCode;

use pageros_renderer::{decode_frame_cbor, png_export, render_frame, Framebuffer};

fn main() -> ExitCode {
    let mut args = std::env::args().skip(1);
    let cbor = match args.next() {
        Some(p) => PathBuf::from(p),
        None => {
            eprintln!("usage: render-vector <input.cbor> <output.png>");
            return ExitCode::from(2);
        }
    };
    let out = match args.next() {
        Some(p) => PathBuf::from(p),
        None => {
            eprintln!("usage: render-vector <input.cbor> <output.png>");
            return ExitCode::from(2);
        }
    };
    let bytes = match std::fs::read(&cbor) {
        Ok(b) => b,
        Err(e) => {
            eprintln!("read {}: {e}", cbor.display());
            return ExitCode::from(1);
        }
    };
    let frame = match decode_frame_cbor(&bytes) {
        Ok(f) => f,
        Err(e) => {
            eprintln!("decode {}: {e}", cbor.display());
            return ExitCode::from(1);
        }
    };
    let mut fb = Framebuffer::new();
    render_frame(&frame, &mut fb);
    let png = png_export::encode_png(&fb);
    if let Err(e) = std::fs::write(&out, &png) {
        eprintln!("write {}: {e}", out.display());
        return ExitCode::from(1);
    }
    ExitCode::SUCCESS
}
