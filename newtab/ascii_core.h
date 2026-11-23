#ifndef ZEPHIUM_ASCII_CORE_H
#define ZEPHIUM_ASCII_CORE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Renders one wallpaper frame. Returns the number of cells written.
 * scene: 0 = moonlit bay, 1 = alpine dawn, 2 = aurora. */
int ascii_render(float t, int scene, int cols, int rows, float aspect);
int ascii_cols(void);
int ascii_rows(void);
const unsigned short *ascii_cells(void);
int ascii_version(void);

#ifdef __cplusplus
}
#endif

#endif
