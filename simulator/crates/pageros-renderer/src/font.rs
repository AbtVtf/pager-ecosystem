//! Bundled 5×7 ASCII bitmap font.
//!
//! This is the reference font for the v1 renderer. It covers printable ASCII
//! (`0x20..=0x7E`); every other codepoint renders as the missing-glyph box
//! "□" defined by [`MISSING_GLYPH`]. Real Unicode coverage lands with FW-019
//! (Noto Sans subsets); until then, this gives both simulator and firmware
//! the same deterministic glyph shapes for the ASCII subset that PROTO-003
//! vectors exercise.

mod data {
    include!("font_data.rs");
}

pub use data::{FIRST_CODEPOINT, FONT_5X7, GLYPH_HEIGHT, GLYPH_WIDTH, LAST_CODEPOINT};

/// Renderer-side spacing constants. The font cell is 6×8 (one column + one
/// row of inter-glyph padding around the 5×7 ink).
pub const CELL_WIDTH: u32 = GLYPH_WIDTH + 1;
pub const CELL_HEIGHT: u32 = GLYPH_HEIGHT + 1;

/// Missing-glyph bitmap: a hollow box 5×7. Used for any codepoint outside
/// the bundled ASCII range and as the fallback when a string contains a
/// codepoint the font cannot represent.
pub const MISSING_GLYPH: [u8; 7] = [0x1F, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1F];

/// Look up the bitmap for `codepoint`. Returns the missing-glyph bitmap for
/// any codepoint outside the bundled ASCII range.
#[inline]
pub fn glyph(codepoint: u32) -> &'static [u8; 7] {
    if codepoint < FIRST_CODEPOINT as u32 || codepoint > LAST_CODEPOINT as u32 {
        return &MISSING_GLYPH;
    }
    &FONT_5X7[(codepoint - FIRST_CODEPOINT as u32) as usize]
}

/// True if `codepoint` is column-1 of a "wide" punctuation glyph that should
/// not have leading space (currently always false — uniform 6-px cell).
#[inline]
pub fn is_visible(codepoint: u32) -> bool {
    codepoint >= FIRST_CODEPOINT as u32 && codepoint <= LAST_CODEPOINT as u32
}

/// Width of a fixed-pitch string in pixels (codepoint count × cell width).
pub fn text_width(s: &str) -> u32 {
    s.chars().count() as u32 * CELL_WIDTH
}
