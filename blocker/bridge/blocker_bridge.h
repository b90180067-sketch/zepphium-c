#ifndef ZEPHIUM_BLOCKER_BRIDGE_H
#define ZEPHIUM_BLOCKER_BRIDGE_H

#include "../port.h"

#ifdef __cplusplus
extern "C" {
#endif

bool c_zephium_blocker_simplify_canonical_host_boundary(const char *pattern, char *out_buf, size_t out_cap);
bool c_zephium_blocker_split_separator_end(const char *pattern,
                                           char *out_at_end, size_t cap_end,
                                           char *out_before_sep, size_t cap_sep);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHIUM_BLOCKER_BRIDGE_H */
