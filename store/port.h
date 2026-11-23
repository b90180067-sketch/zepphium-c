#ifndef ZEPHIUM_STORE_PORT_H
#define ZEPHIUM_STORE_PORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zephium_store zephium_store_t;

typedef enum {
    ZEPHIUM_DOWNLOAD_IN_PROGRESS = 0,
    ZEPHIUM_DOWNLOAD_COMPLETED   = 1,
    ZEPHIUM_DOWNLOAD_FAILED      = 2,
    ZEPHIUM_DOWNLOAD_CANCELLED   = 3
} zephium_download_state_t;

/**
 * Row structs returned by the list/search functions below.
 *
 * OWNERSHIP: every string in a row, and the row array itself, is allocated by the
 * store and owned by the caller once the function returns true. Release the whole
 * result with the matching zephium_store_free_*_rows() call - never free() a
 * string field or the array directly, and never reuse a row after freeing it.
 * On failure (false) nothing is allocated: *out_rows is NULL and *out_count is 0.
 * The free functions accept NULL / 0 so "no rows" needs no special case.
 */
typedef struct {
    char *url;
    char *title;
    int64_t visit_time;   /* milliseconds since the Unix epoch */
} zephium_history_row_t;

typedef struct {
    char *id;
    char *url;
    char *title;
    int64_t created_at;   /* seconds since the Unix epoch */
} zephium_bookmark_row_t;

typedef struct {
    char *id;
    char *url;
    char *target_path;    /* empty string when unknown */
    int64_t received_bytes;
    int64_t total_bytes;  /* 0 when unknown */
    zephium_download_state_t state;
    int64_t created_at;   /* seconds since the Unix epoch */
} zephium_download_row_t;

/**
 * Opens or creates a SQLite persistent store at path (":memory:" for transient).
 * Configures WAL mode, foreign keys, and executes schema migrations.
 */
bool zephium_store_open(const char *path, zephium_store_t **out_store);

/**
 * Closes the store and releases SQLite resources.
 */
void zephium_store_close(zephium_store_t *store);

/**
 * Bookmarks API
 */
bool zephium_store_add_bookmark(zephium_store_t *store, const char *id, const char *url, const char *title);
bool zephium_store_delete_bookmark(zephium_store_t *store, const char *id);
size_t zephium_store_count_bookmarks(zephium_store_t *store);

/**
 * Lists bookmarks, newest first (created_at descending, id descending as a stable
 * tie-break), starting at `offset` and returning at most `limit` rows.
 * `limit` 0 means "no limit". Rows must be released with
 * zephium_store_free_bookmark_rows().
 */
bool zephium_store_list_bookmarks(zephium_store_t *store, size_t limit, size_t offset,
                                  zephium_bookmark_row_t **out_rows, size_t *out_count);

/**
 * Browsing History API
 */
bool zephium_store_record_visit(zephium_store_t *store, const char *url, const char *title, int64_t timestamp_ms);
size_t zephium_store_count_history(zephium_store_t *store);

/**
 * Removes every history row. Returns false only if the DELETE cannot be executed.
 */
bool zephium_store_clear_history(zephium_store_t *store);

/**
 * Lists history, most recent visit first (visit_time descending, id descending as
 * a stable tie-break), starting at `offset` and returning at most `limit` rows.
 * `limit` 0 means "no limit". Rows must be released with
 * zephium_store_free_history_rows().
 */
bool zephium_store_list_history(zephium_store_t *store, size_t limit, size_t offset,
                                zephium_history_row_t **out_rows, size_t *out_count);

/**
 * Searches history for `query` in the URL or the title, most recent first.
 * The query is matched literally: `%`, `_` and `\` have no wildcard meaning (they are
 * escaped into the LIKE pattern), and SQL metacharacters are bound as data, never
 * concatenated into the statement. `limit` 0 means "no limit".
 * Rows must be released with zephium_store_free_history_rows().
 */
bool zephium_store_search_history(zephium_store_t *store, const char *query, size_t limit,
                                  zephium_history_row_t **out_rows, size_t *out_count);

/**
 * Downloads API
 */
bool zephium_store_record_download(zephium_store_t *store, const char *id, const char *url,
                                   const char *target_path, int64_t total_bytes);
bool zephium_store_update_download(zephium_store_t *store, const char *id,
                                   int64_t received_bytes, zephium_download_state_t state);

/**
 * Updates the progress counters of an existing download. Returns false when no
 * download with that id exists (or the UPDATE fails); state is left untouched.
 */
bool zephium_store_update_download_progress(zephium_store_t *store, const char *id,
                                            int64_t received_bytes, int64_t total_bytes);

/**
 * Lists downloads, newest first (created_at descending, id descending as a stable
 * tie-break), starting at `offset` and returning at most `limit` rows.
 * `limit` 0 means "no limit". Rows must be released with
 * zephium_store_free_download_rows().
 */
bool zephium_store_list_downloads(zephium_store_t *store, size_t limit, size_t offset,
                                  zephium_download_row_t **out_rows, size_t *out_count);

/**
 * Releases a result returned by zephium_store_list_history()/search_history().
 */
void zephium_store_free_history_rows(zephium_history_row_t *rows, size_t count);

/**
 * Releases a result returned by zephium_store_list_bookmarks().
 */
void zephium_store_free_bookmark_rows(zephium_bookmark_row_t *rows, size_t count);

/**
 * Releases a result returned by zephium_store_list_downloads().
 */
void zephium_store_free_download_rows(zephium_download_row_t *rows, size_t count);

/**
 * Preferences API
 */
bool zephium_store_set_preference(zephium_store_t *store, const char *key, const char *value);
bool zephium_store_get_preference(zephium_store_t *store, const char *key, char *out_val, size_t val_cap);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHIUM_STORE_PORT_H */
