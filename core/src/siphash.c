/* SPDX-License-Identifier: GPL-2.0-only */
#include "dhp/siphash.h"

#include <string.h>

#define ROTL64(x, b) (uint64_t)(((x) << (b)) | ((x) >> (64 - (b))))

#define SIPROUND                                                               \
    do {                                                                       \
        v0 += v1;                                                              \
        v1 = ROTL64(v1, 13);                                                   \
        v1 ^= v0;                                                              \
        v0 = ROTL64(v0, 32);                                                   \
        v2 += v3;                                                              \
        v3 = ROTL64(v3, 16);                                                   \
        v3 ^= v2;                                                              \
        v0 += v3;                                                              \
        v3 = ROTL64(v3, 21);                                                   \
        v3 ^= v0;                                                              \
        v2 += v1;                                                              \
        v1 = ROTL64(v1, 17);                                                   \
        v1 ^= v2;                                                              \
        v2 = ROTL64(v2, 32);                                                   \
    } while (0)

static uint64_t load64_le(const uint8_t *p)
{
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) |
           ((uint64_t)p[3] << 24) | ((uint64_t)p[4] << 32) |
           ((uint64_t)p[5] << 40) | ((uint64_t)p[6] << 48) |
           ((uint64_t)p[7] << 56);
}

uint64_t dhp_siphash24(const uint8_t *in, size_t inlen,
                       const uint8_t key[DHP_SIPHASH_KEY_BYTES])
{
    uint64_t v0 = 0x736f6d6570736575ULL;
    uint64_t v1 = 0x646f72616e646f6dULL;
    uint64_t v2 = 0x6c7967656e657261ULL;
    uint64_t v3 = 0x7465646279746573ULL;

    const uint64_t k0 = load64_le(key);
    const uint64_t k1 = load64_le(key + 8);

    v3 ^= k1;
    v2 ^= k0;
    v1 ^= k1;
    v0 ^= k0;

    const size_t blocks = inlen & ~(size_t)7;
    for (size_t i = 0; i < blocks; i += 8) {
        const uint64_t m = load64_le(in + i);
        v3 ^= m;
        SIPROUND;
        SIPROUND;
        v0 ^= m;
    }

    /* Final block: remaining bytes, top byte holds the length mod 256. */
    uint64_t b = ((uint64_t)inlen) << 56;
    const size_t left = inlen & 7;
    for (size_t i = 0; i < left; i++) {
        b |= ((uint64_t)in[blocks + i]) << (8 * i);
    }

    v3 ^= b;
    SIPROUND;
    SIPROUND;
    v0 ^= b;

    v2 ^= 0xff;
    SIPROUND;
    SIPROUND;
    SIPROUND;
    SIPROUND;

    return v0 ^ v1 ^ v2 ^ v3;
}

bool dhp_ct_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0;
}
