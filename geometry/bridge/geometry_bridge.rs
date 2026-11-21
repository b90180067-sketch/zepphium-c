#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Rect {
    pub x: f64,
    pub y: f64,
    pub width: f64,
    pub height: f64,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Size {
    pub width: f64,
    pub height: f64,
}

unsafe extern "C" {
    fn c_zephium_rect_new(x: f64, y: f64, width: f64, height: f64) -> Rect;
    fn c_zephium_size_new(width: f64, height: f64) -> Size;
}

impl Rect {
    pub fn new(x: f64, y: f64, width: f64, height: f64) -> Self {
        unsafe { c_zephium_rect_new(x, y, width, height) }
    }
}

impl Size {
    pub fn new(width: f64, height: f64) -> Self {
        unsafe { c_zephium_size_new(width, height) }
    }
}
