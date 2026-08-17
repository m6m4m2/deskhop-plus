/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Pairing behaviour.
 *
 * The crypto backend here is a MOCK. It is not secure and is not meant to be:
 * its only job is to satisfy the algebraic property the state machine relies
 * on -- that both sides derive the same shared value -- so that the protocol
 * logic can be tested without depending on a curve implementation. The real
 * backend is Monocypher; see firmware/rp2040/src/crypto_backend.c.
 */
#include "dhp/auth.h"
#include "dhp/frame.h"
#include "test_util.h"

/* ---- mock backend ----------------------------------------------------- */

typedef struct {
    uint32_t counter;
    uint8_t  seed;
} mock_ctx_t;

static void mock_random(void *vctx, uint8_t *out, size_t len)
{
    mock_ctx_t *c = vctx;
    for (size_t i = 0; i < len; i++) {
        c->counter = c->counter * 1103515245u + 12345u + c->seed;
        out[i] = (uint8_t)(c->counter >> 16);
    }
}

/* public = secret ^ 0x5A, so the "shared secret" can be recovered
 * symmetrically. Obviously not a real key exchange. */
static void mock_keypair(void *vctx, uint8_t secret[32], uint8_t pub[32])
{
    mock_random(vctx, secret, 32);
    for (int i = 0; i < 32; i++) {
        pub[i] = (uint8_t)(secret[i] ^ 0x5A);
    }
}

static bool mock_shared(void *vctx, uint8_t out[32], const uint8_t secret[32],
                        const uint8_t peer_pub[32])
{
    (void)vctx;

    uint8_t peer_secret[32];
    bool all_zero = true;
    for (int i = 0; i < 32; i++) {
        peer_secret[i] = (uint8_t)(peer_pub[i] ^ 0x5A);
        if (peer_pub[i] != 0) {
            all_zero = false;
        }
    }
    if (all_zero) {
        return false; /* mimic rejecting a degenerate public key */
    }

    /* Order the two secrets so both sides get the same answer. */
    const bool self_first = memcmp(secret, peer_secret, 32) < 0;
    const uint8_t *a = self_first ? secret : peer_secret;
    const uint8_t *b = self_first ? peer_secret : secret;
    for (int i = 0; i < 32; i++) {
        out[i] = (uint8_t)(a[i] + b[i] * 3u);
    }
    return true;
}

/* SipHash in counter mode: a perfectly reasonable PRF for a mock. */
static void mock_kdf(void *vctx, uint8_t *out, size_t out_len,
                     const uint8_t *ikm, size_t ikm_len, const uint8_t *info,
                     size_t info_len)
{
    (void)vctx;
    (void)ikm_len;

    uint8_t key[16];
    memcpy(key, ikm, 16);

    size_t done = 0;
    uint8_t block = 0;
    while (done < out_len) {
        uint8_t buf[256];
        const size_t n = info_len > sizeof(buf) - 1 ? sizeof(buf) - 1 : info_len;
        memcpy(buf, info, n);
        buf[n] = block++;

        const uint64_t h = dhp_siphash24(buf, n + 1, key);
        for (int i = 0; i < 8 && done < out_len; i++) {
            out[done++] = (uint8_t)(h >> (8 * i));
        }
    }
}

static dhp_crypto_t make_crypto(mock_ctx_t *ctx, uint8_t seed)
{
    ctx->counter = 0x1234u + seed;
    ctx->seed = seed;
    const dhp_crypto_t c = {
        .random = mock_random, .dh_keypair = mock_keypair,
        .dh_shared = mock_shared, .kdf = mock_kdf, .ctx = ctx,
    };
    return c;
}

/* ---- harness ---------------------------------------------------------- */

typedef struct {
    dhp_pair_t p;
    mock_ctx_t ctx;
} party_t;

static void party_init(party_t *w, dhp_uid_t uid, uint8_t seed)
{
    memset(w, 0, sizeof(*w));
    const dhp_crypto_t c = make_crypto(&w->ctx, seed);
    dhp_pair_init(&w->p, &c, uid);
}

/* Drain everything a party wants to send and deliver it, which is exactly
 * what the firmware loop does: take the events once, then act on each flag
 * that is set. Offers go before confirmations, since a confirmation is
 * meaningless to a peer that does not yet hold the matching offer. */
static void deliver(party_t *dst, const uint8_t *buf, uint8_t n, dhp_time_t now)
{
    if (dst && n) {
        dhp_pair_rx(&dst->p, buf, n, now);
    }
}

static void drain(party_t *from, party_t *t1, party_t *t2, dhp_time_t now)
{
    const dhp_pair_events_t ev = dhp_pair_take_events(&from->p);
    uint8_t buf[128];
    uint8_t n;

    if (ev.send_offer) {
        n = dhp_pair_build_offer(&from->p, buf, sizeof(buf));
        deliver(t1, buf, n, now);
        deliver(t2, buf, n, now);
    }
    if (ev.send_confirm) {
        n = dhp_pair_build_confirm(&from->p, buf, sizeof(buf));
        deliver(t1, buf, n, now);
        deliver(t2, buf, n, now);
    }
    if (ev.send_abort) {
        n = dhp_pair_build_abort(&from->p, buf, sizeof(buf));
        deliver(t1, buf, n, now);
        deliver(t2, buf, n, now);
    }
}

/* Run two parties to completion, optionally with a third joining. */
static void exchange(party_t *a, party_t *b, party_t *third, dhp_time_t now)
{
    for (int round = 0; round < 12; round++) {
        drain(a, b, third, now);
        drain(b, a, third, now);
        if (third) {
            drain(third, a, b, now);
        }
    }
}

/* ---- tests ------------------------------------------------------------ */

/* The ordinary case: a button held on each board, within the window. */
static void test_two_boards_pair(void)
{
    party_t a, b;
    party_init(&a, 0x1111111111111111ULL, 1);
    party_init(&b, 0x2222222222222222ULL, 2);

    dhp_pair_begin(&a.p, 1000, 30000);
    dhp_pair_begin(&b.p, 1000, 30000);
    exchange(&a, &b, NULL, 1000);

    CHECK_EQ(a.p.state, DHP_PAIR_DONE);
    CHECK_EQ(b.p.state, DHP_PAIR_DONE);

    const uint8_t *ka = dhp_pair_chain_key(&a.p);
    const uint8_t *kb = dhp_pair_chain_key(&b.p);
    CHECK(ka != NULL);
    CHECK(kb != NULL);
    if (ka && kb) {
        CHECK_EQ(memcmp(ka, kb, DHP_SIPHASH_KEY_BYTES), 0);
    }

    /* Both boards show the user the same short string. */
    char sa[7], sb[7];
    dhp_pair_sas_string(&a.p, sa);
    dhp_pair_sas_string(&b.p, sb);
    CHECK_STR(sa, sb);
    printf("    paired, SAS %s\n", sa);
}

/* The derived key must actually work as a chain key: this is the whole point
 * of pairing, so check it end to end rather than just comparing bytes. */
static void test_derived_key_authenticates_frames(void)
{
    party_t a, b;
    party_init(&a, 0xAAAA, 1);
    party_init(&b, 0xBBBB, 2);
    dhp_pair_begin(&a.p, 1000, 30000);
    dhp_pair_begin(&b.p, 1000, 30000);
    exchange(&a, &b, NULL, 1000);

    const uint8_t *ka = dhp_pair_chain_key(&a.p);
    const uint8_t *kb = dhp_pair_chain_key(&b.p);
    CHECK(ka && kb);
    if (!ka || !kb) {
        return;
    }

    const uint8_t payload[] = {1, 2, 3};
    dhp_frame_t f = {.type = DHP_MSG_HELLO, .ttl = 8, .src = 1, .dst = 2,
                     .seq = 7, .len = 3, .payload = payload};
    uint8_t wire[DHP_WIRE_MAX];
    size_t n = 0;
    CHECK_EQ(dhp_frame_encode(&f, ka, wire, sizeof(wire), &n), DHP_OK);

    dhp_framer_t fr;
    dhp_framer_init(&fr);
    int got = 0;
    for (size_t i = 0; i < n; i++) {
        dhp_frame_t out = {0};
        if (dhp_framer_push(&fr, wire[i], kb, &out) == DHP_OK) {
            got++;
        }
    }
    CHECK_EQ(got, 1);
}

/* The rule that matters: if a third device joins the exchange, everyone
 * aborts rather than anyone deciding which of the two to trust. */
static void test_third_party_aborts_everyone(void)
{
    party_t a, b, c;
    party_init(&a, 0x1111, 1);
    party_init(&b, 0x2222, 2);
    party_init(&c, 0x3333, 3); /* uninvited */

    dhp_pair_begin(&a.p, 1000, 30000);
    dhp_pair_begin(&b.p, 1000, 30000);
    dhp_pair_begin(&c.p, 1000, 30000);

    exchange(&a, &b, &c, 1000);

    /* Nobody ends up paired, and nobody ends up holding a key. */
    CHECK_EQ(a.p.state, DHP_PAIR_ABORTED);
    CHECK_EQ(b.p.state, DHP_PAIR_ABORTED);
    CHECK(dhp_pair_chain_key(&a.p) == NULL);
    CHECK(dhp_pair_chain_key(&b.p) == NULL);
}

/* A tampered confirmation -- what a machine in the middle produces, since it
 * cannot know the real shared secret -- must abort rather than proceed. */
static void test_bad_confirm_aborts(void)
{
    party_t a, b;
    party_init(&a, 0x1111, 1);
    party_init(&b, 0x2222, 2);
    dhp_pair_begin(&a.p, 1000, 30000);
    dhp_pair_begin(&b.p, 1000, 30000);

    uint8_t buf[128];
    uint8_t n;

    /* Exchange offers so both sides hold a shared secret. */
    n = dhp_pair_build_offer(&a.p, buf, sizeof(buf));
    dhp_pair_rx(&b.p, buf, n, 1000);
    n = dhp_pair_build_offer(&b.p, buf, sizeof(buf));
    dhp_pair_rx(&a.p, buf, n, 1000);
    CHECK_EQ(a.p.state, DHP_PAIR_CONFIRM_WAIT);

    /* Now corrupt b's confirmation on its way to a. */
    n = dhp_pair_build_confirm(&b.p, buf, sizeof(buf));
    CHECK(n > 9);
    buf[9] ^= 0xFF; /* first byte of the tag */
    dhp_pair_rx(&a.p, buf, n, 1000);

    CHECK_EQ(a.p.state, DHP_PAIR_ABORTED);
    CHECK_EQ(a.p.abort_reason, DHP_ABORT_BAD_CONFIRM);
    CHECK(dhp_pair_chain_key(&a.p) == NULL);
}

/* Pressing the button on one board only must time out rather than wait
 * forever with the door open. */
static void test_lonely_pairing_times_out(void)
{
    party_t a;
    party_init(&a, 0x1111, 1);
    dhp_pair_begin(&a.p, 1000, 5000);

    dhp_pair_tick(&a.p, 3000);
    CHECK_EQ(a.p.state, DHP_PAIR_WAITING);

    dhp_pair_tick(&a.p, 1000 + 5001);
    CHECK_EQ(a.p.state, DHP_PAIR_ABORTED);
    CHECK_EQ(a.p.abort_reason, DHP_ABORT_TIMEOUT);
}

/* A board that never had the button pressed must ignore pairing traffic
 * entirely: the physical action is what authorises the exchange. */
static void test_ignores_traffic_when_not_pairing(void)
{
    party_t a, b;
    party_init(&a, 0x1111, 1);
    party_init(&b, 0x2222, 2);

    dhp_pair_begin(&a.p, 1000, 30000);

    uint8_t buf[128];
    const uint8_t n = dhp_pair_build_offer(&a.p, buf, sizeof(buf));
    CHECK(n > 0);

    dhp_pair_rx(&b.p, buf, n, 1000); /* b never pressed anything */
    CHECK_EQ(b.p.state, DHP_PAIR_IDLE);
    CHECK(dhp_pair_chain_key(&b.p) == NULL);
}

/* A late third offer after a successful pairing is still a third party. */
static void test_late_third_party_aborts(void)
{
    party_t a, b, c;
    party_init(&a, 0x1111, 1);
    party_init(&b, 0x2222, 2);
    party_init(&c, 0x3333, 3);

    dhp_pair_begin(&a.p, 1000, 30000);
    dhp_pair_begin(&b.p, 1000, 30000);
    exchange(&a, &b, NULL, 1000);
    CHECK_EQ(a.p.state, DHP_PAIR_DONE);

    dhp_pair_begin(&c.p, 1200, 30000);
    uint8_t buf[128];
    const uint8_t n = dhp_pair_build_offer(&c.p, buf, sizeof(buf));
    dhp_pair_rx(&a.p, buf, n, 1200);

    CHECK_EQ(a.p.state, DHP_PAIR_ABORTED);
    CHECK_EQ(a.p.abort_reason, DHP_ABORT_THIRD_PARTY);
}

void test_auth_suite(void)
{
    RUN(test_two_boards_pair);
    RUN(test_derived_key_authenticates_frames);
    RUN(test_third_party_aborts_everyone);
    RUN(test_bad_confirm_aborts);
    RUN(test_lonely_pairing_times_out);
    RUN(test_ignores_traffic_when_not_pairing);
    RUN(test_late_third_party_aborts);
}
