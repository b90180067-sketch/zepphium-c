#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "../port.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size >= sizeof(uint16_t)) {
        uint16_t percent = 0;
        memcpy(&percent, data, sizeof(uint16_t));
        (void)zephium_zoom_valid_percent(percent);
        (void)zephium_zoom_scale(percent);
        (void)zephium_zoom_step(percent, (size % 2 == 0));
    }

    if (size >= sizeof(double)) {
        double scale = 0.0;
        memcpy(&scale, data, sizeof(double));
        (void)zephium_zoom_percent(scale);
    }

    return 0;
}
