//! Pixel-accurate PagerOS Frame renderer.
//!
//! This crate is the **reference renderer** for the PagerOS UI protocol
//! (`SPEC.md` §5, `protocol/spec.md`, `protocol/tag-registry.md`). It targets
//! the 480×222 16-bit display defined for the LILYGO T-LoRa Pager and renders
//! every PROTO-003 test vector to a fixed framebuffer with deterministic
//! pixel output.
//!
//! The crate is intentionally narrow: no I/O, no platform code, no fonts
//! beyond the bundled 5×7 ASCII bitmap. The Tauri simulator wires this crate
//! to a window; FW-019/FW-020 will pull the same crate in and replace the
//! ASCII font with the Noto Sans bundle on flash. The pixel placement,
//! widget layout, and palette are the canonical reference.
//!
//! ## Wire format
//!
//! Frames are decoded from canonical RFC 8949 CBOR. Both the **string** and
//! **numeric** widget/event tag forms (tag-registry §3) are accepted; the
//! decoder normalises every widget to the structured Rust representation in
//! [`frame`]. Unknown widgets render as `[unsupported: <tag>]` per spec
//! §5.3 / tag-registry §2.2.
//!
//! ## Determinism
//!
//! Every pixel is produced by code in this crate — no system fonts, no host
//! rasterizer. The same `Frame` byte sequence will always produce the same
//! framebuffer bytes regardless of platform. This is what makes the
//! golden-image comparison in `tests/golden.rs` meaningful: any rendering
//! drift between the simulator and the firmware will show up as a pixel
//! diff against the committed PNGs.

#![cfg_attr(not(feature = "std"), no_std)]

extern crate alloc;

pub mod font;
pub mod framebuffer;
pub mod frame;
pub mod palette;
pub mod renderer;

#[cfg(feature = "std")]
pub mod png_export;

pub use frame::{
    decode_frame_cbor, Action, ChatMessage, ErrorBody, Frame, Member, MenuItem, RenderError,
    Style, Widget,
};
pub use framebuffer::{Framebuffer, Rgb565, DISPLAY_HEIGHT, DISPLAY_WIDTH};
pub use renderer::render_frame;

/// Convenience: decode CBOR bytes and render to a fresh framebuffer.
///
/// On decode failure, renders a single `[invalid frame: …]` placeholder line
/// and returns the error in the `Result`'s `Err` arm; callers can choose to
/// log + display, or propagate. The framebuffer is always populated.
#[cfg(feature = "std")]
pub fn render_cbor(cbor: &[u8]) -> (Framebuffer, Result<(), RenderError>) {
    let mut fb = Framebuffer::new();
    match decode_frame_cbor(cbor) {
        Ok(frame) => {
            render_frame(&frame, &mut fb);
            (fb, Ok(()))
        }
        Err(err) => {
            renderer::render_error_placeholder(&mut fb, &err.to_string());
            (fb, Err(err))
        }
    }
}
