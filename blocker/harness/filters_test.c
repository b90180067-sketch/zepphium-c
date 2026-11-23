/*
 * Unit tests for the portable filter-list cache module (blocker/filters.c).
 *
 * Run through CTest: ctest --test-dir build-wsl --output-on-failure
 * or directly: ./filters_test
 *
 * The fixture mirrors the shape of bnema/ublock-webkit-filters manifest.json
 * (object-valued "lists", combined part file names) without depending on the
 * network.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../filters.h"

static int g_failures;
static int g_checks;

#define CHECK(condition)                                                                  \
    do {                                                                                  \
        ++g_checks;                                                                       \
        if (!(condition)) {                                                               \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);          \
            ++g_failures;                                                                 \
        }                                                                                 \
    } while (0)

/* Representative upstream manifest: object "lists" with per-list rules_count and
 * a "combined" object with total_rules + file array. */
static const char *MANIFEST_GOOD =
    "{\n"
    "  \"version\": \"2026.11.23\",\n"
    "  \"generated_at\": \"2026-11-23T04:11:08Z\",\n"
    "  \"lists\": {\n"
    "    \"easylist\": {\"name\": \"easylist\", \"rules_count\": 124266, \"skipped_count\": 1863},\n"
    "    \"easyprivacy\": {\"name\": \"easyprivacy\", \"rules_count\": 104422, \"skipped_count\": 107},\n"
    "    \"peter-lowe\": {\"name\": \"peter-lowe\", \"rules_count\": 7114, \"skipped_count\": 0},\n"
    "    \"ublock-filters\": {\"name\": \"ublock-filters\", \"rules_count\": 2312, \"skipped_count\": 3980}\n"
    "  },\n"
    "  \"combined\": {\n"
    "    \"total_rules\": 246880,\n"
    "    \"files\": [\"combined-part1.json\", \"combined-part2.json\"]\n"
    "  }\n"
    "}\n";

static const char *MANIFEST_LISTS_ONLY =
    "{\"version\":\"1.2.3\",\"lists\":{\"easylist\":{\"rules_count\":100},"
    "\"easyprivacy\":{\"rules_count\":200}}}";

static const char *MANIFEST_EMPTY = "{}";

static const char *MANIFEST_BAD_FILES =
    "{\"version\":\"9.9.9\",\"combined\":{\"total_rules\":10,"
    "\"files\":[\"../evil.json\",\"/etc/passwd\",\"..\\\\x.json\"]}}";

/* Two small rules plus tricky string content: braces, brackets, escaped quotes
 * and regex backslashes must not confuse the scanner. */
static const char *JSON_RULES =
    "[\n"
    "  {\"trigger\":{\"url-filter\":\"^https?://([^/]+\\\\.)?ads\\\\..*\","
    "\"resource-type\":[\"image\",\"script\"]},\"action\":{\"type\":\"block\"}},\n"
    "  {\"trigger\":{\"url-filter\":\".*\",\"unless-domain\":[\"example.com\"]},"
    "\"action\":{\"type\":\"css-display-none\",\"selector\":\"[id^='ad-'], .ad\"}},\n"
    "  {\"trigger\":{\"url-filter\":\"\\\\[banner\\\\]\"},\"action\":{\"type\":\"ignore-previous-rules\"}}\n"
    "]\n";

static void test_manifest_good(void) {
    zephium_filter_manifest_t manifest;
    zephium_filters_manifest_init(&manifest);
    CHECK(zephium_filters_manifest_parse(MANIFEST_GOOD, strlen(MANIFEST_GOOD), &manifest));
    CHECK(strcmp(manifest.version, "2026.11.23") == 0);
    CHECK(manifest.declared_total_rules == 246880u);
    CHECK(manifest.combined_count == 2);
    CHECK(strcmp(manifest.combined[0].file, "combined-part1.json") == 0);
    CHECK(strcmp(manifest.combined[0].label, "part1") == 0);
    CHECK(strcmp(manifest.combined[1].file, "combined-part2.json") == 0);
    CHECK(strcmp(manifest.combined[1].label, "part2") == 0);
    /* 246880 / 2 rounded up: fits the 150000 per-filter cap. */
    CHECK(manifest.combined[0].rules_count == 123440);
    CHECK(manifest.list_count == 4);
    CHECK(strcmp(manifest.lists[0].file, "easylist.json") == 0);
    CHECK(manifest.lists[0].rules_count == 124266);
    CHECK(strcmp(manifest.lists[1].label, "easyprivacy") == 0);
    CHECK(manifest.lists[3].rules_count == 2312);
    /* parse must be deterministic when called twice on the same output */
    CHECK(zephium_filters_manifest_parse(MANIFEST_GOOD, strlen(MANIFEST_GOOD), &manifest));
    CHECK(manifest.combined_count == 2 && manifest.list_count == 4);
}

static void test_manifest_bad(void) {
    zephium_filter_manifest_t manifest;
    zephium_filters_manifest_init(&manifest);
    CHECK(!zephium_filters_manifest_parse(NULL, 0, &manifest));
    CHECK(!zephium_filters_manifest_parse("", 0, &manifest));
    CHECK(!zephium_filters_manifest_parse("not json at all", 15, &manifest));
    CHECK(!zephium_filters_manifest_parse("[]", 2, &manifest));
    CHECK(!zephium_filters_manifest_parse("null", 4, &manifest));
    CHECK(!zephium_filters_manifest_parse(MANIFEST_EMPTY, strlen(MANIFEST_EMPTY), &manifest));
    CHECK(!zephium_filters_manifest_parse("{\"version\":\"1.0\"}", 17, &manifest));
    CHECK(!zephium_filters_manifest_parse("{}", 2, &manifest));
    /* unbalanced object */
    CHECK(!zephium_filters_manifest_parse("{\"version\":\"1.0\"", 18, &manifest));
    /* A manifest whose only files are unsafe path fragments is useless: the
     * parser must drop them instead of feeding traversal sequences to the cache
     * path builder. */
    CHECK(!zephium_filters_manifest_parse(MANIFEST_BAD_FILES, strlen(MANIFEST_BAD_FILES),
                                          &manifest));
    CHECK(manifest.combined_count == 0);
}

static void test_manifest_truncated(void) {
    zephium_filter_manifest_t manifest;
    size_t full = strlen(MANIFEST_GOOD);
    /* Every proper prefix with one byte removed must be rejected: a truncated
     * download must never be treated as a valid (and therefore stale-proof)
     * manifest. */
    /* Losing the closing brace (the last byte is the trailing newline) must fail. */
    CHECK(!zephium_filters_manifest_parse(MANIFEST_GOOD, full - 2, &manifest));
    for (size_t cut = 1; cut < full; cut += 7) {
        zephium_filters_manifest_init(&manifest);
        CHECK(!zephium_filters_manifest_parse(MANIFEST_GOOD, cut, &manifest));
    }
}

static void test_manifest_lists_only(void) {
    zephium_filter_manifest_t manifest;
    zephium_filters_manifest_init(&manifest);
    CHECK(zephium_filters_manifest_parse(MANIFEST_LISTS_ONLY, strlen(MANIFEST_LISTS_ONLY),
                                         &manifest));
    CHECK(strcmp(manifest.version, "1.2.3") == 0);
    CHECK(manifest.combined_count == 0);
    CHECK(manifest.list_count == 2);
    CHECK(strcmp(manifest.lists[0].file, "easylist.json") == 0);
    CHECK(manifest.lists[0].rules_count == 100);

    /* With no combined list, selection falls back to individual lists. */
    zephium_filter_file_t selected[ZEPHIUM_FILTERS_MAX_FILTERS];
    memset(selected, 0, sizeof(selected));
    size_t count = zephium_filters_select(&manifest, true, selected,
                                          ZEPHIUM_FILTERS_MAX_FILTERS);
    CHECK(count == 2);
    CHECK(strcmp(selected[0].file, "easylist.json") == 0);
    CHECK(strcmp(selected[1].file, "easyprivacy.json") == 0);
}

static void test_select(void) {
    zephium_filter_manifest_t manifest;
    zephium_filters_manifest_init(&manifest);
    CHECK(zephium_filters_manifest_parse(MANIFEST_GOOD, strlen(MANIFEST_GOOD), &manifest));

    zephium_filter_file_t selected[ZEPHIUM_FILTERS_MAX_FILTERS];
    memset(selected, 0, sizeof(selected));

    size_t count = zephium_filters_select(&manifest, true, selected,
                                          ZEPHIUM_FILTERS_MAX_FILTERS);
    CHECK(count == 2);
    CHECK(strcmp(selected[0].file, "combined-part1.json") == 0);

    /* prefer_combined=false forces the individual lists. */
    count = zephium_filters_select(&manifest, false, selected, ZEPHIUM_FILTERS_MAX_FILTERS);
    CHECK(count == 4);
    CHECK(strcmp(selected[0].file, "easylist.json") == 0);
    CHECK(strcmp(selected[1].file, "easyprivacy.json") == 0);

    /* A caller cap smaller than the file count is honoured. */
    count = zephium_filters_select(&manifest, true, selected, 1);
    CHECK(count == 1);
    CHECK(strcmp(selected[0].file, "combined-part1.json") == 0);

    CHECK(zephium_filters_select(NULL, true, selected, 1) == 0);
    CHECK(zephium_filters_select(&manifest, true, NULL, 1) == 0);
    CHECK(zephium_filters_select(&manifest, true, selected, 0) == 0);

    /* Combined parts that cannot fit one filter each force the list fallback. */
    manifest.declared_total_rules = 900000;
    for (size_t i = 0; i < manifest.combined_count; ++i) {
        manifest.combined[i].rules_count = 450000;
    }
    count = zephium_filters_select(&manifest, true, selected, ZEPHIUM_FILTERS_MAX_FILTERS);
    CHECK(count == 4);
    CHECK(strcmp(selected[0].file, "easylist.json") == 0);
}

static void test_cache_policy(void) {
    zephium_filters_cache_state_t state;
    memset(&state, 0, sizeof(state));
    state.manifest_present = true;
    state.stamp_present = true;
    state.stamp_fetched_at = 1000000;
    state.lists_complete = true;

    const int64_t now = 1000000 + 60;
    CHECK(zephium_filters_cache_is_complete(&state));
    CHECK(zephium_filters_cache_can_reuse(&state, now));

    /* Just under 24h: still reusable. */
    CHECK(zephium_filters_cache_can_reuse(&state, 1000000 +
                                             (int64_t)ZEPHIUM_FILTERS_REFRESH_SECONDS - 1));
    /* Exactly 24h and beyond: stale. */
    CHECK(!zephium_filters_cache_can_reuse(&state, 1000000 +
                                             (int64_t)ZEPHIUM_FILTERS_REFRESH_SECONDS));

    /* Missing pieces are never reusable. */
    zephium_filters_cache_state_t incomplete = state;
    incomplete.lists_complete = false;
    CHECK(!zephium_filters_cache_is_complete(&incomplete));
    CHECK(!zephium_filters_cache_can_reuse(&incomplete, now));
    incomplete = state;
    incomplete.manifest_present = false;
    CHECK(!zephium_filters_cache_can_reuse(&incomplete, now));
    incomplete = state;
    incomplete.stamp_present = false;
    CHECK(!zephium_filters_cache_can_reuse(&incomplete, now));
    incomplete = state;
    incomplete.stamp_fetched_at = 0;
    CHECK(!zephium_filters_cache_can_reuse(&incomplete, now));

    /* Clock jumped backwards / corrupted stamp: refresh. */
    CHECK(!zephium_filters_cache_can_reuse(&state, 1000000 - 7200));
    CHECK(!zephium_filters_cache_can_reuse(NULL, now));
    CHECK(!zephium_filters_cache_is_complete(NULL));
}

static void test_stamp(void) {
    char stamp[256];
    size_t length = zephium_filters_stamp_format(stamp, sizeof(stamp), "2026.11.23", 246880,
                                                 1700000000);
    CHECK(length > 0);
    CHECK(strstr(stamp, "version=2026.11.23") != NULL);
    CHECK(strstr(stamp, "fetched_at=1700000000") != NULL);
    CHECK(strstr(stamp, "rules=246880") != NULL);

    char version[ZEPHIUM_FILTERS_VERSION_CAP];
    uint64_t rules = 0;
    int64_t fetched_at = 0;
    CHECK(zephium_filters_stamp_parse(stamp, length, version, sizeof(version), &rules,
                                      &fetched_at));
    CHECK(strcmp(version, "2026.11.23") == 0);
    CHECK(rules == 246880u);
    CHECK(fetched_at == 1700000000);

    /* Corrupt / partial stamps must fail instead of pretending to be fresh. */
    CHECK(!zephium_filters_stamp_parse(stamp, 0, version, sizeof(version), &rules, &fetched_at));
    CHECK(!zephium_filters_stamp_parse("garbage", 7, version, sizeof(version), &rules,
                                       &fetched_at));
    CHECK(!zephium_filters_stamp_parse("version=1.0\n", 13, version, sizeof(version), &rules,
                                       &fetched_at));
    CHECK(!zephium_filters_stamp_parse("version=1.0\nfetched_at=abc\n",
                                       strlen("version=1.0\nfetched_at=abc\n"), version,
                                       sizeof(version), &rules, &fetched_at));
    CHECK(!zephium_filters_stamp_parse("version=1.0\nfetched_at=-5\n",
                                       strlen("version=1.0\nfetched_at=-5\n"), version,
                                       sizeof(version), &rules, &fetched_at));

    /* Formatting rejects versions that cannot name a cache directory. */
    CHECK(zephium_filters_stamp_format(stamp, sizeof(stamp), "", 0, 1) == 0);
    CHECK(zephium_filters_stamp_format(stamp, 8, "2026.11.23", 0, 1) == 0);
    CHECK(zephium_filters_stamp_format(NULL, 8, "2026.11.23", 0, 1) == 0);
}

static void test_json_validate(void) {
    zephium_filter_json_stats_t stats;
    size_t length = strlen(JSON_RULES);
    CHECK(zephium_filters_json_validate(JSON_RULES, length, &stats));
    CHECK(stats.valid);
    CHECK(stats.top_level_rules == 3);
    CHECK(stats.bytes == length);

    CHECK(zephium_filters_json_validate("[]", 2, &stats));
    CHECK(stats.valid && stats.top_level_rules == 0);

    CHECK(zephium_filters_json_validate("[\n{}\n]", 6, &stats));
    CHECK(stats.valid && stats.top_level_rules == 1);

    /* Shape violations. */
    CHECK(!zephium_filters_json_validate("{}", 2, &stats));
    CHECK(!zephium_filters_json_validate("", 0, &stats));
    CHECK(!zephium_filters_json_validate(NULL, 0, &stats));
    CHECK(!zephium_filters_json_validate("[", 1, &stats));
    CHECK(!zephium_filters_json_validate("[{\"a\":1}", 8, &stats));
    CHECK(!zephium_filters_json_validate("[1,] ", 5, &stats));
    CHECK(!zephium_filters_json_validate("[1] x", 5, &stats));
    CHECK(!zephium_filters_json_validate("[{\"a\":\"unterminated}]",
                                         strlen("[{\"a\":\"unterminated}]"), &stats));
    CHECK(!zephium_filters_json_validate("[\"ok\"", 5, &stats));

    /* A trailing comma or a bare top-level object is caught, but the number of
     * rules is only trusted when the document is valid. */
    CHECK(zephium_filters_json_validate("[{\"a\":1},]", strlen("[{\"a\":1},]"), &stats) == false);

    /* Rule counting continues past nested arrays/objects. */
    CHECK(zephium_filters_json_validate(
        "[{\"trigger\":{\"resource-type\":[\"image\"]}},{\"action\":{\"type\":\"block\"}}]",
        strlen("[{\"trigger\":{\"resource-type\":[\"image\"]}},"
               "{\"action\":{\"type\":\"block\"}}]"),
        &stats));
    CHECK(stats.top_level_rules == 2);
}

static void test_json_file_validate(void) {
    const char *path = "filters_test_tmp.json";
    FILE *file = fopen(path, "wb");
    CHECK(file != NULL);
    if (!file) return;
    size_t length = strlen(JSON_RULES);
    CHECK(fwrite(JSON_RULES, 1, length, file) == length);
    CHECK(fclose(file) == 0);

    zephium_filter_json_stats_t stats;
    CHECK(zephium_filters_json_file_validate(path, &stats));
    CHECK(stats.valid);
    CHECK(stats.top_level_rules == 3);
    CHECK(stats.bytes == length);
    CHECK(remove(path) == 0);

    CHECK(!zephium_filters_json_file_validate(path, &stats));
    CHECK(!zephium_filters_json_file_validate(NULL, &stats));
    CHECK(!zephium_filters_json_file_validate("", &stats));

    /* Empty file, and a file with trailing garbage. */
    file = fopen(path, "wb");
    CHECK(file != NULL);
    if (file) {
        CHECK(fclose(file) == 0);
        CHECK(!zephium_filters_json_file_validate(path, &stats));
        file = fopen(path, "wb");
        if (file) {
            CHECK(fwrite("[1] junk", 1, 8, file) == 8);
            CHECK(fclose(file) == 0);
            CHECK(!zephium_filters_json_file_validate(path, &stats));
        }
        CHECK(remove(path) == 0);
    }

    CHECK(zephium_filters_rule_count_ok(0));
    CHECK(zephium_filters_rule_count_ok(ZEPHIUM_FILTERS_RULE_CAP));
    CHECK(!zephium_filters_rule_count_ok(ZEPHIUM_FILTERS_RULE_CAP + 1));
}

static void test_names_and_identifiers(void) {
    CHECK(zephium_filters_file_name_safe("easylist.json"));
    CHECK(zephium_filters_file_name_safe("combined-part1.json"));
    CHECK(!zephium_filters_file_name_safe(".hidden.json"));
    CHECK(!zephium_filters_file_name_safe("../evil.json"));
    CHECK(!zephium_filters_file_name_safe("a/b.json"));
    CHECK(!zephium_filters_file_name_safe("evil.txt"));
    CHECK(!zephium_filters_file_name_safe("a..b.json"));
    CHECK(!zephium_filters_file_name_safe(""));
    CHECK(!zephium_filters_file_name_safe(NULL));

    char label[ZEPHIUM_FILTERS_NAME_CAP];
    CHECK(zephium_filters_part_label("combined-part1.json", label, sizeof(label)) == 5);
    CHECK(strcmp(label, "part1") == 0);
    CHECK(zephium_filters_part_label("easylist.json", label, sizeof(label)) == 8);
    CHECK(strcmp(label, "easylist") == 0);
    CHECK(zephium_filters_part_label("/tmp/dir/easyprivacy.json", label, sizeof(label)) == 11);
    CHECK(strcmp(label, "easyprivacy") == 0);
    CHECK(zephium_filters_part_label("", label, sizeof(label)) == 0);
    CHECK(zephium_filters_part_label("easylist.json", label, 4) == 0);

    char identifier[ZEPHIUM_FILTERS_IDENTIFIER_CAP];
    size_t length = zephium_filters_store_identifier("combined-part1.json", "2026.11.23",
                                                     identifier, sizeof(identifier));
    CHECK(length > 0);
    CHECK(strcmp(identifier, "zephium-adblock-part1-2026.11.23") == 0);
    CHECK(strcmp(identifier, "zephium-adblock-easylist-2026.11.23") != 0);
    CHECK(zephium_filters_store_identifier("easylist.json", "2026.11.23", identifier,
                                           sizeof(identifier)) > 0);
    CHECK(strcmp(identifier, "zephium-adblock-easylist-2026.11.23") == 0);
    /* A hostile release tag cannot escape the identifier/file-name alphabet. */
    CHECK(zephium_filters_store_identifier("easylist.json", "../../etc", identifier,
                                           sizeof(identifier)) > 0);
    CHECK(strstr(identifier, "..") == NULL);
    CHECK(strchr(identifier, '/') == NULL);
    CHECK(zephium_filters_store_identifier("../evil.json", "1.0", identifier,
                                           sizeof(identifier)) == 0);
    CHECK(zephium_filters_store_identifier("easylist.json", "", identifier,
                                           sizeof(identifier)) == 0);
    CHECK(zephium_filters_store_identifier("easylist.json", "1.0", identifier, 8) == 0);

    CHECK(zephium_filters_identifier_owned("zephium-adblock-part1-2026.11.23"));
    CHECK(zephium_filters_identifier_owned("zephium-adblock-easylist-1.0"));
    CHECK(!zephium_filters_identifier_owned("zephium-adblock"));
    CHECK(!zephium_filters_identifier_owned("other-filter"));
    CHECK(!zephium_filters_identifier_owned(""));
    CHECK(!zephium_filters_identifier_owned(NULL));

    char version[ZEPHIUM_FILTERS_VERSION_CAP];
    CHECK(zephium_filters_version_sanitize("2026.11.23", version, sizeof(version)));
    CHECK(strcmp(version, "2026.11.23") == 0);
    CHECK(zephium_filters_version_sanitize("2026.11.23+build/1", version, sizeof(version)));
    CHECK(strcmp(version, "2026.11.23-build-1") == 0);
    CHECK(zephium_filters_version_sanitize("..", version, sizeof(version)));
    CHECK(strstr(version, "..") == NULL);
    CHECK(strcmp(version, "v") == 0);
    CHECK(zephium_filters_version_sanitize("", version, sizeof(version)) == false);
    CHECK(zephium_filters_version_sanitize(NULL, version, sizeof(version)) == false);
    CHECK(zephium_filters_version_sanitize(".hidden", version, sizeof(version)));
    CHECK(version[0] == 'v');
}

static void test_paths(void) {
    char path[ZEPHIUM_FILTERS_PATH_CAP];
    size_t length = zephium_filters_path_join(path, sizeof(path), "/tmp/cache", "manifest.json");
    CHECK(length == strlen("/tmp/cache/manifest.json"));
    CHECK(strcmp(path, "/tmp/cache/manifest.json") == 0);

    /* Trailing separator in the directory must not double up. */
    CHECK(zephium_filters_manifest_path(path, sizeof(path), "/tmp/cache/") > 0);
    CHECK(strcmp(path, "/tmp/cache/manifest.json") == 0);
    CHECK(zephium_filters_stamp_path(path, sizeof(path), "/tmp/cache") > 0);
    CHECK(strcmp(path, "/tmp/cache/manifest.stamp") == 0);

    CHECK(zephium_filters_list_dir_path(path, sizeof(path), "/tmp/cache", "2026.11.23") > 0);
    CHECK(strcmp(path, "/tmp/cache/v/2026.11.23") == 0);
    CHECK(zephium_filters_list_path(path, sizeof(path), "/tmp/cache", "2026.11.23",
                                    "combined-part1.json") > 0);
    CHECK(strcmp(path, "/tmp/cache/v/2026.11.23/combined-part1.json") == 0);

    /* Unsafe components and short buffers are rejected, never truncated. */
    CHECK(zephium_filters_path_join(path, sizeof(path), "/tmp/cache", "../escape.json") == 0);
    CHECK(zephium_filters_path_join(path, sizeof(path), "/tmp/cache", "a/b.json") == 0);
    CHECK(zephium_filters_path_join(path, sizeof(path), "/tmp/cache", "") == 0);
    CHECK(zephium_filters_path_join(path, sizeof(path), "", "x") == 0);
    CHECK(zephium_filters_path_join(path, sizeof(path), NULL, "x") == 0);
    CHECK(zephium_filters_path_join(path, 5, "/tmp/cache", "manifest.json") == 0);
    CHECK(zephium_filters_list_path(path, sizeof(path), "/tmp/cache", "1.0", "../evil.json") == 0);
    CHECK(zephium_filters_list_dir_path(path, sizeof(path), "/tmp/cache", "") == 0);

    CHECK(zephium_filters_tmp_path(path, sizeof(path), "/tmp/cache/manifest.json") > 0);
    CHECK(strcmp(path, "/tmp/cache/manifest.json.part") == 0);
    CHECK(zephium_filters_tmp_path(path, 4, "/tmp/cache/manifest.json") == 0);
}

int main(void) {
    test_manifest_good();
    test_manifest_bad();
    test_manifest_truncated();
    test_manifest_lists_only();
    test_select();
    test_cache_policy();
    test_stamp();
    test_json_validate();
    test_json_file_validate();
    test_names_and_identifiers();
    test_paths();

    if (g_failures == 0) {
        printf("filters_test: all %d checks passed.\n", g_checks);
        return 0;
    }
    fprintf(stderr, "filters_test: %d of %d checks FAILED.\n", g_failures, g_checks);
    return 1;
}