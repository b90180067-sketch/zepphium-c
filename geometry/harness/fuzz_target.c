#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "../port.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size >= sizeof(double) * 4) {
        double vals[4];
        memcpy(vals, data, sizeof(double) * 4);
        (void)zephium_rect_new(vals[0], vals[1], vals[2], vals[3]);
        (void)zephium_size_new(vals[0], vals[1]);
    }
    return 0;
}
