/* SPDX-License-Identifier: GPL-2.0-only */
#include "dhp/frame.h"
#include "dhp/msg.h"
#include "test_util.h"

static const uint8_t KEY[16] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
                                0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00};

/* Push a whole wire buffer through a framer, returning the number of frames
 * that came out cleanly and reporting the last one. */
static int feed_all(dhp_framer_t *fr, const uint8_t *buf, size_t n,
                    const uint8_t *key, dhp_frame_t *last)
{
    int got = 0;
    for (size_t i = 0; i < n; i++) {
        dhp_frame_t f = {0};
        if (dhp_framer_push(fr, buf[i], key, &f) == DHP_OK) {
            got++;
            if (last) {
                *last = f;
            }
        }
    }
    return got;
}

static void test_roundtrip(void)
{
    const uint8_t payload[] = {1, 2, 3, 4, 5};
    dhp_frame_t f = {
        .type = DHP_MSG_KBD, .ttl = 8, .src = 0x1234, .dst = 0xABCD,
        .seq = 0x5678, .len = sizeof(payload), .payload = payload,
    };

    uint8_t wire[DHP_WIRE_MAX];
    size_t n = 0;
    CHECK_EQ(dhp_frame_encode(&f, KEY, wire, sizeof(wire), &n), DHP_OK);

    dhp_framer_t fr;
    dhp_framer_init(&fr);
    dhp_frame_t out = {0};
    CHECK_EQ(feed_all(&fr, wire, n, KEY, &out), 1);

    CHECK_EQ(out.type, DHP_MSG_KBD);
    CHECK_EQ(out.src, 0x1234);
    CHECK_EQ(out.dst, 0xABCD);
    CHECK_EQ(out.seq, 0x5678);
    CHECK_EQ(out.len, sizeof(payload));
    CHECK_EQ(memcmp(out.payload, payload, sizeof(payload)), 0);
}

/* A payload containing the flag and escape bytes must survive: this is the
 * property that makes flag delimiting safe in the first place. */
static void test_byte_stuffing(void)
{
    const uint8_t payload[] = {0x7E, 0x7D, 0x7E, 0x7E, 0x7D, 0x00, 0x7E};
    dhp_frame_t f = {
        .type = DHP_MSG_DATA, .ttl = 4, .src = 1, .dst = 2, .seq = 3,
        .len = sizeof(payload), .payload = payload,
    };

    uint8_t wire[DHP_WIRE_MAX];
    size_t n = 0;
    CHECK_EQ(dhp_frame_encode(&f, KEY, wire, sizeof(wire), &n), DHP_OK);

    /* Only the delimiters may be bare flags. */
    for (size_t i = 1; i + 1 < n; i++) {
        CHECK(wire[i] != DHP_FLAG);
    }

    dhp_framer_t fr;
    dhp_framer_init(&fr);
    dhp_frame_t out = {0};
    CHECK_EQ(feed_all(&fr, wire, n, KEY, &out), 1);
    CHECK_EQ(out.len, sizeof(payload));
    CHECK_EQ(memcmp(out.payload, payload, sizeof(payload)), 0);
}

static void test_crc_rejects_corruption(void)
{
    const uint8_t payload[] = {0xAA, 0xBB};
    dhp_frame_t f = {.type = DHP_MSG_PING, .ttl = 4, .src = 1, .dst = 2,
                     .seq = 1, .len = 2, .payload = payload};

    uint8_t wire[DHP_WIRE_MAX];
    size_t n = 0;
    dhp_frame_encode(&f, KEY, wire, sizeof(wire), &n);

    wire[5] ^= 0x01; /* flip a bit somewhere in the body */

    dhp_framer_t fr;
    dhp_framer_init(&fr);
    CHECK_EQ(feed_all(&fr, wire, n, KEY, NULL), 0);
    CHECK(fr.n_crc_err > 0);
}

/* A board with the wrong key must not be able to inject anything. */
static void test_wrong_key_rejected(void)
{
    uint8_t other[16];
    memcpy(other, KEY, 16);
    other[0] ^= 0xFF;

    const uint8_t payload[] = {0x01};
    dhp_frame_t f = {.type = DHP_MSG_HELLO, .ttl = 4, .src = 7, .dst = 8,
                     .seq = 1, .len = 1, .payload = payload};

    uint8_t wire[DHP_WIRE_MAX];
    size_t n = 0;
    dhp_frame_encode(&f, other, wire, sizeof(wire), &n);

    dhp_framer_t fr;
    dhp_framer_init(&fr);
    CHECK_EQ(feed_all(&fr, wire, n, KEY, NULL), 0);
    CHECK(fr.n_auth_err > 0);
    CHECK_EQ(fr.n_crc_err, 0); /* CRC was fine; it was authenticity that failed */
}

/* The property the whole framing choice exists for: a receiver that starts
 * listening halfway through a frame must recover on its own, with no reset,
 * no timeout and no negotiation. */
static void test_midstream_resync(void)
{
    const uint8_t payload[] = {9, 9, 9};
    dhp_frame_t f = {.type = DHP_MSG_PONG, .ttl = 4, .src = 3, .dst = 4,
                     .seq = 1, .len = 3, .payload = payload};

    uint8_t one[DHP_WIRE_MAX];
    size_t n = 0;
    dhp_frame_encode(&f, KEY, one, sizeof(one), &n);

    /* Stream: the tail half of a frame, then two whole ones. */
    uint8_t stream[DHP_WIRE_MAX * 3];
    size_t sn = 0;
    memcpy(stream, one + n / 2, n - n / 2);
    sn += n - n / 2;
    memcpy(stream + sn, one, n); sn += n;
    memcpy(stream + sn, one, n); sn += n;

    dhp_framer_t fr;
    dhp_framer_init(&fr);
    /* The truncated fragment is discarded; both complete frames arrive. */
    CHECK_EQ(feed_all(&fr, stream, sn, KEY, NULL), 2);
}

/* Forwarding must decrement the hop limit and leave the MAC intact -- if the
 * MAC covered the TTL, relaying would break authentication at hop two. */
static void test_forward_preserves_mac(void)
{
    const uint8_t payload[] = {0x42};
    dhp_frame_t f = {.type = DHP_MSG_MOUSE, .ttl = 3, .src = 1, .dst = 2,
                     .seq = 99, .len = 1, .payload = payload};

    uint8_t wire[DHP_WIRE_MAX];
    size_t n = 0;
    dhp_frame_encode(&f, KEY, wire, sizeof(wire), &n);

    dhp_framer_t fr;
    dhp_framer_init(&fr);
    dhp_frame_t out = {0};
    CHECK_EQ(feed_all(&fr, wire, n, KEY, &out), 1);
    CHECK_EQ(out.ttl, 3);

    /* Relay it: decrement in place, repair the CRC, re-encode and re-verify. */
    uint8_t body[DHP_BODY_MAX];
    const size_t body_len = out.body_len;
    CHECK(body_len > 0);
    memcpy(body, out.body, body_len);

    CHECK_EQ(dhp_frame_forward_prepare(body, body_len), DHP_OK);
    CHECK_EQ(body[DHP_OFF_TTL], 2);
    CHECK(dhp_frame_mac_valid(body, body_len, KEY));

    CHECK_EQ(dhp_frame_forward_prepare(body, body_len), DHP_OK);
    CHECK_EQ(body[DHP_OFF_TTL], 1);
    CHECK(dhp_frame_mac_valid(body, body_len, KEY));

    /* Hop limit exhausted: the frame must now be dropped rather than looped. */
    CHECK_EQ(dhp_frame_forward_prepare(body, body_len), DHP_ERR_TTL);
}

static void test_max_payload(void)
{
    uint8_t payload[DHP_MAX_PAYLOAD];
    for (int i = 0; i < DHP_MAX_PAYLOAD; i++) {
        payload[i] = (uint8_t)(i * 7);
    }
    dhp_frame_t f = {.type = DHP_MSG_DATA, .ttl = 4, .src = 1, .dst = 2,
                     .seq = 5, .len = DHP_MAX_PAYLOAD, .payload = payload};

    uint8_t wire[DHP_WIRE_MAX];
    size_t n = 0;
    CHECK_EQ(dhp_frame_encode(&f, KEY, wire, sizeof(wire), &n), DHP_OK);
    CHECK(n <= DHP_WIRE_MAX);

    dhp_framer_t fr;
    dhp_framer_init(&fr);
    dhp_frame_t out = {0};
    CHECK_EQ(feed_all(&fr, wire, n, KEY, &out), 1);
    CHECK_EQ(out.len, DHP_MAX_PAYLOAD);
    CHECK_EQ(memcmp(out.payload, payload, DHP_MAX_PAYLOAD), 0);
}

static void test_oversize_rejected(void)
{
    uint8_t payload[DHP_MAX_PAYLOAD + 1] = {0};
    dhp_frame_t f = {.type = DHP_MSG_DATA, .ttl = 4, .src = 1, .dst = 2,
                     .seq = 5, .len = DHP_MAX_PAYLOAD + 1, .payload = payload};
    uint8_t wire[DHP_WIRE_MAX];
    size_t n = 0;
    CHECK_EQ(dhp_frame_encode(&f, KEY, wire, sizeof(wire), &n), DHP_ERR_INVAL);
}

/* Garbage on the line must never wedge the decoder. */
static void test_noise_then_valid(void)
{
    const uint8_t payload[] = {1};
    dhp_frame_t f = {.type = DHP_MSG_PING, .ttl = 4, .src = 1, .dst = 2,
                     .seq = 1, .len = 1, .payload = payload};
    uint8_t wire[DHP_WIRE_MAX];
    size_t n = 0;
    dhp_frame_encode(&f, KEY, wire, sizeof(wire), &n);

    dhp_framer_t fr;
    dhp_framer_init(&fr);

    uint32_t seed = 12345;
    for (int i = 0; i < 5000; i++) {
        seed = seed * 1103515245u + 12345u;
        dhp_frame_t junk = {0};
        dhp_framer_push(&fr, (uint8_t)(seed >> 16), KEY, &junk);
    }
    CHECK_EQ(feed_all(&fr, wire, n, KEY, NULL), 1);
}

void test_frame_suite(void)
{
    RUN(test_roundtrip);
    RUN(test_byte_stuffing);
    RUN(test_crc_rejects_corruption);
    RUN(test_wrong_key_rejected);
    RUN(test_midstream_resync);
    RUN(test_forward_preserves_mac);
    RUN(test_max_payload);
    RUN(test_oversize_rejected);
    RUN(test_noise_then_valid);
}
