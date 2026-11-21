#ifndef ZEPHIUM_STORE_BRIDGE_H
#define ZEPHIUM_STORE_BRIDGE_H

#include "../port.h"

#ifdef __cplusplus
extern "C" {
#endif

bool c_zephium_store_open(const char *path, zephium_store_t **out_store);
void c_zephium_store_close(zephium_store_t *store);
bool c_zephium_store_add_bookmark(zephium_store_t *store, const char *id, const char *url, const char *title);
size_t c_zephium_store_count_bookmarks(zephium_store_t *store);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHIUM_STORE_BRIDGE_H */
