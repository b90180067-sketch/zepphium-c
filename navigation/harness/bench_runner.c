#include <stdio.h>
#include <time.h>
#include <string.h>
#include "../port.h"

#define BENCH_ITERS 1000000

int main(void) {
    const char *inputs[] = {
        "example.com",
        "https://github.com/rust-lang/rust",
        "what is the difference between c and rust?",
        "192.168.1.1:8080/dashboard",
        "[2001:db8::1]/path",
        "file:///etc/passwd",
        "zoommtg://zoom.us/join?confno=123"
    };
    size_t num_inputs = sizeof(inputs) / sizeof(inputs[0]);

    clock_t start = clock();
    char out_buf[ZEPHIUM_MAX_URL_BYTES];
    int hits = 0;

    for (int i = 0; i < BENCH_ITERS; ++i) {
        const char *inp = inputs[(size_t)i % num_inputs];
        if (zephium_nav_classify(inp, strlen(inp), out_buf, sizeof(out_buf))) {
            hits++;
        }
        if (zephium_nav_is_query(inp, strlen(inp))) {
            hits++;
        }
    }

    clock_t end = clock();
    double elapsed_sec = (double)(end - start) / CLOCKS_PER_SEC;
    double ops_per_sec = (double)(BENCH_ITERS * 2) / elapsed_sec;

    printf("\n=== ZEPHIUM NAVIGATION BENCHMARK ===\n");
    printf("Iterations : %d (2 ops each)\n", BENCH_ITERS);
    printf("Elapsed    : %.4f seconds\n", elapsed_sec);
    printf("Throughput : %.2f million ops/sec\n", ops_per_sec / 1e6);
    printf("Hits       : %d\n\n", hits);

    return 0;
}
