#include <stdio.h>
#include <time.h>
#include "../port.h"

#define BENCH_ITERS 100000

int main(void) {
    zephium_store_t *store = NULL;
    if (!zephium_store_open(":memory:", &store)) {
        fprintf(stderr, "Failed to open store\n");
        return 1;
    }

    clock_t start = clock();
    char val_buf[64];

    for (int i = 0; i < BENCH_ITERS; ++i) {
        zephium_store_set_preference(store, "test.key", "test_value_12345");
        zephium_store_get_preference(store, "test.key", val_buf, sizeof(val_buf));
    }

    clock_t end = clock();
    double elapsed_sec = (double)(end - start) / CLOCKS_PER_SEC;
    double ops_per_sec = (double)(BENCH_ITERS * 2) / elapsed_sec;

    printf("\n=== ZEPHIUM STORE BENCHMARK ===\n");
    printf("Iterations : %d (set + get in :memory: SQLite)\n", BENCH_ITERS);
    printf("Elapsed    : %.4f seconds\n", elapsed_sec);
    printf("Throughput : %.2f thousand ops/sec\n", ops_per_sec / 1e3);
    printf("Final val  : %s\n\n", val_buf);

    zephium_store_close(store);
    return 0;
}
