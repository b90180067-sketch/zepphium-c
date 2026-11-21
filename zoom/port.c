#include "port.h"

#include <math.h>

const uint16_t ZEPHIUM_ZOOM_STEPS[ZEPHIUM_ZOOM_NUM_STEPS] = {
    30, 50, 67, 75, 80, 90, 100, 110, 125, 150, 175, 200, 250, 300
};

const uint16_t ZEPHIUM_ZOOM_DEFAULTS[ZEPHIUM_ZOOM_NUM_DEFAULTS] = {
    50, 67, 75, 80, 90, 100, 110, 125, 150, 175, 200
};

bool zephium_zoom_valid_percent(uint16_t percent) {
    return (percent >= ZEPHIUM_ZOOM_MIN_PERCENT && percent <= ZEPHIUM_ZOOM_MAX_PERCENT);
}

uint16_t zephium_zoom_percent(double scale) {
    if (!isfinite(scale)) {
        return ZEPHIUM_ZOOM_DEFAULT_PERCENT;
    }

    double scaled = scale * 100.0;
    double rounded = round(scaled);

    if (rounded < (double)ZEPHIUM_ZOOM_MIN_PERCENT) {
        rounded = (double)ZEPHIUM_ZOOM_MIN_PERCENT;
    } else if (rounded > (double)ZEPHIUM_ZOOM_MAX_PERCENT) {
        rounded = (double)ZEPHIUM_ZOOM_MAX_PERCENT;
    }

    /* SAFETY: rounded is bounded to [30.0, 300.0], fitting comfortably within uint16_t */
    return (uint16_t)rounded;
}

double zephium_zoom_scale(uint16_t percent) {
    return (double)percent / 100.0;
}

uint16_t zephium_zoom_step(uint16_t from, bool up) {
    if (up) {
        for (size_t i = 0; i < ZEPHIUM_ZOOM_NUM_STEPS; ++i) {
            if (ZEPHIUM_ZOOM_STEPS[i] > from) {
                return ZEPHIUM_ZOOM_STEPS[i];
            }
        }
        return from;
    }

    for (size_t i = ZEPHIUM_ZOOM_NUM_STEPS; i > 0; --i) {
        if (ZEPHIUM_ZOOM_STEPS[i - 1] < from) {
            return ZEPHIUM_ZOOM_STEPS[i - 1];
        }
    }
    return from;
}
