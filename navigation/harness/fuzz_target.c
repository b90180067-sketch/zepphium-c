#include <stdint.h>
#include <stddef.h>
#include "../port.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char out[ZEPHIUM_MAX_URL_BYTES];
    (void)zephium_nav_classify((const char *)data, size, out, sizeof(out));
    (void)zephium_nav_is_query((const char *)data, size);
    (void)zephium_nav_search_query((const char *)data, size, out, sizeof(out));
    (void)zephium_nav_is_allowed((const char *)data, size);
    (void)zephium_nav_web_address((const char *)data, size, out, sizeof(out));
    (void)zephium_nav_external_target((const char *)data, size, out, sizeof(out));
    (void)zephium_nav_is_subframe_document((const char *)data, size);
    (void)zephium_nav_app_link_scheme((const char *)data, size);
    (void)zephium_nav_external_app_link((const char *)data, size, out, sizeof(out));
    return 0;
}
