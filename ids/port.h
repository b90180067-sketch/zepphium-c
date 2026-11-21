#ifndef ZEPHIUM_IDS_PORT_H
#define ZEPHIUM_IDS_PORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZEPHIUM_ID_BYTES 16
#define ZEPHIUM_ID_STRING_LEN 26

/**
 * 128-bit sortable identifier (ULID).
 * Stored in big-endian byte order matching Rust's Ulid::to_be_bytes().
 */
typedef struct {
    uint8_t bytes[ZEPHIUM_ID_BYTES];
} zephium_id_t;

typedef uint64_t zephium_window_id_t;

/**
 * Parses a 26-character Crockford Base32 string into a 128-bit ULID.
 * Returns true on success, false if invalid length, illegal characters, or 128-bit overflow.
 */
bool zephium_id_parse(const char *s, size_t len, zephium_id_t *out);

/**
 * Formats a 128-bit ULID into a 26-character uppercase Crockford Base32 string + NUL terminator.
 * out_buf must be at least 27 bytes. Returns true on success.
 */
bool zephium_id_to_string(const zephium_id_t *id, char *out_buf, size_t buf_len);

/**
 * Compares two ULIDs in big-endian byte order (monotonic order).
 * Returns < 0 if a < b, 0 if a == b, > 0 if a > b.
 */
int zephium_id_compare(const zephium_id_t *a, const zephium_id_t *b);

/**
 * Preserves a candidate ID if it is strictly greater than high_water.
 * Otherwise advances the high_water floor by exactly 1.
 * If high_water is NULL, candidate is accepted.
 * Returns true if accepted or advanced, false if 128-bit space is exhausted (overflow).
 */
bool zephium_id_candidate_after(const zephium_id_t *candidate,
                                const zephium_id_t *high_water,
                                zephium_id_t *out);

/**
 * Generates a fresh ULID with current UTC millisecond timestamp and 80-bit secure randomness.
 */
void zephium_id_generate(zephium_id_t *out);

/**
 * Extracts the 48-bit Unix timestamp in milliseconds from the ULID.
 */
uint64_t zephium_id_timestamp_ms(const zephium_id_t *id);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHIUM_IDS_PORT_H */
