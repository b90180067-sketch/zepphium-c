#ifndef ZEPHIUM_GEOMETRY_BRIDGE_H
#define ZEPHIUM_GEOMETRY_BRIDGE_H

#include "../port.h"

#ifdef __cplusplus
extern "C" {
#endif

zephium_rect_t c_zephium_rect_new(double x, double y, double width, double height);
zephium_size_t c_zephium_size_new(double width, double height);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHIUM_GEOMETRY_BRIDGE_H */
