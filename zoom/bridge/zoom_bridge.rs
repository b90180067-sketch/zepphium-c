//! Rust FFI bridge allowing Zephium Rust code to call the C zoom implementation seamlessly.

unsafe extern "C" {
    fn c_zephium_zoom_valid_percent(percent: u16) -> bool;
    fn c_zephium_zoom_percent(scale: f64) -> u16;
    fn c_zephium_zoom_scale(percent: u16) -> f64;
    fn c_zephium_zoom_step(from: u16, up: bool) -> u16;
}

pub fn valid_percent(percent: u16) -> bool {
    unsafe { c_zephium_zoom_valid_percent(percent) }
}

pub fn percent(scale: f64) -> u16 {
    unsafe { c_zephium_zoom_percent(scale) }
}

pub fn scale(percent: u16) -> f64 {
    unsafe { c_zephium_zoom_scale(percent) }
}

pub fn step(from: u16, up: bool) -> u16 {
    unsafe { c_zephium_zoom_step(from, up) }
}
