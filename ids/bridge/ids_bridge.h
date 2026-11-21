#ifndef ZEPHIUM_IDS_BRIDGE_H
#define ZEPHIUM_IDS_BRIDGE_H

#include "../port.h"

#ifdef __cplusplus
extern "C" {
#endif

bool c_zephium_id_parse(const char *s, size_t len, zephium_id_t *out);
bool c_zephium_id_to_string(const zephium_id_t *id, char *out_buf, size_t buf_len);
bool c_zephium_id_candidate_after(const zephium_id_t *candidate,
                                  const zephium_id_t *high_water,
                                  zephium_id_t *out);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHIUM_IDS_BRIDGE_H */
