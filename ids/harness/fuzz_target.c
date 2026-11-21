#include <stdint.h>
#include <stddef.h>
#include "../port.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    zephium_id_t id;
    if (zephium_id_parse((const char *)data, size, &id)) {
        char buf[32];
        (void)zephium_id_to_string(&id, buf, sizeof(buf));
        (void)zephium_id_timestamp_ms(&id);
    }
    return 0;
}
