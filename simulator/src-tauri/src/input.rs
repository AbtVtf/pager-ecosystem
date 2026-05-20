//! SIM-003 — Keyboard input mapping.
//!
//! Defines the canonical `InputEvent` the device firmware emits (per
//! `SPEC.md` §3 "Input: 44-key QWERTY, rotary encoder, ENTER/BACK") and the
//! pure mapping from host-OS keyboard activity (`HostKey`) to those events.
//! The simulator dispatches identical events to what FW-006 (QWERTY matrix)
//! and FW-007 (encoder + ENTER/BACK) will produce on hardware, so an app
//! tested in the simulator behaves the same on a real device.
//!
//! The mapping function is intentionally pure — no Tauri, no JS, no I/O —
//! so it is exercised directly by `cargo test`. The Tauri command in
//! `lib.rs` is a thin wrapper that records events for the frontend's
//! status bar.
//!
//! Key map is documented in `simulator/KEYMAP.md`.
//!
//! ## Why long-press lives here
//!
//! `SPEC.md` §5.6.2 ("Hold BACK = force-quit") requires distinguishing a
//! short BACK from a long BACK. The simulator emits `BackLong` when the
//! frontend reports `press_ms >= LONG_PRESS_MS` on key release; otherwise
//! `Back`. Firmware will use the same threshold.

use serde::{Deserialize, Serialize};

/// Press duration (ms) above which a BACK key release is `BackLong`.
pub const LONG_PRESS_MS: u32 = 800;

/// Canonical device input event.
///
/// `kind` + `value` is the wire shape used between the Tauri shell and the
/// frontend; the value is only present for events that carry data
/// (`char`, `encoder`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "kind", content = "value", rename_all = "snake_case")]
pub enum InputEvent {
    /// Printable QWERTY character (already case- / FN-resolved by host).
    Char(char),
    /// BACKSPACE — text-edit only.
    Backspace,
    /// Rotary encoder tick. `+1` = clockwise (next), `-1` = counter-CW (prev).
    /// Fast spins may coalesce magnitude (`|v| > 1`).
    Encoder(i32),
    /// Encoder click / dedicated ENTER. Activates selected item / submits form.
    Enter,
    /// BACK short press. Per `SPEC.md` §5.4.1, the firmware additionally
    /// emits the protocol `back` event to the foregrounded app; that lives
    /// in the input router, not here.
    Back,
    /// BACK long press (≥ `LONG_PRESS_MS`). Force-quit per `SPEC.md` §5.6.2.
    BackLong,
}

/// What the host OS reports for one keyboard activation. The frontend
/// translates a DOM `KeyboardEvent` into this shape and sends it across the
/// Tauri bridge.
///
/// `key` is the resolved character (e.g. `"A"` if shift was held, `"?"`
/// for shift-`/` on US layout). `code` is the physical key
/// (e.g. `"KeyA"`). We prefer `key` for printable mapping so non-US
/// layouts work without per-layout tables. `code` is only consulted for
/// non-printable keys (arrows, escape, etc.) where `key` would be a
/// locale-dependent word.
#[derive(Debug, Clone, Deserialize)]
pub struct HostKey {
    pub key: String,
    #[serde(default)]
    pub code: String,
    /// `keydown` (initial press) vs `keyup` (release). Long-press
    /// resolution happens on release.
    #[serde(default)]
    pub event_type: HostKeyEventType,
    #[serde(default)]
    pub shift: bool,
    #[serde(default)]
    pub ctrl: bool,
    #[serde(default)]
    pub alt: bool,
    #[serde(default)]
    pub meta: bool,
    /// True when the host OS is auto-repeating (key held). Suppressed for
    /// BACK so a held BACK does not flood the event log.
    #[serde(default)]
    pub repeat: bool,
    /// Press duration so far, in milliseconds. Frontend stamps this on
    /// `keyup` events for long-press resolution.
    #[serde(default)]
    pub press_ms: u32,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Deserialize, Default)]
#[serde(rename_all = "snake_case")]
pub enum HostKeyEventType {
    #[default]
    KeyDown,
    KeyUp,
}

/// Translate a host key activation into a device input event. Returns
/// `None` for keys that are not bound on the device or that should be
/// passed through to the host OS (e.g. host shortcuts using ctrl/meta).
///
/// Rules — see `KEYMAP.md` for the full table:
///
/// * Modifier-only chords (ctrl/alt/meta + anything) are passed through to
///   the host. The device has no such modifiers.
/// * `BACK` (Escape) emits on key release so the press duration is known.
///   All other events emit on key press to match firmware latency.
/// * `BACK` auto-repeat is dropped; the firmware debounces in hardware.
pub fn map_host_key(k: &HostKey) -> Option<InputEvent> {
    if k.ctrl || k.alt || k.meta {
        return None;
    }

    let code: &str = &k.code;
    let key: &str = &k.key;

    if matches!(code, "Escape") || matches!(key, "Escape") {
        if k.repeat {
            return None;
        }
        return match k.event_type {
            HostKeyEventType::KeyUp => Some(if k.press_ms >= LONG_PRESS_MS {
                InputEvent::BackLong
            } else {
                InputEvent::Back
            }),
            HostKeyEventType::KeyDown => None,
        };
    }

    if k.event_type == HostKeyEventType::KeyUp {
        return None;
    }

    match (code, key) {
        ("Enter" | "NumpadEnter", _) => Some(InputEvent::Enter),
        (_, "Enter") => Some(InputEvent::Enter),

        ("Backspace", _) | (_, "Backspace") => Some(InputEvent::Backspace),

        ("ArrowDown" | "ArrowRight", _) | (_, "ArrowDown" | "ArrowRight") => {
            Some(InputEvent::Encoder(1))
        }
        ("ArrowUp" | "ArrowLeft", _) | (_, "ArrowUp" | "ArrowLeft") => {
            Some(InputEvent::Encoder(-1))
        }
        ("PageDown", _) | (_, "PageDown") => Some(InputEvent::Encoder(5)),
        ("PageUp", _) | (_, "PageUp") => Some(InputEvent::Encoder(-5)),

        // SPACE on the device is a regular QWERTY key.
        ("Space", _) | (_, " " | "Spacebar") => Some(InputEvent::Char(' ')),

        // Printable character: `key` is a single Unicode scalar already
        // shifted / locale-resolved by the host. Tab and other control
        // names ("Tab", "Home", …) are multi-char and naturally rejected
        // by this filter.
        _ => single_printable(key).map(InputEvent::Char),
    }
}

fn single_printable(s: &str) -> Option<char> {
    let mut chars = s.chars();
    let c = chars.next()?;
    if chars.next().is_some() {
        return None;
    }
    if c.is_control() {
        return None;
    }
    Some(c)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn down(key: &str, code: &str) -> HostKey {
        HostKey {
            key: key.into(),
            code: code.into(),
            event_type: HostKeyEventType::KeyDown,
            shift: false,
            ctrl: false,
            alt: false,
            meta: false,
            repeat: false,
            press_ms: 0,
        }
    }

    fn up(key: &str, code: &str, press_ms: u32) -> HostKey {
        HostKey {
            key: key.into(),
            code: code.into(),
            event_type: HostKeyEventType::KeyUp,
            shift: false,
            ctrl: false,
            alt: false,
            meta: false,
            repeat: false,
            press_ms,
        }
    }

    #[test]
    fn letters_map_to_char() {
        assert_eq!(map_host_key(&down("a", "KeyA")), Some(InputEvent::Char('a')));
        assert_eq!(map_host_key(&down("z", "KeyZ")), Some(InputEvent::Char('z')));
    }

    #[test]
    fn shifted_letters_use_resolved_key() {
        let mut k = down("A", "KeyA");
        k.shift = true;
        assert_eq!(map_host_key(&k), Some(InputEvent::Char('A')));
    }

    #[test]
    fn digits_and_punctuation() {
        assert_eq!(map_host_key(&down("0", "Digit0")), Some(InputEvent::Char('0')));
        assert_eq!(map_host_key(&down("/", "Slash")), Some(InputEvent::Char('/')));
        assert_eq!(map_host_key(&down(",", "Comma")), Some(InputEvent::Char(',')));
        let mut q = down("?", "Slash");
        q.shift = true;
        assert_eq!(map_host_key(&q), Some(InputEvent::Char('?')));
    }

    #[test]
    fn space_is_a_char() {
        assert_eq!(map_host_key(&down(" ", "Space")), Some(InputEvent::Char(' ')));
    }

    #[test]
    fn backspace_emits_backspace() {
        assert_eq!(map_host_key(&down("Backspace", "Backspace")), Some(InputEvent::Backspace));
    }

    #[test]
    fn enter_keys() {
        assert_eq!(map_host_key(&down("Enter", "Enter")), Some(InputEvent::Enter));
        assert_eq!(map_host_key(&down("Enter", "NumpadEnter")), Some(InputEvent::Enter));
    }

    #[test]
    fn arrows_are_encoder_ticks() {
        assert_eq!(map_host_key(&down("ArrowDown", "ArrowDown")), Some(InputEvent::Encoder(1)));
        assert_eq!(map_host_key(&down("ArrowRight", "ArrowRight")), Some(InputEvent::Encoder(1)));
        assert_eq!(map_host_key(&down("ArrowUp", "ArrowUp")), Some(InputEvent::Encoder(-1)));
        assert_eq!(map_host_key(&down("ArrowLeft", "ArrowLeft")), Some(InputEvent::Encoder(-1)));
    }

    #[test]
    fn page_keys_coalesce_into_larger_ticks() {
        assert_eq!(map_host_key(&down("PageDown", "PageDown")), Some(InputEvent::Encoder(5)));
        assert_eq!(map_host_key(&down("PageUp", "PageUp")), Some(InputEvent::Encoder(-5)));
    }

    #[test]
    fn escape_on_release_is_back() {
        // keydown emits nothing — we wait for release.
        assert_eq!(map_host_key(&down("Escape", "Escape")), None);
        // short release → Back
        assert_eq!(
            map_host_key(&up("Escape", "Escape", LONG_PRESS_MS - 1)),
            Some(InputEvent::Back),
        );
    }

    #[test]
    fn escape_held_emits_back_long() {
        assert_eq!(
            map_host_key(&up("Escape", "Escape", LONG_PRESS_MS)),
            Some(InputEvent::BackLong),
        );
        assert_eq!(
            map_host_key(&up("Escape", "Escape", 5_000)),
            Some(InputEvent::BackLong),
        );
    }

    #[test]
    fn escape_autorepeat_dropped() {
        let mut k = down("Escape", "Escape");
        k.repeat = true;
        assert_eq!(map_host_key(&k), None);
    }

    #[test]
    fn modifier_combos_pass_through_to_host() {
        let mut k = down("r", "KeyR");
        k.ctrl = true;
        assert_eq!(map_host_key(&k), None, "Ctrl+R is a host reload, not a device key");

        let mut k = down("c", "KeyC");
        k.meta = true;
        assert_eq!(map_host_key(&k), None, "Cmd+C copies on the host");

        let mut k = down("a", "KeyA");
        k.alt = true;
        assert_eq!(map_host_key(&k), None);
    }

    #[test]
    fn keyup_for_non_back_keys_is_ignored() {
        // Only Escape resolves on key-up.
        assert_eq!(map_host_key(&up("a", "KeyA", 0)), None);
        assert_eq!(map_host_key(&up("Enter", "Enter", 0)), None);
        assert_eq!(map_host_key(&up("ArrowDown", "ArrowDown", 0)), None);
    }

    #[test]
    fn unknown_keys_return_none() {
        // Function keys, Tab, etc. have no device equivalent.
        assert_eq!(map_host_key(&down("Tab", "Tab")), None);
        assert_eq!(map_host_key(&down("F5", "F5")), None);
        assert_eq!(map_host_key(&down("Home", "Home")), None);
        assert_eq!(map_host_key(&down("Insert", "Insert")), None);
    }

    #[test]
    fn empty_key_returns_none() {
        assert_eq!(map_host_key(&down("", "")), None);
    }

    #[test]
    fn unicode_letter_passes_through() {
        // Non-ASCII printable letters from non-US layouts map to Char.
        // (Device has 44 keys but apps may receive a wider Unicode range
        // via paste / future extensions; the mapping is forward-compatible.)
        assert_eq!(map_host_key(&down("é", "KeyE")), Some(InputEvent::Char('é')));
    }

    #[test]
    fn event_serialization_shape() {
        let ev = InputEvent::Char('h');
        let s = serde_json::to_string(&ev).unwrap();
        assert_eq!(s, r#"{"kind":"char","value":"h"}"#);

        let ev = InputEvent::Encoder(-1);
        let s = serde_json::to_string(&ev).unwrap();
        assert_eq!(s, r#"{"kind":"encoder","value":-1}"#);

        let ev = InputEvent::Back;
        let s = serde_json::to_string(&ev).unwrap();
        assert_eq!(s, r#"{"kind":"back"}"#);

        let ev = InputEvent::BackLong;
        let s = serde_json::to_string(&ev).unwrap();
        assert_eq!(s, r#"{"kind":"back_long"}"#);
    }
}
