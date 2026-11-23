/* umask()/chmod() below are POSIX; the feature test macros must be set before any
 * system header is pulled in (the CMake build sets them too, a direct gcc build
 * does not). */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "port.h"

#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

struct zephium_store {
    sqlite3 *db;
};

static const char SCHEMA_SQL[] =
    "PRAGMA foreign_keys = ON;"
    "CREATE TABLE IF NOT EXISTS bookmarks ("
    "  id TEXT PRIMARY KEY NOT NULL,"
    "  url TEXT NOT NULL,"
    "  title TEXT NOT NULL,"
    "  created_at INTEGER NOT NULL"
    ");"
    "CREATE TABLE IF NOT EXISTS history ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  url TEXT NOT NULL,"
    "  title TEXT NOT NULL,"
    "  visit_time INTEGER NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_history_url ON history(url);"
    "CREATE INDEX IF NOT EXISTS idx_history_time ON history(visit_time DESC);"
    "CREATE TABLE IF NOT EXISTS downloads ("
    "  id TEXT PRIMARY KEY NOT NULL,"
    "  url TEXT NOT NULL,"
    "  target_path TEXT NOT NULL,"
    "  received_bytes INTEGER NOT NULL,"
    "  total_bytes INTEGER NOT NULL,"
    "  state INTEGER NOT NULL,"
    "  created_at INTEGER NOT NULL"
    ");"
    "CREATE TABLE IF NOT EXISTS preferences ("
    "  key TEXT PRIMARY KEY NOT NULL,"
    "  value TEXT NOT NULL"
    ");";

#if !defined(_WIN32)
/* The profile holds the complete browsing history, so it belongs to this user
 * only. Windows has no POSIX modes (the per-user profile directory ACL applies
 * there); elsewhere the file is created under a 077 umask ("mode 0600") and a
 * profile written by an older build is tightened explicitly. */
static void restrict_file_mode(const char *path) {
    if (path && *path) (void)chmod(path, 0600);
}

/* -wal and -shm hold the same data as the database; sqlite creates them while the
 * pragmas and schema below run, i.e. inside the umask window, but an existing
 * pair from an older build is chmod()ed here as well. */
static void restrict_profile_files(const char *path) {
    restrict_file_mode(path);
    size_t len = strlen(path);
    char *side = (char *)malloc(len + 5);
    if (!side) return;
    memcpy(side, path, len);
    memcpy(side + len, "-wal", 5);
    restrict_file_mode(side);
    memcpy(side + len, "-shm", 5);
    restrict_file_mode(side);
    free(side);
}
#endif /* !_WIN32 */

static bool open_database(const char *path, sqlite3 **out_db) {
    int rc = sqlite3_open_v2(path, out_db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    if (rc != SQLITE_OK) return false;

    /* WAL + NORMAL keep the navigation path responsive: the profile usually lives
     * on a DrvFs (/mnt/c) mount, where the default rollback journal costs ~26ms
     * per commit versus ~8ms in WAL mode. Both pragmas are best-effort (an
     * in-memory database simply keeps its own journal mode). */
    (void)sqlite3_exec(*out_db,
                       "PRAGMA journal_mode = WAL;"
                       "PRAGMA synchronous = NORMAL;"
                       "PRAGMA temp_store = MEMORY;",
                       NULL, NULL, NULL);

    char *err_msg = NULL;
    rc = sqlite3_exec(*out_db, SCHEMA_SQL, NULL, NULL, &err_msg);
    if (err_msg) sqlite3_free(err_msg);
    return rc == SQLITE_OK;
}

bool zephium_store_open(const char *path, zephium_store_t **out_store) {
    if (!path || !out_store) return false;

    zephium_store_t *s = (zephium_store_t *)calloc(1, sizeof(zephium_store_t));
    if (!s) return false;

    bool ok;
#if defined(_WIN32)
    ok = open_database(path, &s->db);
#else
    bool on_disk = (strcmp(path, ":memory:") != 0);
    mode_t saved_umask = on_disk ? umask(0077) : 0;
    ok = open_database(path, &s->db);
    if (on_disk) {
        umask(saved_umask);
        restrict_profile_files(path);
    }
#endif

    if (!ok) {
        if (s->db) sqlite3_close(s->db);
        free(s);
        return false;
    }

    *out_store = s;
    return true;
}

void zephium_store_close(zephium_store_t *store) {
    if (!store) return;
    if (store->db) {
        sqlite3_close(store->db);
    }
    free(store);
}

bool zephium_store_add_bookmark(zephium_store_t *store, const char *id, const char *url, const char *title) {
    if (!store || !id || !url || !title) return false;

    const char *sql = "INSERT INTO bookmarks (id, url, title, created_at) VALUES (?, ?, ?, ?);";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, url, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, title, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 4, (int64_t)time(NULL));

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool zephium_store_delete_bookmark(zephium_store_t *store, const char *id) {
    if (!store || !id) return false;

    const char *sql = "DELETE FROM bookmarks WHERE id = ?;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

size_t zephium_store_count_bookmarks(zephium_store_t *store) {
    if (!store) return 0;

    const char *sql = "SELECT count(*) FROM bookmarks;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;

    size_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = (size_t)sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

bool zephium_store_record_visit(zephium_store_t *store, const char *url, const char *title, int64_t timestamp_ms) {
    if (!store || !url || !title) return false;

    const char *sql = "INSERT INTO history (url, title, visit_time) VALUES (?, ?, ?);";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, url, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, title, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 3, timestamp_ms);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

size_t zephium_store_count_history(zephium_store_t *store) {
    if (!store) return 0;

    const char *sql = "SELECT count(*) FROM history;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;

    size_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = (size_t)sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

bool zephium_store_clear_history(zephium_store_t *store) {
    if (!store) return false;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(store->db, "DELETE FROM history;", -1, &stmt, NULL) != SQLITE_OK) {
        return false;
    }
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool zephium_store_record_download(zephium_store_t *store, const char *id, const char *url,
                                   const char *target_path, int64_t total_bytes) {
    if (!store || !id || !url || !target_path) return false;

    const char *sql = "INSERT INTO downloads (id, url, target_path, received_bytes, total_bytes, state, created_at) "
                      "VALUES (?, ?, ?, 0, ?, 0, ?);";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, url, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, target_path, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 4, total_bytes);
    sqlite3_bind_int64(stmt, 5, (int64_t)time(NULL));

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool zephium_store_update_download(zephium_store_t *store, const char *id,
                                   int64_t received_bytes, zephium_download_state_t state) {
    if (!store || !id) return false;

    const char *sql = "UPDATE downloads SET received_bytes = ?, state = ? WHERE id = ?;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) != SQLITE_OK) return false;

    sqlite3_bind_int64(stmt, 1, received_bytes);
    sqlite3_bind_int(stmt, 2, (int)state);
    sqlite3_bind_text(stmt, 3, id, -1, SQLITE_STATIC);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool zephium_store_set_preference(zephium_store_t *store, const char *key, const char *value) {
    if (!store || !key || !value) return false;

    const char *sql = "INSERT INTO preferences (key, value) VALUES (?, ?) "
                      "ON CONFLICT(key) DO UPDATE SET value = excluded.value;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, value, -1, SQLITE_STATIC);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool zephium_store_get_preference(zephium_store_t *store, const char *key, char *out_val, size_t val_cap) {
    if (!store || !key || !out_val || val_cap == 0) return false;

    const char *sql = "SELECT value FROM preferences WHERE key = ?;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);

    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *text = sqlite3_column_text(stmt, 0);
        if (text) {
            size_t len = (size_t)sqlite3_column_bytes(stmt, 0);
            if (val_cap > len) {
                memcpy(out_val, text, len);
                out_val[len] = '\0';
                found = true;
            }
        }
    }

    sqlite3_finalize(stmt);
    return found;
}

/* ---------------------------------------------------------------------------
 * Listing and searching
 *
 * Every statement below is prepared once and fed through sqlite3_bind_*(): ids,
 * limits, offsets and search text are data, never SQL. Results own their strings;
 * the caller releases them with the matching zephium_store_free_*_rows().
 * ------------------------------------------------------------------------- */

/* SQLite reads a negative LIMIT as "no limit", which is how limit == 0 is exposed. */
static int64_t sql_limit(size_t limit) {
    if (limit == 0) return -1;
    uint64_t value = (uint64_t)limit;
    if (value > (uint64_t)INT64_MAX) return INT64_MAX;
    return (int64_t)value;
}

static int64_t sql_offset(size_t offset) {
    uint64_t value = (uint64_t)offset;
    if (value > (uint64_t)INT64_MAX) return INT64_MAX;
    return (int64_t)value;
}

/* Copies a TEXT column. The schema marks every listed column NOT NULL, so a NULL
 * here means the allocation failed. */
static char *dup_column(sqlite3_stmt *stmt, int column) {
    const unsigned char *text = sqlite3_column_text(stmt, column);
    if (!text) return NULL;
    size_t len = (size_t)sqlite3_column_bytes(stmt, column);
    char *copy = (char *)malloc(len + 1);
    if (!copy) return NULL;
    memcpy(copy, text, len);
    copy[len] = '\0';
    return copy;
}

void zephium_store_free_history_rows(zephium_history_row_t *rows, size_t count) {
    if (!rows) return;
    for (size_t i = 0; i < count; ++i) {
        free(rows[i].url);
        free(rows[i].title);
    }
    free(rows);
}

void zephium_store_free_bookmark_rows(zephium_bookmark_row_t *rows, size_t count) {
    if (!rows) return;
    for (size_t i = 0; i < count; ++i) {
        free(rows[i].id);
        free(rows[i].url);
        free(rows[i].title);
    }
    free(rows);
}

void zephium_store_free_download_rows(zephium_download_row_t *rows, size_t count) {
    if (!rows) return;
    for (size_t i = 0; i < count; ++i) {
        free(rows[i].id);
        free(rows[i].url);
        free(rows[i].target_path);
    }
    free(rows);
}

/* Grows *rows to hold at least (count + 1) entries. */
static bool grow_rows(void **rows, size_t *capacity, size_t count, size_t element_size) {
    if (count < *capacity) return true;
    size_t next = *capacity ? *capacity * 2 : 16;
    void *grown = realloc(*rows, next * element_size);
    if (!grown) return false;
    *rows = grown;
    *capacity = next;
    return true;
}

bool zephium_store_list_history(zephium_store_t *store, size_t limit, size_t offset,
                                zephium_history_row_t **out_rows, size_t *out_count) {
    if (!store || !out_rows || !out_count) return false;
    *out_rows = NULL;
    *out_count = 0;

    const char *sql = "SELECT url, title, visit_time FROM history "
                      "ORDER BY visit_time DESC, id DESC LIMIT ? OFFSET ?;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) != SQLITE_OK) return false;

    sqlite3_bind_int64(stmt, 1, sql_limit(limit));
    sqlite3_bind_int64(stmt, 2, sql_offset(offset));

    zephium_history_row_t *rows = NULL;
    size_t capacity = 0, count = 0;
    int rc = SQLITE_ROW;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (!grow_rows((void **)&rows, &capacity, count, sizeof(*rows))) {
            rc = SQLITE_NOMEM;
            break;
        }
        zephium_history_row_t *row = &rows[count];
        row->url = dup_column(stmt, 0);
        row->title = dup_column(stmt, 1);
        row->visit_time = sqlite3_column_int64(stmt, 2);
        if (!row->url || !row->title) {
            free(row->url);
            free(row->title);
            rc = SQLITE_NOMEM;
            break;
        }
        ++count;
    }
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        zephium_store_free_history_rows(rows, count);
        return false;
    }
    *out_rows = rows;
    *out_count = count;
    return true;
}

bool zephium_store_search_history(zephium_store_t *store, const char *query, size_t limit,
                                  zephium_history_row_t **out_rows, size_t *out_count) {
    if (!store || !query || !out_rows || !out_count) return false;
    *out_rows = NULL;
    *out_count = 0;

    /* LIKE treats % and _ as wildcards and \ as the escape character, so the query
     * is escaped to stay literal: searching for "100%" must not match "1000". */
    size_t query_len = strlen(query);
    char *pattern = (char *)malloc(query_len * 2 + 3);
    if (!pattern) return false;
    size_t p = 0;
    pattern[p++] = '%'; /* substring match */
    for (size_t i = 0; i < query_len; ++i) {
        char c = query[i];
        if (c == '\\' || c == '%' || c == '_') pattern[p++] = '\\';
        pattern[p++] = c;
    }
    pattern[p++] = '%';
    pattern[p] = '\0';

    const char *sql = "SELECT url, title, visit_time FROM history "
                      "WHERE url LIKE ? ESCAPE '\\' OR title LIKE ? ESCAPE '\\' "
                      "ORDER BY visit_time DESC, id DESC LIMIT ?;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        free(pattern);
        return false;
    }

    sqlite3_bind_text(stmt, 1, pattern, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, pattern, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, sql_limit(limit));
    free(pattern);

    zephium_history_row_t *rows = NULL;
    size_t capacity = 0, count = 0;
    int rc = SQLITE_ROW;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (!grow_rows((void **)&rows, &capacity, count, sizeof(*rows))) {
            rc = SQLITE_NOMEM;
            break;
        }
        zephium_history_row_t *row = &rows[count];
        row->url = dup_column(stmt, 0);
        row->title = dup_column(stmt, 1);
        row->visit_time = sqlite3_column_int64(stmt, 2);
        if (!row->url || !row->title) {
            free(row->url);
            free(row->title);
            rc = SQLITE_NOMEM;
            break;
        }
        ++count;
    }
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        zephium_store_free_history_rows(rows, count);
        return false;
    }
    *out_rows = rows;
    *out_count = count;
    return true;
}

bool zephium_store_list_bookmarks(zephium_store_t *store, size_t limit, size_t offset,
                                  zephium_bookmark_row_t **out_rows, size_t *out_count) {
    if (!store || !out_rows || !out_count) return false;
    *out_rows = NULL;
    *out_count = 0;

    const char *sql = "SELECT id, url, title, created_at FROM bookmarks "
                      "ORDER BY created_at DESC, id DESC LIMIT ? OFFSET ?;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) != SQLITE_OK) return false;

    sqlite3_bind_int64(stmt, 1, sql_limit(limit));
    sqlite3_bind_int64(stmt, 2, sql_offset(offset));

    zephium_bookmark_row_t *rows = NULL;
    size_t capacity = 0, count = 0;
    int rc = SQLITE_ROW;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (!grow_rows((void **)&rows, &capacity, count, sizeof(*rows))) {
            rc = SQLITE_NOMEM;
            break;
        }
        zephium_bookmark_row_t *row = &rows[count];
        row->id = dup_column(stmt, 0);
        row->url = dup_column(stmt, 1);
        row->title = dup_column(stmt, 2);
        row->created_at = sqlite3_column_int64(stmt, 3);
        if (!row->id || !row->url || !row->title) {
            free(row->id);
            free(row->url);
            free(row->title);
            rc = SQLITE_NOMEM;
            break;
        }
        ++count;
    }
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        zephium_store_free_bookmark_rows(rows, count);
        return false;
    }
    *out_rows = rows;
    *out_count = count;
    return true;
}

bool zephium_store_list_downloads(zephium_store_t *store, size_t limit, size_t offset,
                                  zephium_download_row_t **out_rows, size_t *out_count) {
    if (!store || !out_rows || !out_count) return false;
    *out_rows = NULL;
    *out_count = 0;

    const char *sql = "SELECT id, url, target_path, received_bytes, total_bytes, state, created_at "
                      "FROM downloads ORDER BY created_at DESC, id DESC LIMIT ? OFFSET ?;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) != SQLITE_OK) return false;

    sqlite3_bind_int64(stmt, 1, sql_limit(limit));
    sqlite3_bind_int64(stmt, 2, sql_offset(offset));

    zephium_download_row_t *rows = NULL;
    size_t capacity = 0, count = 0;
    int rc = SQLITE_ROW;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (!grow_rows((void **)&rows, &capacity, count, sizeof(*rows))) {
            rc = SQLITE_NOMEM;
            break;
        }
        zephium_download_row_t *row = &rows[count];
        row->id = dup_column(stmt, 0);
        row->url = dup_column(stmt, 1);
        row->target_path = dup_column(stmt, 2);
        row->received_bytes = sqlite3_column_int64(stmt, 3);
        row->total_bytes = sqlite3_column_int64(stmt, 4);
        row->state = (zephium_download_state_t)sqlite3_column_int(stmt, 5);
        row->created_at = sqlite3_column_int64(stmt, 6);
        if (!row->id || !row->url || !row->target_path) {
            free(row->id);
            free(row->url);
            free(row->target_path);
            rc = SQLITE_NOMEM;
            break;
        }
        ++count;
    }
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        zephium_store_free_download_rows(rows, count);
        return false;
    }
    *out_rows = rows;
    *out_count = count;
    return true;
}

bool zephium_store_update_download_progress(zephium_store_t *store, const char *id,
                                            int64_t received_bytes, int64_t total_bytes) {
    if (!store || !id) return false;

    const char *sql = "UPDATE downloads SET received_bytes = ?, total_bytes = ? WHERE id = ?;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) != SQLITE_OK) return false;

    sqlite3_bind_int64(stmt, 1, received_bytes);
    sqlite3_bind_int64(stmt, 2, total_bytes);
    sqlite3_bind_text(stmt, 3, id, -1, SQLITE_STATIC);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    /* No row matched: the caller passed an id that does not exist. */
    return rc == SQLITE_DONE && sqlite3_changes(store->db) == 1;
}
