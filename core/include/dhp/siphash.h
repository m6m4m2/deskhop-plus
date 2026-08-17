/* SPDX-License-Identifier: GPL-2.0-only
 *
 * SipHash-2-4, the keyed MAC used to tag every link frame.
 *
 * Chosen over HMAC-SHA256 because frames are tiny and frequent: a 1 kHz mouse
 * generates 1000 MACs/second on every board in the chain, and SipHash was
 * designed precisely for short authenticated messages on machines without
 * crypto acceleration. The RP2040 has no crypto peripheral, so this matters.
 *
 * This is the standard construction (Aumasson & Bernstein, 2012) and is
 * verified against the reference test vectors in tests/test_siphash.c.
 */
#ifndef DHP_SIPHASH_H
#define DHP_SIPHASH_H

#include "dhp/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DHP_SIPHASH_KEY_BYTES 16

/* Full 64-bit SipHash-2-4 of `in` under the 16-byte `key`. */
uint64_t dhp_siphash24(const uint8_t *in, size_t inlen,
                       const uint8_t key[DHP_SIPHASH_KEY_BYTES]);

/* Constant-time equality for MAC comparison. Returns true if equal.
 * Never use memcmp() to compare a MAC: early exit leaks the position of the
 * first differing byte, which is enough to forge one byte at a time. */
bool dhp_ct_equal(const uint8_t *a, const uint8_t *b, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* DHP_SIPHASH_H */
