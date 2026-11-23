#ifndef ZEPHIUM_SAFE_STRING_H
#define ZEPHIUM_SAFE_STRING_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/*
 * Bounded copy into a fixed-size field.
 *
 * Returns 0 on success and -1 when the source did not fit. The destination is
 * always NUL-terminated when dst_cap > 0, including on truncation, so callers
 * can keep using the (possibly shortened) value safely.
 *
 * TODO(agent): route the truncation warning through the central configurable
 * logger once the core modules can see it; this direct stderr fallback exists
 * so browser_core has no dependency on main.c's static logger.
 */
static inline int zephium_strcpy_safe(char *dst, size_t dst_cap, const char *src) {
    if (!dst || dst_cap == 0) return -1;
    if (!src) {
        dst[0] = '\0';
        return -1;
    }

    size_t len = strlen(src);
    if (len >= dst_cap) {
        memcpy(dst, src, dst_cap - 1);
        dst[dst_cap - 1] = '\0';
        fprintf(stderr,
                "[WARN] [STRING_TRUNCATION] %zu-byte value truncated to fit a %zu-byte field\n",
                len, dst_cap);
        return -1;
    }

    memcpy(dst, src, len + 1);
    return 0;
}

#endif /* ZEPHIUM_SAFE_STRING_H */
