//! 480×222 RGB565 framebuffer + drawing primitives.
//!
//! The framebuffer holds raw 16-bit pixels in row-major order. Every
//! drawing routine in this crate (font blits, rectangles, separators) goes
//! through the small primitive set here; downstream code never touches the
//! pixel array directly.
//!
//! Working in RGB565 is deliberate: the firmware writes 16-bit pixels to
//! the display controller, and matching its bit depth here is what makes
//! pixel-exact comparison meaningful. PNG export widens to RGB888 only at
//! the boundary.

use alloc::vec;
use alloc::vec::Vec;

use crate::font::{self, glyph, CELL_HEIGHT, CELL_WIDTH, GLYPH_HEIGHT, GLYPH_WIDTH};

/// Display width in pixels.
pub const DISPLAY_WIDTH: u32 = 480;
/// Display height in pixels.
pub const DISPLAY_HEIGHT: u32 = 222;

/// 16-bit RGB565 pixel (R5 G6 B5, big-endian in the high bits per RFC).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
#[repr(transparent)]
pub struct Rgb565(pub u16);

impl Rgb565 {
    /// Expand to (R8, G8, B8) using the canonical `<< 3 | >> 2` widening.
    #[inline]
    pub const fn to_rgb888(self) -> (u8, u8, u8) {
        let v = self.0;
        let r5 = ((v >> 11) & 0x1F) as u8;
        let g6 = ((v >> 5) & 0x3F) as u8;
        let b5 = (v & 0x1F) as u8;
        let r = (r5 << 3) | (r5 >> 2);
        let g = (g6 << 2) | (g6 >> 4);
        let b = (b5 << 3) | (b5 >> 2);
        (r, g, b)
    }
}

/// Pixel buffer the size of the display. Cheap to clone; allocates once.
#[derive(Clone)]
pub struct Framebuffer {
    pixels: Vec<Rgb565>,
}

impl Framebuffer {
    /// Allocate a framebuffer cleared to the canonical background.
    pub fn new() -> Self {
        let mut fb = Framebuffer {
            pixels: vec![Rgb565(0); (DISPLAY_WIDTH * DISPLAY_HEIGHT) as usize],
        };
        fb.clear(crate::palette::BG);
        fb
    }

    /// Clear the entire framebuffer to `color`.
    pub fn clear(&mut self, color: Rgb565) {
        for px in &mut self.pixels {
            *px = color;
        }
    }

    /// Width in pixels.
    #[inline]
    pub fn width(&self) -> u32 {
        DISPLAY_WIDTH
    }

    /// Height in pixels.
    #[inline]
    pub fn height(&self) -> u32 {
        DISPLAY_HEIGHT
    }

    /// Get the pixel at (`x`, `y`). Returns BG if out of bounds.
    #[inline]
    pub fn get(&self, x: i32, y: i32) -> Rgb565 {
        if x < 0 || y < 0 || x as u32 >= DISPLAY_WIDTH || y as u32 >= DISPLAY_HEIGHT {
            return crate::palette::BG;
        }
        self.pixels[(y as u32 * DISPLAY_WIDTH + x as u32) as usize]
    }

    /// Set the pixel at (`x`, `y`). Silently no-ops when out of bounds.
    #[inline]
    pub fn put(&mut self, x: i32, y: i32, color: Rgb565) {
        if x < 0 || y < 0 || x as u32 >= DISPLAY_WIDTH || y as u32 >= DISPLAY_HEIGHT {
            return;
        }
        self.pixels[(y as u32 * DISPLAY_WIDTH + x as u32) as usize] = color;
    }

    /// Borrow the raw pixel slice (row-major, RGB565).
    #[inline]
    pub fn pixels(&self) -> &[Rgb565] {
        &self.pixels
    }

    /// Filled rectangle in inclusive screen coords. Clipped to the display.
    pub fn fill_rect(&mut self, x: i32, y: i32, w: i32, h: i32, color: Rgb565) {
        if w <= 0 || h <= 0 {
            return;
        }
        let x0 = x.max(0);
        let y0 = y.max(0);
        let x1 = (x + w).min(DISPLAY_WIDTH as i32);
        let y1 = (y + h).min(DISPLAY_HEIGHT as i32);
        for yy in y0..y1 {
            for xx in x0..x1 {
                self.pixels[(yy as u32 * DISPLAY_WIDTH + xx as u32) as usize] = color;
            }
        }
    }

    /// 1-pixel rectangle outline (no fill).
    pub fn stroke_rect(&mut self, x: i32, y: i32, w: i32, h: i32, color: Rgb565) {
        if w <= 0 || h <= 0 {
            return;
        }
        self.fill_rect(x, y, w, 1, color);
        self.fill_rect(x, y + h - 1, w, 1, color);
        self.fill_rect(x, y, 1, h, color);
        self.fill_rect(x + w - 1, y, 1, h, color);
    }

    /// Horizontal hairline.
    pub fn hline(&mut self, x: i32, y: i32, w: i32, color: Rgb565) {
        self.fill_rect(x, y, w, 1, color);
    }

    /// Draw a single character at top-left (`x`, `y`) in `color`.
    pub fn draw_char(&mut self, x: i32, y: i32, ch: char, color: Rgb565) {
        let bits = glyph(ch as u32);
        for row in 0..GLYPH_HEIGHT as i32 {
            let r = bits[row as usize];
            for col in 0..GLYPH_WIDTH as i32 {
                let mask = 1 << ((GLYPH_WIDTH as i32 - 1 - col) as u32);
                if r & (mask as u8) != 0 {
                    self.put(x + col, y + row, color);
                }
            }
        }
    }

    /// Draw a fixed-pitch string left-to-right starting at (`x`, `y`).
    /// Returns the resulting cursor x after the string (one cell past the
    /// last glyph). Codepoints outside the bundled ASCII range render as
    /// the missing-glyph box but still advance one cell.
    pub fn draw_text(&mut self, x: i32, y: i32, s: &str, color: Rgb565) -> i32 {
        let mut cx = x;
        for ch in s.chars() {
            self.draw_char(cx, y, ch, color);
            cx += CELL_WIDTH as i32;
        }
        cx
    }

    /// Soft-wrap `s` into the rectangle (`x`, `y`, `w`, *) starting at the
    /// given baseline and stepping by `CELL_HEIGHT` per row. Splits on
    /// whitespace; any single token wider than `w` is hard-wrapped at the
    /// cell boundary. Returns the y-cursor after the last row rendered.
    pub fn draw_wrapped_text(
        &mut self,
        x: i32,
        y: i32,
        w: i32,
        s: &str,
        color: Rgb565,
    ) -> i32 {
        let cells_per_row = (w / CELL_WIDTH as i32).max(1);
        let mut cy = y;
        let mut line: alloc::string::String = alloc::string::String::new();
        let mut line_chars: i32 = 0;
        for word in split_with_spaces(s) {
            let wlen = word.chars().count() as i32;
            if line_chars == 0 && word == " " {
                continue;
            }
            if line_chars + wlen <= cells_per_row {
                line.push_str(word);
                line_chars += wlen;
            } else if wlen <= cells_per_row {
                if !line.is_empty() {
                    self.draw_text(x, cy, &line, color);
                    cy += CELL_HEIGHT as i32;
                    line.clear();
                    line_chars = 0;
                }
                if word != " " {
                    line.push_str(word);
                    line_chars = wlen;
                }
            } else {
                // Hard-break an oversize token.
                for ch in word.chars() {
                    if line_chars >= cells_per_row {
                        self.draw_text(x, cy, &line, color);
                        cy += CELL_HEIGHT as i32;
                        line.clear();
                        line_chars = 0;
                    }
                    line.push(ch);
                    line_chars += 1;
                }
            }
        }
        if !line.is_empty() {
            self.draw_text(x, cy, &line, color);
            cy += CELL_HEIGHT as i32;
        }
        cy
    }
}

impl Default for Framebuffer {
    fn default() -> Self {
        Framebuffer::new()
    }
}

/// Split `s` keeping whitespace runs as their own tokens, so wrapping can
/// drop leading whitespace on new lines but preserve in-line spacing.
fn split_with_spaces(s: &str) -> alloc::vec::Vec<&str> {
    let mut out = alloc::vec::Vec::new();
    let bytes = s.as_bytes();
    let mut i = 0;
    while i < bytes.len() {
        let start = i;
        let in_space = bytes[i] == b' ' || bytes[i] == b'\t';
        while i < bytes.len() {
            let b = bytes[i];
            let space_now = b == b' ' || b == b'\t';
            if space_now != in_space {
                break;
            }
            i += 1;
        }
        out.push(&s[start..i]);
    }
    out
}

// Re-export the cell metrics so the renderer can compute layout.
pub use font::{CELL_HEIGHT as TEXT_CELL_HEIGHT, CELL_WIDTH as TEXT_CELL_WIDTH};
