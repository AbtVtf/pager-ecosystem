// PagerOS Simulator — Rust core entrypoint (Tauri 2).
//
// SIM-001 scope: launch an empty 480x222 window on macOS, Linux, Windows.
// Pixel-accurate Frame rendering (SIM-002) and input mapping (SIM-003)
// land on top of this shell.

pub fn run() {
    tauri::Builder::default()
        .setup(|_app| Ok(()))
        .run(tauri::generate_context!())
        .expect("error while running PagerOS simulator");
}
