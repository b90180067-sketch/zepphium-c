#include <stdint.h>
#include <stddef.h>
#include "../port.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 4) return 0;
    zephium_store_t *s = NULL;
    if (zephium_store_open(":memory:", &s)) {
        char key[64] = {0};
        char val[64] = {0};
        size_t klen = size / 2;
        if (klen >= sizeof(key)) klen = sizeof(key) - 1;
        for (size_t i = 0; i < klen; ++i) key[i] = (char)data[i];

        size_t vlen = size - klen;
        if (vlen >= sizeof(val)) vlen = sizeof(val) - 1;
        for (size_t i = 0; i < vlen; ++i) val[i] = (char)data[klen + i];

        zephium_store_set_preference(s, key, val);
        char out[64];
        zephium_store_get_preference(s, key, out, sizeof(out));

        zephium_store_close(s);
    }
    return 0;
}
