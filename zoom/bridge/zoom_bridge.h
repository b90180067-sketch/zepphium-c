#ifndef ZEPHIUM_ZOOM_BRIDGE_H
#define ZEPHIUM_ZOOM_BRIDGE_H

#include "../port.h"

#ifdef __cplusplus
extern "C" {
#endif

/* C ABI exports for external FFI bridges (Rust, WebKitGTK C caller) */
bool c_zephium_zoom_valid_percent(uint16_t percent);
uint16_t c_zephium_zoom_percent(double scale);
double c_zephium_zoom_scale(uint16_t percent);
uint16_t c_zephium_zoom_step(uint16_t from, bool up);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHIUM_ZOOM_BRIDGE_H */
