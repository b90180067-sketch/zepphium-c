#include <stdint.h>
#include <stddef.h>
#include "../port.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char buf1[1024];
    char buf2[1024];
    char in_buf[1024];
    if (size >= sizeof(in_buf)) return 0;
    for (size_t i = 0; i < size; ++i) in_buf[i] = (char)data[i];
    in_buf[size] = '\0';

    (void)zephium_blocker_simplify_canonical_host_boundary(in_buf, buf1, sizeof(buf1));
    (void)zephium_blocker_split_separator_end(in_buf, buf1, sizeof(buf1), buf2, sizeof(buf2));

    zephium_blocker_rule_t r = {
        .url_filter = in_buf,
        .resource_types = ZEPHIUM_RES_SCRIPT | ZEPHIUM_RES_IMAGE,
        .action = ZEPHIUM_ACTION_BLOCK,
        .selector = NULL
    };
    (void)zephium_blocker_rules_to_webkit_json(&r, 1, buf1, sizeof(buf1));

    return 0;
}
