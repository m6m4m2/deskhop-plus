/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Crypto primitives for pairing, backed by Monocypher and the RP2040's
 * ring-oscillator entropy source.
 *
 * core/ deliberately does not implement these. Hand-rolling X25519 is a good
 * way to ship a subtly broken curve implementation, so the curve arithmetic
 * comes from Monocypher (public domain, single file, auditable, and small
 * enough for an M0+) and this file is only wiring.
 */
#include "crypto_backend.h"

#include <string.h>

#include "hardware/structs/rosc.h"
#include "hardware/sync.h"
#include "monocypher.h"
#include "pico/time.h"

static dhp_crypto_t g_crypto;

/* ------------------------------------------------------------------ *
 * Entropy
 *
 * The ring oscillator's random bit is genuinely nondeterministic but is also
 * biased and correlated between adjacent samples, so it must never be used
 * raw. Bits are sampled well apart, accumulated into a pool, and the pool is
 * hashed -- output is therefore only as good as the pool's real entropy, but
 * is at least uniformly distributed and free of the raw bias.
 *
 * This must not be replaced by a PRNG seeded at boot. Every board runs an
 * identical image, so a boot-seeded PRNG would have every board agreeing on
 * its "random" values, and the pairing nonces would collide across the whole
 * production run.
 * ------------------------------------------------------------------ */

#define ROSC_POOL_BYTES 64

static void rosc_fill(uint8_t *pool, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        uint8_t b = 0;
        for (int bit = 0; bit < 8; bit++) {
            /* Sample far enough apart that consecutive reads are not simply
             * the same oscillator phase observed twice. */
            busy_wait_at_least_cycles(50);
            b = (uint8_t)((b << 1) | (rosc_hw->randombit & 1u));
        }
        pool[i] = b;
    }
}

static void be_random(void *ctx, uint8_t *out, size_t len)
{
    (void)ctx;

    uint8_t pool[ROSC_POOL_BYTES];
    rosc_fill(pool, sizeof(pool));

    /* Fold in the clock as a second, independent source: it does not add much
     * entropy but it costs nothing and removes any chance of two boards
     * producing identical output from an unlucky pool. */
    const uint64_t t = time_us_64();

    size_t done = 0;
    uint32_t counter = 0;
    while (done < len) {
        uint8_t block[64];
        crypto_blake2b_ctx h;
        crypto_blake2b_init(&h, sizeof(block));
        crypto_blake2b_update(&h, pool, sizeof(pool));
        crypto_blake2b_update(&h, (const uint8_t *)&t, sizeof(t));
        crypto_blake2b_update(&h, (const uint8_t *)&counter, sizeof(counter));
        crypto_blake2b_final(&h, block);
        counter++;

        const size_t n = (len - done) < sizeof(block) ? (len - done)
                                                      : sizeof(block);
        memcpy(out + done, block, n);
        done += n;
        crypto_wipe(block, sizeof(block));

        /* Re-stir between blocks so a long request does not stretch one pool. */
        rosc_fill(pool, sizeof(pool));
    }

    crypto_wipe(pool, sizeof(pool));
}

/* ------------------------------------------------------------------ *
 * X25519
 * ------------------------------------------------------------------ */

static void be_keypair(void *ctx, uint8_t secret[32], uint8_t public_key[32])
{
    be_random(ctx, secret, 32);
    crypto_x25519_public_key(public_key, secret);
}

static bool be_shared(void *ctx, uint8_t out[32], const uint8_t secret[32],
                      const uint8_t peer_public[32])
{
    (void)ctx;
    crypto_x25519(out, secret, peer_public);

    /* An all-zero result means the peer supplied a low-order point, which
     * forces the shared secret to a value the attacker knows. Monocypher
     * computes it rather than refusing, so the check is ours to make. */
    uint8_t acc = 0;
    for (int i = 0; i < 32; i++) {
        acc |= out[i];
    }
    if (acc == 0) {
        crypto_wipe(out, 32);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ *
 * KDF: keyed BLAKE2b in counter mode
 * ------------------------------------------------------------------ */

static void be_kdf(void *ctx, uint8_t *out, size_t out_len, const uint8_t *ikm,
                   size_t ikm_len, const uint8_t *info, size_t info_len)
{
    (void)ctx;

    size_t done = 0;
    uint32_t counter = 0;
    while (done < out_len) {
        uint8_t block[64];
        crypto_blake2b_ctx h;
        crypto_blake2b_keyed_init(&h, sizeof(block), ikm, ikm_len);
        crypto_blake2b_update(&h, info, info_len);
        crypto_blake2b_update(&h, (const uint8_t *)&counter, sizeof(counter));
        crypto_blake2b_final(&h, block);
        counter++;

        const size_t n = (out_len - done) < sizeof(block) ? (out_len - done)
                                                          : sizeof(block);
        memcpy(out + done, block, n);
        done += n;
        crypto_wipe(block, sizeof(block));
    }
}

void crypto_backend_init(void)
{
    g_crypto.random = be_random;
    g_crypto.dh_keypair = be_keypair;
    g_crypto.dh_shared = be_shared;
    g_crypto.kdf = be_kdf;
    g_crypto.ctx = NULL;
}

const dhp_crypto_t *crypto_backend(void)
{
    return &g_crypto;
}
