/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Pairing primitives on Linux: the same Monocypher the firmware uses, over the
 * kernel CSPRNG instead of a ring oscillator.
 *
 * Deliberately the same library and the same constructions as
 * firmware/rp2040/src/crypto_backend.c. Two implementations of a key exchange
 * that must agree byte-for-byte is a good way to discover, on a bench, that
 * they do not.
 */
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>

#include "crypto_linux.h"
#include "monocypher.h"
#include "platform.h"

void plat_random(uint8_t *out, size_t len)
{
    size_t done = 0;
    while (done < len) {
        const ssize_t n = getrandom(out + done, len - done, 0);
        if (n < 0) {
            /* No fallback. A pairing nonce that is not random is a pairing
             * that can be predicted, so stopping is strictly better than
             * continuing with something weaker. */
            fprintf(stderr, "deskhop-coord: getrandom failed; refusing to "
                            "continue with weak randomness\n");
            abort();
        }
        done += (size_t)n;
    }
}

static void be_random(void *ctx, uint8_t *out, size_t len)
{
    (void)ctx;
    plat_random(out, len);
}

static void be_keypair(void *ctx, uint8_t secret[32], uint8_t public_key[32])
{
    (void)ctx;
    plat_random(secret, 32);
    crypto_x25519_public_key(public_key, secret);
}

static bool be_shared(void *ctx, uint8_t out[32], const uint8_t secret[32],
                      const uint8_t peer_public[32])
{
    (void)ctx;
    crypto_x25519(out, secret, peer_public);

    /* All-zero means the peer offered a low-order point, forcing a shared
     * secret the attacker already knows. Monocypher computes it rather than
     * refusing, so the check belongs here. */
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

static dhp_crypto_t g_crypto;

const dhp_crypto_t *crypto_linux(void)
{
    g_crypto.random = be_random;
    g_crypto.dh_keypair = be_keypair;
    g_crypto.dh_shared = be_shared;
    g_crypto.kdf = be_kdf;
    g_crypto.ctx = NULL;
    return &g_crypto;
}
