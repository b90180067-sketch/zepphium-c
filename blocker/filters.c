/*
 * Portable WebKit content-filter cache helpers.
 *
 * See blocker/filters.h for the contract. This file deliberately depends only on
 * the C standard library: the WebKitGTK shell owns XDG paths, libsoup downloads
 * and the WebKitUserContentFilterStore. Keeping the policy and the parsers here
 * makes them testable on any host (and on the Windows build, where the file is
 * linked into the CLI core but never used for downloads).
 */

#include "filters.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Small bounded string helpers                                              */
/* ------------------------------------------------------------------------- */

static bool filters_copy_string(char *dst, size_t dst_cap, const char *src) {
    if (!dst || dst_cap == 0 || !src) return false;
    size_t len = strlen(src);
    if (len >= dst_cap) return false;
    memcpy(dst, src, len + 1);
    return true;
}

static bool filters_copy_range(char *dst, size_t dst_cap, const char *src, size_t src_len) {
    if (!dst || dst_cap == 0 || src_len >= dst_cap) return false;
    if (src_len > 0) memcpy(dst, src, src_len);
    dst[src_len] = '\0';
    return true;
}

static bool filters_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static bool filters_has_separator(const char *text) {
    for (const char *p = text; p && *p; ++p) {
        if (*p == '/' || *p == '\\') return true;
    }
    return false;
}

void zephium_filters_manifest_init(zephium_filter_manifest_t *manifest) {
    if (!manifest) return;
    memset(manifest, 0, sizeof(*manifest));
}

/* ------------------------------------------------------------------------- */
/* Minimal JSON reader (manifest only)                                       */
/* ------------------------------------------------------------------------- */

/* A manifest is a few kilobytes of trusted-shape JSON. These helpers only need
 * to locate object members and read short strings/numbers; they do not build a
 * document tree, which is why no JSON library is required. */

static const char *json_skip_space(const char *p, const char *end) {
    while (p < end && filters_is_space(*p)) ++p;
    return p;
}

static const char *json_skip_string(const char *p, const char *end) {
    if (p >= end || *p != '"') return NULL;
    ++p;
    while (p < end) {
        unsigned char ch = (unsigned char)*p;
        if (ch == '\\') {
            p += 2; /* the escaped character cannot terminate the string */
            continue;
        }
        if (ch == '"') return p + 1;
        ++p;
    }
    return NULL;
}

static const char *json_skip_value(const char *p, const char *end, unsigned depth) {
    if (depth > 32) return NULL; /* malformed or hostile nesting */
    p = json_skip_space(p, end);
    if (p >= end) return NULL;
    if (*p == '"') return json_skip_string(p, end);
    if (*p == '{' || *p == '[') {
        const char close = (*p == '{') ? '}' : ']';
        const char open = *p;
        ++p;
        for (;;) {
            p = json_skip_space(p, end);
            if (p >= end) return NULL;
            if (*p == close) return p + 1;
            if (open == '{') {
                p = json_skip_string(p, end);
                if (!p) return NULL;
                p = json_skip_space(p, end);
                if (p >= end || *p != ':') return NULL;
                ++p;
            }
            p = json_skip_value(p, end, depth + 1);
            if (!p) return NULL;
            p = json_skip_space(p, end);
            if (p >= end) return NULL;
            if (*p == ',') {
                ++p;
                continue;
            }
            if (*p == close) return p + 1;
            return NULL;
        }
    }
    /* number / true / false / null: consume the bare token */
    while (p < end && *p != ',' && *p != '}' && *p != ']' && !filters_is_space(*p)) ++p;
    return p;
}

/* Locates "key" in the object at obj and returns the start of its value, or NULL
 * when the member is absent or the object is malformed. */
static const char *json_object_member(const char *obj, const char *end, const char *key) {
    size_t key_len = strlen(key);
    const char *p = json_skip_space(obj, end);
    if (p >= end || *p != '{') return NULL;
    ++p;
    for (;;) {
        p = json_skip_space(p, end);
        if (p >= end || *p == '}') return NULL;
        const char *key_start = p;
        p = json_skip_string(p, end);
        if (!p) return NULL;
        size_t raw_len = (size_t)(p - key_start);
        bool match = raw_len == key_len + 2 && memcmp(key_start + 1, key, key_len) == 0;
        p = json_skip_space(p, end);
        if (p >= end || *p != ':') return NULL;
        ++p;
        p = json_skip_space(p, end);
        if (match) return p;
        p = json_skip_value(p, end, 0);
        if (!p) return NULL;
        p = json_skip_space(p, end);
        if (p >= end) return NULL;
        if (*p == ',') {
            ++p;
            continue;
        }
        if (*p == '}') return NULL;
        return NULL;
    }
}

typedef bool (*json_member_fn)(const char *key, size_t key_len, const char *value,
                               const char *end, void *ctx);

/* Iterates the members of the object at obj. Returns true when the object was
 * structurally walkable; a member callback returning false stops the walk. */
static bool json_object_foreach(const char *obj, const char *end, json_member_fn fn, void *ctx) {
    const char *p = json_skip_space(obj, end);
    if (p >= end || *p != '{') return false;
    ++p;
    for (;;) {
        p = json_skip_space(p, end);
        if (p >= end) return false;
        if (*p == '}') return true;
        const char *key_start = p;
        p = json_skip_string(p, end);
        if (!p) return false;
        size_t key_len = (size_t)(p - key_start) - 2;
        p = json_skip_space(p, end);
        if (p >= end || *p != ':') return false;
        ++p;
        p = json_skip_space(p, end);
        if (!fn(key_start + 1, key_len, p, end, ctx)) return true;
        p = json_skip_value(p, end, 0);
        if (!p) return false;
        p = json_skip_space(p, end);
        if (p >= end) return false;
        if (*p == ',') {
            ++p;
            continue;
        }
        if (*p == '}') return true;
        return false;
    }
}

/* Reads the JSON string at p into out. Escapes are decoded for the ASCII range;
 * other escapes become '-' because every consumer only accepts a safe charset. */
static bool json_read_string(const char *p, const char *end, char *out, size_t out_cap) {
    p = json_skip_space(p, end);
    if (p >= end || *p != '"') return false;
    ++p;
    size_t o = 0;
    while (p < end && *p != '"') {
        char ch = *p;
        if (ch == '\\') {
            ++p;
            if (p >= end) return false;
            char esc = *p;
            switch (esc) {
                case '"': ch = '"'; break;
                case '\\': ch = '\\'; break;
                case '/': ch = '/'; break;
                case 'u': {
                    /* \uXXXX: only ASCII survives sanitising anyway. */
                    ch = '-';
                    for (int i = 0; i < 4 && p + 1 < end; ++i) ++p;
                    break;
                }
                default: ch = '-'; break;
            }
        }
        if (o + 1 >= out_cap) return false;
        out[o++] = ch;
        ++p;
    }
    if (p >= end) return false;
    out[o] = '\0';
    return true;
}

static bool json_read_u64(const char *p, const char *end, uint64_t *out) {
    p = json_skip_space(p, end);
    uint64_t value = 0;
    size_t digits = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        uint64_t digit = (uint64_t)(*p - '0');
        if (value > (UINT64_MAX - digit) / 10) return false;
        value = value * 10 + digit;
        ++digits;
        ++p;
    }
    if (digits == 0) return false;
    *out = value;
    return true;
}

/* ------------------------------------------------------------------------- */
/* Manifest parsing                                                          */
/* ------------------------------------------------------------------------- */

static void manifest_add_file(zephium_filter_file_t *files, size_t *count, size_t max_count,
                              const char *file_name) {
    if (*count >= max_count) return;
    if (!zephium_filters_file_name_safe(file_name)) return;
    zephium_filter_file_t *entry = &files[*count];
    if (!filters_copy_string(entry->file, sizeof(entry->file), file_name)) return;
    (void)zephium_filters_part_label(file_name, entry->label, sizeof(entry->label));
    entry->rules_count = 0;
    ++(*count);
}

static bool combined_files_member(const char *key, size_t key_len, const char *value,
                                  const char *end, void *ctx) {
    (void)key;
    (void)key_len;
    zephium_filter_manifest_t *manifest = (zephium_filter_manifest_t *)ctx;
    const char *p = json_skip_space(value, end);
    if (p >= end || *p != '[') return false;
    ++p;
    for (;;) {
        p = json_skip_space(p, end);
        if (p >= end) return false;
        if (*p == ']') return true;
        char file_name[ZEPHIUM_FILTERS_NAME_CAP];
        if (!json_read_string(p, end, file_name, sizeof(file_name))) return false;
        manifest_add_file(manifest->combined, &manifest->combined_count,
                          ZEPHIUM_FILTERS_MAX_FILES, file_name);
        p = json_skip_string(p, end);
        if (!p) return false;
        p = json_skip_space(p, end);
        if (p >= end) return false;
        if (*p == ',') {
            ++p;
            continue;
        }
        if (*p == ']') return true;
        return false;
    }
}

static bool combined_members(const char *key, size_t key_len, const char *value,
                             const char *end, void *ctx) {
    zephium_filter_manifest_t *manifest = (zephium_filter_manifest_t *)ctx;
    if (key_len == strlen("total_rules") && memcmp(key, "total_rules", key_len) == 0) {
        uint64_t total = 0;
        if (json_read_u64(value, end, &total)) manifest->declared_total_rules = total;
        return true;
    }
    if (key_len == strlen("files") && memcmp(key, "files", key_len) == 0) {
        return combined_files_member(key, key_len, value, end, ctx);
    }
    return true;
}

static bool list_member(const char *key, size_t key_len, const char *value,
                        const char *end, void *ctx) {
    zephium_filter_manifest_t *manifest = (zephium_filter_manifest_t *)ctx;
    if (manifest->list_count >= ZEPHIUM_FILTERS_MAX_FILES) return true;

    /* The per-list objects carry "rules_count"; the file name is derived from
     * the list name ("easylist" -> "easylist.json"), which is how the upstream
     * release publishes the assets. */
    char name[ZEPHIUM_FILTERS_NAME_CAP];
    if (!filters_copy_range(name, sizeof(name), key, key_len)) return true;
    char file_name[ZEPHIUM_FILTERS_NAME_CAP];
    int written = snprintf(file_name, sizeof(file_name), "%s.json", name);
    if (written <= 0 || (size_t)written >= sizeof(file_name)) return true;
    if (!zephium_filters_file_name_safe(file_name)) return true;

    zephium_filter_file_t entry;
    memset(&entry, 0, sizeof(entry));
    if (!filters_copy_string(entry.file, sizeof(entry.file), file_name)) return true;
    if (!filters_copy_string(entry.label, sizeof(entry.label), name)) return true;

    const char *rules = json_object_member(value, end, "rules_count");
    if (rules) {
        uint64_t count = 0;
        if (json_read_u64(rules, end, &count) && count <= (uint64_t)UINT32_MAX) {
            entry.rules_count = (uint32_t)count;
        }
    }
    manifest->lists[manifest->list_count++] = entry;
    return true;
}

bool zephium_filters_manifest_parse(const char *json, size_t json_len,
                                    zephium_filter_manifest_t *manifest) {
    if (!manifest) return false;
    zephium_filters_manifest_init(manifest);
    if (!json || json_len == 0) return false;

    const char *p = json;
    const char *end = json + json_len;
    /* Tolerate a UTF-8 BOM: manifests written by Windows tooling may carry one. */
    if (json_len >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB &&
        (unsigned char)p[2] == 0xBF) {
        p += 3;
    }
    p = json_skip_space(p, end);
    if (p >= end || *p != '{') return false;

    /* Reject truncated documents up front: the value scanner must consume the
     * whole document and leave only whitespace, so a half-downloaded manifest is
     * never mistaken for a shorter but valid one. */
    const char *document_end = json_skip_value(p, end, 0);
    if (!document_end) return false;
    if (json_skip_space(document_end, end) != end) return false;

    char raw_version[ZEPHIUM_FILTERS_VERSION_CAP * 2];
    const char *version = json_object_member(p, end, "version");
    if (!version || !json_read_string(version, end, raw_version, sizeof(raw_version))) return false;
    if (!zephium_filters_version_sanitize(raw_version, manifest->version,
                                          sizeof(manifest->version))) {
        return false;
    }

    const char *combined = json_object_member(p, end, "combined");
    if (combined) {
        const char *c = json_skip_space(combined, end);
        if (c < end && *c == '{') {
            (void)json_object_foreach(c, end, combined_members, manifest);
        }
    }

    /* Estimate the per-part rule count: the manifest only publishes the combined
     * total, but the per-filter cap applies to each part individually. */
    if (manifest->combined_count > 0 && manifest->declared_total_rules > 0) {
        uint64_t parts = (uint64_t)manifest->combined_count;
        uint64_t per_part = (manifest->declared_total_rules + parts - 1u) / parts;
        uint32_t estimate = per_part > (uint64_t)ZEPHIUM_FILTERS_RULE_CAP
                                ? ZEPHIUM_FILTERS_RULE_CAP
                                : (uint32_t)per_part;
        for (size_t i = 0; i < manifest->combined_count; ++i) {
            manifest->combined[i].rules_count = estimate;
        }
    }

    /* Individual lists let the shell fall back to lists that each fit the cap
     * when the combined parts do not, so they are always worth parsing. */
    const char *lists = json_object_member(p, end, "lists");
    if (lists) {
        const char *l = json_skip_space(lists, end);
        if (l < end && *l == '{') {
            /* Each member of "lists" is one list; its key is the list name. */
            if (!json_object_foreach(l, end, list_member, manifest)) return false;
        }
    }

    if (manifest->version[0] == '\0') return false;
    return manifest->combined_count > 0 || manifest->list_count > 0;
}

/* ------------------------------------------------------------------------- */
/* File selection                                                            */
/* ------------------------------------------------------------------------- */

static bool selected_already(const zephium_filter_file_t *out, size_t count, const char *file) {
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(out[i].file, file) == 0) return true;
    }
    return false;
}

static size_t select_preferred_list(const zephium_filter_manifest_t *manifest, const char *label,
                                    zephium_filter_file_t *out, size_t count, size_t cap) {
    for (size_t i = 0; i < manifest->list_count && count < cap; ++i) {
        const zephium_filter_file_t *candidate = &manifest->lists[i];
        if (strcmp(candidate->label, label) != 0) continue;
        if (candidate->rules_count > ZEPHIUM_FILTERS_RULE_CAP) continue;
        if (selected_already(out, count, candidate->file)) continue;
        out[count++] = *candidate;
    }
    return count;
}

size_t zephium_filters_select(const zephium_filter_manifest_t *manifest, bool prefer_combined,
                              zephium_filter_file_t *out, size_t out_cap) {
    if (!manifest || !out || out_cap == 0) return 0;
    size_t cap = out_cap < (size_t)ZEPHIUM_FILTERS_MAX_FILTERS
                     ? out_cap
                     : (size_t)ZEPHIUM_FILTERS_MAX_FILTERS;

    if (prefer_combined && manifest->combined_count > 0) {
        bool fits = true;
        for (size_t i = 0; i < manifest->combined_count; ++i) {
            if (manifest->combined[i].rules_count > ZEPHIUM_FILTERS_RULE_CAP) fits = false;
        }
        if (fits) {
            size_t count = 0;
            for (size_t i = 0; i < manifest->combined_count && count < cap; ++i) {
                out[count++] = manifest->combined[i];
            }
            return count;
        }
    }

    /* Combined parts cannot be one filter each: use the individual lists. The
     * two largest lists carry most of the value, so they are picked first. */
    static const char *const preferred[] = { "easylist", "easyprivacy" };
    size_t count = 0;
    for (size_t i = 0; i < sizeof(preferred) / sizeof(preferred[0]) && count < cap; ++i) {
        count = select_preferred_list(manifest, preferred[i], out, count, cap);
    }
    for (size_t i = 0; i < manifest->list_count && count < cap; ++i) {
        const zephium_filter_file_t *candidate = &manifest->lists[i];
        if (candidate->rules_count > ZEPHIUM_FILTERS_RULE_CAP) continue;
        if (selected_already(out, count, candidate->file)) continue;
        out[count++] = *candidate;
    }
    return count;
}

/* ------------------------------------------------------------------------- */
/* Cache stamp and reuse policy                                              */
/* ------------------------------------------------------------------------- */

size_t zephium_filters_stamp_format(char *out, size_t out_cap, const char *version,
                                    uint64_t declared_total_rules, int64_t fetched_at) {
    if (!out || out_cap == 0 || !version || !*version) return 0;
    char safe_version[ZEPHIUM_FILTERS_VERSION_CAP];
    if (!zephium_filters_version_sanitize(version, safe_version, sizeof(safe_version))) return 0;
    int written = snprintf(out, out_cap, "version=%s\nfetched_at=%lld\nrules=%llu\n", safe_version,
                           (long long)fetched_at, (unsigned long long)declared_total_rules);
    if (written < 0 || (size_t)written >= out_cap) return 0;
    return (size_t)written;
}

/* Finds "key=" in the stamp text; returns the value range or NULL. */
static const char *stamp_value(const char *text, size_t text_len, const char *key,
                               size_t *value_len) {
    size_t key_len = strlen(key);
    size_t line_start = 0;
    while (line_start < text_len) {
        size_t line_end = line_start;
        while (line_end < text_len && text[line_end] != '\n' && text[line_end] != '\r') ++line_end;
        size_t len = line_end - line_start;
        if (len > key_len && text[line_start + key_len] == '=' &&
            memcmp(text + line_start, key, key_len) == 0) {
            *value_len = len - key_len - 1;
            return text + line_start + key_len + 1;
        }
        line_start = line_end;
        while (line_start < text_len && (text[line_start] == '\n' || text[line_start] == '\r')) {
            ++line_start;
        }
    }
    return NULL;
}

bool zephium_filters_stamp_parse(const char *text, size_t text_len, char *version_out,
                                 size_t version_cap, uint64_t *declared_total_rules_out,
                                 int64_t *fetched_at_out) {
    if (!text || text_len == 0) return false;
    size_t value_len = 0;
    const char *version = stamp_value(text, text_len, "version", &value_len);
    if (!version || value_len == 0 || value_len >= version_cap) return false;
    if (!filters_copy_range(version_out, version_cap, version, value_len)) return false;

    const char *fetched = stamp_value(text, text_len, "fetched_at", &value_len);
    if (!fetched || value_len == 0) return false;
    char number[32];
    if (!filters_copy_range(number, sizeof(number), fetched, value_len)) return false;
    char *parse_end = NULL;
    long long parsed = strtoll(number, &parse_end, 10);
    if (!parse_end || *parse_end != '\0' || parsed <= 0) return false;
    if (fetched_at_out) *fetched_at_out = (int64_t)parsed;

    if (declared_total_rules_out) {
        const char *rules = stamp_value(text, text_len, "rules", &value_len);
        *declared_total_rules_out = 0;
        if (rules && value_len > 0 && value_len < sizeof(number)) {
            if (filters_copy_range(number, sizeof(number), rules, value_len)) {
                parse_end = NULL;
                unsigned long long parsed_rules = strtoull(number, &parse_end, 10);
                if (parse_end && *parse_end == '\0') *declared_total_rules_out = parsed_rules;
            }
        }
    }
    return true;
}

bool zephium_filters_cache_is_complete(const zephium_filters_cache_state_t *state) {
    if (!state) return false;
    return state->manifest_present && state->stamp_present && state->stamp_fetched_at > 0 &&
           state->lists_complete;
}

bool zephium_filters_cache_can_reuse(const zephium_filters_cache_state_t *state, int64_t now) {
    if (!zephium_filters_cache_is_complete(state)) return false;
    if (now < 0) return false;
    /* A stamp in the future means a broken clock or a corrupted file: refresh. */
    if (state->stamp_fetched_at > now + 3600) return false;
    return (now - state->stamp_fetched_at) < (int64_t)ZEPHIUM_FILTERS_REFRESH_SECONDS;
}

/* ------------------------------------------------------------------------- */
/* WebKit JSON validation                                                    */
/* ------------------------------------------------------------------------- */

typedef struct {
    size_t bytes;
    uint32_t rules;
    unsigned depth;
    bool started;
    bool finished;
    bool in_string;
    bool escaped;
    bool expect_value;
    bool valid;
} filter_scan_t;

static void filter_scan_init(filter_scan_t *scan) {
    memset(scan, 0, sizeof(*scan));
    scan->valid = true;
}

static void filter_scan_feed(filter_scan_t *scan, const char *data, size_t len) {
    if (!scan || !data || !scan->valid) return;
    for (size_t i = 0; i < len; ++i) {
        const char ch = data[i];
        ++scan->bytes;

        if (!scan->started) {
            if (filters_is_space(ch)) continue;
            if (ch == '[') {
                scan->started = true;
                scan->depth = 1;
                scan->expect_value = true;
                continue;
            }
            scan->valid = false;
            return;
        }
        if (scan->finished) {
            if (filters_is_space(ch)) continue;
            scan->valid = false; /* trailing garbage after the array */
            return;
        }
        if (scan->in_string) {
            if (scan->escaped) {
                scan->escaped = false;
            } else if (ch == '\\') {
                scan->escaped = true;
            } else if (ch == '"') {
                scan->in_string = false;
            }
            continue;
        }
        if (filters_is_space(ch)) continue;

        switch (ch) {
            case '"':
                if (scan->depth == 1 && scan->expect_value) {
                    if (scan->rules < UINT32_MAX) ++scan->rules;
                    scan->expect_value = false;
                }
                scan->in_string = true;
                break;
            case '{':
            case '[':
                if (scan->depth == 1 && scan->expect_value) {
                    if (scan->rules < UINT32_MAX) ++scan->rules;
                    scan->expect_value = false;
                }
                ++scan->depth;
                if (scan->depth > 64) scan->valid = false;
                break;
            case '}':
                if (scan->depth <= 1) {
                    scan->valid = false; /* a top-level array cannot close with '}' */
                    break;
                }
                --scan->depth;
                break;
            case ']':
                if (scan->depth == 0) {
                    scan->valid = false;
                    break;
                }
                if (scan->depth == 1 && scan->expect_value && scan->rules > 0) {
                    scan->valid = false; /* trailing comma before the closing bracket */
                    break;
                }
                --scan->depth;
                if (scan->depth == 0) {
                    scan->finished = true;
                    scan->expect_value = false;
                }
                break;
            case ',':
                if (scan->depth == 1) {
                    if (scan->expect_value) {
                        scan->valid = false;
                    } else {
                        scan->expect_value = true;
                    }
                }
                break;
            default:
                /* First character of a number or true/false/null element. */
                if (scan->depth == 1 && scan->expect_value) {
                    if (scan->rules < UINT32_MAX) ++scan->rules;
                    scan->expect_value = false;
                }
                break;
        }
        if (!scan->valid) return;
    }
}

static bool filter_scan_finish(const filter_scan_t *scan, zephium_filter_json_stats_t *stats) {
    bool ok = scan->valid && scan->started && scan->finished && !scan->in_string && !scan->escaped;
    if (stats) {
        stats->bytes = scan->bytes;
        stats->top_level_rules = scan->rules;
        stats->valid = ok;
    }
    return ok;
}

bool zephium_filters_json_validate(const char *json, size_t json_len,
                                   zephium_filter_json_stats_t *stats) {
    filter_scan_t scan;
    filter_scan_init(&scan);
    if (json && json_len > 0) filter_scan_feed(&scan, json, json_len);
    return filter_scan_finish(&scan, stats);
}

bool zephium_filters_json_file_validate(const char *path, zephium_filter_json_stats_t *stats) {
    if (!path || !*path) return false;
    FILE *file = fopen(path, "rb");
    if (!file) return false;

    filter_scan_t scan;
    filter_scan_init(&scan);
    char chunk[64 * 1024];
    size_t total = 0;
    while (scan.valid) {
        size_t read_bytes = fread(chunk, 1, sizeof(chunk), file);
        if (read_bytes == 0) break;
        total += read_bytes;
        filter_scan_feed(&scan, chunk, read_bytes);
    }
    bool read_ok = ferror(file) == 0;
    (void)fclose(file);

    scan.bytes = total;
    return read_ok && filter_scan_finish(&scan, stats);
}

bool zephium_filters_rule_count_ok(uint32_t rule_count) {
    return rule_count <= ZEPHIUM_FILTERS_RULE_CAP;
}

/* ------------------------------------------------------------------------- */
/* Paths                                                                     */
/* ------------------------------------------------------------------------- */

size_t zephium_filters_path_join(char *out, size_t out_cap, const char *dir, const char *name) {
    if (!out || out_cap == 0 || !dir || !*dir || !name || !*name) return 0;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return 0;
    if (filters_has_separator(name)) return 0;

    size_t dir_len = strlen(dir);
    size_t name_len = strlen(name);
    bool need_separator = dir[dir_len - 1] != '/';
    size_t total = dir_len + (need_separator ? 1u : 0u) + name_len;
    if (total + 1 > out_cap) return 0;

    memcpy(out, dir, dir_len);
    size_t offset = dir_len;
    if (need_separator) out[offset++] = '/';
    memcpy(out + offset, name, name_len + 1);
    return total;
}

size_t zephium_filters_manifest_path(char *out, size_t out_cap, const char *cache_dir) {
    return zephium_filters_path_join(out, out_cap, cache_dir, "manifest.json");
}

size_t zephium_filters_stamp_path(char *out, size_t out_cap, const char *cache_dir) {
    return zephium_filters_path_join(out, out_cap, cache_dir, "manifest.stamp");
}

size_t zephium_filters_list_dir_path(char *out, size_t out_cap, const char *cache_dir,
                                     const char *version) {
    char safe_version[ZEPHIUM_FILTERS_VERSION_CAP];
    if (!zephium_filters_version_sanitize(version, safe_version, sizeof(safe_version))) return 0;

    char version_root[ZEPHIUM_FILTERS_PATH_CAP];
    if (zephium_filters_path_join(version_root, sizeof(version_root), cache_dir, "v") == 0) return 0;
    return zephium_filters_path_join(out, out_cap, version_root, safe_version);
}

size_t zephium_filters_list_path(char *out, size_t out_cap, const char *cache_dir,
                                 const char *version, const char *file) {
    if (!file || !zephium_filters_file_name_safe(file)) return 0;
    char dir[ZEPHIUM_FILTERS_PATH_CAP];
    if (zephium_filters_list_dir_path(dir, sizeof(dir), cache_dir, version) == 0) return 0;
    return zephium_filters_path_join(out, out_cap, dir, file);
}

size_t zephium_filters_tmp_path(char *out, size_t out_cap, const char *path) {
    if (!out || out_cap == 0 || !path || !*path) return 0;
    size_t path_len = strlen(path);
    if (path_len + 5 + 1 > out_cap) return 0;
    memcpy(out, path, path_len);
    memcpy(out + path_len, ".part", 6);
    return path_len + 5;
}

/* ------------------------------------------------------------------------- */
/* Names, versions and store identifiers                                     */
/* ------------------------------------------------------------------------- */

bool zephium_filters_file_name_safe(const char *file_name) {
    if (!file_name || !*file_name) return false;
    size_t len = strlen(file_name);
    if (len < 6 || len >= ZEPHIUM_FILTERS_NAME_CAP) return false;
    if (file_name[0] == '.') return false;
    if (strcmp(file_name + len - 5, ".json") != 0) return false;
    for (size_t i = 0; i < len; ++i) {
        char ch = file_name[i];
        bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                  (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.';
        if (!ok) return false;
    }
    if (strstr(file_name, "..") != NULL) return false;
    return true;
}

bool zephium_filters_version_sanitize(const char *version, char *out, size_t out_cap) {
    if (!version || !*version || !out || out_cap == 0) return false;
    size_t offset = 0;
    char previous = '\0';
    for (const char *p = version; *p; ++p) {
        char ch = *p;
        bool safe = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                    (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.';
        if (!safe) ch = '-';
        /* Never emit a leading dot or a ".." run: the version names a directory. */
        if (offset == 0 && ch == '.') ch = 'v';
        if (ch == '.' && previous == '.') ch = '-';
        if (offset + 1 >= out_cap) return false;
        out[offset++] = ch;
        previous = ch;
    }
    /* A version becomes a directory name: never leave a trailing dot or dash
     * behind (Windows strips those, which would desynchronise the cache path). */
    while (offset > 0 && (out[offset - 1] == '.' || out[offset - 1] == '-')) --offset;
    out[offset] = '\0';
    if (offset == 0) return false;
    for (size_t i = 0; i < offset; ++i) {
        if (out[i] != '.' && out[i] != '-') return true;
    }
    return false; /* nothing but dots/dashes: not a usable directory name */
}

size_t zephium_filters_part_label(const char *file_name, char *out, size_t out_cap) {
    if (!file_name || !*file_name || !out || out_cap == 0) return 0;
    const char *base = file_name;
    for (const char *p = file_name; *p; ++p) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    size_t len = strlen(base);
    if (len > 5 && strcmp(base + len - 5, ".json") == 0) len -= 5;
    const char *label = base;
    size_t label_len = len;
    if (label_len > 9 && strncmp(label, "combined-", 9) == 0) {
        label += 9;
        label_len -= 9;
    }
    if (label_len == 0 || label_len >= out_cap) return 0;
    memcpy(out, label, label_len);
    out[label_len] = '\0';
    return label_len;
}

size_t zephium_filters_store_identifier(const char *file_name, const char *version,
                                        char *out, size_t out_cap) {
    if (!out || out_cap == 0 || !zephium_filters_file_name_safe(file_name)) return 0;
    char safe_version[ZEPHIUM_FILTERS_VERSION_CAP];
    if (!zephium_filters_version_sanitize(version, safe_version, sizeof(safe_version))) return 0;
    char label[ZEPHIUM_FILTERS_NAME_CAP];
    if (zephium_filters_part_label(file_name, label, sizeof(label)) == 0) return 0;
    int written = snprintf(out, out_cap, "zephium-adblock-%s-%s", label, safe_version);
    if (written <= 0 || (size_t)written >= out_cap) return 0;
    return (size_t)written;
}

bool zephium_filters_identifier_owned(const char *identifier) {
    static const char prefix[] = "zephium-adblock-";
    if (!identifier) return false;
    size_t prefix_len = sizeof(prefix) - 1;
    if (strncmp(identifier, prefix, prefix_len) != 0) return false;
    return identifier[prefix_len] != '\0';
}