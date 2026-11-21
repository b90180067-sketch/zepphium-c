#ifndef ZEPHIUM_BROWSER_CORE_PORT_H
#define ZEPHIUM_BROWSER_CORE_PORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../zoom/port.h"
#include "../geometry/port.h"
#include "../ids/port.h"
#include "../navigation/port.h"
#include "../blocker/port.h"
#include "../store/port.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZEPHIUM_MAX_TABS 128

typedef struct {
    zephium_id_t id;
    char current_url[ZEPHIUM_MAX_URL_BYTES];
    char title[512];
    uint16_t zoom_percent;
    bool is_loading;
    bool is_muted;
} zephium_tab_t;

typedef struct {
    zephium_store_t *store;
    zephium_tab_t tabs[ZEPHIUM_MAX_TABS];
    size_t tab_count;
    size_t active_tab_index;
    zephium_rect_t window_rect;
} zephium_browser_context_t;

/**
 * Initializes the standalone C browser core with persistent SQLite store.
 */
zephium_browser_context_t *zephium_browser_init(const char *db_path);

/**
 * Destroys the browser core, flushing tabs and closing the persistent store.
 */
void zephium_browser_free(zephium_browser_context_t *ctx);

/**
 * Creates a new tab navigating to the given URL (or "about:blank" if NULL).
 */
zephium_tab_t *zephium_browser_new_tab(zephium_browser_context_t *ctx, const char *url);

/**
 * Closes the tab at the given index.
 */
bool zephium_browser_close_tab(zephium_browser_context_t *ctx, size_t index);

/**
 * Switches the active tab to the specified index.
 */
bool zephium_browser_switch_tab(zephium_browser_context_t *ctx, size_t index);

/**
 * Returns pointer to the currently active tab, or NULL if none.
 */
zephium_tab_t *zephium_browser_get_active_tab(zephium_browser_context_t *ctx);

/**
 * Returns pointer to the tab at specified index, or NULL if out of bounds.
 */
zephium_tab_t *zephium_browser_get_tab(zephium_browser_context_t *ctx, size_t index);

/**
 * Sets the title of the tab at specified index.
 */
bool zephium_browser_set_tab_title(zephium_browser_context_t *ctx, size_t index, const char *title);

/**
 * Navigates the currently active tab using omnibox classification rules.
 * Automatically verifies security, searches, and records visit in history.
 */
bool zephium_browser_navigate(zephium_browser_context_t *ctx, const char *raw_input);

/**
 * Steps the zoom percent of the active tab (up if zoom_in is true, else down).
 */
uint16_t zephium_browser_step_zoom(zephium_browser_context_t *ctx, bool zoom_in);

/**
 * Checks whether navigation to the given URL is allowed by security policy.
 */
bool zephium_browser_is_nav_allowed(zephium_browser_context_t *ctx, const char *url);

/**
 * Generates WebKit content filter JSON for ad & tracker blocking.
 * Caller provides buffer, returns number of bytes written or 0 on error.
 */
size_t zephium_browser_generate_adblock_rules(char *out_buf, size_t out_capacity);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHIUM_BROWSER_CORE_PORT_H */
