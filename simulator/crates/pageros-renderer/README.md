# pageros-renderer

Pixel-accurate PagerOS Frame renderer — the **reference implementation** of
the v1 UI protocol from `SPEC.md` §5 and `protocol/spec.md`. This crate is
shared between the Tauri simulator (today) and the firmware reference
renderer (FW-019 / FW-020).

Every pixel is produced by code in this crate; there's no system font, no
host rasteriser, and no GPU path. That's deliberate — the firmware will
pull this crate as a `path` dependency once it has a working framebuffer
driver, and the simulator's golden PNGs become the same per-pixel oracle
that gates firmware renderer changes (`tests/golden.rs`, ≤1% pixel diff).

## Usage

### From Rust

```rust
use pageros_renderer::{decode_frame_cbor, render_frame, Framebuffer, png_export};

let cbor = std::fs::read("vectors/widget_text_minimal_string.cbor")?;
let frame = decode_frame_cbor(&cbor)?;
let mut fb = Framebuffer::new();
render_frame(&frame, &mut fb);
std::fs::write("out.png", png_export::encode_png(&fb))?;
```

The framebuffer is 480×222 RGB565 (`DISPLAY_WIDTH`, `DISPLAY_HEIGHT`).
[`Framebuffer::pixels`] returns the raw `&[Rgb565]` slice for downstream
display drivers; [`Rgb565::to_rgb888`] widens for PNG / SDL / Tauri.

### From the CLI

```bash
cargo run --release --bin render-vector -- \
    ../protocol/test-vectors/ui/vectors/widget_text_minimal_string.cbor out.png
```

### From the Tauri simulator

The simulator wires three commands on top of this crate
(`simulator/src-tauri/src/lib.rs`):

| Command | Args | Returns |
|---|---|---|
| `list_vectors` | — | `[{ name, category, kind }]` for every PROTO-003 vector. |
| `render_vector` | `{ name }` | `{ png_b64, width, height, vector, decode_error }`. |
| `render_cbor_hex` | `{ hex }` | Same shape, decoded from raw hex bytes. |

The shipped HTML cycles vectors with `←` / `→`, opens a category picker
with `p`, and reloads the current vector with `r`.

## Coverage

The decoder handles every widget in `tag-registry.md` §3 (both string and
numeric tag forms). Unknown widgets render as `[unsupported: <tag>]` per
spec §5.3 — the dedicated `forward_compat_*` vectors exercise the v1.x,
v2, on-device-runtime, and vendor reserved ranges.

The renderer's layout is intentionally narrow:

* The top `TITLE_BAR_HEIGHT` rows hold the frame title + soft-key action chips.
* Body widgets stack top-to-bottom with `WIDGET_GAP` between them. Widgets
  that overflow the bottom of the display are truncated and an ellipsis is
  drawn — this matches the firmware's static reference render, where the
  encoder/input router (FW-024) is responsible for scrolling.
* Locally-built error frames (spec §7.4.1) get a coloured top stripe + a
  `Style::Error` text colour so they're visually distinct from server
  frames.

## Font

The bundled 5×7 ASCII bitmap font (`src/font_data.rs`) is the v0 reference.
Real Unicode coverage (Noto Sans subsets, 1.5 MB on flash) lands with
FW-019. Non-ASCII codepoints render as the missing-glyph box `□` defined
in `src/font.rs::MISSING_GLYPH`.

Glyph shapes are checked into the repo as Rust source. To regenerate after
editing `build_font.py`:

```bash
python3 build_font.py
```

The output is deterministic and gets reviewed alongside any glyph change.

## Golden image tests

`tests/golden.rs` walks `protocol/test-vectors/ui/index.json` and asserts
each rendered PNG matches the committed reference in `testdata/goldens/`.
The acceptance criterion from `TASKS.md` (SIM-002) is ≤1% pixel diff per
vector; current renderer is at 0%.

To intentionally update the goldens (e.g. after a layout change), run:

```bash
RENDERER_REGEN=1 cargo test --release
```

Every golden change must be diffed by eye before commit — the goldens are
the de-facto spec for the reference renderer, and FW-020 will be measured
against them.

## No-std plan

The crate currently builds against `std` so it can use `ciborium`'s reader
and the `png` encoder. Migration to `no_std + alloc` for firmware is
tracked alongside FW-019; the only changes required are:

1. Swap `ciborium::de::from_reader` for `ciborium-io`'s `Read` adapter.
2. Move PNG export behind the existing `std` feature gate (already done —
   see `Cargo.toml`).

The rendering core (`framebuffer.rs`, `renderer.rs`, `font.rs`,
`palette.rs`) is already `no_std`-clean.
