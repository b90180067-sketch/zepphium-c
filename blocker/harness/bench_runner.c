#include <stdio.h>
#include <time.h>
#include "../port.h"

#define BENCH_ITERS 1000000

int main(void) {
    const char *pattern = "^[^:]+:(//)?([^/]+\\.)?doubleclick\\.net([^A-Za-z0-9_.%-].*)?$";
    char out1[1024];
    char out2[1024];

    clock_t start = clock();
    int matches = 0;

    for (int i = 0; i < BENCH_ITERS; ++i) {
        if (zephium_blocker_simplify_canonical_host_boundary(pattern, out1, sizeof(out1))) {
            matches++;
        }
        if (zephium_blocker_split_separator_end(pattern, out1, sizeof(out1), out2, sizeof(out2))) {
            matches++;
        }
    }

    clock_t end = clock();
    double elapsed_sec = (double)(end - start) / CLOCKS_PER_SEC;
    double ops_per_sec = (double)(BENCH_ITERS * 2) / elapsed_sec;

    printf("\n=== ZEPHIUM BLOCKER BENCHMARK ===\n");
    printf("Iterations : %d (2 ops each)\n", BENCH_ITERS);
    printf("Elapsed    : %.4f seconds\n", elapsed_sec);
    printf("Throughput : %.2f million ops/sec\n", ops_per_sec / 1e6);
    printf("Matches    : %d\n\n", matches);

    return 0;
}
