#include <stdio.h>
#include <stdlib.h>
#include "ascii_core.h"

int main(int argc, char **argv) {
    int scene = argc > 1 ? atoi(argv[1]) : 0;
    int cols = argc > 2 ? atoi(argv[2]) : 160;
    int rows = argc > 3 ? atoi(argv[3]) : 48;
    float t = argc > 4 ? (float)atof(argv[4]) : 3.0f;
    float aspect = (float)cols / (float)rows * 0.55f;

    ascii_render(t, scene, cols, rows, aspect);
    const unsigned short *cells = ascii_cells();
    for (int y = 0; y < rows; ++y) {
        for (int x = 0; x < cols; ++x) {
            putchar((int)(cells[y * cols + x] & 0xff));
        }
        putchar('\n');
    }
    return 0;
}
