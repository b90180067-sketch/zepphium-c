#include <stdio.h>
#include <time.h>
#include "../port.h"

#define BENCH_ITERS 10000000

int main(void) {
    clock_t start = clock();
    double acc = 0.0;

    for (int i = 0; i < BENCH_ITERS; ++i) {
        zephium_rect_t r = zephium_rect_new((double)i, (double)-i, (double)(i - 500), (double)(i + 100));
        zephium_size_t s = zephium_size_new((double)i, (double)(i * 2));
        acc += r.width + s.height;
    }

    clock_t end = clock();
    double elapsed_sec = (double)(end - start) / CLOCKS_PER_SEC;
    double ops_per_sec = (double)(BENCH_ITERS * 2) / elapsed_sec;

    printf("\n=== ZEPHIUM GEOMETRY BENCHMARK ===\n");
    printf("Iterations : %d (2 ops each)\n", BENCH_ITERS);
    printf("Elapsed    : %.4f seconds\n", elapsed_sec);
    printf("Throughput : %.2f million ops/sec\n", ops_per_sec / 1e6);
    printf("Accumulator: %.2f\n\n", acc);

    return 0;
}
