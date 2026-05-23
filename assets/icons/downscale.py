#!/usr/bin/env python3
"""Pipe a Gemini-generated icon PNG down to a 16x16 RGB565 sprite.

Usage:
    ./downscale.py <input.png> <name>

Writes:
    16/<name>.png          — 16x16 preview (upscaled 16x for human inspection)
    16/<name>_raw.png      — true 16x16 PNG
    rgb565/<name>.bin      — 512-byte raw RGB565 LE
    rgb565/<name>.h.snippet — C array snippet for the firmware header
"""
from __future__ import annotations

import sys
from pathlib import Path

from PIL import Image, ImageOps

HERE = Path(__file__).resolve().parent
OUT_16 = HERE / "16"
OUT_RGB = HERE / "rgb565"
OUT_16.mkdir(exist_ok=True)
OUT_RGB.mkdir(exist_ok=True)


def crop_to_subject(img: Image.Image) -> Image.Image:
    """Trim the uniform border (black OR white) and re-center as a square."""
    rgb = img.convert("RGB")
    L = rgb.convert("L")
    # Sample 4 corner pixels to guess the background brightness.
    w, h = L.size
    corners = [L.getpixel((0, 0)), L.getpixel((w - 1, 0)),
               L.getpixel((0, h - 1)), L.getpixel((w - 1, h - 1))]
    bg_avg = sum(corners) / 4
    bg_is_dark = bg_avg < 64
    if bg_is_dark:
        mask = Image.eval(L, lambda v: 255 if v > 32 else 0)
    else:
        mask = Image.eval(L, lambda v: 0 if v >= 240 else 255)
    bbox = mask.getbbox()
    if bbox is None:
        return rgb
    l, t, r, b = bbox
    side = max(r - l, b - t) + 8
    cx, cy = (l + r) // 2, (t + b) // 2
    half = side // 2
    Lc = max(0, cx - half); Tc = max(0, cy - half)
    Rc = min(rgb.width, cx + half); Bc = min(rgb.height, cy + half)
    return rgb.crop((Lc, Tc, Rc, Bc))


def to_size(img: Image.Image, side: int, method: str = "lanczos",
             pad_color=(0, 0, 0)) -> Image.Image:
    """Square-pad and downscale to side×side using the given resampler."""
    big_side = max(img.width, img.height)
    sq = ImageOps.pad(img, (big_side, big_side), color=pad_color)
    m = {"lanczos": Image.LANCZOS,
         "nearest": Image.NEAREST,
         "bicubic": Image.BICUBIC,
         "area":    Image.BOX}[method]
    return sq.resize((side, side), m)


def keyed_bg_to_black(img: Image.Image, threshold: int = 230) -> Image.Image:
    """Turn near-white background pixels to pure black for the dark pager UI."""
    out = img.copy()
    px = out.load()
    for y in range(out.height):
        for x in range(out.width):
            r, g, b = px[x, y][:3]
            if r > threshold and g > threshold and b > threshold:
                px[x, y] = (0, 0, 0)
    return out


def rgb565(r: int, g: int, b: int) -> int:
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def to_rgb565_bytes(img: Image.Image, side: int) -> bytes:
    out = bytearray()
    rgb = img.convert("RGB")
    for y in range(side):
        for x in range(side):
            r, g, b = rgb.getpixel((x, y))
            v = rgb565(r, g, b)
            out.append(v & 0xFF)
            out.append((v >> 8) & 0xFF)
    return bytes(out)


def emit_c_array(name: str, data: bytes, side: int) -> str:
    n = side * side
    rows = [f"static const uint16_t icon_{name}[{n}] = {{"]
    for y in range(side):
        words = []
        for x in range(side):
            i = (y * side + x) * 2
            v = data[i] | (data[i + 1] << 8)
            words.append(f"0x{v:04x}")
        rows.append("    " + ", ".join(words) + ",")
    rows.append("};")
    return "\n".join(rows)


def main():
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    src = Path(sys.argv[1])
    name = sys.argv[2]
    side = int(sys.argv[3]) if len(sys.argv) > 3 else 16
    method = sys.argv[4] if len(sys.argv) > 4 else "lanczos"
    img = Image.open(src)
    cropped = crop_to_subject(img)
    small = to_size(cropped, side, method)
    small = keyed_bg_to_black(small)
    suffix = f"{side}_{method}"
    small.save(OUT_16 / f"{name}_{suffix}_raw.png")
    # 16× upscaled preview for human inspection.
    preview = small.resize((side * 16, side * 16), Image.NEAREST)
    preview.save(OUT_16 / f"{name}_{suffix}.png")
    raw = to_rgb565_bytes(small, side)
    (OUT_RGB / f"{name}_{suffix}.bin").write_bytes(raw)
    (OUT_RGB / f"{name}_{suffix}.h.snippet").write_text(
        emit_c_array(f"{name}_{suffix}", raw, side) + "\n"
    )
    print(f"{name} {side}×{side} {method}: {len(raw)} B RGB565")


if __name__ == "__main__":
    main()
