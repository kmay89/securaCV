// Prevents an extra console window on Windows in release. The app never
// targets Windows for testing, but the guard is free and keeps parity.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

fn main() {
    securacv_flasher_lib::run();
}
