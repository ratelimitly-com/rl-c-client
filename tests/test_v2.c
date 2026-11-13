#include "r_crypto.h"
#include <assert.h>
int main(void) {
 r_bech32_quotas_t q; r_auth_type_t t; uint64_t id; uint8_t secret[32]; size_t n;
 assert(r_decode_api_key_bech32_with_quotas("rl-cookie1qgqsqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqz8scfwu4f", &t, &id, secret, 32, &n, &q)==0);
 assert(q.format_version==2);
 assert(q.rate_window_size_ms_max==UINT64_C(31536000000));
}
