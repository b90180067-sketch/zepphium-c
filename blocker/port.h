#ifndef ZEPHIUM_BLOCKER_PORT_H
#define ZEPHIUM_BLOCKER_PORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ZEPHIUM_RES_CHILD_DOCUMENT = (1 << 0),
    ZEPHIUM_RES_FETCH          = (1 << 1),
    ZEPHIUM_RES_FONT           = (1 << 2),
    ZEPHIUM_RES_IMAGE          = (1 << 3),
    ZEPHIUM_RES_MEDIA          = (1 << 4),
    ZEPHIUM_RES_OTHER          = (1 << 5),
    ZEPHIUM_RES_PING           = (1 << 6),
    ZEPHIUM_RES_SCRIPT         = (1 << 7),
    ZEPHIUM_RES_STYLESHEET     = (1 << 8),
    ZEPHIUM_RES_SVG_DOCUMENT   = (1 << 9),
    ZEPHIUM_RES_TOP_DOCUMENT   = (1 << 10),
    ZEPHIUM_RES_WEBSOCKET      = (1 << 11),
    ZEPHIUM_RES_ALL            = 0x0FFF
} zephium_resource_mask_t;

typedef enum {
    ZEPHIUM_ACTION_ALLOW = 0,
    ZEPHIUM_ACTION_BLOCK = 1,
    ZEPHIUM_ACTION_BLOCK_COOKIES = 2,
    ZEPHIUM_ACTION_CSS_DISPLAY_NONE = 3,
    ZEPHIUM_ACTION_IGNORE_PREVIOUS_RULES = 4,
    ZEPHIUM_ACTION_MAKE_HTTPS = 5
} zephium_blocker_action_t;

/**
 * Optimizes WebKit hostname regex boundaries by stripping unreachable .* tails.
 * Returns true if rewritten into out_buf, false if not eligible.
 */
bool zephium_blocker_simplify_canonical_host_boundary(const char *pattern, char *out_buf, size_t out_cap);

/**
 * Splits a trailing separator rule into two exact rules without .*
 * Returns true if split into out_at_end and out_before_sep, false if not eligible.
 */
bool zephium_blocker_split_separator_end(const char *pattern,
                                         char *out_at_end, size_t cap_end,
                                         char *out_before_sep, size_t cap_sep);

/**
 * Represents a single declarative WebKit Content Blocking rule.
 */
typedef struct {
    const char *url_filter;
    uint32_t resource_types; /* zephium_resource_mask_t */
    zephium_blocker_action_t action;
    const char *selector; /* for CSS_DISPLAY_NONE */
} zephium_blocker_rule_t;

/**
 * Emits canonical compact WebKit Content Blocking JSON array suitable for
 * webkit_user_content_filter_store_save.
 * Returns number of bytes written (excluding NUL) or 0 on error.
 */
size_t zephium_blocker_rules_to_webkit_json(const zephium_blocker_rule_t *rules,
                                            size_t num_rules,
                                            char *out_json,
                                            size_t json_cap);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHIUM_BLOCKER_PORT_H */
