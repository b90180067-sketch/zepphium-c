#include "port.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

zephium_browser_context_t *zephium_browser_init(const char *db_path) {
    zephium_browser_context_t *ctx = (zephium_browser_context_t *)calloc(1, sizeof(zephium_browser_context_t));
    if (!ctx) return NULL;

    const char *path = db_path ? db_path : ":memory:";
    if (!zephium_store_open(path, &ctx->store)) {
        free(ctx);
        return NULL;
    }

    ctx->window_rect = zephium_rect_new(100.0, 100.0, 1280.0, 800.0);
    zephium_browser_new_tab(ctx, "about:blank");

    return ctx;
}

void zephium_browser_free(zephium_browser_context_t *ctx) {
    if (!ctx) return;
    if (ctx->store) {
        zephium_store_close(ctx->store);
    }
    free(ctx);
}

zephium_tab_t *zephium_browser_new_tab(zephium_browser_context_t *ctx, const char *url) {
    if (!ctx || ctx->tab_count >= ZEPHIUM_MAX_TABS) return NULL;

    zephium_tab_t *tab = &ctx->tabs[ctx->tab_count];
    memset(tab, 0, sizeof(*tab));

    zephium_id_generate(&tab->id);
    tab->zoom_percent = ZEPHIUM_ZOOM_DEFAULT_PERCENT;

    const char *target = (url && *url) ? url : "about:blank";
    char destination[ZEPHIUM_MAX_URL_BYTES];
    if (zephium_nav_classify(target, strlen(target), destination, sizeof(destination))) {
        (void)zephium_strcpy_safe(tab->current_url, sizeof(tab->current_url), destination);
    } else {
        (void)zephium_strcpy_safe(tab->current_url, sizeof(tab->current_url), "about:blank");
    }

    (void)zephium_strcpy_safe(tab->title, sizeof(tab->title), "New Tab");
    ctx->active_tab_index = ctx->tab_count;
    ctx->tab_count++;

    return tab;
}

bool zephium_browser_close_tab(zephium_browser_context_t *ctx, size_t index) {
    if (!ctx || index >= ctx->tab_count || ctx->tab_count <= 1) return false;

    for (size_t i = index; i + 1 < ctx->tab_count; ++i) {
        ctx->tabs[i] = ctx->tabs[i + 1];
    }
    ctx->tab_count--;

    if (ctx->active_tab_index >= ctx->tab_count) {
        ctx->active_tab_index = ctx->tab_count - 1;
    }
    return true;
}

bool zephium_browser_switch_tab(zephium_browser_context_t *ctx, size_t index) {
    if (!ctx || index >= ctx->tab_count) return false;
    ctx->active_tab_index = index;
    return true;
}

zephium_tab_t *zephium_browser_get_active_tab(zephium_browser_context_t *ctx) {
    if (!ctx || ctx->tab_count == 0 || ctx->active_tab_index >= ctx->tab_count) return NULL;
    return &ctx->tabs[ctx->active_tab_index];
}

zephium_tab_t *zephium_browser_get_tab(zephium_browser_context_t *ctx, size_t index) {
    if (!ctx || index >= ctx->tab_count) return NULL;
    return &ctx->tabs[index];
}

bool zephium_browser_set_tab_title(zephium_browser_context_t *ctx, size_t index, const char *title) {
    if (!ctx || index >= ctx->tab_count || !title) return false;
    (void)zephium_strcpy_safe(ctx->tabs[index].title, sizeof(ctx->tabs[index].title), title);
    return true;
}

bool zephium_browser_navigate(zephium_browser_context_t *ctx, const char *raw_input) {
    if (!ctx || !raw_input || ctx->tab_count == 0) return false;

    zephium_tab_t *tab = &ctx->tabs[ctx->active_tab_index];
    char destination[ZEPHIUM_MAX_URL_BYTES];
    if (!zephium_nav_classify(raw_input, strlen(raw_input), destination, sizeof(destination))) {
        return false;
    }

    size_t dlen = strlen(destination);
    if (dlen >= sizeof(tab->current_url)) return false;
    (void)zephium_strcpy_safe(tab->current_url, sizeof(tab->current_url), destination);

    int64_t now_ms = (int64_t)time(NULL) * 1000;
    if (ctx->store) {
        zephium_store_record_visit(ctx->store, destination, tab->title[0] ? tab->title : destination, now_ms);
    }
    return true;
}

uint16_t zephium_browser_step_zoom(zephium_browser_context_t *ctx, bool zoom_in) {
    if (!ctx || ctx->tab_count == 0) return ZEPHIUM_ZOOM_DEFAULT_PERCENT;

    zephium_tab_t *tab = &ctx->tabs[ctx->active_tab_index];
    tab->zoom_percent = zephium_zoom_step(tab->zoom_percent, zoom_in);
    return tab->zoom_percent;
}

bool zephium_browser_is_nav_allowed(zephium_browser_context_t *ctx, const char *url) {
    (void)ctx;
    if (!url) return false;
    return zephium_nav_is_allowed(url, strlen(url));
}

static const zephium_blocker_rule_t DEFAULT_ADBLOCK_RULES[] = {
    {
        .url_filter = "^https?://([^/]+\\.)?doubleclick\\.net/",
        .resource_types = ZEPHIUM_RES_ALL,
        .action = ZEPHIUM_ACTION_BLOCK,
        .selector = NULL
    },
    {
        .url_filter = "^https?://([^/]+\\.)?google-analytics\\.com/",
        .resource_types = ZEPHIUM_RES_SCRIPT | ZEPHIUM_RES_PING,
        .action = ZEPHIUM_ACTION_BLOCK,
        .selector = NULL
    },
    {
        .url_filter = "^https?://([^/]+\\.)?facebook\\.com/tr/",
        .resource_types = ZEPHIUM_RES_IMAGE | ZEPHIUM_RES_PING | ZEPHIUM_RES_SCRIPT,
        .action = ZEPHIUM_ACTION_BLOCK,
        .selector = NULL
    },
    {
        .url_filter = ".*",
        .resource_types = 0,
        .action = ZEPHIUM_ACTION_CSS_DISPLAY_NONE,
        .selector = ".adsbox, .ad-container, [id^='google_ads_']"
    }
};

size_t zephium_browser_generate_adblock_rules(char *out_buf, size_t out_capacity) {
    if (!out_buf || out_capacity == 0) return 0;
    size_t count = sizeof(DEFAULT_ADBLOCK_RULES) / sizeof(DEFAULT_ADBLOCK_RULES[0]);
    return zephium_blocker_rules_to_webkit_json(DEFAULT_ADBLOCK_RULES, count, out_buf, out_capacity);
}
