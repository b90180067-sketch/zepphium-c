#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "../port.h"

static void unescape_json(const char *src, char *dest, size_t dest_cap) {
    size_t d = 0;
    for (size_t s = 0; src[s] && d + 1 < dest_cap; ++s) {
        if (src[s] == '\\' && src[s + 1]) {
            s++;
            switch (src[s]) {
                case 'n': dest[d++] = '\n'; break;
                case 'r': dest[d++] = '\r'; break;
                case 't': dest[d++] = '\t'; break;
                case '"': dest[d++] = '"'; break;
                case '\\': dest[d++] = '\\'; break;
                default: dest[d++] = src[s]; break;
            }
        } else {
            dest[d++] = src[s];
        }
    }
    dest[d] = '\0';
}

static bool extract_json_arg(const char *line, char *out, size_t out_cap) {
    const char *p = strstr(line, "\"args\":[\"");
    if (!p) return false;
    p += 9;
    char raw[4096] = {0};
    size_t i = 0;
    while (*p && !(p[0] == '"' && p[-1] != '\\') && i + 1 < sizeof(raw)) {
        raw[i++] = *p++;
    }
    raw[i] = '\0';
    unescape_json(raw, out, out_cap);
    return true;
}

static bool extract_json_result_str(const char *line, char *out, size_t out_cap) {
    const char *p = strstr(line, "\"result\":\"");
    if (!p) return false;
    p += 10;
    char raw[4096] = {0};
    size_t i = 0;
    while (*p && !(p[0] == '"' && p[-1] != '\\') && i + 1 < sizeof(raw)) {
        raw[i++] = *p++;
    }
    raw[i] = '\0';
    unescape_json(raw, out, out_cap);
    return true;
}

int main(int argc, char **argv) {
    const char *cases_path = "cases.jsonl";
    if (argc > 1) cases_path = argv[1];

    FILE *f = fopen(cases_path, "r");
    if (!f) {
        fprintf(stderr, "Cannot open '%s'\n", cases_path);
        return 1;
    }

    char line[16384];
    size_t total = 0, passed = 0, failed = 0;

    while (fgets(line, sizeof(line), f)) {
        if (strlen(line) < 10) continue;
        total++;

        char arg[4096] = {0};
        bool has_arg = extract_json_arg(line, arg, sizeof(arg));

        if (strstr(line, "\"fn\":\"classify\"")) {
            if (!has_arg) { failed++; continue; }
            char act[ZEPHIUM_MAX_URL_BYTES] = {0};
            bool ok = zephium_nav_classify(arg, strlen(arg), act, sizeof(act));
            if (strstr(line, "\"result\":null")) {
                if (!ok) passed++;
                else { failed++; fprintf(stderr, "MISMATCH classify('%s'): exp null, got '%s'\n", arg, act); }
            } else {
                char exp[4096] = {0};
                extract_json_result_str(line, exp, sizeof(exp));
                if (ok && strcmp(act, exp) == 0) passed++;
                else { failed++; fprintf(stderr, "MISMATCH classify('%s'): exp '%s', got '%s'\n", arg, exp, act); }
            }
        } else if (strstr(line, "\"fn\":\"is_query\"")) {
            if (!has_arg) { failed++; continue; }
            bool act = zephium_nav_is_query(arg, strlen(arg));
            bool exp = (strstr(line, "\"result\":true") != NULL);
            if (act == exp) passed++;
            else { failed++; fprintf(stderr, "MISMATCH is_query('%s'): exp %d, got %d\n", arg, exp, act); }
        } else if (strstr(line, "\"fn\":\"search_query\"")) {
            if (!has_arg) { failed++; continue; }
            char act[ZEPHIUM_MAX_URL_BYTES] = {0};
            bool ok = zephium_nav_search_query(arg, strlen(arg), act, sizeof(act));
            if (strstr(line, "\"result\":null")) {
                if (!ok) passed++;
                else { failed++; fprintf(stderr, "MISMATCH search_query('%s'): exp null, got '%s'\n", arg, act); }
            } else {
                char exp[4096] = {0};
                extract_json_result_str(line, exp, sizeof(exp));
                if (ok && strcmp(act, exp) == 0) passed++;
                else { failed++; fprintf(stderr, "MISMATCH search_query('%s'): exp '%s', got '%s'\n", arg, exp, act); }
            }
        } else if (strstr(line, "\"fn\":\"is_allowed_str\"")) {
            if (!has_arg) { failed++; continue; }
            bool act = zephium_nav_is_allowed(arg, strlen(arg));
            bool exp = (strstr(line, "\"result\":true") != NULL);
            if (act == exp) passed++;
            else { failed++; fprintf(stderr, "MISMATCH is_allowed_str('%s'): exp %d, got %d\n", arg, exp, act); }
        } else if (strstr(line, "\"fn\":\"web_address\"")) {
            if (!has_arg) { failed++; continue; }
            char act[ZEPHIUM_MAX_URL_BYTES] = {0};
            bool ok = zephium_nav_web_address(arg, strlen(arg), act, sizeof(act));
            if (strstr(line, "\"result\":null")) {
                if (!ok) passed++;
                else { failed++; fprintf(stderr, "MISMATCH web_address('%s'): exp null, got '%s'\n", arg, act); }
            } else {
                char exp[4096] = {0};
                extract_json_result_str(line, exp, sizeof(exp));
                if (ok && strcmp(act, exp) == 0) passed++;
                else { failed++; fprintf(stderr, "MISMATCH web_address('%s'): exp '%s', got '%s'\n", arg, exp, act); }
            }
        } else if (strstr(line, "\"fn\":\"external_target\"")) {
            if (!has_arg) { failed++; continue; }
            char act[ZEPHIUM_MAX_URL_BYTES] = {0};
            bool ok = zephium_nav_external_target(arg, strlen(arg), act, sizeof(act));
            if (strstr(line, "\"result\":null")) {
                if (!ok) passed++;
                else { failed++; fprintf(stderr, "MISMATCH external_target('%s'): exp null, got '%s'\n", arg, act); }
            } else {
                char exp[4096] = {0};
                extract_json_result_str(line, exp, sizeof(exp));
                if (ok && strcmp(act, exp) == 0) passed++;
                else { failed++; fprintf(stderr, "MISMATCH external_target('%s'): exp '%s', got '%s'\n", arg, exp, act); }
            }
        } else if (strstr(line, "\"fn\":\"is_subframe_document\"")) {
            if (!has_arg) { failed++; continue; }
            bool act = zephium_nav_is_subframe_document(arg, strlen(arg));
            bool exp = (strstr(line, "\"result\":true") != NULL);
            if (act == exp) passed++;
            else { failed++; fprintf(stderr, "MISMATCH is_subframe_document('%s'): exp %d, got %d\n", arg, exp, act); }
        } else if (strstr(line, "\"fn\":\"external_app_link\"")) {
            if (!has_arg) { failed++; continue; }
            char act[ZEPHIUM_MAX_URL_BYTES] = {0};
            bool ok = zephium_nav_external_app_link(arg, strlen(arg), act, sizeof(act));
            if (strstr(line, "\"result\":null")) {
                if (!ok) passed++;
                else { failed++; fprintf(stderr, "MISMATCH external_app_link('%s'): exp null, got '%s'\n", arg, act); }
            } else {
                char exp[4096] = {0};
                extract_json_result_str(line, exp, sizeof(exp));
                if (ok && strcmp(act, exp) == 0) passed++;
                else { failed++; fprintf(stderr, "MISMATCH external_app_link('%s'): exp '%s', got '%s'\n", arg, exp, act); }
            }
        } else if (strstr(line, "\"fn\":\"app_link_scheme\"")) {
            if (!has_arg) { failed++; continue; }
            bool act = zephium_nav_app_link_scheme(arg, strlen(arg));
            bool exp = (strstr(line, "\"result\":true") != NULL);
            if (act == exp) passed++;
            else { failed++; fprintf(stderr, "MISMATCH app_link_scheme('%s'): exp %d, got %d\n", arg, exp, act); }
        } else if (strstr(line, "\"fn\":\"app_link_outcome\"")) {
            char k_str[16] = {0}, g_str[16] = {0}, exp[16] = {0};
            if (sscanf(line, "{\"fn\":\"app_link_outcome\",\"args\":[%15[^,],%15[^]]],\"result\":\"%15[^\"]\"", k_str, g_str, exp) == 3) {
                bool k = (strcmp(k_str, "true") == 0);
                bool g = (strcmp(g_str, "true") == 0);
                zephium_app_link_outcome_t outcome = zephium_nav_app_link_outcome(k, g);
                const char *act = (outcome == ZEPHIUM_APP_LINK_ASK) ? "Ask" : (outcome == ZEPHIUM_APP_LINK_SHOW) ? "Show" : "Silent";
                if (strcmp(act, exp) == 0) passed++;
                else { failed++; fprintf(stderr, "MISMATCH app_link_outcome(%d,%d): exp %s, got %s\n", k, g, exp, act); }
            } else {
                failed++;
            }
        }
    }

    fclose(f);

    printf("\n=== ZEPHIUM NAVIGATION DIFF REPORT ===\n");
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
