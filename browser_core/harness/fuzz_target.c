#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../port.h"

int main(void) {
    zephium_browser_context_t *ctx = zephium_browser_init(":memory:");
    if (!ctx) return 1;

    /* Deterministic pseudo-random seed */
    uint32_t seed = 0x12345678;
    #define NEXT_RND() (seed = seed * 1664525u + 1013904223u)

    char fuzz_url[256];
    for (size_t i = 0; i < 10000; ++i) {
        uint32_t r = NEXT_RND();
        int op = (int)(r % 6);

        switch (op) {
            case 0: {
                snprintf(fuzz_url, sizeof(fuzz_url), "https://site%u.org/path", (r >> 8) % 100);
                zephium_browser_new_tab(ctx, fuzz_url);
                break;
            }
            case 1: {
                if (ctx->tab_count > 1) {
                    size_t idx = (size_t)((r >> 4) % ctx->tab_count);
                    zephium_browser_close_tab(ctx, idx);
                }
                break;
            }
            case 2: {
                if (ctx->tab_count > 0) {
                    size_t idx = (size_t)((r >> 4) % ctx->tab_count);
                    zephium_browser_switch_tab(ctx, idx);
                }
                break;
            }
            case 3: {
                snprintf(fuzz_url, sizeof(fuzz_url), "search term %u", (r >> 8) % 500);
                zephium_browser_navigate(ctx, fuzz_url);
                break;
            }
            case 4: {
                zephium_browser_step_zoom(ctx, (r & 1) != 0);
                break;
            }
            case 5: {
                char junk[32];
                for (size_t j = 0; j < sizeof(junk) - 1; ++j) {
                    junk[j] = (char)(32 + (NEXT_RND() % 95));
                }
                junk[sizeof(junk) - 1] = '\0';
                zephium_browser_is_nav_allowed(ctx, junk);
                break;
            }
        }
    }

    zephium_browser_free(ctx);
    printf("Browser Core Fuzz Target: 10000 randomized iterations passed\n");
    return 0;
}
