# SIM-003 — Host → Device key map

The simulator translates host-OS keyboard activity into the canonical
`InputEvent` set that firmware emits (`SPEC.md` §3, FW-006/FW-007). The
mapping table below is normative: any change must keep firmware and
simulator in sync.

The full pipeline:

1. DOM `keydown` / `keyup` → JS handler in `simulator/src/index.html`.
2. JS sends a `HostKey` payload (`key`, `code`, modifier flags, `repeat`,
   `press_ms`) to the Tauri command `dispatch_input`.
3. `dispatch_input` calls `input::map_host_key` (pure Rust, fully
   unit-tested in `simulator/src-tauri/src/input.rs`) and appends the
   resulting `InputEvent` to the recent-input log.
4. The frontend status bar surfaces the latest event for debugging.
   Apps consume the log via SIM-004's transport once it lands.

## Mapping table

| Host key                              | Device `InputEvent`            | Emit on   | Notes |
|---------------------------------------|--------------------------------|-----------|-------|
| Letter / digit / punctuation          | `Char(<resolved>)`             | `keydown` | `KeyboardEvent.key` is used — host layout already resolves shift + AltGr. |
| Shift + letter                        | `Char(<uppercase>)`            | `keydown` | Same path; shift is reflected in `key`. |
| Space                                 | `Char(' ')`                    | `keydown` | Space is a regular QWERTY key on the device. |
| Backspace                             | `Backspace`                    | `keydown` | Used by `input` widgets for edit. |
| Enter / NumpadEnter                   | `Enter`                        | `keydown` | Activates selected widget / submits form. |
| Escape (short, < 800 ms)              | `Back`                         | `keyup`   | Emitted on release so duration is known. |
| Escape (held, ≥ 800 ms)               | `BackLong`                     | `keyup`   | Per `SPEC.md` §5.6.2 — force-quit foregrounded app. |
| ArrowDown / ArrowRight                | `Encoder(+1)`                  | `keydown` | One tick clockwise (next). |
| ArrowUp / ArrowLeft                   | `Encoder(-1)`                  | `keydown` | One tick counter-clockwise (prev). |
| PageDown                              | `Encoder(+5)`                  | `keydown` | Coalesced fast spin. |
| PageUp                                | `Encoder(-5)`                  | `keydown` | Coalesced fast spin. |
| Ctrl/Alt/Meta + anything              | *(no device event)*            | —         | Reserved for host shortcuts. Device has no such modifiers. |
| F-keys, Tab, Home, Insert, …          | *(no device event)*            | —         | No device equivalent. |

## Dev shortcuts (Alt-gated)

The vector picker and debug controls from SIM-002 live behind `Alt` so they
never collide with device input:

| Combo  | Effect |
|--------|--------|
| `Alt+J` | Next PROTO-003 vector |
| `Alt+K` | Previous PROTO-003 vector |
| `Alt+P` | Toggle the vector picker overlay |
| `Alt+G` | Toggle the SIM-004 direct-mode address bar |
| `Alt+R` | Re-render the current vector (or reload the direct-mode URL) |
| `Alt+H` | Hide the on-screen hint/status overlay |

`Escape` while the picker is open closes the picker only — it is not
forwarded to the device, so it cannot accidentally fire `Back`.

## Long-press semantics

`LONG_PRESS_MS = 800`. The frontend stamps `press_ms` on `keyup`; the Rust
mapper compares against the threshold. Auto-repeated Escape `keydown`
events are dropped so a held key does not produce a stream of `Back`s.

## Relationship to firmware

FW-006 (QWERTY matrix) and FW-007 (rotary encoder + ENTER/BACK) will emit
the same `InputEvent` variants over the firmware's input bus. A widget /
form / list tested against the simulator must behave identically on
hardware — that property is the SIM-003 acceptance gate.
