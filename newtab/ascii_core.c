/*
 * Zephium new-tab ASCII wallpaper core.
 *
 * Pure C17, freestanding: no libc, no allocation, no imports. The browser side
 * compiles this file to WebAssembly and reads the cell buffer directly; the
 * native demo links the same code so the art can be inspected in a terminal.
 *
 * A cell is one uint16_t:
 *     bits 0..7   the ASCII glyph
 *     bits 8..11  a palette slot (the tab page owns the actual colours)
 *
 * The scenes are hand-drawn rather than sampled from an image: ridges are one
 * stroke per column, land is stippled, the moon lays a broken path on the
 * water, and the aurora is a stack of drifting vertical curtains.
 */

#include <stdint.h>

#define AC_MAX_COLS 320
#define AC_MAX_ROWS 140
#define AC_MAX_CELLS (AC_MAX_COLS * AC_MAX_ROWS)

/* Palette slots. The JS side maps these to colours per scene. */
enum {
    AC_NONE = 0,
    AC_FAR = 1,
    AC_MID = 2,
    AC_NEAR = 3,
    AC_DARK = 4,
    AC_STAR = 5,
    AC_STAR_BRIGHT = 6,
    AC_LIGHT = 7,
    AC_LIGHT_DIM = 8,
    AC_CLOUD = 9,
    AC_WATER = 10,
    AC_WATER_MID = 11,
    AC_GLINT = 12,
    AC_LAND = 13,
    AC_ACCENT = 14,
    AC_ACCENT2 = 15,
    AC_FILL_FAR = 16,
    AC_FILL_MID = 17,
    AC_FILL_NEAR = 18
};

static uint16_t g_cells[AC_MAX_CELLS];
static int g_cols = 0;
static int g_rows = 0;

/* ------------------------------------------------------------------ maths */

static inline float ac_abs(float x) { return x < 0.0f ? -x : x; }
static inline float ac_minf(float a, float b) { return a < b ? a : b; }
static inline float ac_maxf(float a, float b) { return a > b ? a : b; }
static inline int ac_clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
static inline float ac_clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline float ac_floor(float x) {
    int i = (int)x;
    if ((float)i > x) --i;
    return (float)i;
}

static inline float ac_frac(float x) { return x - ac_floor(x); }

static inline float ac_sqrt(float x) {
    return x <= 0.0f ? 0.0f : __builtin_sqrtf(x);
}

/* Bhaskara-ish Taylor sine; plenty for waves, twinkle and curtain drift. */
static inline float ac_sin(float x) {
    const float PI = 3.14159265358979f;
    const float TAU = 6.28318530717959f;
    x = x - TAU * ac_floor(x / TAU);
    if (x > PI) x -= TAU;
    float x2 = x * x;
    return x * (1.0f - x2 * (1.0f / 6.0f - x2 * (1.0f / 120.0f - x2 * (1.0f / 5040.0f))));
}

static inline float ac_cos(float x) { return ac_sin(x + 1.57079632679f); }

static inline uint32_t ac_mix(uint32_t h) {
    h ^= h >> 16;
    h *= 0x7feb352dU;
    h ^= h >> 15;
    h *= 0x846ca68bU;
    h ^= h >> 16;
    return h;
}

static inline float ac_hash2(int x, int y, uint32_t seed) {
    uint32_t h = (uint32_t)x * 0x9e3779b1U ^ (uint32_t)y * 0x85ebca6bU ^ seed;
    return (float)(ac_mix(h) >> 8) * (1.0f / 16777216.0f);
}

static inline float ac_fade(float t) { return t * t * (3.0f - 2.0f * t); }

static inline float ac_noise2(float x, float y, uint32_t seed) {
    int xi = (int)ac_floor(x);
    int yi = (int)ac_floor(y);
    float fx = x - (float)xi;
    float fy = y - (float)yi;
    float a = ac_hash2(xi, yi, seed);
    float b = ac_hash2(xi + 1, yi, seed);
    float c = ac_hash2(xi, yi + 1, seed);
    float d = ac_hash2(xi + 1, yi + 1, seed);
    float u = ac_fade(fx);
    float v = ac_fade(fy);
    float top = a + (b - a) * u;
    float bot = c + (d - c) * u;
    return top + (bot - top) * v;
}

static inline float ac_fbm2(float x, float y, int octaves, uint32_t seed) {
    float sum = 0.0f;
    float amp = 0.5f;
    float norm = 0.0f;
    for (int i = 0; i < octaves; ++i) {
        sum += amp * ac_noise2(x, y, seed + (uint32_t)i * 131U);
        norm += amp;
        x = x * 2.03f + 7.13f;
        y = y * 2.01f + 3.71f;
        amp *= 0.5f;
    }
    return norm > 0.0f ? sum / norm : 0.0f;
}

/* --------------------------------------------------------------- drawing */

static inline void ac_put(int x, int y, char ch, int color) {
    if ((unsigned)x >= (unsigned)g_cols || (unsigned)y >= (unsigned)g_rows) return;
    if (color < 0) color = 0;
    if (color > 31) color = 31;
    g_cells[(unsigned)y * (unsigned)g_cols + (unsigned)x] =
        (uint16_t)(((uint16_t)color << 8) | (unsigned char)ch);
}

static inline char ac_at(int x, int y) {
    if ((unsigned)x >= (unsigned)g_cols || (unsigned)y >= (unsigned)g_rows) return ' ';
    return (char)(g_cells[(unsigned)y * (unsigned)g_cols + (unsigned)x] & 0xffU);
}

static void ac_clear(void) {
    const int n = g_cols * g_rows;
    for (int i = 0; i < n; ++i) g_cells[i] = (uint16_t)' ';
}

/* ------------------------------------------------------------------- sky */

static void ac_stars(float t, int horizon, uint32_t seed, float density, int dim, int bright) {
    for (int y = 1; y < horizon - 1; ++y) {
        for (int x = 0; x < g_cols; ++x) {
            uint32_t h = ac_mix((uint32_t)x * 0x9e3779b1U ^ (uint32_t)y * 0x85ebca6bU ^ seed);
            float r = (float)(h >> 8) * (1.0f / 16777216.0f);
            if (r >= density) continue;
            float tw = 0.55f + 0.45f * ac_sin(t * (0.55f + (float)((h >> 4) & 7U) * 0.17f) +
                                              (float)(h & 1023U) * 0.01f);
            if (tw < 0.12f) continue;
            int hot = ((h >> 12) & 31U) == 0U;
            char ch = hot ? '*' : (r < density * 0.35f ? '+' : '.');
            ac_put(x, y, ch, hot ? bright : dim);
        }
    }
}

static void ac_clouds(float t, int horizon, uint32_t seed, int col_a, int col_b) {
    /* A few drifting horizontal bands, not a noisy sky. */
    for (int y = 3; y < horizon - 2; ++y) {
        float band = ac_fbm2(0.61f, (float)y * 0.34f - t * 0.010f, 2, seed + 3U);
        if (band < 0.56f) continue;
        for (int x = 0; x < g_cols; ++x) {
            float n = ac_fbm2((float)x * 0.020f + t * 0.012f,
                              (float)y * 0.55f + 9.0f, 3, seed + 9U);
            if (n < 0.700f) continue;
            char ch = n > 0.760f ? '-' : (n > 0.730f ? '~' : '.');
            ac_put(x, y, ch, n > 0.735f ? col_b : col_a);
        }
    }
}

static void ac_moon(float cx, float cy, float r, float aspect, float t) {
    (void)t;
    float halo = r * 2.7f;
    int x0 = (int)(cx - halo * aspect) - 1;
    int x1 = (int)(cx + halo * aspect) + 1;
    int y0 = (int)(cy - halo) - 1;
    int y1 = (int)(cy + halo) + 1;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            float dx = ((float)x - cx) / aspect;
            float dy = (float)y - cy;
            float d = ac_sqrt(dx * dx + dy * dy);
            if (d <= r) {
                char ch = '#';
                if (d > r - 0.85f) {
                    ch = dx < 0.0f ? '(' : ')';
                } else {
                    float n = ac_fbm2(dx * 0.65f + 10.0f, dy * 0.90f + 3.0f, 3, 0x51U);
                    if (n > 0.620f) ch = '.';
                    else if (n < 0.360f) ch = ':';
                }
                ac_put(x, y, ch, (ch == '.' || ch == ':') ? AC_LIGHT_DIM : AC_LIGHT);
            } else if (d < halo) {
                float n = ac_hash2(x, y, 0x88U);
                float fall = 1.0f - (d - r) / (halo - r);
                if (n < 0.16f * fall * fall) ac_put(x, y, '.', AC_LIGHT_DIM);
            }
        }
    }
}

static void ac_sun(float cx, float cy, float r, float aspect, float t) {
    /* Short dotted rays, so the disc reads as a sun and not a moon. */
    for (int k = 0; k < 16; ++k) {
        float a = (float)k * 0.3926990817f + t * 0.015f;
        for (int step = 2; step <= 4; ++step) {
            int x = (int)(cx + ac_cos(a) * r * (float)step * aspect);
            int y = (int)(cy + ac_sin(a) * r * (float)step);
            if ((step + k) & 1) ac_put(x, y, (ac_abs(ac_cos(a)) > ac_abs(ac_sin(a))) ? '-' : '|', AC_LIGHT_DIM);
        }
    }
    for (int y = (int)(cy - r) - 1; y <= (int)(cy + r) + 1; ++y) {
        for (int x = (int)(cx - r * aspect) - 1; x <= (int)(cx + r * aspect) + 1; ++x) {
            float dx = ((float)x - cx) / aspect;
            float dy = (float)y - cy;
            float d = ac_sqrt(dx * dx + dy * dy);
            if (d <= r) {
                char ch = d > r - 0.8f ? '-' : '@';
                ac_put(x, y, ch, AC_LIGHT);
            } else if (d < r * 2.2f) {
                float fall = 1.0f - (d - r) / (r * 1.2f);
                if (ac_hash2(x, y, 0xA11U) < 0.10f * fall * fall) {
                    ac_put(x, y, '.', AC_LIGHT_DIM);
                }
            }
        }
    }
}

static void ac_birds(float t) {
    for (int i = 0; i < 5; ++i) {
        float x = ac_frac((t * (2.2f + (float)i * 0.35f) + (float)i * 0.17f + 0.2f)) * (float)(g_cols + 24) - 12.0f;
        float y = (float)g_rows * (0.16f + 0.035f * (float)i) +
                  2.0f * ac_sin(t * 0.7f + (float)i * 1.7f);
        int ix = (int)x;
        int iy = (int)y;
        int flap = ((int)(t * 2.5f + (float)i)) & 1;
        if (flap) {
            ac_put(ix, iy, '^', AC_DARK);
            ac_put(ix + 1, iy, '^', AC_DARK);
        } else {
            ac_put(ix, iy, '~', AC_DARK);
            ac_put(ix + 1, iy, '~', AC_DARK);
        }
    }
}

/* --------------------------------------------------------------- terrain */

static float g_ridge_tmp[AC_MAX_COLS];

typedef struct {
    float x;      /* summit, as a fraction of the width */
    float h;      /* height above the base, as a fraction of the height */
    float wl;     /* west slope, as a fraction of the width */
    float wr;     /* east slope, as a fraction of the width */
} ac_peak_t;

/* A range made of a handful of drawn peaks, not a noise field: each summit
 * falls off smoothly to the shore, and a little noise keeps the line alive. */
static void ac_make_ridge(float *h, int horizon, const ac_peak_t *peaks, int count,
                          float base, float noise_amp, uint32_t seed) {
    for (int x = 0; x < g_cols; ++x) {
        float best = 0.0f;
        for (int i = 0; i < count; ++i) {
            float px = peaks[i].x * (float)g_cols;
            float w = (x < (int)px ? peaks[i].wl : peaks[i].wr) * (float)g_cols;
            if (w < 1.0f) w = 1.0f;
            float d = ac_abs((float)x - px) / w;
            if (d >= 1.0f) continue;
            float v = peaks[i].h * (1.0f - d);
            v = v * v * (3.0f - 2.0f * v);
            if (v > best) best = v;
        }
        float n = ac_fbm2((float)x * 0.045f, 0.5f, 2, seed) - 0.5f;
        h[x] = (float)horizon - base - best * (float)g_rows - noise_amp * n;
    }
    for (int x = 0; x < g_cols; ++x) {
        float left = h[x > 0 ? x - 1 : x];
        float right = h[x < g_cols - 1 ? x + 1 : x];
        g_ridge_tmp[x] = (left + 2.0f * h[x] + right) * 0.25f;
    }
    for (int x = 0; x < g_cols; ++x) h[x] = g_ridge_tmp[x];
}

static char ac_slope_char(const float *h, int x) {
    if (x <= 0) return '_';
    if (h[x] < h[x - 1] - 0.34f) return '/';
    if (h[x] > h[x - 1] + 0.34f) return '\\';
    return '_';
}

static void ac_draw_ridge(const float *h, int horizon, int stroke, int fill,
                          int snow_color, float snow_line, int solid) {
    for (int x = 0; x < g_cols; ++x) {
        int y = (int)(h[x] + 0.5f);
        if (y < 0) y = 0;
        if (y >= horizon) y = horizon - 1;
        ac_put(x, y, ac_slope_char(h, x), stroke);

        if (solid) {
            for (int yy = y + 1; yy < horizon; ++yy) ac_put(x, yy, '#', fill);
            if (y + 1 < horizon && ac_hash2(x, y + 1, 0x51EDU) < 0.38f) {
                ac_put(x, y + 1, ':', stroke);
            }
        } else {
            for (int yy = y + 1; yy < horizon; ++yy) {
                int d = yy - y;
                float rr = ac_hash2(x, yy, 0x51EDU + (uint32_t)stroke);
                float dens = d < 2 ? 0.52f : (d < 7 ? 0.12f : 0.028f);
                if (rr < dens) ac_put(x, yy, d < 2 ? ':' : '.', fill);
            }
        }

        if (snow_color >= 0 && (float)y < snow_line) {
            ac_put(x, y, '*', snow_color);
            if (ac_hash2(x, y + 1, 0x5A0DU) < 0.42f) ac_put(x, y + 1, '^', snow_color);
        }
    }
}

static void ac_draw_reflection(const float *h, int horizon, int water, float t) {
    for (int x = 0; x < g_cols; ++x) {
        char s = ac_slope_char(h, x);
        if (s == '/') s = '\\';
        else if (s == '\\') s = '/';
        else if (s == '_') s = '-';
        int wobble = (int)(0.8f * ac_sin((float)x * 0.23f + t * 0.7f));
        int y = 2 * horizon - (int)(h[x] + 0.5f) + wobble;
        if (y <= horizon || y >= g_rows) continue;
        ac_put(x, y, s, water);
        if (ac_hash2(x, y + 1, 0x1234U) < 0.10f) ac_put(x, y + 1, '.', water);
    }
}

static void ac_water(int horizon, float t, uint32_t seed, int base, int mid, int solid) {
    /* A still plane of water, then drifting dashes and broken light on top. */
    int span = g_rows - horizon;
    if (span < 1) return;
    if (solid) {
        for (int y = horizon + 1; y < g_rows; ++y) {
            for (int x = 0; x < g_cols; ++x) ac_put(x, y, '#', base);
        }
    }
    for (int x = 0; x < g_cols; ++x) {
        if ((x & 1) == 0) ac_put(x, horizon, (x % 8 == 0) ? '~' : '-', mid);
    }

    for (int y = horizon + 1; y < g_rows; ++y) {
        float depth = (float)(y - horizon) / (float)span;
        int segs = 3 + (int)(depth * 5.0f);
        for (int k = 0; k < segs; ++k) {
            float drift = t * (0.010f + 0.006f * (float)k) * ((k & 1) ? -1.0f : 1.0f);
            float pos = ac_frac(ac_hash2(k, y, seed) + drift);
            int x0 = (int)(pos * (float)g_cols);
            int len = 3 + (int)(ac_hash2(k, y, seed + 11U) * (6.0f + depth * 8.0f));
            char ch = ((k + y) & 1) ? '-' : '~';
            int color = depth < 0.60f ? mid : (k & 1 ? mid : base);
            for (int i = 0; i < len; ++i) ac_put(x0 + i, y, ch, color);
        }
    }
}

static void ac_light_path(float lx, int horizon, float t, float spread,
                          uint32_t seed, int core, int edge) {
    for (int y = horizon + 1; y < g_rows; ++y) {
        float depth = (float)(y - horizon) / (float)(g_rows - horizon);
        float sway = 1.7f * ac_sin((float)y * 0.52f + t * 1.3f);
        float cx = lx + sway;
        float w = ((0.018f + 0.075f * depth) * (float)g_cols) + spread;
        int x0 = ac_clampi((int)(cx - w), 0, g_cols - 1);
        int x1 = ac_clampi((int)(cx + w), 0, g_cols - 1);
        for (int x = x0; x <= x1; ++x) {
            float d = ac_abs((float)x - cx) / (w > 0.0f ? w : 1.0f);
            if (d > 1.0f) continue;
            float flick = ac_hash2(x, y + (int)(t * 2.0f), seed);
            if (d < 0.30f) {
                char ch = flick < 0.20f ? '=' : ((y & 1) ? '-' : '~');
                ac_put(x, y, ch, core);
            } else if (d < 0.70f) {
                if (flick < 0.52f) ac_put(x, y, '-', edge);
            } else if (flick < 0.26f) {
                ac_put(x, y, '~', edge);
            }
        }
    }
}

static void ac_aurora(float t, int horizon) {
    /* Three distinct curtains, each a ribbon with a soft envelope. */
    for (int band = 0; band < 3; ++band) {
        float center = (band == 0 ? 0.24f : (band == 1 ? 0.53f : 0.78f)) +
                       0.018f * ac_sin(t * 0.07f + (float)band * 2.1f);
        center += 0.014f * (ac_fbm2((float)band * 3.0f, 0.5f, 2, 0x71U) - 0.5f);
        float width = (band == 1 ? 0.060f : 0.165f) +
                      0.015f * ac_sin(t * 0.11f + (float)band);
        for (int x = 0; x < g_cols; ++x) {
            float u = ((float)x / (float)g_cols - center) / width;
            float env = 1.0f / (1.0f + u * u * 4.5f);
            env *= env;
            if (env < 0.05f) continue;
            float top = (float)g_rows * (0.020f + 0.030f * (float)band) +
                        (float)g_rows * 0.045f *
                            ac_sin((float)x * 0.040f + t * 0.18f + (float)band * 1.7f);
            float len = (float)g_rows * (0.30f + 0.10f *
                          ac_fbm2((float)x * 0.02f + t * 0.02f, (float)band, 2,
                                  0x31U + (uint32_t)band));
            int y0 = (int)top;
            int y1 = (int)(top + len);
            for (int y = y0; y < y1; ++y) {
                if (y >= horizon) break;
                float v = (float)(y - y0) / (len > 0.0f ? len : 1.0f);
                float dens = env * (1.0f - v * 0.65f) *
                             (0.55f + 0.45f * ac_fbm2((float)x * 0.06f + t * 0.10f,
                                                      (float)y * 0.10f, 2,
                                                      0x99U + (uint32_t)band));
                if (dens < 0.06f) continue;
                if (ac_hash2(x, y, 0xABCU + (uint32_t)band) < dens * 0.90f) {
                    char ch = ac_hash2(x, y, 0xDEFU + (uint32_t)band) < 0.18f ? ':' : '|';
                    int color = band == 1 ? AC_ACCENT2 : AC_ACCENT;
                    ac_put(x, y, ch, color);
                }
            }
        }
    }
}

static void ac_aurora_reflection(int horizon, float t, uint32_t seed) {
    const float centers[3] = {0.24f, 0.53f, 0.78f};
    const float widths[3] = {0.165f, 0.060f, 0.165f};
    for (int y = horizon + 1; y < g_rows; ++y) {
        float depth = (float)(y - horizon) / (float)(g_rows - horizon);
        for (int x = 0; x < g_cols; ++x) {
            float u = (float)x / (float)g_cols;
            float env = 0.0f;
            int band = 0;
            for (int k = 0; k < 3; ++k) {
                float d = (u - centers[k]) / widths[k];
                float e = 1.0f / (1.0f + d * d * 3.0f);
                if (e > env) { env = e; band = k; }
            }
            float dens = 0.055f * env * (1.0f - depth * 0.65f);
            if (ac_hash2(x, y + (int)(t * 1.6f), seed + 7U) < dens) {
                ac_put(x, y, depth < 0.42f ? '|' : '-', band == 1 ? AC_ACCENT2 : AC_ACCENT);
            }
        }
    }
}

/* ------------------------------------------------------------- foreground */

/* ------------------------------------------------------------- foreground */

static void ac_pine(int cx, int base, int h, int color) {
    if (h < 5) h = 5;
    if (h > 14) h = 14;
    int top = base - h + 1;
    ac_put(cx, top, '^', color);
    for (int i = 1; i < h - 2; ++i) {
        int half = (i + 1) / 2;
        if (half < 1) half = 1;
        ac_put(cx - half, top + i, '/', color);
        ac_put(cx + half, top + i, '\\', color);
        for (int k = -half + 1; k < half; ++k) {
            if (((i + k) & 1) == 0) ac_put(cx + k, top + i, '.', color);
        }
    }
    int half = (h - 2) / 2 + 1;
    for (int k = -half; k <= half; ++k) ac_put(cx + k, base, '_', color);
    ac_put(cx, base + 1, '|', color);
}

/* A low rock in one corner, with two pines. It frames the water without
 * becoming the subject. */
static void ac_rock(int right_side, uint32_t seed) {
    int w = (int)((float)g_cols * (right_side ? 0.075f : 0.105f));
    if (w < 6) w = 6;
    int tops[AC_MAX_COLS];
    for (int i = 0; i < w; ++i) {
        int x = right_side ? g_cols - 1 - i : i;
        float u = (float)i / (float)w;
        float topf = (right_side ? 0.84f : 0.73f) +
                     (right_side ? 0.10f : 0.19f) * u +
                     0.012f * (ac_fbm2(u * 6.0f, right_side ? 3.0f : 8.0f, 3, seed) - 0.5f);
        int top = ac_clampi((int)((float)g_rows * topf), 0, g_rows - 1);
        tops[i] = top;
        ac_put(x, top, right_side ? '/' : '\\', AC_NEAR);
        for (int y = top + 1; y < g_rows; ++y) {
            float r = ac_hash2(x, y, seed + 5U);
            if (r < 0.58f) ac_put(x, y, '#', AC_FILL_NEAR);
            else if (r < 0.66f) ac_put(x, y, ':', AC_NEAR);
        }
    }
    int tree_at[2] = {w / 3, (2 * w) / 3};
    for (int k = 0; k < 2; ++k) {
        int i = tree_at[k];
        if (i < 1 || i >= w - 1) continue;
        int x = right_side ? g_cols - 1 - i : i;
        int h = 7 + (int)(ac_hash2(i, right_side, seed) * 4.0f);
        ac_pine(x, tops[i] + 2, h, AC_NEAR);
    }
}

/* ---------------------------------------------------------------- scenes */

static void scene_moonlit_bay(float t, float aspect) {
    int horizon = (int)((float)g_rows * 0.60f);
    float mx = (float)g_cols * 0.715f;
    float my = (float)g_rows * 0.205f;
    float mr = (float)g_rows * 0.067f;

    ac_stars(t, horizon, 0xA11CEU, 0.0052f, AC_STAR, AC_STAR_BRIGHT);
    ac_clouds(t, horizon, 0xC10D5U, AC_CLOUD, AC_CLOUD);
    ac_moon(mx, my, mr, aspect, t);

    static const ac_peak_t far_peaks[] = {
        {0.03f, 0.085f, 0.11f, 0.09f}, {0.20f, 0.120f, 0.10f, 0.14f},
        {0.39f, 0.100f, 0.12f, 0.09f}, {0.58f, 0.140f, 0.10f, 0.13f},
        {0.78f, 0.110f, 0.14f, 0.10f}, {0.95f, 0.090f, 0.10f, 0.08f}
    };
    static const ac_peak_t mid_peaks[] = {
        {0.12f, 0.240f, 0.13f, 0.18f}, {0.42f, 0.300f, 0.16f, 0.14f},
        {0.66f, 0.220f, 0.10f, 0.17f}, {0.88f, 0.280f, 0.15f, 0.10f}
    };
    static const ac_peak_t near_peaks[] = {
        {0.06f, 0.075f, 0.12f, 0.16f}, {0.31f, 0.095f, 0.18f, 0.12f},
        {0.57f, 0.070f, 0.11f, 0.19f}, {0.82f, 0.085f, 0.15f, 0.12f}
    };
    float far[AC_MAX_COLS], mid[AC_MAX_COLS], near[AC_MAX_COLS];
    ac_make_ridge(far, horizon, far_peaks, 6, 5.0f, 0.8f, 0x111U);
    ac_make_ridge(mid, horizon, mid_peaks, 4, 2.0f, 0.8f, 0x222U);
    ac_make_ridge(near, horizon, near_peaks, 4, 0.4f, 0.5f, 0x333U);

    ac_draw_ridge(far, horizon, AC_FAR, AC_FILL_FAR, -1, 0.0f, 1);
    ac_draw_ridge(mid, horizon, AC_MID, AC_FILL_MID, -1, 0.0f, 1);
    ac_draw_ridge(near, horizon, AC_NEAR, AC_FILL_NEAR, -1, 0.0f, 1);

    ac_water(horizon, t, 0x444U, AC_WATER, AC_WATER_MID, 1);
    ac_draw_reflection(far, horizon, AC_WATER_MID, t);
    ac_draw_reflection(mid, horizon, AC_WATER_MID, t);
    ac_light_path(mx, horizon, t, 1.5f, 0x555U, AC_GLINT, AC_ACCENT);
    ac_rock(0, 0x666U);
    ac_rock(1, 0x777U);
}

static void scene_alpine_dawn(float t, float aspect) {
    int horizon = (int)((float)g_rows * 0.66f);
    float sx = (float)g_cols * 0.66f;
    float sy = (float)g_rows * 0.28f;
    float sr = (float)g_rows * 0.065f;

    ac_stars(t, horizon, 0xBEEFU, 0.0011f, AC_STAR, AC_STAR_BRIGHT);
    ac_clouds(t, horizon, 0xD00DU, AC_CLOUD, AC_CLOUD);
    ac_sun(sx, sy, sr, aspect, t);
    ac_birds(t);

    static const ac_peak_t far_peaks[] = {
        {0.04f, 0.110f, 0.12f, 0.10f}, {0.22f, 0.150f, 0.11f, 0.15f},
        {0.44f, 0.120f, 0.14f, 0.10f}, {0.66f, 0.170f, 0.10f, 0.15f},
        {0.88f, 0.130f, 0.15f, 0.11f}
    };
    static const ac_peak_t mid_peaks[] = {
        {0.14f, 0.270f, 0.15f, 0.19f}, {0.43f, 0.340f, 0.19f, 0.15f},
        {0.72f, 0.250f, 0.13f, 0.18f}, {0.92f, 0.300f, 0.16f, 0.09f}
    };
    static const ac_peak_t inner_peaks[] = {
        {0.05f, 0.100f, 0.12f, 0.20f}, {0.32f, 0.130f, 0.20f, 0.13f},
        {0.60f, 0.105f, 0.15f, 0.17f}, {0.84f, 0.120f, 0.17f, 0.12f}
    };
    static const ac_peak_t near_peaks[] = {
        {0.10f, 0.055f, 0.18f, 0.24f}, {0.55f, 0.065f, 0.22f, 0.18f},
        {0.90f, 0.050f, 0.16f, 0.13f}
    };
    float far[AC_MAX_COLS], mid[AC_MAX_COLS], inner[AC_MAX_COLS], near[AC_MAX_COLS];
    ac_make_ridge(far, horizon, far_peaks, 5, 8.0f, 0.9f, 0x811U);
    ac_make_ridge(mid, horizon, mid_peaks, 4, 4.0f, 0.9f, 0x822U);
    ac_make_ridge(inner, horizon, inner_peaks, 4, 1.5f, 0.6f, 0x833U);
    ac_make_ridge(near, horizon, near_peaks, 3, 0.3f, 0.4f, 0x844U);

    ac_draw_ridge(far, horizon, AC_FAR, AC_FILL_FAR, AC_STAR_BRIGHT, (float)horizon * 0.50f, 0);
    ac_draw_ridge(mid, horizon, AC_MID, AC_FILL_MID, AC_STAR_BRIGHT, (float)horizon * 0.54f, 0);
    ac_draw_ridge(inner, horizon, AC_NEAR, AC_FILL_NEAR, -1, 0.0f, 0);
    ac_draw_ridge(near, horizon, AC_DARK, AC_FILL_NEAR, -1, 0.0f, 0);

    /* A low mist between the nearest ridges. */
    for (int x = 0; x < g_cols; ++x) {
        if (ac_hash2(x, 3, 0x99U) < 0.16f) ac_put(x, horizon - 2, '~', AC_CLOUD);
        if (ac_hash2(x, 5, 0x99U) < 0.09f) ac_put(x, horizon - 1, '-', AC_CLOUD);
    }

    ac_water(horizon, t, 0x855U, AC_WATER, AC_WATER_MID, 0);
    ac_draw_reflection(far, horizon, AC_WATER_MID, t);
    ac_draw_reflection(mid, horizon, AC_WATER_MID, t);
    ac_light_path(sx, horizon, t, 2.0f, 0x866U, AC_GLINT, AC_ACCENT);
}

static void scene_aurora(float t, float aspect) {
    int horizon = (int)((float)g_rows * 0.67f);
    float mx = (float)g_cols * 0.185f;
    float my = (float)g_rows * 0.16f;
    float mr = (float)g_rows * 0.034f;

    ac_stars(t, horizon, 0xCAFEU, 0.0058f, AC_STAR, AC_STAR_BRIGHT);
    ac_aurora(t, horizon);
    ac_moon(mx, my, mr, aspect, t);

    static const ac_peak_t far_peaks[] = {
        {0.05f, 0.100f, 0.12f, 0.13f}, {0.28f, 0.140f, 0.16f, 0.10f},
        {0.50f, 0.090f, 0.12f, 0.15f}, {0.72f, 0.130f, 0.15f, 0.11f},
        {0.94f, 0.100f, 0.14f, 0.06f}
    };
    static const ac_peak_t mid_peaks[] = {
        {0.13f, 0.240f, 0.16f, 0.14f}, {0.48f, 0.150f, 0.18f, 0.16f},
        {0.84f, 0.260f, 0.15f, 0.12f}
    };
    static const ac_peak_t near_peaks[] = {
        {0.08f, 0.060f, 0.15f, 0.22f}, {0.38f, 0.075f, 0.20f, 0.15f},
        {0.68f, 0.060f, 0.15f, 0.20f}, {0.92f, 0.070f, 0.18f, 0.08f}
    };
    float far[AC_MAX_COLS], mid[AC_MAX_COLS], near[AC_MAX_COLS];
    ac_make_ridge(far, horizon, far_peaks, 5, 4.0f, 0.8f, 0x911U);
    ac_make_ridge(mid, horizon, mid_peaks, 3, 1.5f, 0.7f, 0x922U);
    ac_make_ridge(near, horizon, near_peaks, 4, 0.3f, 0.4f, 0x933U);

    ac_draw_ridge(far, horizon, AC_FAR, AC_FILL_FAR, -1, 0.0f, 1);
    ac_draw_ridge(mid, horizon, AC_MID, AC_FILL_MID, -1, 0.0f, 1);
    ac_draw_ridge(near, horizon, AC_NEAR, AC_FILL_NEAR, -1, 0.0f, 1);

    ac_water(horizon, t, 0x944U, AC_WATER, AC_WATER_MID, 1);
    ac_aurora_reflection(horizon, t, 0x955U);

    /* A shooting star now and then. */
    float cyc = ac_frac(t / 12.0f) * 12.0f;
    if (cyc < 0.9f) {
        float p = cyc / 0.9f;
        int x0 = (int)((float)g_cols * 0.83f - p * (float)g_cols * 0.28f);
        int y0 = (int)((float)g_rows * 0.10f + p * (float)g_rows * 0.09f);
        for (int k = 0; k < 9; ++k) {
            char ch = k == 0 ? '*' : (k < 4 ? '-' : '.');
            ac_put(x0 + k, y0 - k / 2, ch, AC_STAR_BRIGHT);
        }
    }

    ac_rock(0, 0x966U);
    ac_rock(1, 0x977U);
}

/* ------------------------------------------------------------------ API */

int ascii_render(float t, int scene, int cols, int rows, float aspect) {
    g_cols = ac_clampi(cols, 8, AC_MAX_COLS);
    g_rows = ac_clampi(rows, 8, AC_MAX_ROWS);
    if (aspect < 0.4f) aspect = 0.4f;
    if (aspect > 4.0f) aspect = 4.0f;
    if (scene < 0) scene = 0;
    if (scene > 2) scene = 2;

    ac_clear();
    if (scene == 0) scene_moonlit_bay(t, aspect);
    else if (scene == 1) scene_alpine_dawn(t, aspect);
    else scene_aurora(t, aspect);
    return g_cols * g_rows;
}

int ascii_cols(void) { return g_cols; }
int ascii_rows(void) { return g_rows; }
const uint16_t *ascii_cells(void) { return g_cells; }
int ascii_version(void) { return 1; }

/* wasm-ld has no libc behind it; keep the few helpers the compiler may emit. */
#if defined(__wasm__)
__attribute__((used)) void *memset(void *dst, int c, unsigned long n) {
    unsigned char *d = (unsigned char *)dst;
    for (unsigned long i = 0; i < n; ++i) d[i] = (unsigned char)c;
    return dst;
}
__attribute__((used)) void *memcpy(void *dst, const void *src, unsigned long n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (unsigned long i = 0; i < n; ++i) d[i] = s[i];
    return dst;
}
#endif
