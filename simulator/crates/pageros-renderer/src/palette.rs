//! Canonical PagerOS palette.
//!
//! The display is 16-bit RGB565. To stay byte-deterministic, the renderer
//! works in RGB565 throughout — every constant here is the exact 565 value
//! a pixel takes on the wire. Expansion to RGB888 for PNG output uses the
//! standard `8 = (v << 3) | (v >> 2)` / `8 = (v << 2) | (v >> 4)` widening.

use crate::framebuffer::Rgb565;

#[inline]
const fn rgb(r: u8, g: u8, b: u8) -> Rgb565 {
    // Pack 8-bit channels into 5-6-5.
    let r5 = (r >> 3) as u16;
    let g6 = (g >> 2) as u16;
    let b5 = (b >> 3) as u16;
    Rgb565((r5 << 11) | (g6 << 5) | b5)
}

/// Display background. Pure black to match an IPS panel in its idle state.
pub const BG: Rgb565 = rgb(0, 0, 0);

/// Primary foreground text — near-white, kept off pure white so anti-aliased
/// glyphs in higher-DPI renderers can land on a known luminance.
pub const FG: Rgb565 = rgb(232, 232, 232);

/// Dim secondary text and `style: dim`.
pub const DIM: Rgb565 = rgb(140, 140, 140);

/// `style: heading` accent.
pub const HEADING: Rgb565 = rgb(255, 255, 255);

/// `style: mono` accent — slightly desaturated cyan, matches firmware mono.
pub const MONO: Rgb565 = rgb(180, 220, 200);

/// Top-bar separator + interactive widget borders.
pub const SEPARATOR: Rgb565 = rgb(56, 56, 64);

/// Border for input boxes, buttons, and form fields.
pub const BORDER: Rgb565 = rgb(96, 100, 116);

/// Selection / highlight tint (list cursor, focused button).
pub const ACCENT: Rgb565 = rgb(72, 144, 240);

/// `notification level: info`.
pub const INFO: Rgb565 = rgb(72, 144, 240);

/// `notification level: warn`.
pub const WARN: Rgb565 = rgb(240, 184, 56);

/// `notification level: error` and 4xx/5xx error frames.
pub const ERROR: Rgb565 = rgb(232, 88, 88);

/// Online presence dot.
pub const ONLINE: Rgb565 = rgb(96, 200, 120);

/// Offline presence dot.
pub const OFFLINE: Rgb565 = rgb(80, 80, 88);
