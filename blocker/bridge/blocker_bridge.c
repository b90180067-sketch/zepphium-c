#include "blocker_bridge.h"

bool c_zephium_blocker_simplify_canonical_host_boundary(const char *pattern, char *out_buf, size_t out_cap) {
    return zephium_blocker_simplify_canonical_host_boundary(pattern, out_buf, out_cap);
}

bool c_zephium_blocker_split_separator_end(const char *pattern,
                                           char *out_at_end, size_t cap_end,
                                           char *out_before_sep, size_t cap_sep) {
    return zephium_blocker_split_separator_end(pattern, out_at_end, cap_end, out_before_sep, cap_sep);
}
