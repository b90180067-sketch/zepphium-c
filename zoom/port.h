#ifndef ZEPHIUM_ZOOM_PORT_H
#define ZEPHIUM_ZOOM_PORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZEPHIUM_ZOOM_MIN_PERCENT ((uint16_t)30)
#define ZEPHIUM_ZOOM_MAX_PERCENT ((uint16_t)300)
#define ZEPHIUM_ZOOM_MAX_SITE_LEVELS ((size_t)512)
#define ZEPHIUM_ZOOM_DEFAULT_PERCENT ((uint16_t)100)

#define ZEPHIUM_ZOOM_NUM_STEPS ((size_t)14)
#define ZEPHIUM_ZOOM_NUM_DEFAULTS ((size_t)11)

extern const uint16_t ZEPHIUM_ZOOM_STEPS[ZEPHIUM_ZOOM_NUM_STEPS];
extern const uint16_t ZEPHIUM_ZOOM_DEFAULTS[ZEPHIUM_ZOOM_NUM_DEFAULTS];

/**
 * Checks whether a zoom percentage is within the permitted range [30, 300].
 */
bool zephium_zoom_valid_percent(uint16_t percent);

/**
 * Converts a floating-point scale factor to an integer zoom percent clamped to [30, 300].
 * If scale is NaN or infinite, returns ZEPHIUM_ZOOM_DEFAULT_PERCENT (100).
 */
uint16_t zephium_zoom_percent(double scale);

/**
 * Converts an integer zoom percent to a floating-point scale factor (percent / 100.0).
 */
double zephium_zoom_scale(uint16_t percent);

/**
 * Steps the zoom percent up or down along the standard ladder.
 * If off-ladder, steps to the nearest step in that direction.
 * Clamps at boundaries (does not wrap).
 */
uint16_t zephium_zoom_step(uint16_t from, bool up);

/**
 * A zoom level saved for a specific site.
 * Memory ownership: site pointer is owned by caller unless explicitly duplicated.
 */
typedef struct {
    const char *site;
    size_t site_len;
    uint16_t percent;
    int64_t set_at;
} zephium_zoom_saved_level_t;

#ifdef __cplusplus
}
#endif

#endif /* ZEPHIUM_ZOOM_PORT_H */
