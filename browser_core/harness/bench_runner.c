#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../port.h"

int main(void) {
    zephium_browser_context_t *ctx = zephium_browser_init(":memory:");
    if (!ctx) {
        fprintf(stderr, "Failed to init ctx\n");
        return 1;
    }

    const size_t ITERS = 100000;
    clock_t start = clock();

    for (size_t i = 0; i < ITERS; ++i) {
        zephium_browser_navigate(ctx, "https://example.org/test");
        zephium_browser_step_zoom(ctx, (i % 2 == 0));
    }

    clock_t end = clock();
    double secs = (double)(end - start) / CLOCKS_PER_SEC;
    if (secs <= 0.0) secs = 0.000001;
    double ops_per_sec = (double)ITERS / secs;

    printf("Browser Core Benchmark: %zu ops in %.4f s (%.2f ops/sec)\n",
           ITERS, secs, ops_per_sec);

    zephium_browser_free(ctx);
    return 0;
}
