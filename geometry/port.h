#ifndef ZEPHIUM_GEOMETRY_PORT_H
#define ZEPHIUM_GEOMETRY_PORT_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Logical-pixel rectangle of the content region a view occupies.
 */
typedef struct {
    double x;
    double y;
    double width;
    double height;
} zephium_rect_t;

/**
 * Logical-pixel size of the window's inner area.
 */
typedef struct {
    double width;
    double height;
} zephium_size_t;

/**
 * Creates a new logical-pixel rectangle.
 * Width and height are clamped against 0.0: (val >= 0.0 ? val : 0.0).
 */
zephium_rect_t zephium_rect_new(double x, double y, double width, double height);

/**
 * Creates a new logical-pixel size.
 */
zephium_size_t zephium_size_new(double width, double height);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHIUM_GEOMETRY_PORT_H */
