#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "../port.h"

int main(int argc, char **argv) {
    const char *cases_path = "cases.jsonl";
    if (argc > 1) cases_path = argv[1];

    FILE *f = fopen(cases_path, "r");
    if (!f) {
        fprintf(stderr, "Cannot open '%s'\n", cases_path);
        return 1;
    }

    zephium_browser_context_t *ctx = zephium_browser_init(":memory:");
    if (!ctx) {
        fprintf(stderr, "Failed to initialize browser context\n");
        fclose(f);
        return 1;
    }

    char line[4096];
    size_t total = 0, passed = 0, failed = 0;

    while (fgets(line, sizeof(line), f)) {
        if (strlen(line) < 10) continue;
        total++;

        bool ok = false;
        if (strstr(line, "\"action\": \"init\"")) {
            zephium_tab_t *act = zephium_browser_get_active_tab(ctx);
            if (ctx->tab_count == 1 && ctx->active_tab_index == 0 && act && strcmp(act->current_url, "about:blank") == 0) {
                ok = true;
            }
        } else if (strstr(line, "\"action\": \"new_tab\"")) {
            if (strstr(line, "\"url\": \"https://rust-lang.org\"")) {
                zephium_tab_t *tab = zephium_browser_new_tab(ctx, "https://rust-lang.org");
                if (tab && ctx->tab_count == 2 && ctx->active_tab_index == 1 && strcmp(tab->current_url, "https://rust-lang.org") == 0) {
                    ok = true;
                }
            } else if (strstr(line, "\"url\": \"github.com\"")) {
                zephium_tab_t *tab = zephium_browser_new_tab(ctx, "github.com");
                if (tab && ctx->tab_count == 3 && ctx->active_tab_index == 2 && strcmp(tab->current_url, "https://github.com/") == 0) {
                    ok = true;
                }
            }
        } else if (strstr(line, "\"action\": \"navigate\"")) {
            if (strstr(line, "\"input\": \"https://gnu.org\"")) {
                if (zephium_browser_navigate(ctx, "https://gnu.org")) {
                    zephium_tab_t *act = zephium_browser_get_active_tab(ctx);
                    if (act && strcmp(act->current_url, "https://gnu.org") == 0) {
                        ok = true;
                    }
                }
            } else if (strstr(line, "\"input\": \"c standard library\"")) {
                if (zephium_browser_navigate(ctx, "c standard library")) {
                    zephium_tab_t *act = zephium_browser_get_active_tab(ctx);
                    if (act && strcmp(act->current_url, "https://duckduckgo.com/?q=c+standard+library") == 0) ok = true;
                }
            }
        } else if (strstr(line, "\"action\": \"step_zoom\"")) {
            if (strstr(line, "\"expected_zoom\": 110") && strstr(line, "\"zoom_in\": true")) {
                uint16_t z = zephium_browser_step_zoom(ctx, true);
                if (z == 110) ok = true;
            } else if (strstr(line, "\"expected_zoom\": 125")) {
                uint16_t z = zephium_browser_step_zoom(ctx, true);
                if (z == 125) ok = true;
            } else if (strstr(line, "\"expected_zoom\": 110") && strstr(line, "\"zoom_in\": false")) {
                uint16_t z = zephium_browser_step_zoom(ctx, false);
                if (z == 110) ok = true;
            }
        } else if (strstr(line, "\"action\": \"switch_tab\"")) {
            if (zephium_browser_switch_tab(ctx, 0)) {
                zephium_tab_t *act = zephium_browser_get_active_tab(ctx);
                if (ctx->active_tab_index == 0 && act && strcmp(act->current_url, "about:blank") == 0) ok = true;
            }
        } else if (strstr(line, "\"action\": \"close_tab\"")) {
            if (zephium_browser_close_tab(ctx, 1)) {
                if (ctx->tab_count == 2 && ctx->active_tab_index == 0) ok = true;
            }
        } else if (strstr(line, "\"action\": \"set_title\"")) {
            if (zephium_browser_set_tab_title(ctx, 0, "Blank Tab")) {
                zephium_tab_t *t = zephium_browser_get_tab(ctx, 0);
                if (t && strcmp(t->title, "Blank Tab") == 0) ok = true;
            }
        } else if (strstr(line, "\"action\": \"is_nav_allowed\"")) {
            if (strstr(line, "\"url\": \"https://example.com\"")) {
                if (zephium_browser_is_nav_allowed(ctx, "https://example.com") == true) ok = true;
            } else if (strstr(line, "\"url\": \"javascript:alert(1)\"")) {
                if (zephium_browser_is_nav_allowed(ctx, "javascript:alert(1)") == false) ok = true;
            } else if (strstr(line, "\"url\": \"vbscript:msgbox\"")) {
                if (zephium_browser_is_nav_allowed(ctx, "vbscript:msgbox") == false) ok = true;
            } else if (strstr(line, "\"url\": \"data:text/html,test\"")) {
                if (zephium_browser_is_nav_allowed(ctx, "data:text/html,test") == false) ok = true;
            }
        } else if (strstr(line, "\"action\": \"generate_adblock\"")) {
            char buf[8192];
            size_t n = zephium_browser_generate_adblock_rules(buf, sizeof(buf));
            if (n > 0 && strstr(buf, "doubleclick") != NULL) {
                ok = true;
            }
        }

        if (ok) {
            passed++;
        } else {
            failed++;
            fprintf(stderr, "DIFF MISMATCH in browser_core on line: %s", line);
        }
    }

    zephium_browser_free(ctx);
    fclose(f);

    printf("Browser Core Diff Runner: %zu total, %zu passed, %zu failed\n", total, passed, failed);
    return failed == 0 ? 0 : 1;
}
