/* SPDX-License-Identifier: GPL-2.0-only */
#include "clientlink.h"

#include <string.h>

/* Deliberately no tusb.h: this file is the gate between the client and the
 * chain, and nothing in it needs USB. Keeping it free of hardware headers is
 * what lets the rule it enforces be tested on a development machine -- see
 * tests/test_clientlink.c -- rather than only reasoned about. */
#include "hid_bridge.h"

/* Must match client/src/boardlink.h. Deliberately not a secret. */
static const uint8_t LOCAL_KEY[DHP_SIPHASH_KEY_BYTES] = {
    'd', 'h', 'p', '-', 'l', 'o', 'c', 'a', 'l', '-', 'v', '1', 0, 0, 0, 0,
};

void clientlink_init(clientlink_t *cl, dhp_link_t *chain, dhp_router_t *router)
{
    memset(cl, 0, sizeof(*cl));
    cl->chain = chain;
    cl->router = router;
    memcpy(cl->key, LOCAL_KEY, sizeof(cl->key));
    dhp_framer_init(&cl->framer);
}

/* ---- outbound queue ---------------------------------------------- */

static void tx_push(clientlink_t *cl, const uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        const uint16_t next = (uint16_t)((cl->tx_head + 1) % sizeof(cl->tx));
        if (next == cl->tx_tail) {
            /* The client is not draining. Dropping is right: every level 3
             * message is either retransmitted by dhp_data or is a status
             * update that will be sent again shortly. */
            return;
        }
        cl->tx[cl->tx_head] = d[i];
        cl->tx_head = next;
    }
}

static uint16_t tx_pending(const clientlink_t *cl)
{
    return (uint16_t)((cl->tx_head + sizeof(cl->tx) - cl->tx_tail) %
                      sizeof(cl->tx));
}

/* ---- frames toward the client ------------------------------------ */

static void send_local(clientlink_t *cl, uint8_t type, dhp_addr_t src,
                       dhp_addr_t dst, uint16_t seq, const uint8_t *payload,
                       uint8_t len)
{
    const dhp_frame_t f = {
        .type = type, .ttl = DHP_TTL_DEFAULT, .src = src, .dst = dst,
        .seq = seq, .len = len, .payload = payload,
    };

    uint8_t wire[DHP_WIRE_MAX];
    size_t n = 0;
    if (dhp_frame_encode(&f, cl->key, wire, sizeof(wire), &n) == DHP_OK) {
        tx_push(cl, wire, n);
    }
}

bool clientlink_deliver(clientlink_t *cl, const dhp_frame_t *f)
{
    if (!cl->attached) {
        return false;
    }
    /* Source preserved: dhp_data on the client keys its streams by peer, so
     * rewriting it here would merge every machine's transfers into one. */
    send_local(cl, f->type, f->src, f->dst, f->seq, f->payload, f->len);
    cl->n_to_client++;
    return true;
}

static void send_status(clientlink_t *cl, dhp_time_t now)
{
    const dhp_local_status_t st = {
        .self = cl->router->cfg.self,
        .focus = dhp_router_focus(cl->router),
        .level = (uint8_t)dhp_router_level(cl->router),
        .boards = dhp_router_chain_size(cl->router),
    };
    uint8_t buf[DHP_LOCAL_STATUS_BYTES];
    dhp_local_status_pack(&st, buf);

    send_local(cl, DHP_MSG_CAP, cl->router->cfg.self, 0x00FF, cl->tx_seq++, buf,
               sizeof(buf));
    cl->next_status = now + 1000;
}

/* ---- frames from the client -------------------------------------- */

static void handle_from_client(clientlink_t *cl, const dhp_frame_t *f,
                               dhp_time_t now)
{
    cl->last_rx = now;

    if (f->type == DHP_MSG_CAP) {
        dhp_local_attach_t a;
        if (dhp_local_attach_unpack(f->payload, f->len, &a)) {
            /* Only the level 3 bits are honoured. A client announcing
             * DHP_CAP_COORD or DHP_CAP_HID_IN would otherwise raise this
             * board's election priority from userspace, which is exactly the
             * escalation this whole arrangement exists to prevent. */
            cl->client_caps = a.caps & (uint16_t)(DHP_CAP_CLIPBOARD |
                                                  DHP_CAP_FILES |
                                                  DHP_CAP_IMAGES |
                                                  DHP_CAP_SHARE);
            if (!cl->attached) {
                cl->attached = true;
                cl->next_status = 0; /* tell it who it is immediately */
            }
        }
        return; /* never reaches the chain */
    }

    if (f->type != DHP_MSG_DATA) {
        /* A HELLO claiming a priority, a FOCUS moving the user, a KBD report:
         * all refused. See the header. */
        cl->n_refused++;
        return;
    }

    if (!cl->attached) {
        return;
    }

    /* Sent through dhp_link_send rather than re-encoded by hand, so the source
     * is this board's address and the sequence number comes from the board's
     * single counter. Preserving the client's own numbering would put two
     * independent counters behind one source address, and every peer's replay
     * window would start discarding one of them. */
    dhp_link_send(cl->chain, DHP_MSG_DATA, f->dst, f->payload, f->len, now);
    cl->n_proxied++;
}

void clientlink_rx_report(clientlink_t *cl, const uint8_t *data, uint16_t len,
                          dhp_time_t now)
{
    for (uint16_t i = 0; i < len; i++) {
        dhp_frame_t f;
        if (dhp_framer_push(&cl->framer, data[i], cl->key, &f) == DHP_OK) {
            handle_from_client(cl, &f, now);
        }
    }
}

/* ---- periodic ----------------------------------------------------- */

void clientlink_task(clientlink_t *cl, dhp_time_t now)
{
    if (cl->attached && dhp_time_after(now, cl->last_rx + CLIENTLINK_TIMEOUT_MS)) {
        /* The client stopped answering: stop advertising what it provided, so
         * the chain's level follows what is actually deliverable. */
        cl->attached = false;
        cl->client_caps = 0;
        cl->tx_head = cl->tx_tail = 0;
        dhp_framer_init(&cl->framer);
        return;
    }

    if (!cl->attached) {
        return;
    }

    if (dhp_time_after(now, cl->next_status)) {
        send_status(cl, now);
    }

    /* One full report per opportunity rather than one frame, so a short frame
     * does not waste most of a millisecond of a 64 KB/s link. */
    if (tx_pending(cl) == 0 || !hid_bridge_vendor_ready()) {
        return;
    }

    uint8_t report[CLIENTLINK_REPORT_BYTES];
    uint16_t n = 0;
    while (n < sizeof(report) && cl->tx_tail != cl->tx_head) {
        report[n++] = cl->tx[cl->tx_tail];
        cl->tx_tail = (uint16_t)((cl->tx_tail + 1) % sizeof(cl->tx));
    }
    /* Pad with flag bytes. A flag between frames is idle filler by definition,
     * so the receiver discards it without needing a length field. */
    while (n < sizeof(report)) {
        report[n++] = DHP_FLAG;
    }

    hid_bridge_send_vendor(report, sizeof(report));
}

uint16_t clientlink_caps(const clientlink_t *cl)
{
    return cl->attached ? cl->client_caps : 0;
}
