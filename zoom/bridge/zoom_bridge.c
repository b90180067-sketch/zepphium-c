#include "zoom_bridge.h"

bool c_zephium_zoom_valid_percent(uint16_t percent) {
    return zephium_zoom_valid_percent(percent);
}

uint16_t c_zephium_zoom_percent(double scale) {
    return zephium_zoom_percent(scale);
}

double c_zephium_zoom_scale(uint16_t percent) {
    return zephium_zoom_scale(percent);
}

uint16_t c_zephium_zoom_step(uint16_t from, bool up) {
    return zephium_zoom_step(from, up);
}
