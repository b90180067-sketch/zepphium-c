#ifndef ZEPHIUM_NAVIGATION_BRIDGE_H
#define ZEPHIUM_NAVIGATION_BRIDGE_H

#include "../port.h"

#ifdef __cplusplus
extern "C" {
#endif

bool c_zephium_nav_classify(const char *input, size_t input_len, char *out_url, size_t out_len);
bool c_zephium_nav_is_query(const char *input, size_t input_len);
bool c_zephium_nav_is_allowed(const char *url_str, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHIUM_NAVIGATION_BRIDGE_H */
