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
 * Browsing History API
 */
bool zephium_store_record_visit(zephium_store_t *store, const char *url, const char *title, int64_t timestamp_ms);
size_t zephium_store_count_history(zephium_store_t *store);
bool zephium_store_clear_history(zephium_store_t *store);

/**
 * Downloads API
 */
bool zephium_store_record_download(zephium_store_t *store, const char *id, const char *url,
                                   const char *target_path, int64_t total_bytes);
bool zephium_store_update_download(zephium_store_t *store, const char *id,
                                   int64_t received_bytes, zephium_download_state_t state);

/**
 * Preferences API
 */
bool zephium_store_set_preference(zephium_store_t *store, const char *key, const char *value);
bool zephium_store_get_preference(zephium_store_t *store, const char *key, char *out_val, size_t val_cap);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHIUM_STORE_PORT_H */
