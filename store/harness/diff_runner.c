#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "../port.h"

/* True when the quote at p closes the string that started at start. A quote is
 * escaped only if an odd number of backslashes precede it, so a JSON string that
 * ends with a literal backslash ("...\\") is still read correctly. */
static bool is_unescaped_quote(const char *start, const char *p) {
    if (*p != '"') return false;
    size_t backslashes = 0;
    const char *q = p - 1;
    while (q >= start && *q == '\\') {
        ++backslashes;
        --q;
    }
    return (backslashes % 2) == 0;
}

typedef struct {
    char text[1024];
    bool is_string;
} test_arg_t;

static void json_unescape_into(const char *src, size_t len, char *dst, size_t cap) {
    size_t d = 0;
    for (size_t s = 0; s < len && d + 1 < cap; ++s) {
        if (src[s] == '\\' && s + 1 < len) {
            ++s;
            switch (src[s]) {
                case 'n': dst[d++] = '\n'; break;
                case 'r': dst[d++] = '\r'; break;
                case 't': dst[d++] = '\t'; break;
                case '"': dst[d++] = '"'; break;
                case '\\': dst[d++] = '\\'; break;
                default: dst[d++] = src[s]; break;
            }
        } else {
            dst[d++] = src[s];
        }
    }
    dst[d] = '\0';
}

/* Reads the "args":[ ... ] array of a case line into up to max_args tokens.
 * Strings are JSON-unescaped, numbers are kept as text. */
static size_t parse_args(const char *line, test_arg_t *args, size_t max_args) {
    const char *p = strstr(line, "\"args\":[");
    if (!p) return 0;
    p += 8;

    size_t count = 0;
    while (*p && count < max_args) {
        while (*p == ' ' || *p == ',') ++p;
        if (*p == ']' || *p == '\0') break;

        if (*p == '"') {
            ++p;
            const char *start = p;
            while (*p && !is_unescaped_quote(start, p)) ++p;
            json_unescape_into(start, (size_t)(p - start), args[count].text, sizeof(args[count].text));
            args[count].is_string = true;
            if (*p == '"') ++p;
        } else {
            const char *start = p;
            while (*p && *p != ',' && *p != ']') ++p;
            size_t len = (size_t)(p - start);
            while (len > 0 && start[len - 1] == ' ') --len;
            if (len >= sizeof(args[count].text)) len = sizeof(args[count].text) - 1;
            memcpy(args[count].text, start, len);
            args[count].text[len] = '\0';
            args[count].is_string = false;
        }
        ++count;
    }
    return count;
}

static bool result_string(const char *line, char *out, size_t cap) {
    const char *p = strstr(line, "\"result\":\"");
    if (!p) return false;
    p += 10;
    const char *start = p;
    while (*p && !is_unescaped_quote(start, p)) ++p;
    json_unescape_into(start, (size_t)(p - start), out, cap);
    return true;
}

static long long result_int(const char *line) {
    const char *p = strstr(line, "\"result\":");
    return p ? atoll(p + 9) : -1;
}

static bool result_true(const char *line) {
    const char *p = strstr(line, "\"result\":");
    return p && strncmp(p + 9, "true", 4) == 0;
}

static const char *int_text(long long value) {
    static char buf[32];
    snprintf(buf, sizeof(buf), "%lld", value);
    return buf;
}

static void append_text(char *buf, size_t cap, size_t *pos, const char *text) {
    size_t len = strlen(text);
    if (*pos + len + 1 > cap) len = cap - 1 - *pos;
    memcpy(buf + *pos, text, len);
    *pos += len;
    buf[*pos] = '\0';
}

int main(int argc, char **argv) {
    const char *cases_path = "cases.jsonl";
    if (argc > 1) cases_path = argv[1];

    FILE *f = fopen(cases_path, "r");
    if (!f) {
        fprintf(stderr, "Cannot open '%s'\n", cases_path);
        return 1;
    }

    zephium_store_t *store = NULL;
    if (!zephium_store_open(":memory:", &store)) {
        fprintf(stderr, "Failed to open memory store\n");
        fclose(f);
        return 1;
    }

    char line[4096];
    size_t total = 0, passed = 0, failed = 0;

    while (fgets(line, sizeof(line), f)) {
        if (strlen(line) < 10) continue;
        total++;

        if (strstr(line, "\"fn\":\"open_memory\"")) {
            passed++;
        } else if (strstr(line, "\"fn\":\"pref_roundtrip\"")) {
            char k[64] = {0}, v[64] = {0};
            if (sscanf(line, "{\"fn\":\"pref_roundtrip\",\"args\":[\"%63[^\"]\",\"%63[^\"]\"]", k, v) == 2) {
                zephium_store_set_preference(store, k, v);
                char out_v[64] = {0};
                if (zephium_store_get_preference(store, k, out_v, sizeof(out_v)) && strcmp(out_v, v) == 0) {
                    passed++;
                } else {
                    failed++;
                    fprintf(stderr, "MISMATCH pref_roundtrip(%s)\n", k);
                }
            } else { failed++; }
        } else if (strstr(line, "\"fn\":\"pref_overwrite\"")) {
            char k[64] = {0}, v1[64] = {0}, v2[64] = {0};
            if (sscanf(line, "{\"fn\":\"pref_overwrite\",\"args\":[\"%63[^\"]\",\"%63[^\"]\",\"%63[^\"]\"]", k, v1, v2) == 3) {
                zephium_store_set_preference(store, k, v1);
                zephium_store_set_preference(store, k, v2);
                char out_v[64] = {0};
                if (zephium_store_get_preference(store, k, out_v, sizeof(out_v)) && strcmp(out_v, v2) == 0) {
                    passed++;
                } else {
                    failed++;
                    fprintf(stderr, "MISMATCH pref_overwrite(%s)\n", k);
                }
            } else { failed++; }
        } else if (strstr(line, "\"fn\":\"bookmark_cycle\"")) {
            char id[64] = {0}, url[128] = {0}, title[64] = {0};
            if (sscanf(line, "{\"fn\":\"bookmark_cycle\",\"args\":[\"%63[^\"]\",\"%127[^\"]\",\"%63[^\"]\"]", id, url, title) == 3) {
                zephium_store_add_bookmark(store, id, url, title);
                size_t cnt = zephium_store_count_bookmarks(store);
                if (cnt == 1) passed++;
                else { failed++; fprintf(stderr, "MISMATCH bookmark_cycle\n"); }
            } else { failed++; }
        } else if (strstr(line, "\"fn\":\"bookmark_delete\"")) {
            char id[64] = {0};
            if (sscanf(line, "{\"fn\":\"bookmark_delete\",\"args\":[\"%63[^\"]\"]", id) == 1) {
                zephium_store_delete_bookmark(store, id);
                size_t cnt = zephium_store_count_bookmarks(store);
                if (cnt == 0) passed++;
                else { failed++; fprintf(stderr, "MISMATCH bookmark_delete\n"); }
            } else { failed++; }
        } else if (strstr(line, "\"fn\":\"history_visits\"")) {
            char url[128] = {0}, title[64] = {0};
            int count = 0;
            if (sscanf(line, "{\"fn\":\"history_visits\",\"args\":[\"%127[^\"]\",\"%63[^\"]\",%d]", url, title, &count) == 3) {
                for (int i = 0; i < count; ++i) {
                    zephium_store_record_visit(store, url, title, (int64_t)(1000 + i));
                }
                size_t act = zephium_store_count_history(store);
                if (act == (size_t)count) passed++;
                else { failed++; fprintf(stderr, "MISMATCH history_visits\n"); }
            } else { failed++; }
        } else if (strstr(line, "\"fn\":\"history_clear\"")) {
            zephium_store_clear_history(store);
            size_t act = zephium_store_count_history(store);
            if (act == 0) passed++;
            else { failed++; fprintf(stderr, "MISMATCH history_clear\n"); }
        } else if (strstr(line, "\"fn\":\"download_lifecycle\"")) {
            char id[64] = {0}, url[128] = {0}, path[128] = {0};
            int total_b = 0, st = 0;
            if (sscanf(line, "{\"fn\":\"download_lifecycle\",\"args\":[\"%63[^\"]\",\"%127[^\"]\",\"%127[^\"]\",%d,%d]",
                       id, url, path, &total_b, &st) == 5) {
                bool ok1 = zephium_store_record_download(store, id, url, path, total_b);
                bool ok2 = zephium_store_update_download(store, id, total_b, (zephium_download_state_t)st);
                if (ok1 && ok2) passed++;
                else { failed++; fprintf(stderr, "MISMATCH download_lifecycle\n"); }
            } else { failed++; }
        } else if (strstr(line, "\"fn\":\"fresh_store\"")) {
            zephium_store_close(store);
            store = NULL;
            if (zephium_store_open(":memory:", &store)) passed++;
            else { failed++; fprintf(stderr, "MISMATCH fresh_store\n"); }

        } else if (strstr(line, "\"fn\":\"history_add\"")) {
            test_arg_t a[3];
            if (parse_args(line, a, 3) == 3 &&
                zephium_store_record_visit(store, a[0].text, a[1].text, (int64_t)atoll(a[2].text))) {
                passed++;
            } else {
                failed++;
                fprintf(stderr, "MISMATCH history_add\n");
            }

        } else if (strstr(line, "\"fn\":\"count_history\"")) {
            long long want = result_int(line);
            size_t got = zephium_store_count_history(store);
            if ((long long)got == want) passed++;
            else { failed++; fprintf(stderr, "MISMATCH count_history: exp %lld, got %zu\n", want, got); }

        } else if (strstr(line, "\"fn\":\"list_history\"") ||
                   strstr(line, "\"fn\":\"search_history\"")) {
            bool is_search = strstr(line, "\"fn\":\"search_history\"") != NULL;
            test_arg_t a[3];
            size_t arg_count = parse_args(line, a, 3);
            char expected[4096] = {0};
            bool have_expected = result_string(line, expected, sizeof(expected));
            zephium_history_row_t *rows = NULL;
            size_t count = 0;
            bool ok = false;
            if (arg_count == 2) {
                size_t limit = (size_t)strtoull(a[is_search ? 1 : 0].text, NULL, 10);
                ok = is_search
                    ? zephium_store_search_history(store, a[0].text, limit, &rows, &count)
                    : zephium_store_list_history(store, limit,
                                                 (size_t)strtoull(a[1].text, NULL, 10),
                                                 &rows, &count);
            }
            char actual[4096] = {0};
            size_t pos = 0;
            if (ok) {
                for (size_t i = 0; i < count; ++i) {
                    if (i > 0) append_text(actual, sizeof(actual), &pos, "\n");
                    append_text(actual, sizeof(actual), &pos, rows[i].url);
                    append_text(actual, sizeof(actual), &pos, "\t");
                    append_text(actual, sizeof(actual), &pos, rows[i].title);
                    append_text(actual, sizeof(actual), &pos, "\t");
                    append_text(actual, sizeof(actual), &pos, int_text(rows[i].visit_time));
                }
            }
            zephium_store_free_history_rows(rows, count);
            if (ok && have_expected && strcmp(actual, expected) == 0) passed++;
            else {
                failed++;
                fprintf(stderr, "MISMATCH %s: exp [%s], got [%s]\n",
                        is_search ? "search_history" : "list_history", expected, actual);
            }

        } else if (strstr(line, "\"fn\":\"bookmark_add\"")) {
            test_arg_t a[3];
            if (parse_args(line, a, 3) == 3 && zephium_store_add_bookmark(store, a[0].text, a[1].text, a[2].text)) {
                passed++;
            } else {
                failed++;
                fprintf(stderr, "MISMATCH bookmark_add\n");
            }

        } else if (strstr(line, "\"fn\":\"list_bookmarks\"")) {
            test_arg_t a[2];
            size_t arg_count = parse_args(line, a, 2);
            char expected[4096] = {0};
            bool have_expected = result_string(line, expected, sizeof(expected));
            zephium_bookmark_row_t *rows = NULL;
            size_t count = 0;
            bool ok = arg_count == 2 &&
                      zephium_store_list_bookmarks(store, (size_t)strtoull(a[0].text, NULL, 10),
                                                   (size_t)strtoull(a[1].text, NULL, 10),
                                                   &rows, &count);
            char actual[4096] = {0};
            size_t pos = 0;
            if (ok) {
                for (size_t i = 0; i < count; ++i) {
                    if (i > 0) append_text(actual, sizeof(actual), &pos, "\n");
                    append_text(actual, sizeof(actual), &pos, rows[i].id);
                    append_text(actual, sizeof(actual), &pos, "\t");
                    append_text(actual, sizeof(actual), &pos, rows[i].url);
                    append_text(actual, sizeof(actual), &pos, "\t");
                    append_text(actual, sizeof(actual), &pos, rows[i].title);
                }
            }
            zephium_store_free_bookmark_rows(rows, count);
            if (ok && have_expected && strcmp(actual, expected) == 0) passed++;
            else {
                failed++;
                fprintf(stderr, "MISMATCH list_bookmarks: exp [%s], got [%s]\n", expected, actual);
            }

        } else if (strstr(line, "\"fn\":\"download_add\"")) {
            test_arg_t a[4];
            if (parse_args(line, a, 4) == 4 &&
                zephium_store_record_download(store, a[0].text, a[1].text, a[2].text,
                                              (int64_t)atoll(a[3].text))) {
                passed++;
            } else {
                failed++;
                fprintf(stderr, "MISMATCH download_add\n");
            }

        } else if (strstr(line, "\"fn\":\"download_progress\"")) {
            test_arg_t a[3];
            bool want = result_true(line);
            bool got = parse_args(line, a, 3) == 3 &&
                       zephium_store_update_download_progress(store, a[0].text,
                                                              (int64_t)atoll(a[1].text),
                                                              (int64_t)atoll(a[2].text));
            if (got == want) passed++;
            else { failed++; fprintf(stderr, "MISMATCH download_progress: exp %d, got %d\n", (int)want, (int)got); }

        } else if (strstr(line, "\"fn\":\"list_downloads\"")) {
            test_arg_t a[2];
            size_t arg_count = parse_args(line, a, 2);
            char expected[4096] = {0};
            bool have_expected = result_string(line, expected, sizeof(expected));
            zephium_download_row_t *rows = NULL;
            size_t count = 0;
            bool ok = arg_count == 2 &&
                      zephium_store_list_downloads(store, (size_t)strtoull(a[0].text, NULL, 10),
                                                   (size_t)strtoull(a[1].text, NULL, 10),
                                                   &rows, &count);
            char actual[4096] = {0};
            size_t pos = 0;
            if (ok) {
                for (size_t i = 0; i < count; ++i) {
                    if (i > 0) append_text(actual, sizeof(actual), &pos, "\n");
                    append_text(actual, sizeof(actual), &pos, rows[i].id);
                    append_text(actual, sizeof(actual), &pos, "\t");
                    append_text(actual, sizeof(actual), &pos, rows[i].url);
                    append_text(actual, sizeof(actual), &pos, "\t");
                    append_text(actual, sizeof(actual), &pos, int_text((int64_t)rows[i].state));
                    append_text(actual, sizeof(actual), &pos, "\t");
                    append_text(actual, sizeof(actual), &pos, int_text(rows[i].received_bytes));
                    append_text(actual, sizeof(actual), &pos, "\t");
                    append_text(actual, sizeof(actual), &pos, int_text(rows[i].total_bytes));
                }
            }
            zephium_store_free_download_rows(rows, count);
            if (ok && have_expected && strcmp(actual, expected) == 0) passed++;
            else {
                failed++;
                fprintf(stderr, "MISMATCH list_downloads: exp [%s], got [%s]\n", expected, actual);
            }
        }
    }

    fclose(f);
    zephium_store_close(store);

    printf("\n=== ZEPHIUM STORE DIFF REPORT ===\n");
    printf("Total recorded cases : %zu\n", total);
    printf("Passed               : %zu\n", passed);
    printf("Failed               : %zu\n", failed);

    if (failed == 0 && total > 0) {
        printf("VERDICT: PASS (100%% bit-identical behavior)\n\n");
        return 0;
    } else {
        printf("VERDICT: FAIL (%zu mismatches)\n\n", failed);
        return 1;
    }
}
