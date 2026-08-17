/* SPDX-License-Identifier: GPL-2.0-only
 *
 * The board's client proxy.
 *
 * This is the gate that keeps the chain's trust model intact once a level 3
 * client exists, so it is tested directly rather than only reasoned about.
 * The firmware source is compiled into this host build unchanged; only the two
 * USB entry points below are stubbed, because nothing else in it touches
 * hardware.
 */
#include "../firmware/rp2040/src/clientlink.h"
#include "dhp/local.h"
#include "test_util.h"

/* --- stubs for the USB side --- */
static uint8_t g_to_client[8192];
static size_t  g_to_client_len;
static bool    g_endpoint_ready = true;

bool hid_bridge_vendor_ready(void) { return g_endpoint_ready; }

void hid_bridge_send_vendor(const uint8_t *report, uint16_t len)
{
    if (g_to_client_len + len <= sizeof(g_to_client)) {
        memcpy(g_to_client + g_to_client_len, report, len);
        g_to_client_len += len;
    }
}

/* --- a chain the proxy can emit onto --- */
static uint8_t g_to_chain[8192];
static size_t  g_to_chain_len;

static void chain_tx(void *ctx, dhp_port_t port, const uint8_t *d, size_t n)
{
    (void)ctx;
    if (port != DHP_PORT_DOWN) {
        return; /* count each frame once; send() emits on both ports */
    }
    if (g_to_chain_len + n <= sizeof(g_to_chain)) {
        memcpy(g_to_chain + g_to_chain_len, d, n);
        g_to_chain_len += n;
    }
}

static const uint8_t CHAIN_KEY[16] = {1, 1, 2, 3, 5, 8, 13, 21,
                                      34, 55, 89, 144, 233, 21, 9, 7};
static const uint8_t LOCAL_KEY[16] = {'d', 'h', 'p', '-', 'l', 'o', 'c', 'a',
                                      'l', '-', 'v', '1', 0, 0, 0, 0};

#define BOARD_ADDR 0x0007

typedef struct {
    dhp_link_t   chain;
    dhp_router_t router;
    clientlink_t cl;
} rig_t;

static void rig_init(rig_t *r)
{
    g_to_chain_len = 0;
    g_to_client_len = 0;
    g_endpoint_ready = true;

    dhp_link_init(&r->chain, BOARD_ADDR, chain_tx, NULL);
    dhp_link_set_key(&r->chain, CHAIN_KEY);

    const dhp_router_cfg_t cfg = {
        .self = BOARD_ADDR, .uid = 0x1234, .caps = DHP_CAP_HID_OUT,
        .timing = DHP_UHRP_TIMING_DEFAULT, .pointer = DHP_POINTER_CFG_DEFAULT,
    };
    dhp_router_init(&r->router, &cfg, &r->chain, NULL);
    clientlink_init(&r->cl, &r->chain, &r->router);
}

/* Feed the proxy a frame as though the client had sent it. */
static void from_client(rig_t *r, uint8_t type, dhp_addr_t src, dhp_addr_t dst,
                        const uint8_t *payload, uint8_t len, dhp_time_t now)
{
    const dhp_frame_t f = {.type = type, .ttl = DHP_TTL_DEFAULT, .src = src,
                           .dst = dst, .seq = 1, .len = len,
                           .payload = payload};
    uint8_t wire[DHP_WIRE_MAX];
    size_t n = 0;
    dhp_frame_encode(&f, LOCAL_KEY, wire, sizeof(wire), &n);
    clientlink_rx_report(&r->cl, wire, (uint16_t)n, now);
}

static void attach(rig_t *r, uint16_t caps, dhp_time_t now)
{
    const dhp_local_attach_t a = {.caps = caps};
    uint8_t buf[DHP_LOCAL_ATTACH_BYTES];
    dhp_local_attach_pack(&a, buf);
    from_client(r, DHP_MSG_CAP, 0x00FF, DHP_ADDR_BROADCAST, buf, sizeof(buf),
                now);
}

/* Decode whatever reached the chain. Returns the count and the last frame. */
static int chain_frames(dhp_frame_t *last)
{
    /* Static, not automatic. A decoded frame's payload points into the
     * framer's buffer -- dhp_framer_push() documents that it stays valid only
     * until the next call -- so a framer on this stack would leave *last
     * dangling the moment this function returned. ASan caught exactly that. */
    static dhp_framer_t fr;
    dhp_framer_init(&fr);
    int n = 0;
    for (size_t i = 0; i < g_to_chain_len; i++) {
        dhp_frame_t f = {0};
        if (dhp_framer_push(&fr, g_to_chain[i], CHAIN_KEY, &f) == DHP_OK) {
            n++;
            if (last) {
                *last = f;
            }
        }
    }
    return n;
}

/* ---------------------------------------------------------------------- */

static void test_data_is_proxied(void)
{
    rig_t r;
    rig_init(&r);
    attach(&r, DHP_CAP_CLIPBOARD, 1000);
    g_to_chain_len = 0;

    const uint8_t payload[] = {0xAA, 0xBB, 0xCC};
    from_client(&r, DHP_MSG_DATA, 0x00FF, 0x0009, payload, 3, 1000);

    dhp_frame_t got;
    CHECK_EQ(chain_frames(&got), 1);
    CHECK_EQ(got.type, DHP_MSG_DATA);
    CHECK_EQ(got.dst, 0x0009);
    CHECK_EQ(got.len, 3);
    CHECK_EQ(memcmp(got.payload, payload, 3), 0);
    CHECK_EQ(r.cl.n_proxied, 1);
}

/* The security property, stated as a test: nothing but DATA gets out. */
static void test_only_data_reaches_the_chain(void)
{
    rig_t r;
    rig_init(&r);
    attach(&r, DHP_CAP_CLIPBOARD, 1000);
    g_to_chain_len = 0;

    /* A client trying to claim the routing role. */
    dhp_hello_t h;
    memset(&h, 0, sizeof(h));
    h.state = DHP_UHRP_ACTIVE;
    h.priority = 255;
    h.uid = 0xFFFFFFFFFFFFFFFFULL;
    h.caps = DHP_CAP_COORD;
    uint8_t hb[DHP_HELLO_BYTES];
    dhp_hello_pack(&h, hb);
    from_client(&r, DHP_MSG_HELLO, 0x00FF, DHP_ADDR_BROADCAST, hb, sizeof(hb),
                1000);

    /* A client trying to type on another machine. */
    const dhp_kbd_report_t k = {.modifiers = 0x01, .keys = {0x04}};
    uint8_t kb[DHP_KBD_BYTES];
    dhp_kbd_pack(&k, kb);
    from_client(&r, DHP_MSG_KBD, 0x00FF, 0x0009, kb, sizeof(kb), 1000);

    /* A client trying to move the user somewhere else. */
    const dhp_focus_t fo = {.target = 0x0009, .reason = DHP_FOCUS_R_CFG};
    uint8_t fb[DHP_FOCUS_BYTES];
    dhp_focus_pack(&fo, fb);
    from_client(&r, DHP_MSG_FOCUS, 0x00FF, DHP_ADDR_BROADCAST, fb, sizeof(fb),
                1000);

    /* ...and a resignation, to unseat whoever is routing. */
    from_client(&r, DHP_MSG_RESIGN, 0x00FF, DHP_ADDR_BROADCAST, NULL, 0, 1000);

    CHECK_EQ(chain_frames(NULL), 0);
    CHECK_EQ(r.cl.n_refused, 4);
    CHECK_EQ(r.cl.n_proxied, 0);
}

/* A client cannot pretend to be another board either: the proxy substitutes
 * this board's address on the way out. */
static void test_source_cannot_be_spoofed(void)
{
    rig_t r;
    rig_init(&r);
    attach(&r, DHP_CAP_CLIPBOARD, 1000);
    g_to_chain_len = 0;

    const uint8_t payload[] = {1};
    from_client(&r, DHP_MSG_DATA, 0x0042 /* not us */, 0x0009, payload, 1, 1000);

    dhp_frame_t got;
    CHECK_EQ(chain_frames(&got), 1);
    CHECK_EQ(got.src, BOARD_ADDR);
}

/* Capability is advertised only for a client that is actually there, and only
 * the level 3 bits are honoured. */
static void test_caps_follow_the_client(void)
{
    rig_t r;
    rig_init(&r);
    CHECK_EQ(clientlink_caps(&r.cl), 0);
    CHECK(!clientlink_attached(&r.cl));

    attach(&r, DHP_CAP_CLIPBOARD | DHP_CAP_FILES, 1000);
    CHECK(clientlink_attached(&r.cl));
    CHECK_EQ(clientlink_caps(&r.cl), DHP_CAP_CLIPBOARD | DHP_CAP_FILES);

    /* Still nothing on the chain: an attach is local. */
    CHECK_EQ(chain_frames(NULL), 0);
}

/* A client announcing COORD or HID_IN would raise the board's election
 * priority from userspace, which is the escalation the whole arrangement
 * exists to prevent. */
static void test_client_cannot_claim_privileged_caps(void)
{
    rig_t r;
    rig_init(&r);
    attach(&r, (uint16_t)(DHP_CAP_COORD | DHP_CAP_HID_IN | DHP_CAP_DISPLAY |
                          DHP_CAP_CLIPBOARD),
           1000);

    const uint16_t caps = clientlink_caps(&r.cl);
    CHECK_EQ(caps & DHP_CAP_COORD, 0);
    CHECK_EQ(caps & DHP_CAP_HID_IN, 0);
    CHECK_EQ(caps & DHP_CAP_DISPLAY, 0);
    CHECK_EQ(caps & DHP_CAP_CLIPBOARD, DHP_CAP_CLIPBOARD);
}

/* A client that goes quiet must stop being advertised, or the chain would
 * report a clipboard on a machine that can no longer paste. */
static void test_silent_client_is_forgotten(void)
{
    rig_t r;
    rig_init(&r);
    attach(&r, DHP_CAP_CLIPBOARD, 1000);
    CHECK(clientlink_attached(&r.cl));

    clientlink_task(&r.cl, 1000 + CLIENTLINK_TIMEOUT_MS - 100);
    CHECK(clientlink_attached(&r.cl));

    clientlink_task(&r.cl, 1000 + CLIENTLINK_TIMEOUT_MS + 100);
    CHECK(!clientlink_attached(&r.cl));
    CHECK_EQ(clientlink_caps(&r.cl), 0);
}

/* Chain traffic reaches the client with its original source intact, because
 * dhp_data keys its streams by peer. */
static void test_chain_data_reaches_the_client(void)
{
    rig_t r;
    rig_init(&r);
    attach(&r, DHP_CAP_CLIPBOARD, 1000);
    g_to_client_len = 0;

    const uint8_t payload[] = {0x11, 0x22};
    const dhp_frame_t inbound = {
        .type = DHP_MSG_DATA, .ttl = 18, .src = 0x0009, .dst = BOARD_ADDR,
        .seq = 77, .len = 2, .payload = payload,
    };
    CHECK(clientlink_deliver(&r.cl, &inbound));

    /* Drain it toward the client. */
    for (int i = 0; i < 8; i++) {
        clientlink_task(&r.cl, 1000 + i);
    }

    dhp_framer_t fr;
    dhp_framer_init(&fr);
    int n = 0;
    dhp_frame_t got;
    memset(&got, 0, sizeof(got));
    for (size_t i = 0; i < g_to_client_len; i++) {
        dhp_frame_t f = {0};
        if (dhp_framer_push(&fr, g_to_client[i], LOCAL_KEY, &f) == DHP_OK) {
            if (f.type == DHP_MSG_DATA) {
                n++;
                got = f;
            }
        }
    }
    CHECK_EQ(n, 1);
    CHECK_EQ(got.src, 0x0009); /* NOT rewritten: the client needs the peer */
    CHECK_EQ(got.len, 2);
}

/* With nobody attached there is nothing to deliver to, and saying so lets the
 * caller drop the frame instead of queueing it forever. */
static void test_delivery_without_a_client_fails(void)
{
    rig_t r;
    rig_init(&r);
    const uint8_t payload[] = {1};
    const dhp_frame_t f = {.type = DHP_MSG_DATA, .ttl = 18, .src = 9,
                           .dst = BOARD_ADDR, .seq = 1, .len = 1,
                           .payload = payload};
    CHECK(!clientlink_deliver(&r.cl, &f));
}

/* Garbage on the USB link must not wedge the proxy. */
static void test_noise_is_survivable(void)
{
    rig_t r;
    rig_init(&r);

    uint32_t seed = 99;
    for (int i = 0; i < 4000; i++) {
        seed = seed * 1103515245u + 12345u;
        const uint8_t b = (uint8_t)(seed >> 16);
        clientlink_rx_report(&r.cl, &b, 1, 1000);
    }
    CHECK_EQ(chain_frames(NULL), 0);

    attach(&r, DHP_CAP_CLIPBOARD, 1000);
    CHECK(clientlink_attached(&r.cl));
}

void test_clientlink_suite(void)
{
    RUN(test_data_is_proxied);
    RUN(test_only_data_reaches_the_chain);
    RUN(test_source_cannot_be_spoofed);
    RUN(test_caps_follow_the_client);
    RUN(test_client_cannot_claim_privileged_caps);
    RUN(test_silent_client_is_forgotten);
    RUN(test_chain_data_reaches_the_client);
    RUN(test_delivery_without_a_client_fails);
    RUN(test_noise_is_survivable);
}
