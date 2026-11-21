#include <stdio.h>
#include <time.h>
#include <stdint.h>
#include "../port.h"

#define BENCH_ITERS 10000000

int main(void) {
    clock_t start = clock();
    volatile uint16_t dummy = 0;

    for (int i = 0; i < BENCH_ITERS; ++i) {
        dummy += zephium_zoom_step((uint16_t)(i % 350), (i % 2 == 0));
        dummy += zephium_zoom_percent((double)(i % 400) / 100.0);
    }

    clock_t end = clock();
    double elapsed_sec = (double)(end - start) / CLOCKS_PER_SEC;
    double ops_per_sec = (double)(BENCH_ITERS * 2) / elapsed_sec;

    printf("\n=== ZEPHIUM ZOOM BENCHMARK ===\n");
    printf("Iterations : %d (2 ops each)\n", BENCH_ITERS);
    printf("Elapsed    : %.4f seconds\n", elapsed_sec);
    printf("Throughput : %.2f million ops/sec\n", ops_per_sec / 1e6);
    printf("Result checksum: %u\n\n", dummy);

    return 0;
}
