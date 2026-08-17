/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Link layer in isolation: relaying along the chain, replay rejection, and
 * neighbour detection. Three bare links, no routers.
 */
#include "dhp/link.h"
#include "test_util.h"

#define N 3

static const uint8_t KEY[16] = {1, 2, 3, 4, 5, 6, 7, 8,
                                9, 10, 11, 12, 13, 14, 15, 16};

typedef struct {
    uint8_t buf[4096];
    int len;
} pipe_t;

typedef struct {
    dhp_link_t link;
    int        idx;
    struct bus *bus;
    int        delivered;
    uint8_t    last_payload[DHP_MAX_PAYLOAD];
    uint8_t    last_len;
} node_t;

struct bus {
    node_t node[N];
    pipe_t down[N]; /* i -> i+1 */
    pipe_t up[N];   /* i -> i-1 */
};

static void tx(void *ctx, dhp_port_t port, const uint8_t *data, size_t len)
{
    node_t *n = ctx;
    pipe_t *p;

    if (port == DHP_PORT_DOWN) {
        if (n->idx + 1 >= N) return;
        p = &n->bus->down[n->idx];
    } else {
        if (n->idx == 0) return;
        p = &n->bus->up[n->idx];
    }
    if (p->len + (int)len > (int)sizeof(p->buf)) return;
    memcpy(p->buf + p->len, data, len);
    p->len += (int)len;
}

static void bus_init(struct bus *b)
{
    memset(b, 0, sizeof(*b));
    for (int i = 0; i < N; i++) {
        b->node[i].idx = i;
        b->node[i].bus = b;
        dhp_link_init(&b->node[i].link, (dhp_addr_t)(i + 1), tx, &b->node[i]);
        dhp_link_set_key(&b->node[i].link, KEY);
    }
}

static void pump(struct bus *b, dhp_time_t now)
{
    /* One pass of everything currently in flight; repeat so relayed frames
     * continue along the chain in the same call. */
    for (int round = 0; round < N + 2; round++) {
        pipe_t snapshot_down[N], snapshot_up[N];
        memcpy(snapshot_down, b->down, sizeof(snapshot_down));
        memcpy(snapshot_up, b->up, sizeof(snapshot_up));
        memset(b->down, 0, sizeof(b->down));
        memset(b->up, 0, sizeof(b->up));

        bool any = false;
        for (int i = 0; i < N; i++) {
            for (int k = 0; k < snapshot_down[i].len; k++) {
                any = true;
                node_t *dst = &b->node[i + 1];
                dhp_frame_t f = {0};
                if (dhp_link_rx_byte(&dst->link, DHP_PORT_UP,
                                     snapshot_down[i].buf[k], now, &f) == DHP_OK) {
                    dst->delivered++;
                    dst->last_len = f.len;
                    if (f.len) memcpy(dst->last_payload, f.payload, f.len);
                }
            }
            for (int k = 0; k < snapshot_up[i].len; k++) {
                any = true;
                node_t *dst = &b->node[i - 1];
                dhp_frame_t f = {0};
                if (dhp_link_rx_byte(&dst->link, DHP_PORT_DOWN,
                                     snapshot_up[i].buf[k], now, &f) == DHP_OK) {
                    dst->delivered++;
                    dst->last_len = f.len;
                    if (f.len) memcpy(dst->last_payload, f.payload, f.len);
                }
            }
        }
        if (!any) break;
    }
}

/* A unicast to the far end must traverse the middle board. */
static void test_relay_along_chain(void)
{
    struct bus b;
    bus_init(&b);

    const uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
    CHECK_EQ(dhp_link_send(&b.node[0].link, DHP_MSG_DATA, 3, payload, 4, 100),
             DHP_OK);
    pump(&b, 100);

    /* Node 2 is two hops away and received it. */
    CHECK_EQ(b.node[2].delivered, 1);
    CHECK_EQ(b.node[2].last_len, 4);
    CHECK_EQ(memcmp(b.node[2].last_payload, payload, 4), 0);

    /* Node 1 relayed it but was not the addressee, so it was not delivered. */
    CHECK_EQ(b.node[1].delivered, 0);
    CHECK(b.node[1].link.port[DHP_PORT_UP].forwarded > 0);
}

/* A broadcast reaches everybody exactly once, despite arriving from two
 * directions at the middle board. */
static void test_broadcast_reaches_all_once(void)
{
    struct bus b;
    bus_init(&b);

    const uint8_t payload[] = {0x01};
    dhp_link_send(&b.node[1].link, DHP_MSG_HELLO, DHP_ADDR_BROADCAST, payload,
                  1, 100);
    pump(&b, 100);

    CHECK_EQ(b.node[0].delivered, 1);
    CHECK_EQ(b.node[2].delivered, 1);
}

/* The middle board sees the same broadcast arrive on both ports when the two
 * ends talk; the replay window must suppress the duplicate. */
static void test_replay_suppressed(void)
{
    struct bus b;
    bus_init(&b);

    const uint8_t payload[] = {0x55};
    dhp_link_send(&b.node[0].link, DHP_MSG_HELLO, DHP_ADDR_BROADCAST, payload,
                  1, 100);
    pump(&b, 100);
    const int first = b.node[2].delivered;

    /* Replay the exact same bytes by sending them again from node 0's buffer:
     * emulate a wire echo by re-injecting an identical encoded frame. */
    dhp_frame_t f = {.type = DHP_MSG_HELLO, .ttl = DHP_TTL_DEFAULT, .src = 1,
                     .dst = DHP_ADDR_BROADCAST, .seq = 0, .len = 1,
                     .payload = payload};
    uint8_t wire[DHP_WIRE_MAX];
    size_t n = 0;
    dhp_frame_encode(&f, KEY, wire, sizeof(wire), &n);
    for (size_t i = 0; i < n; i++) {
        dhp_frame_t got;
        dhp_link_rx_byte(&b.node[1].link, DHP_PORT_UP, wire[i], 101, &got);
    }
    pump(&b, 101);

    CHECK_EQ(b.node[2].delivered, first); /* nothing new got through */
    CHECK(b.node[1].link.n_replay > 0);
}

/* Neighbour presence, and therefore head/tail position, follows real traffic
 * rather than configuration. */
static void test_neighbour_detection(void)
{
    struct bus b;
    bus_init(&b);

    /* Before any traffic, every board believes it is alone -- which is
     * correct, since it has no evidence otherwise. */
    CHECK(dhp_link_is_head(&b.node[1].link));
    CHECK(dhp_link_is_tail(&b.node[1].link));

    dhp_link_send(&b.node[0].link, DHP_MSG_PING, DHP_ADDR_BROADCAST, NULL, 0,
                  100);
    dhp_link_send(&b.node[2].link, DHP_MSG_PING, DHP_ADDR_BROADCAST, NULL, 0,
                  100);
    pump(&b, 100);

    /* The middle board now has neighbours on both sides. */
    CHECK(!dhp_link_is_head(&b.node[1].link));
    CHECK(!dhp_link_is_tail(&b.node[1].link));

    /* The ends know they are ends. */
    CHECK(dhp_link_is_head(&b.node[0].link));
    CHECK(dhp_link_is_tail(&b.node[2].link));

    /* Silence for longer than the neighbour timeout retracts the claim, which
     * is how unplugging a board becomes observable. */
    dhp_link_tick(&b.node[1].link, 100 + 600);
    CHECK(dhp_link_is_head(&b.node[1].link));
}

/* Without the chain key a board can neither originate nor act on traffic. */
static void test_unpaired_board_is_inert(void)
{
    struct bus b;
    bus_init(&b);
    dhp_link_clear_key(&b.node[1].link);

    CHECK_EQ(dhp_link_send(&b.node[1].link, DHP_MSG_HELLO, DHP_ADDR_BROADCAST,
                           NULL, 0, 100),
             DHP_ERR_STATE);

    /* And a HELLO arriving from a properly-keyed board is refused, because an
     * unpaired board must not be usable as a bridge into somebody's chain. */
    const uint8_t payload[16] = {0};
    dhp_link_send(&b.node[0].link, DHP_MSG_HELLO, DHP_ADDR_BROADCAST, payload,
                  16, 100);
    pump(&b, 100);
    CHECK_EQ(b.node[1].delivered, 0);
}

void test_link_suite(void)
{
    RUN(test_relay_along_chain);
    RUN(test_broadcast_reaches_all_once);
    RUN(test_replay_suppressed);
    RUN(test_neighbour_detection);
    RUN(test_unpaired_board_is_inert);
}
