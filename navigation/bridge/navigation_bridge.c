#include "navigation_bridge.h"

bool c_zephium_nav_classify(const char *input, size_t input_len, char *out_url, size_t out_len) {
    return zephium_nav_classify(input, input_len, out_url, out_len);
}

bool c_zephium_nav_is_query(const char *input, size_t input_len) {
    return zephium_nav_is_query(input, input_len);
}

bool c_zephium_nav_is_allowed(const char *url_str, size_t len) {
    return zephium_nav_is_allowed(url_str, len);
}
