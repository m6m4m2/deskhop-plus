/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Level 3 transport. Two endpoints wired through real dhp_link instances, so
 * every transfer here is genuinely segmented, framed, MAC'd, relayed and
 * reassembled rather than handed across in memory.
 */
#include "dhp/data.h"
#include "dhp/router.h"
#include "test_util.h"

#define MAXN 4

static const uint8_t KEY[16] = {9, 8, 7, 6, 5, 4, 3, 2,
                                1, 0, 1, 2, 3, 4, 5, 6};

typedef struct {
    uint8_t buf[262144];
    int     head, tail;
    bool    cut;
} wire_t;

typedef struct node {
    struct net *net;
    int         idx;
    dhp_link_t  link;
    dhp_data_t  data;

    /* Receive side: reassembled by the test, which is what a real client does */
    uint8_t  got[262144];
    uint32_t got_len;
    bool     auto_accept;
    int      offers;
    int      completed;
    uint8_t  last_result;
    uint8_t  last_kind;
    char     last_name[64];
    int      sent_done;
    uint8_t  sent_result;
} node_t;

typedef struct net {
    node_t     n[MAXN];
    int        count;
    wire_t     down[MAXN]; /* i -> i+1 */
    wire_t     up[MAXN];   /* i -> i-1 */
    dhp_time_t now;
    /* Drop every Nth frame's first byte, to force retransmission. */
    int        drop_every;
    int        byte_counter;
} net_t;

static void wire_put(wire_t *w, const uint8_t *d, size_t n)
{
    if (w->cut) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        const int next = (w->tail + 1) % (int)sizeof(w->buf);
        if (next == w->head) {
            return;
        }
        w->buf[w->tail] = d[i];
        w->tail = next;
    }
}

static bool wire_get(wire_t *w, uint8_t *out)
{
    if (w->head == w->tail) {
        return false;
    }
    *out = w->buf[w->head];
    w->head = (w->head + 1) % (int)sizeof(w->buf);
    return true;
}

static void on_tx(void *ctx, dhp_port_t port, const uint8_t *data, size_t len)
{
    node_t *n = ctx;
    net_t *net = n->net;

    if (port == DHP_PORT_DOWN) {
        if (n->idx + 1 < net->count) {
            wire_put(&net->down[n->idx], data, len);
        }
    } else if (n->idx > 0) {
        wire_put(&net->up[n->idx], data, len);
    }
}

static void on_offer(void *ctx, uint8_t stream, dhp_addr_t from, uint8_t kind,
                     uint32_t total, const char *name)
{
    node_t *n = ctx;
    (void)from;
    (void)total;
    n->offers++;
    n->last_kind = kind;
    snprintf(n->last_name, sizeof(n->last_name), "%s", name);
    n->got_len = 0;

    if (n->auto_accept) {
        dhp_data_accept(&n->data, stream, n->net->now);
    } else {
        dhp_data_reject(&n->data, stream, n->net->now);
    }
}

static void on_chunk(void *ctx, uint8_t stream, uint32_t offset,
                     const uint8_t *d, uint8_t len)
{
    node_t *n = ctx;
    (void)stream;
    if (offset + len <= sizeof(n->got)) {
        memcpy(n->got + offset, d, len);
        if (offset + len > n->got_len) {
            n->got_len = offset + len;
        }
    }
}

static void on_received(void *ctx, uint8_t stream, uint8_t result)
{
    node_t *n = ctx;
    (void)stream;
    n->completed++;
    n->last_result = result;
}

static void on_sent(void *ctx, uint8_t stream, uint8_t result)
{
    node_t *n = ctx;
    (void)stream;
    n->sent_done++;
    n->sent_result = result;
}

static void net_init(net_t *net, int count)
{
    memset(net, 0, sizeof(*net));
    net->now = 1000;
    net->count = count;

    const dhp_data_hooks_t hooks = {
        .on_offer = on_offer, .on_chunk = on_chunk,
        .on_received = on_received, .on_sent = on_sent,
    };

    for (int i = 0; i < count; i++) {
        node_t *n = &net->n[i];
        n->net = net;
        n->idx = i;
        n->auto_accept = true;
        dhp_link_init(&n->link, (dhp_addr_t)(i + 1), on_tx, n);
        dhp_link_set_key(&n->link, KEY);

        dhp_data_hooks_t h = hooks;
        h.ctx = n;
        dhp_data_init(&n->data, &n->link, &h);
    }
}

static void deliver(net_t *net)
{
    for (int i = 0; i < net->count; i++) {
        uint8_t b;
        if (i + 1 < net->count) {
            while (wire_get(&net->down[i], &b)) {
                net->byte_counter++;
                if (net->drop_every && (net->byte_counter % net->drop_every) == 0) {
                    continue; /* corrupt the stream: a frame will fail its CRC */
                }
                node_t *dst = &net->n[i + 1];
                dhp_frame_t f = {0};
                if (dhp_link_rx_byte(&dst->link, DHP_PORT_UP, b, net->now, &f) ==
                    DHP_OK) {
                    dhp_data_rx(&dst->data, &f, net->now);
                }
            }
        }
        if (i > 0) {
            while (wire_get(&net->up[i], &b)) {
                node_t *dst = &net->n[i - 1];
                dhp_frame_t f = {0};
                if (dhp_link_rx_byte(&dst->link, DHP_PORT_DOWN, b, net->now,
                                     &f) == DHP_OK) {
                    dhp_data_rx(&dst->data, &f, net->now);
                }
            }
        }
    }
}

static void net_run(net_t *net, uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t++) {
        net->now++;
        deliver(net);
        for (int i = 0; i < net->count; i++) {
            dhp_data_tick(&net->n[i].data, net->now);
        }
        deliver(net);
    }
}

/* ---------------------------------------------------------------------- */

static void fill(uint8_t *b, uint32_t n, uint32_t seed)
{
    for (uint32_t i = 0; i < n; i++) {
        seed = seed * 1103515245u + 12345u;
        b[i] = (uint8_t)(seed >> 16);
    }
}

/* A payload smaller than one chunk still has to go through the whole
 * offer/accept/chunk/done handshake. */
static void test_tiny_transfer(void)
{
    net_t net;
    net_init(&net, 2);

    const char *msg = "hello desk";
    CHECK(dhp_data_send(&net.n[0].data, 2, DHP_DATA_TEXT, "clip",
                        (const uint8_t *)msg, (uint32_t)strlen(msg),
                        net.now) >= 0);
    net_run(&net, 300);

    CHECK_EQ(net.n[1].offers, 1);
    CHECK_EQ(net.n[1].completed, 1);
    CHECK_EQ(net.n[1].last_result, DHP_DATA_OK);
    CHECK_EQ(net.n[1].got_len, strlen(msg));
    CHECK_EQ(memcmp(net.n[1].got, msg, strlen(msg)), 0);
    CHECK_STR(net.n[1].last_name, "clip");
    CHECK_EQ(net.n[1].last_kind, DHP_DATA_TEXT);

    /* The sender is told too, and only once its buffer is safe to reuse. */
    CHECK_EQ(net.n[0].sent_done, 1);
    CHECK_EQ(net.n[0].sent_result, DHP_DATA_OK);
}

/* Many chunks, exercising the sliding window and the ack cadence. */
static void test_multi_chunk_transfer(void)
{
    net_t net;
    net_init(&net, 2);

    static uint8_t payload[40000];
    fill(payload, sizeof(payload), 0xC0FFEE);

    CHECK(dhp_data_send(&net.n[0].data, 2, DHP_DATA_IMAGE, "shot.png", payload,
                        sizeof(payload), net.now) >= 0);
    net_run(&net, 8000);

    CHECK_EQ(net.n[1].completed, 1);
    CHECK_EQ(net.n[1].last_result, DHP_DATA_OK);
    CHECK_EQ(net.n[1].got_len, sizeof(payload));
    CHECK_EQ(memcmp(net.n[1].got, payload, sizeof(payload)), 0);
    CHECK_EQ(net.n[0].sent_result, DHP_DATA_OK);
}

/* A transfer must survive traversing intermediate boards, which relay the
 * frames without understanding any of this. */
static void test_transfer_across_the_chain(void)
{
    net_t net;
    net_init(&net, 4);

    static uint8_t payload[9000];
    fill(payload, sizeof(payload), 0xABCD);

    /* Node 0 to node 3: two boards in between. */
    CHECK(dhp_data_send(&net.n[0].data, 4, DHP_DATA_FILE, "notes.txt", payload,
                        sizeof(payload), net.now) >= 0);
    net_run(&net, 8000);

    CHECK_EQ(net.n[3].completed, 1);
    CHECK_EQ(net.n[3].last_result, DHP_DATA_OK);
    CHECK_EQ(net.n[3].got_len, sizeof(payload));
    CHECK_EQ(memcmp(net.n[3].got, payload, sizeof(payload)), 0);

    /* The boards in the middle relayed but never reassembled anything. */
    CHECK_EQ(net.n[1].completed, 0);
    CHECK_EQ(net.n[2].completed, 0);
}

/* Nothing is received until the far side agrees. A machine must not be able to
 * push a megabyte at another machine unasked. */
static void test_rejected_transfer(void)
{
    net_t net;
    net_init(&net, 2);
    net.n[1].auto_accept = false;

    static uint8_t payload[5000];
    fill(payload, sizeof(payload), 1);

    CHECK(dhp_data_send(&net.n[0].data, 2, DHP_DATA_FILE, "big", payload,
                        sizeof(payload), net.now) >= 0);
    net_run(&net, 1000);

    CHECK_EQ(net.n[1].offers, 1);
    CHECK_EQ(net.n[1].got_len, 0);       /* not a byte of content moved */
    CHECK_EQ(net.n[0].sent_done, 1);
    CHECK_EQ(net.n[0].sent_result, DHP_DATA_REFUSED);
}

/* Loss must be recovered from, not merely detected. */
static void test_survives_a_lossy_link(void)
{
    net_t net;
    net_init(&net, 2);
    net.drop_every = 700; /* mangles roughly one frame in nine */

    static uint8_t payload[12000];
    fill(payload, sizeof(payload), 0x5EED);

    CHECK(dhp_data_send(&net.n[0].data, 2, DHP_DATA_FILE, "lossy", payload,
                        sizeof(payload), net.now) >= 0);
    net_run(&net, 20000);

    CHECK_EQ(net.n[1].last_result, DHP_DATA_OK);
    CHECK_EQ(net.n[1].got_len, sizeof(payload));
    CHECK_EQ(memcmp(net.n[1].got, payload, sizeof(payload)), 0);
    CHECK(net.n[0].data.n_retransmits > 0);
    printf("    recovered with %u retransmissions\n",
           net.n[0].data.n_retransmits);
}

/* A peer that goes away mid-transfer must not hold a stream slot forever. */
static void test_vanishing_peer_times_out(void)
{
    net_t net;
    net_init(&net, 2);

    /* Large enough that it is certainly still in flight when the cable is
     * cut: the simulated wire has no baud limit, so a small transfer would
     * finish before the cut and test nothing. */
    static uint8_t payload[200000];
    fill(payload, sizeof(payload), 7);

    CHECK(dhp_data_send(&net.n[0].data, 2, DHP_DATA_FILE, "gone", payload,
                        sizeof(payload), net.now) >= 0);
    net_run(&net, 20);
    CHECK(dhp_data_active(&net.n[0].data) > 0);

    net.down[0].cut = true;
    net.up[1].cut = true;
    /* Past the idle timeout, which is deliberately longer than the offer
     * retry budget -- see dhp_data_init(). */
    net_run(&net, 18000);

    CHECK_EQ(net.n[0].sent_done, 1);
    CHECK_EQ(net.n[0].sent_result, DHP_DATA_TIMEOUT);
    CHECK_EQ(dhp_data_active(&net.n[0].data), 0);
    CHECK_EQ(dhp_data_active(&net.n[1].data), 0);
}

/* Several transfers at once must not interfere: ids and slots keep them
 * apart, which is what makes "copy some text while a screenshot is going"
 * work. */
static void test_concurrent_streams(void)
{
    net_t net;
    net_init(&net, 2);

    static uint8_t a[3000], b[4000];
    fill(a, sizeof(a), 11);
    fill(b, sizeof(b), 22);

    CHECK(dhp_data_send(&net.n[0].data, 2, DHP_DATA_TEXT, "a", a, sizeof(a),
                        net.now) >= 0);
    CHECK(dhp_data_send(&net.n[0].data, 2, DHP_DATA_IMAGE, "b", b, sizeof(b),
                        net.now) >= 0);
    net_run(&net, 6000);

    CHECK_EQ(net.n[1].offers, 2);
    CHECK_EQ(net.n[1].completed, 2);
    CHECK_EQ(net.n[0].sent_done, 2);
}

/* The stream table is finite; running out must be reported rather than
 * silently dropping a transfer. */
static void test_stream_exhaustion_is_reported(void)
{
    net_t net;
    net_init(&net, 2);

    static uint8_t payload[2000];
    fill(payload, sizeof(payload), 3);

    int accepted = 0;
    for (int i = 0; i < DHP_DATA_MAX_STREAMS + 3; i++) {
        if (dhp_data_send(&net.n[0].data, 2, DHP_DATA_FILE, "x", payload,
                          sizeof(payload), net.now) >= 0) {
            accepted++;
        }
    }
    CHECK_EQ(accepted, DHP_DATA_MAX_STREAMS);

    net_run(&net, 8000);
    CHECK_EQ(net.n[1].completed, DHP_DATA_MAX_STREAMS);
}

/* An offer larger than the receiver is willing to hold must be refused up
 * front, not discovered halfway through. */
static void test_oversized_offer_refused(void)
{
    net_t net;
    net_init(&net, 2);
    net.n[1].data.max_inbound = 1024;

    static uint8_t payload[5000];
    fill(payload, sizeof(payload), 4);

    CHECK(dhp_data_send(&net.n[0].data, 2, DHP_DATA_FILE, "huge", payload,
                        sizeof(payload), net.now) >= 0);
    net_run(&net, 1000);

    CHECK_EQ(net.n[1].offers, 0); /* the app is never even asked */
    CHECK_EQ(net.n[0].sent_result, DHP_DATA_TOO_BIG);
}

/* Cancelling from the sending side must tear down both ends. */
static void test_cancel(void)
{
    net_t net;
    net_init(&net, 2);

    static uint8_t payload[200000];
    fill(payload, sizeof(payload), 5);

    const int h = dhp_data_send(&net.n[0].data, 2, DHP_DATA_FILE, "nope",
                                payload, sizeof(payload), net.now);
    CHECK(h >= 0);
    net_run(&net, 20);

    dhp_data_cancel(&net.n[0].data, (uint8_t)h, net.now);
    net_run(&net, 300);

    CHECK_EQ(net.n[0].sent_result, DHP_DATA_CANCELLED);
    CHECK_EQ(dhp_data_active(&net.n[0].data), 0);
    CHECK_EQ(dhp_data_active(&net.n[1].data), 0);
}

/* An SMB handoff is a tiny transfer whose content is a location rather than a
 * file: this is how anything large is meant to move. */
static void test_share_handoff_is_small(void)
{
    net_t net;
    net_init(&net, 2);

    const char *loc = "smb://deskhop/share/holiday.mp4";
    CHECK(dhp_data_send(&net.n[0].data, 2, DHP_DATA_SHARE, "holiday.mp4",
                        (const uint8_t *)loc, (uint32_t)strlen(loc),
                        net.now) >= 0);
    net_run(&net, 300);

    CHECK_EQ(net.n[1].last_kind, DHP_DATA_SHARE);
    CHECK_EQ(net.n[1].last_result, DHP_DATA_OK);
    CHECK_EQ(memcmp(net.n[1].got, loc, strlen(loc)), 0);

    /* The point: a 4 GB video costs the chain thirty-odd bytes. */
    CHECK(net.n[1].got_len < 64);
}

/* A name longer than the field must be truncated rather than overrunning, and
 * must never be trusted as a path. */
static void test_long_name_truncated(void)
{
    net_t net;
    net_init(&net, 2);

    char huge[300];
    memset(huge, 'A', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = '\0';

    const char *msg = "x";
    CHECK(dhp_data_send(&net.n[0].data, 2, DHP_DATA_TEXT, huge,
                        (const uint8_t *)msg, 1, net.now) >= 0);
    net_run(&net, 300);

    CHECK_EQ(net.n[1].completed, 1);
    CHECK(strlen(net.n[1].last_name) <= DHP_DATA_NAME_MAX);
}

void test_data_suite(void)
{
    RUN(test_tiny_transfer);
    RUN(test_multi_chunk_transfer);
    RUN(test_transfer_across_the_chain);
    RUN(test_rejected_transfer);
    RUN(test_survives_a_lossy_link);
    RUN(test_vanishing_peer_times_out);
    RUN(test_concurrent_streams);
    RUN(test_stream_exhaustion_is_reported);
    RUN(test_oversized_offer_refused);
    RUN(test_cancel);
    RUN(test_share_handoff_is_small);
    RUN(test_long_name_truncated);
}
