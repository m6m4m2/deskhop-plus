/* SPDX-License-Identifier: GPL-2.0-only
 *
 * The chain link: two ports, store-and-forward relaying, replay rejection.
 *
 * Topology is a line, not a bus or a ring. Each board has an up port (toward
 * the head) and a down port (toward the tail), which is exactly the two UARTs
 * an RP2040 provides -- so "extend the chain by adding a board" needs no hub,
 * no addressing scheme, and no arbitration. A board with nothing on its up
 * port is the head; nothing on its down port makes it the tail.
 *
 * A line has no cycles, so a frame is relayed out the *opposite* port from the
 * one it arrived on and can never come back. The hop limit in the frame header
 * is therefore a backstop against miswiring rather than the primary loop
 * defence, and the replay window closes the remaining gap: a frame whose
 * sequence number has already been seen from that source is neither delivered
 * nor relayed.
 */
#ifndef DHP_LINK_H
#define DHP_LINK_H

#include "dhp/frame.h"
#include "dhp/msg.h"
#include "dhp/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Called when the link wants bytes put on a wire. DHP_PORT_UP / DHP_PORT_DOWN
 * map to the two UARTs. */
typedef void (*dhp_link_tx_fn)(void *ctx, dhp_port_t port, const uint8_t *data,
                               size_t len);

typedef struct {
    dhp_framer_t framer;
    dhp_time_t   last_rx;
    bool         neighbour;   /* have we ever heard a valid frame here */
    uint32_t     tx_frames;
    uint32_t     rx_frames;
    uint32_t     forwarded;
} dhp_link_port_t;

typedef struct {
    bool       used;
    bool       primed;   /* have we accepted anything from this source yet */
    dhp_addr_t addr;
    uint16_t   high_seq;
} dhp_link_seen_t;

typedef struct {
    dhp_addr_t self;
    uint8_t    key[DHP_SIPHASH_KEY_BYTES];
    bool       has_key;
    uint16_t   tx_seq;

    /* A neighbour that stops sending anything at all for this long is treated
     * as gone, which is what makes unplugging a board observable. */
    uint16_t   neighbour_timeout_ms;

    dhp_link_tx_fn tx;
    void          *tx_ctx;

    dhp_link_port_t port[DHP_PORT_COUNT];
    dhp_link_seen_t seen[DHP_MAX_BOARDS];

    uint32_t n_replay;
    uint32_t n_ttl_drop;
} dhp_link_t;

void dhp_link_init(dhp_link_t *l, dhp_addr_t self, dhp_link_tx_fn tx,
                   void *tx_ctx);

/* Install the chain key. Until this is called, only DHP_MSG_PAIR frames are
 * accepted -- an unpaired board can take part in pairing and nothing else. */
void dhp_link_set_key(dhp_link_t *l, const uint8_t key[DHP_SIPHASH_KEY_BYTES]);

void dhp_link_clear_key(dhp_link_t *l);

/* Feed one received byte from a port.
 *
 * Returns DHP_OK and fills *out when a frame addressed to this board (or
 * broadcast) has arrived; the payload pointer stays valid until the next call
 * for the same port. Frames addressed elsewhere are relayed internally and
 * reported as DHP_ERR_AGAIN, so the caller only ever sees its own traffic. */
dhp_result_t dhp_link_rx_byte(dhp_link_t *l, dhp_port_t port, uint8_t byte,
                              dhp_time_t now, dhp_frame_t *out);

/* Originate a frame. Broadcasts go out both ports; unicast goes out both as
 * well, because a line topology gives no way to know which side an address is
 * on until topology discovery has run, and the wrong-way copy simply dies at
 * the end of the chain. */
dhp_result_t dhp_link_send(dhp_link_t *l, uint8_t type, dhp_addr_t dst,
                           const uint8_t *payload, uint8_t len, dhp_time_t now);

/* Send out one specific port only, once the chain order is known. */
dhp_result_t dhp_link_send_port(dhp_link_t *l, dhp_port_t port, uint8_t type,
                                dhp_addr_t dst, const uint8_t *payload,
                                uint8_t len, dhp_time_t now);

/* Age out neighbours that have gone quiet. Call from the main loop. */
void dhp_link_tick(dhp_link_t *l, dhp_time_t now);

bool dhp_link_has_neighbour(const dhp_link_t *l, dhp_port_t port);

/* True when this board sits at the head / tail of the chain. */
static inline bool dhp_link_is_head(const dhp_link_t *l)
{
    return !dhp_link_has_neighbour(l, DHP_PORT_UP);
}

static inline bool dhp_link_is_tail(const dhp_link_t *l)
{
    return !dhp_link_has_neighbour(l, DHP_PORT_DOWN);
}

#ifdef __cplusplus
}
#endif

#endif /* DHP_LINK_H */
