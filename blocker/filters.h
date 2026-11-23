#ifndef ZEPHIUM_BLOCKER_FILTERS_H
#define ZEPHIUM_BLOCKER_FILTERS_H

/*
 * Portable filter-list cache for WebKit content blocking.
 *
 * The WebKitGTK shell downloads precompiled WebKit content-blocker JSON from
 * bnema/ublock-webkit-filters and hands each file to WebKitUserContentFilterStore.
 * Everything that can be decided without GTK/GLib/libsoup lives here so it can be
 * unit tested on any host:
 *
 *   - parsing the upstream manifest.json (release version, per-list rule counts,
 *     combined part file names) without a JSON library,
 *   - the bounded on-disk cache layout (manifest + stamp + one directory per
 *     release version),
 *   - the 24h staleness policy and the "can the cache be reused?" decision,
 *   - minimal validation and top-level rule counting of a downloaded WebKit JSON
 *     array (streaming, so a 24MB list never has to fit in memory),
 *   - versioned WebKitUserContentFilterStore identifiers.
 *
 * The module performs no I/O of its own except for the streaming file validator
 * (plain stdio); every path comes from the caller, which keeps the GTK shell in
 * charge of XDG locations and of the store itself.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* WebKit rejects a single content filter with more than 150000 rules; the
 * upstream project therefore splits its combined list in parts that fit. */
#define ZEPHIUM_FILTERS_RULE_CAP 150000u

/* A new upstream release is looked for at most once a day. */
#define ZEPHIUM_FILTERS_REFRESH_SECONDS (24L * 60L * 60L)

/* Upper bounds. The manifest currently lists 8 individual lists and 2 combined
 * parts; anything beyond these limits is ignored instead of trusted. */
#define ZEPHIUM_FILTERS_MAX_FILES 8
#define ZEPHIUM_FILTERS_MAX_FILTERS 4
#define ZEPHIUM_FILTERS_VERSION_CAP 64
#define ZEPHIUM_FILTERS_NAME_CAP 96
#define ZEPHIUM_FILTERS_IDENTIFIER_CAP 160
/* Conventional size for caller-provided path buffers. */
#define ZEPHIUM_FILTERS_PATH_CAP 1024

/* One download/cache file: a combined part ("combined-part1.json") or an
 * individual list ("easylist.json"). File names are always reduced to
 * [A-Za-z0-9._-] before use, because they come from a downloaded document. */
typedef struct {
    char file[ZEPHIUM_FILTERS_NAME_CAP];
    /* Short identifier fragment used in the content-filter store identifier. */
    char label[ZEPHIUM_FILTERS_NAME_CAP];
    /* Rules the manifest declares for this file, 0 when unknown. For combined
     * parts this is an estimate: the manifest only publishes a total. */
    uint32_t rules_count;
} zephium_filter_file_t;

typedef struct {
    /* Release tag, e.g. "2026.11.23", sanitised to a safe file-name fragment. */
    char version[ZEPHIUM_FILTERS_VERSION_CAP];
    /* Declared rule count of the combined list (0 when not published). */
    uint64_t declared_total_rules;
    zephium_filter_file_t combined[ZEPHIUM_FILTERS_MAX_FILES];
    size_t combined_count;
    /* Individual lists in manifest order; used when the combined parts do not
     * fit the per-filter rule cap. */
    zephium_filter_file_t lists[ZEPHIUM_FILTERS_MAX_FILES];
    size_t list_count;
} zephium_filter_manifest_t;

/*
 * What the caller found on disk. The module owns the policy, the caller owns
 * the filesystem probes (stat / existence checks), so this stays portable.
 */
typedef struct {
    bool manifest_present;   /* the cached manifest.json exists and parses */
    bool stamp_present;      /* the cache stamp exists and parses */
    int64_t stamp_fetched_at;/* unix seconds when the cache was last refreshed */
    bool lists_complete;     /* every file selected from the manifest is cached */
} zephium_filters_cache_state_t;

typedef struct {
    size_t bytes;            /* bytes of JSON inspected */
    uint32_t top_level_rules;/* number of elements in the top-level array */
    bool valid;              /* balanced top-level JSON array */
} zephium_filter_json_stats_t;

/* Zeroes a manifest; always call before parsing into it. */
void zephium_filters_manifest_init(zephium_filter_manifest_t *manifest);

/*
 * Parses the upstream manifest.json. Tolerant: unknown members are ignored,
 * missing optional members are fine, and a document whose version or file list
 * cannot be recovered makes the call fail instead of producing garbage.
 * Returns false on malformed/truncated input or when no usable file list exists.
 */
bool zephium_filters_manifest_parse(const char *json, size_t json_len,
                                    zephium_filter_manifest_t *manifest);

/*
 * Picks the files to download and compile: the combined parts when each part
 * can hold at most ZEPHIUM_FILTERS_RULE_CAP rules and prefer_combined is set,
 * otherwise the individual lists (easylist/easyprivacy first) that fit the cap.
 * Writes at most out_cap entries and returns how many were written.
 */
size_t zephium_filters_select(const zephium_filter_manifest_t *manifest, bool prefer_combined,
                              zephium_filter_file_t *out, size_t out_cap);

/*
 * Cache stamp: a tiny key=value text file recording the release version, the
 * declared rule total and the unix time of the last successful refresh. Written
 * atomically by the caller (tmp file + rename) after a successful compile.
 */
size_t zephium_filters_stamp_format(char *out, size_t out_cap, const char *version,
                                    uint64_t declared_total_rules, int64_t fetched_at);
bool zephium_filters_stamp_parse(const char *text, size_t text_len,
                                 char *version_out, size_t version_cap,
                                 uint64_t *declared_total_rules_out,
                                 int64_t *fetched_at_out);

/* True when a manifest, a stamp and every selected list are cached. */
bool zephium_filters_cache_is_complete(const zephium_filters_cache_state_t *state);

/*
 * True when the cached release can be reused as-is: complete and younger than
 * ZEPHIUM_FILTERS_REFRESH_SECONDS. A caller that gets true may apply the cached
 * compiled filters and skip both the manifest check and the download.
 */
bool zephium_filters_cache_can_reuse(const zephium_filters_cache_state_t *state, int64_t now);

/*
 * Validates a WebKit content-blocker JSON array and counts its top-level rules.
 * The check is intentionally shallow (shape only): WebKit performs the real
 * schema validation when it compiles the filter.
 */
bool zephium_filters_json_validate(const char *json, size_t json_len,
                                   zephium_filter_json_stats_t *stats);

/* Same validation/counting for a file, streamed in fixed-size chunks. */
bool zephium_filters_json_file_validate(const char *path, zephium_filter_json_stats_t *stats);

/* Rule-count cap guard: false when the list cannot be one WebKit filter. */
bool zephium_filters_rule_count_ok(uint32_t rule_count);

/*
 * Path helpers. All of them follow the repo convention: the caller supplies the
 * buffer, the return value is the number of bytes written (excluding the NUL)
 * or 0 on error (buffer too small, unsafe component, empty argument).
 *
 * Layout under the caller-chosen cache directory:
 *   <cache>/manifest.json
 *   <cache>/manifest.stamp
 *   <cache>/v/<version>/<file>.json
 */
size_t zephium_filters_path_join(char *out, size_t out_cap, const char *dir, const char *name);
size_t zephium_filters_manifest_path(char *out, size_t out_cap, const char *cache_dir);
size_t zephium_filters_stamp_path(char *out, size_t out_cap, const char *cache_dir);
size_t zephium_filters_list_dir_path(char *out, size_t out_cap, const char *cache_dir,
                                     const char *version);
size_t zephium_filters_list_path(char *out, size_t out_cap, const char *cache_dir,
                                 const char *version, const char *file);
/* Appends ".part": the atomic-write temporary path for any cache file. */
size_t zephium_filters_tmp_path(char *out, size_t out_cap, const char *path);

/* A manifest file name is usable only if it is a plain *.json leaf name. */
bool zephium_filters_file_name_safe(const char *file_name);

/* Rewrites a release tag to [A-Za-z0-9._-]; false when nothing usable is left. */
bool zephium_filters_version_sanitize(const char *version, char *out, size_t out_cap);

/* "combined-part1.json" -> "part1", "easylist.json" -> "easylist". */
size_t zephium_filters_part_label(const char *file_name, char *out, size_t out_cap);

/*
 * Builds the versioned WebKitUserContentFilterStore identifier, e.g.
 * "zephium-adblock-part1-2026.11.23". Versioning is what lets WebKit reuse a
 * compiled filter instead of recompiling the list on every start.
 */
size_t zephium_filters_store_identifier(const char *file_name, const char *version,
                                        char *out, size_t out_cap);

/* True for this feature's versioned store entries, never for the legacy
 * unversioned fallback identifier "zephium-adblock". */
bool zephium_filters_identifier_owned(const char *identifier);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHIUM_BLOCKER_FILTERS_H */
