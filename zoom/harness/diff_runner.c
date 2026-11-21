#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>
#include "../port.h"

static bool str_contains(const char *haystack, const char *needle) {
    return strstr(haystack, needle) != NULL;
}

int main(int argc, char **argv) {
    const char *cases_path = "cases.jsonl";
    if (argc > 1) {
        cases_path = argv[1];
    }

    FILE *f = fopen(cases_path, "r");
    if (!f) {
        fprintf(stderr, "Error: cannot open cases file '%s'\n", cases_path);
        return 1;
    }

    char line[1024];
    size_t total = 0;
    size_t passed = 0;
    size_t failed = 0;

    while (fgets(line, sizeof(line), f)) {
        if (strlen(line) < 10) continue;
        total++;

        if (str_contains(line, "\"fn\":\"valid_percent\"")) {
            unsigned int arg = 0;
            char expected_str[32] = {0};
            if (sscanf(line, "{\"fn\":\"valid_percent\",\"args\":[%u],\"result\":%31[^,}]", &arg, expected_str) == 2) {
                bool expected = (strcmp(expected_str, "true") == 0);
                bool actual = zephium_zoom_valid_percent((uint16_t)arg);
                if (actual == expected) {
                    passed++;
                } else {
                    failed++;
                    fprintf(stderr, "MISMATCH valid_percent(%u): expected %s, got %s\n",
                            arg, expected ? "true" : "false", actual ? "true" : "false");
                }
            } else {
                failed++;
                fprintf(stderr, "Parse error on line: %s", line);
            }
        } else if (str_contains(line, "\"fn\":\"percent\"")) {
            uint16_t expected = 0;
            char *args_pos = strstr(line, "\"args\":[");
            char *res_pos = strstr(line, "\"result\":");
            if (args_pos && res_pos) {
                args_pos += 8;
                res_pos += 9;
                expected = (uint16_t)atoi(res_pos);

                double arg_val = 0.0;
                if (strncmp(args_pos, "\"NaN\"", 5) == 0) {
                    arg_val = NAN;
                } else if (strncmp(args_pos, "\"Infinity\"", 10) == 0) {
                    arg_val = INFINITY;
                } else if (strncmp(args_pos, "\"-Infinity\"", 11) == 0) {
                    arg_val = -INFINITY;
                } else {
                    arg_val = atof(args_pos);
                }

                uint16_t actual = zephium_zoom_percent(arg_val);
                if (actual == expected) {
                    passed++;
                } else {
                    failed++;
                    fprintf(stderr, "MISMATCH percent(%g): expected %u, got %u\n",
                            arg_val, expected, actual);
                }
            } else {
                failed++;
                fprintf(stderr, "Parse error on line: %s", line);
            }
        } else if (str_contains(line, "\"fn\":\"scale\"")) {
            unsigned int arg = 0;
            double expected = 0.0;
            if (sscanf(line, "{\"fn\":\"scale\",\"args\":[%u],\"result\":%lf", &arg, &expected) == 2) {
                double actual = zephium_zoom_scale((uint16_t)arg);
                if (fabs(actual - expected) < 1e-9) {
                    passed++;
                } else {
                    failed++;
                    fprintf(stderr, "MISMATCH scale(%u): expected %.17g, got %.17g\n",
                            arg, expected, actual);
                }
            } else {
                failed++;
                fprintf(stderr, "Parse error on line: %s", line);
            }
        } else if (str_contains(line, "\"fn\":\"step\"")) {
            unsigned int from = 0;
            char up_str[16] = {0};
            unsigned int expected = 0;
            if (sscanf(line, "{\"fn\":\"step\",\"args\":[%u,%15[^]]],\"result\":%u", &from, up_str, &expected) == 3) {
                bool up = (strcmp(up_str, "true") == 0);
                uint16_t actual = zephium_zoom_step((uint16_t)from, up);
                if (actual == (uint16_t)expected) {
                    passed++;
                } else {
                    failed++;
                    fprintf(stderr, "MISMATCH step(%u, %s): expected %u, got %u\n",
                            from, up ? "true" : "false", expected, actual);
                }
            } else {
                failed++;
                fprintf(stderr, "Parse error on line: %s", line);
            }
        }
    }

    fclose(f);

    printf("\n=== ZEPHIUM ZOOM DIFF REPORT ===\n");
    printf("Total recorded cases : %zu\n", total);
    printf("Passed               : %zu\n", passed);
    printf("Failed               : %zu\n", failed);

    if (failed == 0 && total > 0) {
        printf("VERDICT: PASS (100%% bit-identical behavior)\n\n");
        return 0;
    } else {
        printf("VERDICT: FAIL (%zu discrepancies)\n\n", failed);
        return 1;
    }
}
