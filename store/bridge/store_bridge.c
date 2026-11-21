#include "store_bridge.h"

bool c_zephium_store_open(const char *path, zephium_store_t **out_store) {
    return zephium_store_open(path, out_store);
}

void c_zephium_store_close(zephium_store_t *store) {
    zephium_store_close(store);
}

bool c_zephium_store_add_bookmark(zephium_store_t *store, const char *id, const char *url, const char *title) {
    return zephium_store_add_bookmark(store, id, url, title);
}

size_t c_zephium_store_count_bookmarks(zephium_store_t *store) {
    return zephium_store_count_bookmarks(store);
}
