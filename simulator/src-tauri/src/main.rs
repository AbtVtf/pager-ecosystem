// Prevents additional console window on Windows in release.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

fn main() {
    pageros_simulator_lib::run();
}
