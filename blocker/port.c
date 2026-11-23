#include "port.h"

#include "../browser_core/safe_string.h"

#include <string.h>
#include <ctype.h>
#include <stdio.h>

static const char PREFIX[] = "^[^:]+:(//)?([^/]+\\.)?";
static const char SUFFIX[] = "([^A-Za-z0-9_.%-].*)?$";

bool zephium_blocker_simplify_canonical_host_boundary(const char *pattern, char *out_buf, size_t out_cap) {
    if (!pattern || !out_buf || out_cap == 0) return false;
    size_t prefix_len = sizeof(PREFIX) - 1;
    size_t suffix_len = sizeof(SUFFIX) - 1;

    size_t plen = strlen(pattern);
    if (plen <= prefix_len + suffix_len) return false;

    if (strncmp(pattern, PREFIX, prefix_len) != 0) return false;
    if (strcmp(pattern + (plen - suffix_len), SUFFIX) != 0) return false;

    const char *host_start = pattern + prefix_len;
    size_t host_len = plen - prefix_len - suffix_len;

    size_t labels = 0;
    const char *curr = host_start;
    const char *end = host_start + host_len;

    while (curr < end) {
        const char *dot = NULL;
        for (const char *p = curr; p + 1 < end; ++p) {
            if (p[0] == '\\' && p[1] == '.') {
                dot = p;
                break;
            }
        }
        size_t label_len = dot ? (size_t)(dot - curr) : (size_t)(end - curr);
        if (label_len == 0) return false;

        for (size_t i = 0; i < label_len; ++i) {
            unsigned char b = (unsigned char)curr[i];
            if (!isalnum(b) && b != '-' && b != '_') return false;
        }
        labels++;
        curr = dot ? (dot + 2) : end;
    }

    if (labels < 2) return false;

    const char *replacement_suffix = "[^A-Za-z0-9_.%-]";
    size_t replacement_len = strlen(replacement_suffix);
    size_t needed = prefix_len + host_len + replacement_len + 1;
    if (out_cap < needed) return false;

    memcpy(out_buf, PREFIX, prefix_len);
    memcpy(out_buf + prefix_len, host_start, host_len);
    (void)zephium_strcpy_safe(out_buf + prefix_len + host_len,
                              out_cap - prefix_len - host_len,
                              replacement_suffix);

    return true;
}

bool zephium_blocker_split_separator_end(const char *pattern,
                                         char *out_at_end, size_t cap_end,
                                         char *out_before_sep, size_t cap_sep) {
    if (!pattern || !out_at_end || !out_before_sep) return false;
    size_t suffix_len = sizeof(SUFFIX) - 1;

    size_t plen = strlen(pattern);
    if (plen <= suffix_len) return false;

    if (strcmp(pattern + (plen - suffix_len), SUFFIX) != 0) return false;

    size_t stem_len = plen - suffix_len;
    if (stem_len == 0) return false;

    for (size_t i = 0; i + 1 < stem_len; ++i) {
        if (pattern[i] == '.' && pattern[i + 1] == '*') return false;
    }

    if (pattern[stem_len - 1] == '\\') return false;

    if (cap_end < stem_len + 2) return false;
    memcpy(out_at_end, pattern, stem_len);
    out_at_end[stem_len] = '$';
    out_at_end[stem_len + 1] = '\0';

    const char *sep = "[^A-Za-z0-9_.%-]";
    size_t sep_len = strlen(sep);
    if (cap_sep < stem_len + sep_len + 1) return false;
    memcpy(out_before_sep, pattern, stem_len);
    memcpy(out_before_sep + stem_len, sep, sep_len + 1);

    return true;
}

static const char *ACTION_NAMES[] = {
    "allow",
    "block",
    "block-cookies",
    "css-display-none",
    "ignore-previous-rules",
    "make-https"
};

static bool json_append_raw(char *out_json, size_t json_cap, size_t *pos, const char *text) {
    size_t len = strlen(text);
    if (*pos + len >= json_cap) return false;
    memcpy(out_json + *pos, text, len);
    *pos += len;
    out_json[*pos] = '\0';
    return true;
}

/* Appends text as the body of a JSON string. URL filters are regular expressions
 * full of backslashes, and every one of them has to be doubled for JSON;
 * embedding the regexes verbatim produced a document WebKit rejected with
 * "Failed to parse the JSON String.", i.e. no filter at all. */
static bool json_append_escaped(char *out_json, size_t json_cap, size_t *pos, const char *text) {
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        char scratch[8];
        size_t len;
        if (*p == '"' || *p == '\\') {
            scratch[0] = '\\';
            scratch[1] = (char)*p;
            len = 2;
        } else if (*p < 0x20) {
            int written = snprintf(scratch, sizeof(scratch), "\\u%04x", (unsigned int)*p);
            if (written < 0) return false;
            len = (size_t)written;
        } else {
            scratch[0] = (char)*p;
            len = 1;
        }
        if (*pos + len >= json_cap) return false;
        memcpy(out_json + *pos, scratch, len);
        *pos += len;
        out_json[*pos] = '\0';
    }
    return true;
}

size_t zephium_blocker_rules_to_webkit_json(const zephium_blocker_rule_t *rules,
                                            size_t num_rules,
                                            char *out_json,
                                            size_t json_cap) {
    if (!rules || !out_json || json_cap < 3) return 0;

    size_t pos = 0;
    out_json[pos++] = '[';
    out_json[pos] = '\0';

    for (size_t i = 0; i < num_rules; ++i) {
        if (!rules[i].url_filter) return 0;
        if ((unsigned int)rules[i].action >=
            (unsigned int)(sizeof(ACTION_NAMES) / sizeof(ACTION_NAMES[0]))) {
            return 0;
        }
        if (i > 0 && !json_append_raw(out_json, json_cap, &pos, ",")) return 0;
        if (!json_append_raw(out_json, json_cap, &pos, "{\"trigger\":{\"url-filter\":\"")) return 0;
        if (!json_append_escaped(out_json, json_cap, &pos, rules[i].url_filter)) return 0;
        if (!json_append_raw(out_json, json_cap, &pos, "\"},\"action\":{\"type\":\"")) return 0;
        if (!json_append_escaped(out_json, json_cap, &pos, ACTION_NAMES[rules[i].action])) return 0;
        if (!json_append_raw(out_json, json_cap, &pos, "\"")) return 0;

        if (rules[i].action == ZEPHIUM_ACTION_CSS_DISPLAY_NONE && rules[i].selector) {
            if (!json_append_raw(out_json, json_cap, &pos, ",\"selector\":\"")) return 0;
            if (!json_append_escaped(out_json, json_cap, &pos, rules[i].selector)) return 0;
            if (!json_append_raw(out_json, json_cap, &pos, "\"")) return 0;
        }
        if (!json_append_raw(out_json, json_cap, &pos, "}}")) return 0;
    }

    if (!json_append_raw(out_json, json_cap, &pos, "]")) return 0;
    return pos;
}
