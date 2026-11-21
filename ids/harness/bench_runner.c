#include <stdio.h>
#include <time.h>
#include "../port.h"

#define BENCH_ITERS 1000000

int main(void) {
    clock_t start = clock();
    char str_buf[32] = "01ARZ3NDEKTSV4RRFFQ69G5FAV";
    zephium_id_t id;
    int checksum = 0;

    for (int i = 0; i < BENCH_ITERS; ++i) {
        if (zephium_id_parse(str_buf, 26, &id)) {
            checksum += (int)id.bytes[15];
            zephium_id_to_string(&id, str_buf, sizeof(str_buf));
        }
    }

    clock_t end = clock();
    double elapsed_sec = (double)(end - start) / CLOCKS_PER_SEC;
    double ops_per_sec = (double)(BENCH_ITERS * 2) / elapsed_sec;

    printf("\n=== ZEPHIUM IDS BENCHMARK ===\n");
    printf("Iterations : %d (parse + encode)\n", BENCH_ITERS);
    printf("Elapsed    : %.4f seconds\n", elapsed_sec);
    printf("Throughput : %.2f million ops/sec\n", ops_per_sec / 1e6);
    printf("Checksum   : %d\n\n", checksum);

    return 0;
}
