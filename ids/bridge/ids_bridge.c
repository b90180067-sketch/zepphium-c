#include "ids_bridge.h"

bool c_zephium_id_parse(const char *s, size_t len, zephium_id_t *out) {
    return zephium_id_parse(s, len, out);
}

bool c_zephium_id_to_string(const zephium_id_t *id, char *out_buf, size_t buf_len) {
    return zephium_id_to_string(id, out_buf, buf_len);
}

bool c_zephium_id_candidate_after(const zephium_id_t *candidate,
                                  const zephium_id_t *high_water,
                                  zephium_id_t *out) {
    return zephium_id_candidate_after(candidate, high_water, out);
}
