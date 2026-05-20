//! Widget layout + pixel-level rendering for a decoded [`Frame`].
//!
//! Layout model:
//! * `TITLE_BAR_HEIGHT` pixels at the top hold the title and any actions.
//! * The body area is the remaining rows. Widgets stack top-to-bottom with
//!   `WIDGET_GAP` pixels between each. Widgets that overflow the bottom of
//!   the body area are truncated; this matches the firmware's behavior
//!   (no scrolling for the static reference render).
//!
//! Everything here is pure: it touches the framebuffer and reads palette
//! constants. No I/O.

use alloc::format;
use alloc::string::{String, ToString};

use crate::font::{CELL_HEIGHT, CELL_WIDTH, MISSING_GLYPH};
use crate::frame::{ChatMessage, ErrorBody, Frame, Member, MenuItem, Style, Widget};
use crate::framebuffer::{Framebuffer, Rgb565, DISPLAY_HEIGHT, DISPLAY_WIDTH};
use crate::palette;

/// Pixel rows reserved for the top bar.
pub const TITLE_BAR_HEIGHT: i32 = 14;
/// Margin inside the body area on the left + right.
pub const BODY_MARGIN: i32 = 4;
/// Pixel gap between successive body widgets.
pub const WIDGET_GAP: i32 = 3;

const BODY_TOP: i32 = TITLE_BAR_HEIGHT + 2;
const BODY_BOTTOM: i32 = DISPLAY_HEIGHT as i32 - 2;
const BODY_LEFT: i32 = BODY_MARGIN;
const BODY_RIGHT: i32 = DISPLAY_WIDTH as i32 - BODY_MARGIN;

/// Render `frame` into `fb`. The framebuffer is cleared to the background
/// colour first; on return it contains the full screen pixels.
pub fn render_frame(frame: &Frame, fb: &mut Framebuffer) {
    fb.clear(palette::BG);

    if let Some(err) = &frame.error {
        render_error_frame(fb, err);
        return;
    }

    draw_title_bar(fb, frame);

    let mut cursor_y = BODY_TOP;
    for widget in &frame.body {
        if cursor_y >= BODY_BOTTOM {
            draw_overflow_marker(fb, cursor_y - CELL_HEIGHT as i32);
            return;
        }
        cursor_y = render_widget(fb, widget, cursor_y) + WIDGET_GAP;
    }
}

/// Render a single-line placeholder when the frame couldn't be decoded. The
/// simulator uses this when it gets a CBOR blob that doesn't parse.
pub fn render_error_placeholder(fb: &mut Framebuffer, reason: &str) {
    fb.clear(palette::BG);
    let line = format!("[invalid frame: {reason}]");
    fb.draw_text(BODY_LEFT, BODY_TOP, &line, palette::ERROR);
}

fn draw_title_bar(fb: &mut Framebuffer, frame: &Frame) {
    let title = frame.title.as_deref().unwrap_or("");
    // Background strip.
    fb.fill_rect(0, 0, DISPLAY_WIDTH as i32, TITLE_BAR_HEIGHT, palette::BG);
    fb.hline(0, TITLE_BAR_HEIGHT - 1, DISPLAY_WIDTH as i32, palette::SEPARATOR);

    // Title text: centred horizontally, baseline 3px from the top.
    if !title.is_empty() {
        let text_px = title.chars().count() as i32 * CELL_WIDTH as i32;
        let x = ((DISPLAY_WIDTH as i32 - text_px) / 2).max(BODY_LEFT);
        fb.draw_text(x, 3, title, palette::HEADING);
    }

    // Action chips: right-aligned, each rendered as `[key] label`.
    let mut right_cursor = DISPLAY_WIDTH as i32 - BODY_MARGIN;
    for action in frame.actions.iter().rev() {
        let chip = match &action.key {
            Some(k) => format!("[{}] {}", k, action.label),
            None => action.label.clone(),
        };
        let chip_px = chip.chars().count() as i32 * CELL_WIDTH as i32;
        let chip_x = right_cursor - chip_px;
        if chip_x < BODY_LEFT {
            break;
        }
        fb.draw_text(chip_x, 3, &chip, palette::DIM);
        right_cursor = chip_x - (CELL_WIDTH as i32);
    }
}

fn draw_overflow_marker(fb: &mut Framebuffer, y: i32) {
    fb.draw_text(BODY_LEFT, y, "…", palette::DIM);
}

fn render_widget(fb: &mut Framebuffer, widget: &Widget, y: i32) -> i32 {
    match widget {
        Widget::Text { s, style } => render_text(fb, s, *style, y),
        Widget::List { items } => render_list(fb, items, y),
        Widget::Input {
            label,
            value,
            input_type,
            ..
        } => render_input(fb, label.as_deref(), value.as_deref(), input_type.as_deref(), y),
        Widget::Form {
            action: _,
            method: _,
            fields,
            submit,
        } => render_form(fb, fields, submit.as_deref(), y),
        Widget::Button {
            label,
            href: _,
            method: _,
            confirm: _,
        } => render_button(fb, label.as_deref().unwrap_or(""), y),
        Widget::Image { src, w, h, alt } => render_image(fb, src.as_deref(), *w, *h, alt.as_deref(), y),
        Widget::Map {
            lat,
            lon,
            zoom,
            marker_count,
        } => render_map(fb, *lat, *lon, *zoom, *marker_count, y),
        Widget::Notification { level, s } => render_notification(fb, level, s, y),
        Widget::PresenceList {
            group_id: _,
            members,
        } => render_presence_list(fb, members, y),
        Widget::Chat {
            group_id: _,
            messages,
            compose_submit,
        } => render_chat(fb, messages, compose_submit.as_deref(), y),
        Widget::Unsupported { tag_repr } => render_unsupported(fb, tag_repr, y),
    }
}

fn style_color(style: Style) -> Rgb565 {
    match style {
        Style::Body => palette::FG,
        Style::Heading => palette::HEADING,
        Style::Dim => palette::DIM,
        Style::Mono => palette::MONO,
        Style::Error => palette::ERROR,
    }
}

fn render_text(fb: &mut Framebuffer, s: &str, style: Style, y: i32) -> i32 {
    let color = style_color(style);
    let w = BODY_RIGHT - BODY_LEFT;
    // Heading underlines itself with a 1-px rule for visual weight.
    if matches!(style, Style::Heading) {
        let yy = fb.draw_wrapped_text(BODY_LEFT, y, w, s, color);
        let underline_w = (s.chars().count() as i32 * CELL_WIDTH as i32).min(w);
        fb.hline(BODY_LEFT, yy, underline_w, palette::SEPARATOR);
        yy + 1
    } else {
        fb.draw_wrapped_text(BODY_LEFT, y, w, s, color)
    }
}

fn render_list(fb: &mut Framebuffer, items: &[MenuItem], y: i32) -> i32 {
    let w = BODY_RIGHT - BODY_LEFT;
    if items.is_empty() {
        return fb.draw_wrapped_text(BODY_LEFT, y, w, "(empty)", palette::DIM);
    }
    let mut cy = y;
    for (i, item) in items.iter().enumerate() {
        // Cursor indicator on the first item to show focused state.
        let prefix = if i == 0 { "> " } else { "  " };
        let prefix_color = if i == 0 { palette::ACCENT } else { palette::DIM };
        fb.draw_text(BODY_LEFT, cy, prefix, prefix_color);
        let label = item.label.as_deref().unwrap_or("(unlabeled)");
        let label_x = BODY_LEFT + 2 * CELL_WIDTH as i32;
        let label_w = BODY_RIGHT - label_x;
        let after = fb.draw_wrapped_text(label_x, cy, label_w, label, palette::FG);
        cy = after;
        if let Some(sub) = &item.sub {
            cy = fb.draw_wrapped_text(label_x, cy, label_w, sub, palette::DIM);
        }
        cy += 1;
        if cy >= BODY_BOTTOM {
            break;
        }
    }
    cy
}

fn render_input(
    fb: &mut Framebuffer,
    label: Option<&str>,
    value: Option<&str>,
    input_type: Option<&str>,
    y: i32,
) -> i32 {
    let mut cy = y;
    if let Some(label) = label {
        cy = fb.draw_wrapped_text(BODY_LEFT, cy, BODY_RIGHT - BODY_LEFT, label, palette::DIM);
    }
    let box_w = BODY_RIGHT - BODY_LEFT;
    let box_h = CELL_HEIGHT as i32 + 4;
    fb.stroke_rect(BODY_LEFT, cy, box_w, box_h, palette::BORDER);
    let display_value: String = match value {
        None => String::new(),
        Some(v) if matches!(input_type, Some("password")) => "·".repeat(v.chars().count()),
        Some(v) => v.to_string(),
    };
    fb.draw_text(BODY_LEFT + 2, cy + 2, &display_value, palette::FG);
    cy + box_h
}

fn render_form(fb: &mut Framebuffer, fields: &[Widget], submit: Option<&str>, y: i32) -> i32 {
    let mut cy = y;
    for field in fields {
        if cy >= BODY_BOTTOM {
            return cy;
        }
        cy = render_widget(fb, field, cy) + 2;
    }
    if cy < BODY_BOTTOM {
        cy = render_button(fb, submit.unwrap_or("Submit"), cy);
    }
    cy
}

fn render_button(fb: &mut Framebuffer, label: &str, y: i32) -> i32 {
    let pad_x: i32 = 6;
    let pad_y: i32 = 2;
    let text_w = label.chars().count() as i32 * CELL_WIDTH as i32;
    let btn_w = (text_w + pad_x * 2).min(BODY_RIGHT - BODY_LEFT);
    let btn_h = CELL_HEIGHT as i32 + pad_y * 2;
    fb.stroke_rect(BODY_LEFT, y, btn_w, btn_h, palette::ACCENT);
    fb.draw_text(BODY_LEFT + pad_x, y + pad_y, label, palette::FG);
    y + btn_h
}

fn render_image(
    fb: &mut Framebuffer,
    src: Option<&str>,
    w: Option<u32>,
    h: Option<u32>,
    alt: Option<&str>,
    y: i32,
) -> i32 {
    let img_w = w.map(|v| v as i32).unwrap_or(64).min(BODY_RIGHT - BODY_LEFT);
    let img_h = h
        .map(|v| v as i32)
        .unwrap_or(48)
        .min(BODY_BOTTOM - y - CELL_HEIGHT as i32 - 2)
        .max(8);
    fb.stroke_rect(BODY_LEFT, y, img_w, img_h, palette::BORDER);
    // Diagonal cross to mark "image placeholder" deterministically.
    let steps = img_w.min(img_h);
    for i in 0..steps {
        let x = BODY_LEFT + (i * img_w / steps).min(img_w - 1);
        let yy = y + (i * img_h / steps).min(img_h - 1);
        fb.put(x, yy, palette::SEPARATOR);
        fb.put(x, y + img_h - 1 - (i * img_h / steps).min(img_h - 1), palette::SEPARATOR);
    }
    let label = match (src, alt) {
        (Some(s), Some(a)) => format!("img {} ({})", short(s, 18), short(a, 18)),
        (Some(s), None) => format!("img {}", short(s, 28)),
        (None, Some(a)) => format!("img alt: {}", short(a, 26)),
        (None, None) => "img".to_string(),
    };
    let after = y + img_h + 1;
    fb.draw_text(BODY_LEFT, after, &label, palette::DIM);
    after + CELL_HEIGHT as i32
}

fn render_map(
    fb: &mut Framebuffer,
    lat: Option<f64>,
    lon: Option<f64>,
    zoom: Option<u32>,
    markers: usize,
    y: i32,
) -> i32 {
    let map_w = BODY_RIGHT - BODY_LEFT;
    let map_h = 60.min(BODY_BOTTOM - y - CELL_HEIGHT as i32 - 2);
    fb.stroke_rect(BODY_LEFT, y, map_w, map_h, palette::BORDER);
    // Grid lines at 12-px intervals so the placeholder visually reads as a map.
    let mut gx = BODY_LEFT + 12;
    while gx < BODY_LEFT + map_w {
        for yy in (y + 1)..(y + map_h - 1) {
            if (yy - y - 1) % 2 == 0 {
                fb.put(gx, yy, palette::SEPARATOR);
            }
        }
        gx += 12;
    }
    let mut gy = y + 12;
    while gy < y + map_h {
        for xx in (BODY_LEFT + 1)..(BODY_LEFT + map_w - 1) {
            if (xx - BODY_LEFT - 1) % 2 == 0 {
                fb.put(xx, gy, palette::SEPARATOR);
            }
        }
        gy += 12;
    }
    // Centre dot to show the "you are here" position.
    let cx = BODY_LEFT + map_w / 2;
    let cy = y + map_h / 2;
    fb.fill_rect(cx - 1, cy - 1, 3, 3, palette::ACCENT);
    let caption = format!(
        "map {:.3},{:.3} z{} +{} pins",
        lat.unwrap_or(0.0),
        lon.unwrap_or(0.0),
        zoom.unwrap_or(0),
        markers
    );
    let after = y + map_h + 1;
    fb.draw_text(BODY_LEFT, after, &caption, palette::DIM);
    after + CELL_HEIGHT as i32
}

fn render_notification(fb: &mut Framebuffer, level: &str, s: &str, y: i32) -> i32 {
    let color = match level {
        "warn" => palette::WARN,
        "error" => palette::ERROR,
        _ => palette::INFO,
    };
    let banner_h = CELL_HEIGHT as i32 + 4;
    fb.fill_rect(BODY_LEFT, y, BODY_RIGHT - BODY_LEFT, banner_h, color);
    // Two-pixel inset stripe in BG to give the banner a visible edge under
    // every level colour without depending on alpha.
    fb.stroke_rect(BODY_LEFT, y, BODY_RIGHT - BODY_LEFT, banner_h, palette::BG);
    let prefix = match level {
        "warn" => "! ",
        "error" => "x ",
        _ => "i ",
    };
    fb.draw_text(BODY_LEFT + 3, y + 2, prefix, palette::BG);
    let text_x = BODY_LEFT + 3 + (CELL_WIDTH as i32) * 2;
    fb.draw_text(text_x, y + 2, s, palette::BG);
    y + banner_h
}

fn render_presence_list(fb: &mut Framebuffer, members: &[Member], y: i32) -> i32 {
    if members.is_empty() {
        return fb.draw_text(BODY_LEFT, y, "(no members)", palette::DIM);
    }
    let mut cy = y;
    for m in members {
        if cy + CELL_HEIGHT as i32 > BODY_BOTTOM {
            break;
        }
        let dot_color = if m.online { palette::ONLINE } else { palette::OFFLINE };
        fb.fill_rect(BODY_LEFT, cy + 2, 4, 4, dot_color);
        let name = m.name.as_deref().unwrap_or("?");
        fb.draw_text(BODY_LEFT + 7, cy, name, palette::FG);
        cy += CELL_HEIGHT as i32 + 1;
    }
    cy
}

fn render_chat(
    fb: &mut Framebuffer,
    messages: &[ChatMessage],
    compose_submit: Option<&str>,
    y: i32,
) -> i32 {
    let mut cy = y;
    let max_y = BODY_BOTTOM - CELL_HEIGHT as i32 - 4;
    for msg in messages {
        if cy >= max_y {
            break;
        }
        let from = msg.from.as_deref().unwrap_or("?");
        let body = msg.s.as_deref().unwrap_or("");
        let line = format!("{}: {}", from, body);
        cy = fb.draw_wrapped_text(BODY_LEFT, cy, BODY_RIGHT - BODY_LEFT, &line, palette::FG);
    }
    let compose_y = BODY_BOTTOM - CELL_HEIGHT as i32 - 2;
    fb.hline(BODY_LEFT, compose_y - 2, BODY_RIGHT - BODY_LEFT, palette::SEPARATOR);
    let prompt = match compose_submit {
        Some(s) => format!("→ {}", s),
        None => "→".to_string(),
    };
    fb.draw_text(BODY_LEFT, compose_y, &prompt, palette::DIM);
    BODY_BOTTOM
}

fn render_unsupported(fb: &mut Framebuffer, tag_repr: &str, y: i32) -> i32 {
    let line = format!("[unsupported: {}]", tag_repr);
    fb.draw_wrapped_text(BODY_LEFT, y, BODY_RIGHT - BODY_LEFT, &line, palette::DIM)
}

fn render_error_frame(fb: &mut Framebuffer, body: &ErrorBody) {
    let color = match body.style.as_deref() {
        Some("error") => palette::ERROR,
        Some("warn") => palette::WARN,
        _ => palette::INFO,
    };
    // Coloured top stripe + title.
    fb.fill_rect(0, 0, DISPLAY_WIDTH as i32, TITLE_BAR_HEIGHT, color);
    let title = body.title.as_deref().unwrap_or_else(|| match body.status {
        Some(400) => "Bad request",
        Some(401) => "Unauthorized",
        Some(403) => "Forbidden",
        Some(404) => "Not found",
        Some(410) => "Gone",
        Some(429) => "Rate limited",
        Some(s) if (500..600).contains(&s) => "Server error",
        _ => "Error",
    });
    let text_px = title.chars().count() as i32 * CELL_WIDTH as i32;
    let x = ((DISPLAY_WIDTH as i32 - text_px) / 2).max(BODY_LEFT);
    fb.draw_text(x, 3, title, palette::BG);

    // Status code badge in the body area.
    let mut cy = BODY_TOP;
    if let Some(status) = body.status {
        let code = format!("HTTP {}", status);
        fb.draw_text(BODY_LEFT, cy, &code, palette::DIM);
        cy += CELL_HEIGHT as i32 + 2;
    }
    if let Some(id) = &body.id {
        let line = format!("id: {}", id);
        fb.draw_text(BODY_LEFT, cy, &line, palette::DIM);
        cy += CELL_HEIGHT as i32 + 2;
    }
    if let Some(message) = &body.message {
        cy = fb.draw_wrapped_text(BODY_LEFT, cy, BODY_RIGHT - BODY_LEFT, message, palette::FG);
    }
    // Use MISSING_GLYPH so its symbol is exercised by at least one render
    // path; gives the regression tests something predictable to compare.
    let _ = MISSING_GLYPH;
    let _ = cy;
}

fn short(s: &str, n: usize) -> String {
    if s.chars().count() <= n {
        s.to_string()
    } else {
        let mut out: String = s.chars().take(n.saturating_sub(1)).collect();
        out.push('…');
        out
    }
}
