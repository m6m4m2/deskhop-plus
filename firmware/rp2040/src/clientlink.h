/* SPDX-License-Identifier: GPL-2.0-only
 *
 * The board's proxy for the level 3 client on its own machine.
 *
 * A third HID interface carries a byte stream in both directions, framed
 * exactly like the chain but under the published local key (see
 * client/src/boardlink.h for why that is not a secret). This module is the
 * gate between that stream and the chain.
 *
 * The rule it enforces
 * --------------------
 * Only DHP_MSG_DATA crosses from the client onto the chain, and its source
 * address is overwritten with this board's own. Everything else -- a HELLO
 * claiming a priority, a FOCUS moving the user, a KBD report -- is counted and
 * dropped.
 *
 * That single restriction is what keeps the chain's trust model intact while a
 * client exists. Without it, any process that could open the HID node could
 * advertise itself as the most capable board, win the election, and become the
 * thing that decides where every keystroke goes.
 *
 * Bandwidth
 * ---------
 * A full-speed interrupt endpoint moves 64 bytes per millisecond, so this link
 * tops out around 64 KB/s of wire and roughly 45 KB/s of content -- slower
 * than the chain it feeds. The client link, not the chain, is therefore the
 * level 3 bottleneck, and it is another reason anything large should travel by
 * DHP_DATA_SHARE rather than through the cable.
 *
 * To get near that ceiling, outbound bytes are queued and each poll drains a
 * full report rather than sending one report per frame; a frame that does not
 * fill a report would otherwise waste most of a millisecond.
 */
#ifndef DHP_CLIENTLINK_H
#define DHP_CLIENTLINK_H

#include "dhp/frame.h"
#include "dhp/link.h"
#include "dhp/local.h"
#include "dhp/router.h"

#define CLIENTLINK_REPORT_BYTES 64

/* A client that has said nothing for this long is treated as gone, and the
 * board stops advertising the level 3 capabilities it was standing in for. */
#define CLIENTLINK_TIMEOUT_MS 5000

typedef struct {
    dhp_link_t   *chain;     /* where proxied frames go */
    dhp_router_t *router;    /* for the status message */

    dhp_framer_t  framer;    /* decodes the stream coming from the client */
    uint8_t       key[DHP_SIPHASH_KEY_BYTES];

    /* Outbound queue toward the client. */
    uint8_t  tx[2048];
    uint16_t tx_head, tx_tail;
    uint16_t tx_seq;         /* for frames this board originates locally */

    bool       attached;
    uint16_t   client_caps;
    dhp_time_t last_rx;
    dhp_time_t next_status;

    uint32_t n_proxied;      /* DATA frames put on the chain for the client */
    uint32_t n_to_client;
    uint32_t n_refused;      /* non-DATA the client tried to send */
} clientlink_t;

void clientlink_init(clientlink_t *cl, dhp_link_t *chain, dhp_router_t *router);

/* Feed one HID OUT report received from the host. */
void clientlink_rx_report(clientlink_t *cl, const uint8_t *data, uint16_t len,
                          dhp_time_t now);

/* A DATA frame arrived from the chain addressed to this board. Hands it to the
 * client verbatim, preserving the original source so the client knows which
 * machine it is talking to. Returns false if there is no client to give it to,
 * in which case the frame is dropped -- correctly, since nobody could
 * reassemble it. */
bool clientlink_deliver(clientlink_t *cl, const dhp_frame_t *f);

/* Drains one report toward the client if there is anything queued and the
 * endpoint is free. Call from the main loop. */
void clientlink_task(clientlink_t *cl, dhp_time_t now);

/* Capabilities the board should advertise on the client's behalf, or zero. */
uint16_t clientlink_caps(const clientlink_t *cl);

static inline bool clientlink_attached(const clientlink_t *cl)
{
    return cl->attached;
}

#endif /* DHP_CLIENTLINK_H */
