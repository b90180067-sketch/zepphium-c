#include "port.h"

#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#endif

__extension__ typedef unsigned __int128 uint128_t;

static const char CROCKFORD_ALPHABET[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

static int8_t decode_char(char c) {
    if (c >= '0' && c <= '9') return (int8_t)(c - '0');
    switch (c) {
        case 'O': case 'o': return 0;
        case 'I': case 'i':
        case 'L': case 'l': return 1;
        case 'A': case 'a': return 10;
        case 'B': case 'b': return 11;
        case 'C': case 'c': return 12;
        case 'D': case 'd': return 13;
        case 'E': case 'e': return 14;
        case 'F': case 'f': return 15;
        case 'G': case 'g': return 16;
        case 'H': case 'h': return 17;
        case 'J': case 'j': return 18;
        case 'K': case 'k': return 19;
        case 'M': case 'm': return 20;
        case 'N': case 'n': return 21;
        case 'P': case 'p': return 22;
        case 'Q': case 'q': return 23;
        case 'R': case 'r': return 24;
        case 'S': case 's': return 25;
        case 'T': case 't': return 26;
        case 'V': case 'v': return 27;
        case 'W': case 'w': return 28;
        case 'X': case 'x': return 29;
        case 'Y': case 'y': return 30;
        case 'Z': case 'z': return 31;
        default: return -1;
    }
}

bool zephium_id_parse(const char *s, size_t len, zephium_id_t *out) {
    if (!s || len != ZEPHIUM_ID_STRING_LEN || !out) {
        return false;
    }

    /* 26 chars * 5 bits = 130 bits. Char 0 can only use 3 bits (0..7) */
    int8_t first = decode_char(s[0]);
    if (first < 0 || first > 7) {
        return false;
    }

    uint128_t val = (uint128_t)(uint8_t)first;

    for (size_t i = 1; i < ZEPHIUM_ID_STRING_LEN; ++i) {
        int8_t digit = decode_char(s[i]);
        if (digit < 0) {
            return false;
        }
        val = (val << 5) | (uint128_t)(uint8_t)digit;
    }

    for (size_t i = 0; i < ZEPHIUM_ID_BYTES; ++i) {
        size_t shift = (ZEPHIUM_ID_BYTES - 1 - i) * 8;
        out->bytes[i] = (uint8_t)((val >> shift) & 0xFF);
    }

    return true;
}

bool zephium_id_to_string(const zephium_id_t *id, char *out_buf, size_t buf_len) {
    if (!id || !out_buf || buf_len < ZEPHIUM_ID_STRING_LEN + 1) {
        return false;
    }

    uint128_t val = 0;
    for (size_t i = 0; i < ZEPHIUM_ID_BYTES; ++i) {
        val = (val << 8) | (uint128_t)id->bytes[i];
    }

    for (size_t i = 0; i < ZEPHIUM_ID_STRING_LEN; ++i) {
        size_t shift = (ZEPHIUM_ID_STRING_LEN - 1 - i) * 5;
        uint8_t index = (uint8_t)((val >> shift) & 0x1F);
        out_buf[i] = CROCKFORD_ALPHABET[index];
    }
    out_buf[ZEPHIUM_ID_STRING_LEN] = '\0';

    return true;
}

int zephium_id_compare(const zephium_id_t *a, const zephium_id_t *b) {
    return memcmp(a->bytes, b->bytes, ZEPHIUM_ID_BYTES);
}

bool zephium_id_candidate_after(const zephium_id_t *candidate,
                                const zephium_id_t *high_water,
                                zephium_id_t *out) {
    if (!candidate || !out) {
        return false;
    }

    if (!high_water) {
        *out = *candidate;
        return true;
    }

    if (zephium_id_compare(candidate, high_water) > 0) {
        *out = *candidate;
        return true;
    }

    /* Advance high_water by 1 in big-endian 128-bit arithmetic */
    *out = *high_water;
    uint32_t carry = 1;
    for (size_t i = ZEPHIUM_ID_BYTES; i > 0; --i) {
        uint32_t sum = (uint32_t)out->bytes[i - 1] + carry;
        out->bytes[i - 1] = (uint8_t)(sum & 0xFF);
        carry = sum >> 8;
        if (carry == 0) break;
    }

    /* Exhaustion: carry past the most significant byte */
    return (carry == 0);
}

uint64_t zephium_id_timestamp_ms(const zephium_id_t *id) {
    if (!id) return 0;
    uint64_t ts = 0;
    for (size_t i = 0; i < 6; ++i) {
        ts = (ts << 8) | (uint64_t)id->bytes[i];
    }
    return ts;
}

void zephium_id_generate(zephium_id_t *out) {
    if (!out) return;

    uint64_t now_ms = 0;
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER uli;
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    /* 100-nanosecond intervals since Jan 1 1601 to Unix epoch ms */
    now_ms = (uli.QuadPart - 116444736000000000ULL) / 10000ULL;
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    now_ms = ((uint64_t)tv.tv_sec * 1000ULL) + ((uint64_t)tv.tv_usec / 1000ULL);
#endif

    for (size_t i = 0; i < 6; ++i) {
        out->bytes[i] = (uint8_t)((now_ms >> ((5 - i) * 8)) & 0xFF);
    }

#ifdef _WIN32
    BCryptGenRandom(NULL, &out->bytes[6], 10, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
#else
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t read_bytes = read(fd, &out->bytes[6], 10);
        (void)read_bytes;
        close(fd);
    }
#endif
}
