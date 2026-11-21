#include "port.h"

#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

bool zephium_store_open(const char *path, zephium_store_t **out_store) {
    if (!path || !out_store) return false;

    zephium_store_t *s = (zephium_store_t *)calloc(1, sizeof(zephium_store_t));
    if (!s) return false;

    int rc = sqlite3_open_v2(path, &s->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    if (rc != SQLITE_OK) {
        free(s);
        return false;
    }

    /* WAL + NORMAL keep the navigation path responsive: the profile usually lives
     * on a DrvFs (/mnt/c) mount, where the default rollback journal costs ~26ms
     * per commit versus ~8ms in WAL mode. Both pragmas are best-effort (an
     * in-memory database simply keeps its own journal mode). */
    (void)sqlite3_exec(s->db,
                       "PRAGMA journal_mode = WAL;"
                       "PRAGMA synchronous = NORMAL;"
                       "PRAGMA temp_store = MEMORY;",
                       NULL, NULL, NULL);

    char *err_msg = NULL;
    rc = sqlite3_exec(s->db, SCHEMA_SQL, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        if (err_msg) sqlite3_free(err_msg);
        sqlite3_close(s->db);
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
    char *err_msg = NULL;
    int rc = sqlite3_exec(store->db, "DELETE FROM history;", NULL, NULL, &err_msg);
    if (err_msg) sqlite3_free(err_msg);
    return rc == SQLITE_OK;
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
