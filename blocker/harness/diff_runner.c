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
        if (!extract_json_arg(line, arg, sizeof(arg))) {
            failed++;
            continue;
        }

        if (strstr(line, "\"fn\":\"simplify_canonical_host_boundary\"")) {
            char act[1024] = {0};
            bool ok = zephium_blocker_simplify_canonical_host_boundary(arg, act, sizeof(act));
            if (strstr(line, "\"result\":null")) {
                if (!ok) passed++;
                else {
                    failed++;
                    fprintf(stderr, "MISMATCH simplify_canonical_host_boundary('%s'): exp null, got '%s'\n", arg, act);
                }
            } else {
                const char *res_p = strstr(line, "\"result\":\"");
                if (res_p) {
                    res_p += 10;
                    char raw_exp[1024] = {0};
                    size_t i = 0;
                    while (*res_p && !(res_p[0] == '"' && res_p[-1] != '\\') && i + 1 < sizeof(raw_exp)) {
                        raw_exp[i++] = *res_p++;
                    }
                    raw_exp[i] = '\0';
                    char exp[1024] = {0};
                    unescape_json(raw_exp, exp, sizeof(exp));

                    if (ok && strcmp(act, exp) == 0) passed++;
                    else {
                        failed++;
                        fprintf(stderr, "MISMATCH simplify_canonical_host_boundary('%s'): exp '%s', got '%s'\n", arg, exp, act);
                    }
                }
            }
        } else if (strstr(line, "\"fn\":\"split_separator_end\"")) {
            char act_end[1024] = {0};
            char act_sep[1024] = {0};
            bool ok = zephium_blocker_split_separator_end(arg, act_end, sizeof(act_end), act_sep, sizeof(act_sep));
            if (strstr(line, "\"result\":null")) {
                if (!ok) passed++;
                else {
                    failed++;
                    fprintf(stderr, "MISMATCH split_separator_end('%s'): exp null, got [%s, %s]\n", arg, act_end, act_sep);
                }
            } else {
                const char *res_p = strstr(line, "\"result\":[\"");
                if (res_p) {
                    res_p += 11;
                    char raw1[1024] = {0}, raw2[1024] = {0};
                    size_t i = 0;
                    while (*res_p && !(res_p[0] == '"' && res_p[-1] != '\\') && i + 1 < sizeof(raw1)) {
                        raw1[i++] = *res_p++;
                    }
                    raw1[i] = '\0';
                    const char *p2 = strstr(res_p, ",\"");
                    if (p2) {
                        p2 += 2;
                        i = 0;
                        while (*p2 && !(p2[0] == '"' && p2[-1] != '\\') && i + 1 < sizeof(raw2)) {
                            raw2[i++] = *p2++;
                        }
                        raw2[i] = '\0';
                    }

                    char exp1[1024] = {0}, exp2[1024] = {0};
                    unescape_json(raw1, exp1, sizeof(exp1));
                    unescape_json(raw2, exp2, sizeof(exp2));

                    if (ok && strcmp(act_end, exp1) == 0 && strcmp(act_sep, exp2) == 0) passed++;
                    else {
                        failed++;
                        fprintf(stderr, "MISMATCH split_separator_end('%s'): exp [%s, %s], got [%s, %s]\n", arg, exp1, exp2, act_end, act_sep);
                    }
                }
            }
        } else if (strstr(line, "\"fn\":\"rules_to_webkit_json\"")) {
            zephium_blocker_rule_t rule;
            rule.url_filter = arg;
            rule.resource_types = ZEPHIUM_RES_ALL;
            rule.action = ZEPHIUM_ACTION_BLOCK;
            rule.selector = NULL;

            char act[4096] = {0};
            size_t written = zephium_blocker_rules_to_webkit_json(&rule, 1, act, sizeof(act));
            const char *res_p = strstr(line, "\"result\":\"");
            if (written == 0 || !res_p) {
                failed++;
                fprintf(stderr, "MISMATCH rules_to_webkit_json('%s'): no output\n", arg);
            } else {
                res_p += 10;
                char raw[4096] = {0};
                size_t i = 0;
                while (*res_p && !(res_p[0] == '"' && res_p[-1] != '\\') && i + 1 < sizeof(raw)) {
                    raw[i++] = *res_p++;
                }
                raw[i] = '\0';
                char exp[4096] = {0};
                unescape_json(raw, exp, sizeof(exp));
                if (strcmp(act, exp) == 0) passed++;
                else {
                    failed++;
                    fprintf(stderr, "MISMATCH rules_to_webkit_json('%s'):\n  exp '%s'\n  got '%s'\n", arg, exp, act);
                }
            }
        }
    }

    fclose(f);

    printf("\n=== ZEPHIUM BLOCKER DIFF REPORT ===\n");
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
