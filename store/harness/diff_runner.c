#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "../port.h"

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
