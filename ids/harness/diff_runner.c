#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "../port.h"

static void hex_to_bytes(const char *hex, uint8_t *bytes) {
    for (size_t i = 0; i < 16; ++i) {
        unsigned int byte_val = 0;
        sscanf(hex + i * 2, "%02x", &byte_val);
        bytes[i] = (uint8_t)byte_val;
    }
}

static void bytes_to_hex(const uint8_t *bytes, char *hex) {
    for (size_t i = 0; i < 16; ++i) {
        sprintf(hex + i * 2, "%02x", bytes[i]);
    }
    hex[32] = '\0';
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

        if (strstr(line, "\"fn\":\"encode\"")) {
            char hex_arg[64] = {0};
            char exp_res[64] = {0};
            if (sscanf(line, "{\"fn\":\"encode\",\"args\":[\"%63[^\"]\"],\"result\":\"%63[^\"]\"", hex_arg, exp_res) == 2) {
                zephium_id_t id;
                hex_to_bytes(hex_arg, id.bytes);
                char act_str[32] = {0};
                if (zephium_id_to_string(&id, act_str, sizeof(act_str)) && strcmp(act_str, exp_res) == 0) {
                    passed++;
                } else {
                    failed++;
                    fprintf(stderr, "MISMATCH encode(%s): exp %s, got %s\n", hex_arg, exp_res, act_str);
                }
            } else {
                failed++;
                fprintf(stderr, "Parse error on line: %s", line);
            }
        } else if (strstr(line, "\"fn\":\"decode\"")) {
            char ulid_arg[64] = {0};
            if (strstr(line, "\"result\":null")) {
                /* Expect failure */
                if (sscanf(line, "{\"fn\":\"decode\",\"args\":[\"%63[^\"]\"]", ulid_arg) == 1) {
                    zephium_id_t id;
                    if (!zephium_id_parse(ulid_arg, strlen(ulid_arg), &id)) {
                        passed++;
                    } else {
                        failed++;
                        fprintf(stderr, "MISMATCH decode(%s): expected failure, got success\n", ulid_arg);
                    }
                } else if (strstr(line, "\"args\":[\"\"]")) {
                    /* empty string */
                    zephium_id_t id;
                    if (!zephium_id_parse("", 0, &id)) {
                        passed++;
                    } else {
                        failed++;
                        fprintf(stderr, "MISMATCH decode(\"\"): expected failure, got success\n");
                    }
                } else {
                    failed++;
                }
            } else {
                char exp_hex[64] = {0};
                if (sscanf(line, "{\"fn\":\"decode\",\"args\":[\"%63[^\"]\"],\"result\":\"%63[^\"]\"", ulid_arg, exp_hex) == 2) {
                    zephium_id_t id;
                    if (zephium_id_parse(ulid_arg, strlen(ulid_arg), &id)) {
                        char act_hex[64] = {0};
                        bytes_to_hex(id.bytes, act_hex);
                        if (strcmp(act_hex, exp_hex) == 0) {
                            passed++;
                        } else {
                            failed++;
                            fprintf(stderr, "MISMATCH decode(%s): exp %s, got %s\n", ulid_arg, exp_hex, act_hex);
                        }
                    } else {
                        failed++;
                        fprintf(stderr, "MISMATCH decode(%s): failed to parse valid ULID\n", ulid_arg);
                    }
                }
            }
        } else if (strstr(line, "\"fn\":\"candidate_after\"")) {
            char cand_ulid[64] = {0};
            char hw_ulid[64] = {0};
            char exp_res[64] = {0};

            char *args_p = strstr(line, "\"args\":[\"");
            if (!args_p) { failed++; continue; }
            args_p += 9;
            sscanf(args_p, "%63[^\"]", cand_ulid);

            zephium_id_t cand_id;
            zephium_id_parse(cand_ulid, strlen(cand_ulid), &cand_id);

            zephium_id_t hw_id;
            bool has_hw = false;
            char *comma_p = strchr(args_p, ',');
            if (comma_p) {
                comma_p++;
                while (*comma_p == ' ') comma_p++;
                if (strncmp(comma_p, "null", 4) != 0) {
                    sscanf(comma_p, "\"%63[^\"]\"", hw_ulid);
                    zephium_id_parse(hw_ulid, strlen(hw_ulid), &hw_id);
                    has_hw = true;
                }
            }

            zephium_id_t act_out;
            bool ok = zephium_id_candidate_after(&cand_id, has_hw ? &hw_id : NULL, &act_out);

            if (strstr(line, "\"result\":null")) {
                if (!ok) {
                    passed++;
                } else {
                    failed++;
                    fprintf(stderr, "MISMATCH candidate_after: expected null (exhausted), got success\n");
                }
            } else {
                char *res_p = strstr(line, "\"result\":\"");
                if (res_p) {
                    res_p += 10;
                    sscanf(res_p, "%63[^\"]", exp_res);
                    char act_res[32] = {0};
                    zephium_id_to_string(&act_out, act_res, sizeof(act_res));
                    if (ok && strcmp(act_res, exp_res) == 0) {
                        passed++;
                    } else {
                        failed++;
                        fprintf(stderr, "MISMATCH candidate_after: exp %s, got %s\n", exp_res, act_res);
                    }
                }
            }
        }
    }

    fclose(f);

    printf("\n=== ZEPHIUM IDS DIFF REPORT ===\n");
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
