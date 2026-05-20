//! PNG export for golden-image tests and the Tauri simulator.
//!
//! Encodes the framebuffer as a 480×222 RGB8 PNG with no metadata, so the
//! output is bit-stable across hosts. The encoder lives behind the `std`
//! feature because `png` itself depends on `std::io`.

use alloc::vec::Vec;

use crate::framebuffer::{Framebuffer, DISPLAY_HEIGHT, DISPLAY_WIDTH};

/// Encode the framebuffer as a PNG. Returns the encoded bytes.
pub fn encode_png(fb: &Framebuffer) -> Vec<u8> {
    let mut rgb = Vec::with_capacity((DISPLAY_WIDTH * DISPLAY_HEIGHT * 3) as usize);
    for pixel in fb.pixels() {
        let (r, g, b) = pixel.to_rgb888();
        rgb.push(r);
        rgb.push(g);
        rgb.push(b);
    }
    let mut out = Vec::new();
    {
        let mut encoder = png::Encoder::new(&mut out, DISPLAY_WIDTH, DISPLAY_HEIGHT);
        encoder.set_color(png::ColorType::Rgb);
        encoder.set_depth(png::BitDepth::Eight);
        encoder.set_compression(png::Compression::Best);
        let mut writer = encoder.write_header().expect("png header");
        writer.write_image_data(&rgb).expect("png data");
    }
    out
}

/// Decode a PNG file to a flat `Vec<u8>` of RGB8 bytes plus its dimensions.
/// Used by the golden-image regression tests to compare a freshly-rendered
/// framebuffer against the committed reference image.
pub fn decode_png(bytes: &[u8]) -> std::io::Result<(u32, u32, Vec<u8>)> {
    let decoder = png::Decoder::new(bytes);
    let mut reader = decoder.read_info().map_err(map_png_err)?;
    let info = reader.info().clone();
    let mut buf = vec![0u8; reader.output_buffer_size()];
    reader.next_frame(&mut buf).map_err(map_png_err)?;
    let (w, h) = (info.width, info.height);
    Ok((w, h, buf))
}

fn map_png_err(e: png::DecodingError) -> std::io::Error {
    std::io::Error::new(std::io::ErrorKind::InvalidData, e)
}
