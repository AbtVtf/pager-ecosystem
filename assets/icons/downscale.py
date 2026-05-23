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


def to_rect(img: Image.Image, tw: int, th: int, method: str = "lanczos",
            pad_color=(0, 0, 0)) -> Image.Image:
    """Letterbox the source to the target aspect, then downscale."""
    target_ratio = tw / th
    src_ratio = img.width / img.height
    m = {"lanczos": Image.LANCZOS, "nearest": Image.NEAREST,
         "bicubic": Image.BICUBIC, "area": Image.BOX}[method]
    if abs(src_ratio - target_ratio) < 0.01:
        return img.resize((tw, th), m)
    if src_ratio > target_ratio:
        new_h = int(round(img.width / target_ratio))
        padded = Image.new("RGB", (img.width, new_h), pad_color)
        padded.paste(img.convert("RGB"), (0, (new_h - img.height) // 2))
    else:
        new_w = int(round(img.height * target_ratio))
        padded = Image.new("RGB", (new_w, img.height), pad_color)
        padded.paste(img.convert("RGB"), ((new_w - img.width) // 2, 0))
    return padded.resize((tw, th), m)


def to_size(img: Image.Image, side: int, method: str = "lanczos",
             pad_color=(0, 0, 0)) -> Image.Image:
    """Square-pad and downscale to side×side."""
    return to_rect(img, side, side, method, pad_color)


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


def to_rgb565_bytes(img: Image.Image, w: int, h: int) -> bytes:
    out = bytearray()
    rgb = img.convert("RGB")
    for y in range(h):
        for x in range(w):
            r, g, b = rgb.getpixel((x, y))
            v = rgb565(r, g, b)
            out.append(v & 0xFF)
            out.append((v >> 8) & 0xFF)
    return bytes(out)


def emit_c_array(name: str, data: bytes, w: int, h: int) -> str:
    n = w * h
    rows = [f"static const uint16_t icon_{name}[{n}] = {{"]
    for y in range(h):
        words = []
        for x in range(w):
            i = (y * w + x) * 2
            v = data[i] | (data[i + 1] << 8)
            words.append(f"0x{v:04x}")
        rows.append("    " + ", ".join(words) + ",")
    rows.append("};")
    return "\n".join(rows)


def parse_size(spec: str) -> tuple[int, int]:
    if "x" in spec:
        w, h = spec.split("x", 1)
        return int(w), int(h)
    s = int(spec)
    return s, s


def main():
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    src = Path(sys.argv[1])
    name = sys.argv[2]
    size_spec = sys.argv[3] if len(sys.argv) > 3 else "32"
    method = sys.argv[4] if len(sys.argv) > 4 else "lanczos"
    tw, th = parse_size(size_spec)
    img = Image.open(src)
    # Painterly icons fill the frame already — skip the white-trim crop;
    # it confuses dark cyberpunk backgrounds. Just letterbox + resize.
    small = to_rect(img, tw, th, method)
    # Gemini sometimes ignores "black background" and renders on white;
    # key any near-white pixel to pure black so the icon sits cleanly on
    # the tile's dark backdrop.
    small = keyed_bg_to_black(small, threshold=230)
    suffix = f"{tw}x{th}_{method}"
    small.save(OUT_16 / f"{name}_{suffix}_raw.png")
    preview = small.resize((tw * 16, th * 16), Image.NEAREST)
    preview.save(OUT_16 / f"{name}_{suffix}.png")
    raw = to_rgb565_bytes(small, tw, th)
    (OUT_RGB / f"{name}_{suffix}.bin").write_bytes(raw)
    (OUT_RGB / f"{name}_{suffix}.h.snippet").write_text(
        emit_c_array(f"{name}_{suffix}", raw, tw, th) + "\n"
    )
    print(f"{name} {tw}×{th} {method}: {len(raw)} B RGB565")


if __name__ == "__main__":
    main()
