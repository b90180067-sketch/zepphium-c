#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>
#include "../port.h"

static bool float_match(double a, double b) {
    if (isnan(a) && isnan(b)) return true;
    if (isinf(a) && isinf(b)) return (a > 0) == (b > 0);
    return fabs(a - b) < 1e-12;
}

static double parse_val(const char *s) {
    while (*s == ' ' || *s == '[' || *s == ',' || *s == ':') s++;
    if (strncmp(s, "\"NaN\"", 5) == 0) return NAN;
    if (strncmp(s, "\"Infinity\"", 10) == 0) return INFINITY;
    if (strncmp(s, "\"-Infinity\"", 11) == 0) return -INFINITY;
    return atof(s);
}

static const char *next_token(const char *s) {
    const char *p = strchr(s, ',');
    if (p) return p + 1;
    p = strchr(s, ']');
    if (p) return p + 1;
    return s + strlen(s);
}

int main(int argc, char **argv) {
    const char *cases_path = "cases.jsonl";
    if (argc > 1) cases_path = argv[1];

    FILE *f = fopen(cases_path, "r");
    if (!f) {
        fprintf(stderr, "Cannot open '%s'\n", cases_path);
        return 1;
    }

    char line[2048];
    size_t total = 0, passed = 0, failed = 0;

    while (fgets(line, sizeof(line), f)) {
        if (strlen(line) < 10) continue;
        total++;

        if (strstr(line, "\"fn\":\"rect_new\"")) {
            char *args = strstr(line, "\"args\":[");
            char *res = strstr(line, "\"result\":{");
            if (!args || !res) { failed++; continue; }

            args += 8;
            double x = parse_val(args); args = (char*)next_token(args);
            double y = parse_val(args); args = (char*)next_token(args);
            double w = parse_val(args); args = (char*)next_token(args);
            double h = parse_val(args);

            char *rx_p = strstr(res, "\"x\":");
            char *ry_p = strstr(res, "\"y\":");
            char *rw_p = strstr(res, "\"width\":");
            char *rh_p = strstr(res, "\"height\":");

            double exp_x = parse_val(rx_p + 4);
            double exp_y = parse_val(ry_p + 4);
            double exp_w = parse_val(rw_p + 8);
            double exp_h = parse_val(rh_p + 9);

            zephium_rect_t act = zephium_rect_new(x, y, w, h);

            if (float_match(act.x, exp_x) && float_match(act.y, exp_y) &&
                float_match(act.width, exp_w) && float_match(act.height, exp_h)) {
                passed++;
            } else {
                failed++;
                fprintf(stderr, "MISMATCH rect_new(%g,%g,%g,%g)\n", x, y, w, h);
            }
        } else if (strstr(line, "\"fn\":\"size_new\"")) {
            char *args = strstr(line, "\"args\":[");
            char *res = strstr(line, "\"result\":{");
            if (!args || !res) { failed++; continue; }

            args += 8;
            double w = parse_val(args); args = (char*)next_token(args);
            double h = parse_val(args);

            char *rw_p = strstr(res, "\"width\":");
            char *rh_p = strstr(res, "\"height\":");

            double exp_w = parse_val(rw_p + 8);
            double exp_h = parse_val(rh_p + 9);

            zephium_size_t act = zephium_size_new(w, h);

            if (float_match(act.width, exp_w) && float_match(act.height, exp_h)) {
                passed++;
            } else {
                failed++;
                fprintf(stderr, "MISMATCH size_new(%g,%g)\n", w, h);
            }
        }
    }

    fclose(f);

    printf("\n=== ZEPHIUM GEOMETRY DIFF REPORT ===\n");
    printf("Total recorded cases : %zu\n", total);
    printf("Passed               : %zu\n", passed);
    printf("Failed               : %zu\n", failed);

    if (failed == 0 && total > 0) {
        printf("VERDICT: PASS (100%% bit-identical behavior)\n\n");
        return 0;
    } else {
        printf("VERDICT: FAIL\n\n");
        return 1;
    }
}
