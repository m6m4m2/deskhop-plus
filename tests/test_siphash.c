/* SPDX-License-Identifier: GPL-2.0-only
 *
 * SipHash-2-4 against the reference vectors published with the original paper
 * (key = 00 01 .. 0f, input = 00 01 .. len-1).
 */
#include "dhp/siphash.h"
#include "test_util.h"

static const uint64_t k_vectors[16] = {
    0x726fdb47dd0e0e31ULL, 0x74f839c593dc67fdULL, 0x0d6c8009d9a94f5aULL,
    0x85676696d7fb7e2dULL, 0xcf2794e0277187b7ULL, 0x18765564cd99a68dULL,
    0xcbc9466e58fee3ceULL, 0xab0200f58b01d137ULL, 0x93f5f5799a932462ULL,
    0x9e0082df0ba9e4b0ULL, 0x7a5dbbc594ddb9f3ULL, 0xf4b32f46226bada7ULL,
    0x751e8fbc860ee5fbULL, 0x14ea5627c0843d90ULL, 0xf723ca908e7af2eeULL,
    0xa129ca6149be45e5ULL,
};

static void test_reference_vectors(void)
{
    uint8_t key[16], in[16];
    for (int i = 0; i < 16; i++) {
        key[i] = (uint8_t)i;
        in[i] = (uint8_t)i;
    }
    for (int len = 0; len < 16; len++) {
        CHECK_EQ(dhp_siphash24(in, (size_t)len, key), k_vectors[len]);
    }
}

static void test_key_sensitivity(void)
{
    uint8_t k1[16] = {0}, k2[16] = {0};
    k2[7] = 1;
    const uint8_t msg[] = "deskhop";
    CHECK(dhp_siphash24(msg, sizeof(msg), k1) !=
          dhp_siphash24(msg, sizeof(msg), k2));
}

static void test_ct_equal(void)
{
    const uint8_t a[4] = {1, 2, 3, 4};
    const uint8_t b[4] = {1, 2, 3, 4};
    const uint8_t c[4] = {1, 2, 3, 5};
    CHECK(dhp_ct_equal(a, b, 4));
    CHECK(!dhp_ct_equal(a, c, 4));
    /* A difference in the first byte must be caught just as a difference in
     * the last is -- no early exit. */
    const uint8_t d[4] = {9, 2, 3, 4};
    CHECK(!dhp_ct_equal(a, d, 4));
}

void test_siphash_suite(void)
{
    RUN(test_reference_vectors);
    RUN(test_key_sensitivity);
    RUN(test_ct_equal);
}
