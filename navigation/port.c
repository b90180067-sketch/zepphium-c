#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "port.h"

#include "../browser_core/safe_string.h"

#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

static const char *RESERVED_HOSTS[] = {
    "asset.localhost",
    "ipc.localhost",
    "tauri.localhost",
    "zephium-onboarding.localhost"
};
#define NUM_RESERVED_HOSTS (sizeof(RESERVED_HOSTS) / sizeof(RESERVED_HOSTS[0]))

static const char *NOT_FOR_APPS[] = {
    "about", "afp", "asset", "blob", "chrome", "chrome-extension", "data", "disk", "disks", "edge",
    "file", "filesystem", "ftp", "hcp", "http", "https", "ie.http", "intent", "ipc", "javascript",
    "mk", "ms-appinstaller", "ms-cxh", "ms-cxh-full", "ms-help", "ms-its", "ms-msdt", "ms-officecmd",
    "ms-search", "ms-settings", "nfs", "nntp", "res", "safari-web-extension", "search", "search-ms",
    "shell", "smb", "tauri", "vbscript", "view-source", "vnd.ms.radio", "webkit-extension", "ws",
    "wss", "x-apple-systempreferences", "zephium"
};
#define NUM_NOT_FOR_APPS (sizeof(NOT_FOR_APPS) / sizeof(NOT_FOR_APPS[0]))

typedef struct {
    char scheme[64];
    char username[256];
    char password[256];
    char host[512];
    uint16_t port;
    bool has_port;
    char path[ZEPHIUM_MAX_URL_BYTES];
    char query[ZEPHIUM_MAX_URL_BYTES];
    char fragment[512];
} parsed_url_t;

static void trim_bounds(const char *s, size_t len, const char **start_out, size_t *len_out) {
    const char *p = s;
    const char *end = s + len;
    while (p < end && isspace((unsigned char)*p)) p++;
    while (end > p && isspace((unsigned char)*(end - 1))) end--;
    *start_out = p;
    *len_out = (size_t)(end - p);
}

static bool valid_scheme_str(const char *s, size_t len) {
    if (len == 0 || len > 63) return false;
    if (!isalpha((unsigned char)s[0])) return false;
    for (size_t i = 1; i < len; ++i) {
        if (!isalnum((unsigned char)s[i]) && s[i] != '+' && s[i] != '-' && s[i] != '.') {
            return false;
        }
    }
    return true;
}

static bool parse_url_components(const char *url_str, size_t len, parsed_url_t *out) {
    if (!url_str || len == 0 || len > ZEPHIUM_MAX_URL_BYTES || !out) return false;
    memset(out, 0, sizeof(*out));

    const char *sep = strstr(url_str, "://");
    const char *after_scheme = NULL;

    if (sep && (size_t)(sep - url_str) < len) {
        size_t slen = (size_t)(sep - url_str);
        if (!valid_scheme_str(url_str, slen)) return false;
        memcpy(out->scheme, url_str, slen);
        out->scheme[slen] = '\0';
        for (size_t i = 0; i < slen; ++i) out->scheme[i] = (char)tolower((unsigned char)out->scheme[i]);
        after_scheme = sep + 3;
    } else {
        const char *colon = memchr(url_str, ':', len);
        if (!colon) return false;
        size_t slen = (size_t)(colon - url_str);
        if (!valid_scheme_str(url_str, slen)) return false;
        memcpy(out->scheme, url_str, slen);
        out->scheme[slen] = '\0';
        for (size_t i = 0; i < slen; ++i) out->scheme[i] = (char)tolower((unsigned char)out->scheme[i]);
        after_scheme = colon + 1;
    }

    if (strcmp(out->scheme, "about") == 0 || strcmp(out->scheme, "data") == 0 || strcmp(out->scheme, "blob") == 0) {
        size_t rest_len = (size_t)((url_str + len) - after_scheme);
        if (rest_len >= sizeof(out->path)) return false;
        memcpy(out->path, after_scheme, rest_len);
        out->path[rest_len] = '\0';
        return true;
    }

    const char *curr = after_scheme;
    const char *end = url_str + len;

    const char *slash_or_end = curr;
    while (slash_or_end < end && *slash_or_end != '/' && *slash_or_end != '?' && *slash_or_end != '#') {
        slash_or_end++;
    }

    if (slash_or_end > curr) {
        const char *at = memchr(curr, '@', (size_t)(slash_or_end - curr));
        if (at && at > curr) {
            const char *colon_user = memchr(curr, ':', (size_t)(at - curr));
            if (colon_user) {
                size_t ulen = (size_t)(colon_user - curr);
                size_t plen = (size_t)(at - colon_user - 1);
                if (ulen < sizeof(out->username)) { memcpy(out->username, curr, ulen); out->username[ulen] = '\0'; }
                if (plen < sizeof(out->password)) { memcpy(out->password, colon_user + 1, plen); out->password[plen] = '\0'; }
            } else {
                size_t ulen = (size_t)(at - curr);
                if (ulen < sizeof(out->username)) { memcpy(out->username, curr, ulen); out->username[ulen] = '\0'; }
            }
            curr = at + 1;
        }
    }

    const char *host_start = curr;
    const char *host_end = slash_or_end;

    if (host_start < host_end && *host_start == '[') {
        const char *rb = memchr(host_start, ']', (size_t)(host_end - host_start));
        if (!rb) return false;
        size_t hlen = (size_t)(rb - host_start + 1);
        if (hlen >= sizeof(out->host)) return false;
        memcpy(out->host, host_start, hlen);
        out->host[hlen] = '\0';
        const char *after_rb = rb + 1;
        if (after_rb < host_end && *after_rb == ':') {
            long pval = strtol(after_rb + 1, NULL, 10);
            if (pval <= 0 || pval > 65535) return false;
            out->port = (uint16_t)pval;
            out->has_port = true;
        }
    } else {
        const char *colon_port = memchr(host_start, ':', (size_t)(host_end - host_start));
        if (colon_port) {
            size_t hlen = (size_t)(colon_port - host_start);
            if (hlen >= sizeof(out->host)) return false;
            memcpy(out->host, host_start, hlen);
            out->host[hlen] = '\0';
            long pval = strtol(colon_port + 1, NULL, 10);
            if (pval <= 0 || pval > 65535) return false;
            out->port = (uint16_t)pval;
            out->has_port = true;
        } else {
            size_t hlen = (size_t)(host_end - host_start);
            if (hlen >= sizeof(out->host)) return false;
            memcpy(out->host, host_start, hlen);
            out->host[hlen] = '\0';
        }
    }

    for (size_t i = 0; out->host[i]; ++i) out->host[i] = (char)tolower((unsigned char)out->host[i]);

    curr = slash_or_end;
    if (curr < end && *curr == '/') {
        const char *q_or_f = curr;
        while (q_or_f < end && *q_or_f != '?' && *q_or_f != '#') q_or_f++;
        size_t path_len = (size_t)(q_or_f - curr);
        if (path_len >= sizeof(out->path)) return false;
        memcpy(out->path, curr, path_len);
        out->path[path_len] = '\0';
        curr = q_or_f;
    } else {
        (void)zephium_strcpy_safe(out->path, sizeof(out->path), "/");
    }

    if (curr < end && *curr == '?') {
        curr++;
        const char *f = memchr(curr, '#', (size_t)(end - curr));
        size_t qlen = f ? (size_t)(f - curr) : (size_t)(end - curr);
        if (qlen >= sizeof(out->query)) return false;
        memcpy(out->query, curr, qlen);
        out->query[qlen] = '\0';
        curr = f ? f : end;
    }

    if (curr < end && *curr == '#') {
        curr++;
        size_t flen = (size_t)(end - curr);
        if (flen >= sizeof(out->fragment)) return false;
        memcpy(out->fragment, curr, flen);
        out->fragment[flen] = '\0';
    }

    return true;
}

static bool looks_like_local_path(const char *s, size_t len) {
    if (len == 0) return false;
    if (s[0] == '/' || s[0] == '\\') return true;
    if (len >= 2 && s[0] == '~' && s[1] == '/') return true;
    if (len >= 2 && s[0] == '.' && s[1] == '/') return true;
    if (len >= 3 && s[0] == '.' && s[1] == '.' && s[2] == '/') return true;
    if (len >= 3 && isalpha((unsigned char)s[0]) && s[1] == ':' && (s[2] == '/' || s[2] == '\\')) return true;
    return false;
}

static bool parse_ipv4(const char *s, size_t len) {
    char buf[64];
    if (len >= sizeof(buf)) return false;
    memcpy(buf, s, len);
    buf[len] = '\0';
    int a, b, c, d;
    char extra;
    if (sscanf(buf, "%d.%d.%d.%d%c", &a, &b, &c, &d, &extra) == 4) {
        return (a >= 0 && a <= 255 && b >= 0 && b <= 255 && c >= 0 && c <= 255 && d >= 0 && d <= 255);
    }
    return false;
}

static bool looks_like_host(const char *s, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        if (isspace((unsigned char)s[i])) return false;
    }
    if (len == 9 && strncmp(s, "localhost", 9) == 0) return true;
    if (len > 10 && strncmp(s, "localhost:", 10) == 0) return true;
    if (len > 10 && strncmp(s, "localhost/", 10) == 0) return true;

    size_t auth_len = 0;
    while (auth_len < len && s[auth_len] != '/' && s[auth_len] != '?' && s[auth_len] != '#') {
        if (s[auth_len] == '@') return false;
        auth_len++;
    }

    if (auth_len > 2 && s[0] == '[') {
        const char *rb = memchr(s, ']', auth_len);
        if (rb && (rb - s) > 1) {
            return true;
        }
    }

    size_t host_len = 0;
    while (host_len < auth_len && s[host_len] != ':') host_len++;

    if (parse_ipv4(s, host_len)) return true;

    const char *last_dot = NULL;
    for (size_t i = host_len; i > 0; --i) {
        if (s[i - 1] == '.') { last_dot = &s[i - 1]; break; }
    }
    if (last_dot) {
        size_t label_len = (size_t)(last_dot - s);
        size_t tld_len = (size_t)(&s[host_len] - last_dot - 1);
        if (label_len > 0 && tld_len >= 2) return true;
    }

    return false;
}

static bool has_leading_scheme_separator(const char *s, size_t len) {
    const char *sep = strstr(s, "://");
    if (!sep || (size_t)(sep - s) >= len) return false;
    return valid_scheme_str(s, (size_t)(sep - s));
}

/* ---------------------------------------------------------------------------
 * Loopback / private-network guard
 *
 * zephium_nav_is_allowed() is the last gate before a URL may be committed in a
 * tab, so it must never hand the network stack an address that reaches the
 * user's own machine or LAN: 127.0.0.0/8, ::1, 10.0.0.0/8, 172.16.0.0/12,
 * 192.168.0.0/16, 169.254.0.0/16 (the cloud metadata endpoint 169.254.169.254
 * lives there), fc00::/7, fe80::/10, 0.0.0.0/8 and the "localhost" name are all
 * refused. Internal pages keep working through about:blank and RESERVED_HOSTS,
 * neither of which this guard changes.
 *
 * Browsers accept non-canonical IPv4 spellings, so every number is parsed the
 * way inet_aton() does: one to four parts, the last part filling the remaining
 * bytes, each part decimal, 0-prefixed octal or 0x-prefixed hex. "127.1",
 * "0x7f000001" and "0177.0.0.1" therefore all reach 127.0.0.1 and are refused.
 * ------------------------------------------------------------------------- */

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* inet_aton()-style IPv4 literal: decimal, 0-prefixed octal and 0x-prefixed hex
 * numbers, plus the one-to-three part shorthands ("127.1" == 127.0.0.1). */
static bool parse_ipv4_target(const char *s, size_t len, unsigned char out[4]) {
    if (len == 0 || len > 63) return false;

    unsigned long parts[4] = {0, 0, 0, 0};
    size_t part_count = 0;
    size_t i = 0;

    while (i < len) {
        if (part_count == 4) return false;

        unsigned int base = 10;
        if (i + 2 < len && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
            base = 16;
            i += 2;
        } else if (i + 2 < len && s[i] == '0' && s[i + 1] != '.') {
            base = 8;
            i += 1;
        }

        unsigned long value = 0;
        size_t digits = 0;
        while (i < len && s[i] != '.') {
            int digit;
            if (base == 16) {
                digit = hex_value(s[i]);
            } else if (s[i] >= '0' && s[i] <= '9') {
                digit = s[i] - '0';
            } else {
                digit = -1;
            }
            if (digit < 0 || (unsigned int)digit >= base) return false;
            if (value > (0xFFFFFFFFul - (unsigned long)digit) / base) return false;
            value = value * base + (unsigned long)digit;
            ++digits;
            ++i;
        }
        if (digits == 0) return false;

        parts[part_count] = value;
        ++part_count;

        if (i < len) { /* s[i] == '.' */
            ++i;
            if (i == len) return false;
        }
    }

    /* Every field except the last one is a single byte. */
    for (size_t p = 0; p + 1 < part_count; ++p) {
        if (parts[p] > 255ul) return false;
    }

    unsigned long ip;
    switch (part_count) {
        case 1:
            ip = parts[0];
            break;
        case 2:
            if (parts[1] > 0xFFFFFFul) return false;
            ip = (parts[0] << 24) | parts[1];
            break;
        case 3:
            if (parts[2] > 0xFFFFul) return false;
            ip = (parts[0] << 24) | (parts[1] << 16) | parts[2];
            break;
        default:
            if (parts[3] > 255ul) return false;
            ip = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
            break;
    }

    out[0] = (unsigned char)((ip >> 24) & 0xFFul);
    out[1] = (unsigned char)((ip >> 16) & 0xFFul);
    out[2] = (unsigned char)((ip >> 8) & 0xFFul);
    out[3] = (unsigned char)(ip & 0xFFul);
    return true;
}

static bool ipv4_is_private(const unsigned char ip[4]) {
    if (ip[0] == 0) return true;                           /* 0.0.0.0/8 "this host" */
    if (ip[0] == 127) return true;                         /* 127.0.0.0/8 loopback */
    if (ip[0] == 10) return true;                          /* 10.0.0.0/8 */
    if (ip[0] == 172 && (ip[1] & 0xF0) == 16) return true; /* 172.16.0.0/12 */
    if (ip[0] == 192 && ip[1] == 168) return true;         /* 192.168.0.0/16 */
    if (ip[0] == 169 && ip[1] == 254) return true;         /* 169.254.0.0/16 link-local */
    return false;
}

/* RFC 4291 text form: hex groups with at most one "::", and an optional trailing
 * dotted-quad for the last 32 bits ("::ffff:127.0.0.1"). */
static bool parse_ipv6_target(const char *s, size_t len, unsigned char out[16]) {
    if (len < 2 || len > 64) return false;

    unsigned char buf[16] = {0};
    int filled = 0;
    int zeros_at = -1;
    size_t pos = 0;

    if (s[0] == ':') {
        if (s[1] != ':') return false;
        zeros_at = 0;
        pos = 2;
        if (pos == len) {
            memset(out, 0, 16);
            return true;
        }
    }

    for (;;) {
        const char *token = s + pos;
        size_t tlen = 0;
        while (pos + tlen < len && s[pos + tlen] != ':') ++tlen;
        if (tlen == 0) return false; /* ":::", leading single ':' or dangling ':' */

        if (memchr(token, '.', tlen) != NULL) {
            unsigned char quad[4];
            if (!parse_ipv4_target(token, tlen, quad)) return false;
            if (filled + 4 > 16) return false;
            memcpy(buf + filled, quad, 4);
            filled += 4;
        } else {
            if (tlen > 4) return false;
            unsigned int value = 0;
            for (size_t k = 0; k < tlen; ++k) {
                int digit = hex_value(token[k]);
                if (digit < 0) return false;
                value = (value << 4) | (unsigned int)digit;
            }
            if (filled + 2 > 16) return false;
            buf[filled] = (unsigned char)((value >> 8) & 0xFFu);
            buf[filled + 1] = (unsigned char)(value & 0xFFu);
            filled += 2;
        }

        pos += tlen;
        if (pos == len) break;

        if (pos + 1 < len && s[pos + 1] == ':') {
            if (zeros_at >= 0) return false; /* two "::" */
            zeros_at = filled;
            pos += 2;
            if (pos == len) break;
        } else {
            ++pos;
            if (pos == len) return false; /* trailing single ':' */
        }
    }

    if (zeros_at < 0) {
        if (filled != 16) return false;
        memcpy(out, buf, 16);
        return true;
    }

    size_t tail = (size_t)(16 - filled);
    memcpy(out, buf, (size_t)zeros_at);
    memset(out + zeros_at, 0, tail);
    memcpy(out + zeros_at + tail, buf + zeros_at, (size_t)(filled - zeros_at));
    return true;
}

static bool ipv6_prefix_is_zero(const unsigned char ip[16], size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (ip[i] != 0) return false;
    }
    return true;
}

static bool ipv6_is_private(const unsigned char ip[16]) {
    /* :: (unspecified) and ::1 (loopback). */
    if (ipv6_prefix_is_zero(ip, 16)) return true;
    if (ipv6_prefix_is_zero(ip, 15) && ip[15] == 1) return true;

    /* fc00::/7 unique-local, fe80::/10 link-local. */
    if ((ip[0] & 0xFE) == 0xFC) return true;
    if (ip[0] == 0xFE && (ip[1] & 0xC0) == 0x80) return true;

    /* IPv4-mapped (::ffff:a.b.c.d) and IPv4-compatible (::a.b.c.d) literals reach
     * the same host as the embedded IPv4 address, so reuse the IPv4 rules. */
    if (ipv6_prefix_is_zero(ip, 10)) {
        if (ip[10] == 0xFF && ip[11] == 0xFF) return ipv4_is_private(ip + 12);
        if (ip[10] == 0 && ip[11] == 0) return ipv4_is_private(ip + 12);
    }
    return false;
}

/* True when a host names the local machine or the local network. The caller
 * passes the lower-cased host without userinfo or port; a trailing dot and a
 * bracketed IPv6 literal with an optional zone id are handled here. */
static bool host_is_local_target(const char *host) {
    size_t len = strlen(host);
    if (len == 0) return false;

    if (host[0] == '[') {
        if (len < 3 || host[len - 1] != ']') return false;
        const char *inner = host + 1;
        size_t inner_len = len - 2;
        const char *zone = memchr(inner, '%', inner_len);
        if (zone) inner_len = (size_t)(zone - inner);
        unsigned char ip[16];
        if (!parse_ipv6_target(inner, inner_len, ip)) return false;
        return ipv6_is_private(ip);
    }

    while (len > 1 && host[len - 1] == '.') --len; /* "localhost." is "localhost" */

    if (len == 9 && strncmp(host, "localhost", 9) == 0) return true;
    if (len > 10 && strncmp(host + len - 10, ".localhost", 10) == 0) return true;

    unsigned char ip[4];
    if (parse_ipv4_target(host, len, ip)) return ipv4_is_private(ip);
    return false;
}

bool zephium_nav_is_allowed(const char *url_str, size_t len) {
    if (!url_str || len == 0 || len > ZEPHIUM_MAX_URL_BYTES) return false;
    if (len == 11 && strncmp(url_str, "about:blank", 11) == 0) return true;

    parsed_url_t u;
    if (!parse_url_components(url_str, len, &u)) return false;

    if (strcmp(u.scheme, "http") != 0 && strcmp(u.scheme, "https") != 0) return false;
    if (u.username[0] != '\0' || u.password[0] != '\0') return false;
    if (u.host[0] == '\0') return false;

    for (size_t i = 0; i < NUM_RESERVED_HOSTS; ++i) {
        if (strcmp(u.host, RESERVED_HOSTS[i]) == 0) return false;
    }
    if (host_is_local_target(u.host)) return false;

    return true;
}

/* Query strings and fragments are where URLs carry credentials, session tokens and
 * search terms. Anything persisted (logs, history, diagnostics) goes through this
 * helper, which keeps only scheme, host, port and path. */
bool zephium_nav_redact_url(const char *url, size_t url_len, char *out_url, size_t out_len) {
    if (!url || !out_url || out_len == 0 || url_len > ZEPHIUM_MAX_URL_BYTES) return false;
    if (url_len >= out_len) return false;

    size_t keep = 0;
    while (keep < url_len && url[keep] != '?' && url[keep] != '#') ++keep;

    memcpy(out_url, url, keep);
    out_url[keep] = '\0';
    return true;
}

static bool url_escape_append(char *dest, size_t dest_cap, size_t *pos, const char *src, size_t src_len) {
    for (size_t i = 0; i < src_len; ++i) {
        unsigned char c = (unsigned char)src[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            if (*pos + 1 >= dest_cap) return false;
            dest[(*pos)++] = (char)c;
        } else if (c == ' ') {
            if (*pos + 1 >= dest_cap) return false;
            dest[(*pos)++] = '+';
        } else {
            if (*pos + 3 >= dest_cap) return false;
            snprintf(&dest[*pos], 4, "%%%02X", c);
            *pos += 3;
        }
    }
    dest[*pos] = '\0';
    return true;
}

static bool build_search_url(const char *val, size_t val_len, char *out, size_t out_len) {
    size_t base_len = strlen(ZEPHIUM_SEARCH_BASE) + 3;
    if (out_len < base_len + 1) return false;
    if (zephium_strcpy_safe(out, out_len, ZEPHIUM_SEARCH_BASE "?q=") != 0) return false;
    size_t pos = strlen(out);

    if (!url_escape_append(out, out_len, &pos, val, val_len)) return false;
    return zephium_nav_is_allowed(out, pos);
}

static int ascii_strcasecmp(const char *s1, const char *s2) {
    while (*s1 && *s2) {
        int c1 = tolower((unsigned char)*s1);
        int c2 = tolower((unsigned char)*s2);
        if (c1 != c2) return c1 - c2;
        s1++;
        s2++;
    }
    return (int)(unsigned char)*s1 - (int)(unsigned char)*s2;
}

static bool embedded_web_targets_allowed(const char *val, size_t len) {
    size_t offset = 0;
    while (offset < len) {
        const char *sep = strstr(val + offset, "://");
        if (!sep) break;
        size_t sep_idx = (size_t)(sep - val);
        size_t start = sep_idx;
        while (start > 0) {
            char b = val[start - 1];
            if (isalnum((unsigned char)b) || b == '+' || b == '-' || b == '.') {
                start--;
            } else {
                break;
            }
        }
        if (start == sep_idx || !isalpha((unsigned char)val[start])) {
            offset = sep_idx + 3;
            continue;
        }
        size_t slen = sep_idx - start;
        char scheme[16];
        if (slen >= sizeof(scheme)) return false;
        memcpy(scheme, val + start, slen);
        scheme[slen] = '\0';

        size_t end = sep_idx + 3;
        while (end < len && !isspace((unsigned char)val[end])) end++;
        while (end > sep_idx + 3) {
            char ch = val[end - 1];
            if (ch == ',' || ch == ';' || ch == '.' || ch == '!' || ch == ')' || ch == ']' || ch == '}' || ch == '"' || ch == '\'') {
                end--;
            } else {
                break;
            }
        }

        size_t cand_len = end - start;
        char candidate[ZEPHIUM_MAX_URL_BYTES];
        if (cand_len >= sizeof(candidate)) return false;
        memcpy(candidate, val + start, cand_len);
        candidate[cand_len] = '\0';

        if (ascii_strcasecmp(scheme, "http") != 0 && ascii_strcasecmp(scheme, "https") != 0) {
            return false;
        }
        if (!zephium_nav_is_allowed(candidate, cand_len)) {
            return false;
        }
        offset = sep_idx + 3;
    }
    return true;
}

bool zephium_nav_classify(const char *input, size_t input_len, char *out_url, size_t out_len) {
    if (!input || !out_url || out_len == 0) return false;
    const char *s;
    size_t slen;
    trim_bounds(input, input_len, &s, &slen);

    if (slen == 0) {
        if (out_len < 12) return false;
        return zephium_strcpy_safe(out_url, out_len, "about:blank") == 0;
    }

    if (looks_like_local_path(s, slen)) return false;

    if (looks_like_host(s, slen)) {
        char cand[ZEPHIUM_MAX_URL_BYTES];
        size_t written = 0;
        bool has_path = false;
        for (size_t i = 0; i < slen; ++i) {
            if (s[i] == '/' || s[i] == '?' || s[i] == '#') { has_path = true; break; }
        }
        if (has_path) {
            written = (size_t)snprintf(cand, sizeof(cand), "https://%.*s", (int)slen, s);
        } else {
            written = (size_t)snprintf(cand, sizeof(cand), "https://%.*s/", (int)slen, s);
        }
        if (written < sizeof(cand) && zephium_nav_is_allowed(cand, written)) {
            if (out_len < written + 1) return false;
            memcpy(out_url, cand, written + 1);
            return true;
        }
        return false;
    }

    if (has_leading_scheme_separator(s, slen)) {
        char cand[ZEPHIUM_MAX_URL_BYTES];
        if (slen >= sizeof(cand)) return false;
        memcpy(cand, s, slen);
        cand[slen] = '\0';
        if (zephium_nav_is_allowed(cand, slen)) {
            if (out_len < slen + 1) return false;
            memcpy(out_url, cand, slen + 1);
            return true;
        }
        return false;
    }

    parsed_url_t direct_u;
    if (parse_url_components(s, slen, &direct_u)) {
        if (zephium_nav_is_allowed(s, slen)) {
            if (out_len < slen + 1) return false;
            memcpy(out_url, s, slen);
            out_url[slen] = '\0';
            return true;
        }
        return false;
    }

    if (!embedded_web_targets_allowed(s, slen)) return false;

    return build_search_url(s, slen, out_url, out_len);
}

bool zephium_nav_is_query(const char *input, size_t input_len) {
    if (!input) return false;
    const char *s;
    size_t slen;
    trim_bounds(input, input_len, &s, &slen);
    if (slen == 0) return false;
    if (looks_like_local_path(s, slen)) return false;
    if (looks_like_host(s, slen)) return false;
    if (has_leading_scheme_separator(s, slen)) return false;
    parsed_url_t direct_u;
    if (parse_url_components(s, slen, &direct_u)) return false;
    if (!embedded_web_targets_allowed(s, slen)) return false;

    char dummy[ZEPHIUM_MAX_URL_BYTES];
    return build_search_url(s, slen, dummy, sizeof(dummy));
}

bool zephium_nav_search_query(const char *input, size_t input_len, char *out_url, size_t out_len) {
    if (!input || !out_url) return false;
    const char *s;
    size_t slen;
    trim_bounds(input, input_len, &s, &slen);
    if (slen == 0 || looks_like_local_path(s, slen)) return false;

    if (has_leading_scheme_separator(s, slen)) {
        if (!zephium_nav_is_allowed(s, slen)) return false;
    } else {
        parsed_url_t u;
        if (parse_url_components(s, slen, &u)) {
            if (!zephium_nav_is_allowed(s, slen)) return false;
        } else if (!embedded_web_targets_allowed(s, slen)) {
            return false;
        }
    }

    return build_search_url(s, slen, out_url, out_len);
}

bool zephium_nav_web_address(const char *input, size_t input_len, char *out_url, size_t out_len) {
    if (!input || !out_url || input_len > ZEPHIUM_MAX_URL_BYTES) return false;
    const char *s;
    size_t slen;
    trim_bounds(input, input_len, &s, &slen);
    if (slen == 0) return false;

    /* InputKind must be Direct, never Search */
    if (looks_like_local_path(s, slen)) return false;
    if (!looks_like_host(s, slen) && !has_leading_scheme_separator(s, slen)) {
        parsed_url_t direct_u;
        if (!parse_url_components(s, slen, &direct_u)) return false;
    }

    char classified[ZEPHIUM_MAX_URL_BYTES];
    if (!zephium_nav_classify(input, input_len, classified, sizeof(classified))) return false;
    if (strncmp(classified, "http://", 7) == 0 || strncmp(classified, "https://", 8) == 0) {
        if (strncmp(classified, ZEPHIUM_SEARCH_BASE, strlen(ZEPHIUM_SEARCH_BASE)) == 0) return false;
        if (zephium_nav_is_allowed(classified, strlen(classified))) {
            size_t clen = strlen(classified);
            if (out_len < clen + 1) return false;
            memcpy(out_url, classified, clen + 1);
            return true;
        }
    }
    return false;
}

bool zephium_nav_external_target(const char *arg, size_t arg_len, char *out_url, size_t out_len) {
    if (!arg || !out_url || arg_len > ZEPHIUM_MAX_URL_BYTES) return false;
    const char *s;
    size_t slen;
    trim_bounds(arg, arg_len, &s, &slen);
    if (slen == 0) return false;

    parsed_url_t u;
    if (!parse_url_components(s, slen, &u)) return false;
    if (strcmp(u.scheme, "http") != 0 && strcmp(u.scheme, "https") != 0) return false;
    if (!zephium_nav_is_allowed(s, slen)) return false;

    if (out_len < slen + 1) return false;
    memcpy(out_url, s, slen);
    out_url[slen] = '\0';
    return true;
}

bool zephium_nav_is_subframe_document(const char *target, size_t target_len) {
    if (!target || target_len > ZEPHIUM_MAX_URL_BYTES) return false;
    parsed_url_t u;
    if (!parse_url_components(target, target_len, &u)) return false;

    if (strcmp(u.scheme, "about") == 0) {
        return (strncmp(u.path, "blank", 5) == 0 || strncmp(u.path, "srcdoc", 6) == 0);
    }
    if (strcmp(u.scheme, "data") == 0 || strcmp(u.scheme, "blob") == 0) return true;
    return false;
}

bool zephium_nav_app_link_scheme(const char *scheme, size_t len) {
    if (!scheme || len == 0 || len > 64) return false;
    if (!islower((unsigned char)scheme[0])) return false;
    for (size_t i = 0; i < len; ++i) {
        char b = scheme[i];
        if (!islower((unsigned char)b) && !isdigit((unsigned char)b) && b != '+' && b != '-' && b != '.') {
            return false;
        }
    }
    for (size_t i = 0; i < NUM_NOT_FOR_APPS; ++i) {
        if (strlen(NOT_FOR_APPS[i]) == len && memcmp(scheme, NOT_FOR_APPS[i], len) == 0) {
            return false;
        }
    }
    if (len >= 7 && memcmp(scheme, "zephium", 7) == 0) return false;
    return true;
}

bool zephium_nav_external_app_link(const char *target, size_t target_len, char *out_url, size_t out_len) {
    if (!target || target_len > ZEPHIUM_MAX_URL_BYTES || !out_url) return false;
    parsed_url_t u;
    if (!parse_url_components(target, target_len, &u)) return false;

    if (!zephium_nav_app_link_scheme(u.scheme, strlen(u.scheme))) return false;
    if (u.username[0] != '\0' || u.password[0] != '\0') return false;

    if (out_len < target_len + 1) return false;
    memcpy(out_url, target, target_len);
    out_url[target_len] = '\0';
    /* Rust Url::parse normalizes the scheme to lowercase */
    size_t slen = strlen(u.scheme);
    for (size_t i = 0; i < slen; ++i) {
        out_url[i] = (char)tolower((unsigned char)out_url[i]);
    }
    return true;
}

zephium_app_link_outcome_t zephium_nav_app_link_outcome(bool app_known, bool user_gesture) {
    if (app_known) return ZEPHIUM_APP_LINK_ASK;
    if (user_gesture) return ZEPHIUM_APP_LINK_SHOW;
    return ZEPHIUM_APP_LINK_SILENT;
}
