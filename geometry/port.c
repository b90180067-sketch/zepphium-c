#include "port.h"

/*
 * Bit-identical helper matching Rust's f64.max(0.0):
 * In Rust: if self >= 0.0 { self } else { 0.0 }
 * When self is negative or NaN, self >= 0.0 is false, returning 0.0.
 */
static inline double clamp_non_negative(double val) {
    return (val >= 0.0) ? val : 0.0;
}

zephium_rect_t zephium_rect_new(double x, double y, double width, double height) {
    zephium_rect_t r;
    r.x = x;
    r.y = y;
    r.width = clamp_non_negative(width);
    r.height = clamp_non_negative(height);
    return r;
}

zephium_size_t zephium_size_new(double width, double height) {
    zephium_size_t s;
    s.width = width;
    s.height = height;
    return s;
}
