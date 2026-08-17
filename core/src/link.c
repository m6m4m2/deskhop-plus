/* SPDX-License-Identifier: GPL-2.0-only */
#include "dhp/link.h"

#include <string.h>

#define DHP_NEIGHBOUR_TIMEOUT_MS_DEFAULT 500

void dhp_link_init(dhp_link_t *l, dhp_addr_t self, dhp_link_tx_fn tx,
                   void *tx_ctx)
{
    memset(l, 0, sizeof(*l));
    l->self = self;
    l->tx = tx;
    l->tx_ctx = tx_ctx;
    l->neighbour_timeout_ms = DHP_NEIGHBOUR_TIMEOUT_MS_DEFAULT;
    for (int i = 0; i < DHP_PORT_COUNT; i++) {
        dhp_framer_init(&l->port[i].framer);
    }
}

void dhp_link_set_key(dhp_link_t *l, const uint8_t key[DHP_SIPHASH_KEY_BYTES])
{
    memcpy(l->key, key, DHP_SIPHASH_KEY_BYTES);
    l->has_key = true;
}

void dhp_link_clear_key(dhp_link_t *l)
{
    memset(l->key, 0, sizeof(l->key));
    l->has_key = false;
}

bool dhp_link_has_neighbour(const dhp_link_t *l, dhp_port_t port)
{
    if (port >= DHP_PORT_COUNT) {
        return false;
    }
    return l->port[port].neighbour;
}

/* ---- replay window --------------------------------------------------- */

static dhp_link_seen_t *seen_get(dhp_link_t *l, dhp_addr_t addr)
{
    for (int i = 0; i < DHP_MAX_BOARDS; i++) {
        if (l->seen[i].used && l->seen[i].addr == addr) {
            return &l->seen[i];
        }
    }
    for (int i = 0; i < DHP_MAX_BOARDS; i++) {
        if (!l->seen[i].used) {
            l->seen[i].used = true;
            l->seen[i].addr = addr;
            l->seen[i].high_seq = 0;
            return &l->seen[i];
        }
    }
    return NULL;
}

/* Accept a sequence number only if it is ahead of the high-water mark, using
 * signed wraparound comparison so the 16-bit counter can roll over safely.
 * A board that reboots restarts its counter, so a source that appears to have
 * jumped far backwards is treated as a restart rather than an attack -- the
 * MAC has already established that it holds the chain key. */
static bool seq_is_fresh(dhp_link_t *l, dhp_addr_t src, uint16_t seq)
{
    dhp_link_seen_t *s = seen_get(l, src);
    if (!s) {
        return true; /* table full: fail open rather than deafen the chain */
    }

    if (!s->primed) {
        /* First frame ever seen from this source. Without this the very first
         * frame a board sends -- sequence number zero, against a high-water
         * mark initialised to zero -- would be mistaken for a replay and
         * silently dropped. */
        s->primed = true;
        s->high_seq = seq;
        return true;
    }

    const int16_t delta = (int16_t)(seq - s->high_seq);
    if (delta > 0) {
        s->high_seq = seq;
        return true;
    }

    /* A large negative jump means the peer restarted and its counter went back
     * to zero. Resynchronise instead of ignoring it forever. */
    if (delta < -1024) {
        s->high_seq = seq;
        return true;
    }

    return false;
}

/* ---- transmit -------------------------------------------------------- */

static dhp_result_t emit_frame(dhp_link_t *l, dhp_port_t port,
                               const dhp_frame_t *f)
{
    if (!l->has_key) {
        return DHP_ERR_STATE;
    }

    uint8_t wire[DHP_WIRE_MAX];
    size_t n = 0;
    const dhp_result_t r = dhp_frame_encode(f, l->key, wire, sizeof(wire), &n);
    if (r != DHP_OK) {
        return r;
    }

    if (l->tx) {
        l->tx(l->tx_ctx, port, wire, n);
    }
    l->port[port].tx_frames++;
    return DHP_OK;
}

dhp_result_t dhp_link_send_port(dhp_link_t *l, dhp_port_t port, uint8_t type,
                                dhp_addr_t dst, const uint8_t *payload,
                                uint8_t len, dhp_time_t now)
{
    (void)now;
    if (port >= DHP_PORT_COUNT) {
        return DHP_ERR_INVAL;
    }
    const dhp_frame_t f = {
        .type = type, .ttl = DHP_TTL_DEFAULT, .src = l->self, .dst = dst,
        .seq = l->tx_seq, .len = len, .payload = payload,
    };
    const dhp_result_t r = emit_frame(l, port, &f);
    if (r == DHP_OK) {
        l->tx_seq++;
    }
    return r;
}

dhp_result_t dhp_link_send(dhp_link_t *l, uint8_t type, dhp_addr_t dst,
                           const uint8_t *payload, uint8_t len, dhp_time_t now)
{
    (void)now;

    /* One sequence number for both copies: they are the same frame, and the
     * receiving board must treat the second copy it sees as a duplicate. */
    const dhp_frame_t f = {
        .type = type, .ttl = DHP_TTL_DEFAULT, .src = l->self, .dst = dst,
        .seq = l->tx_seq, .len = len, .payload = payload,
    };

    /* Both ports unconditionally, including ports with no known neighbour.
     * A neighbour only becomes known by receiving from it, so gating
     * transmission on that flag would deadlock every board into waiting for
     * somebody else to speak first and no chain would ever form. Bytes into
     * an unconnected UART cost nothing. */
    dhp_result_t worst = DHP_OK;
    for (int p = 0; p < DHP_PORT_COUNT; p++) {
        const dhp_result_t r = emit_frame(l, (dhp_port_t)p, &f);
        if (r != DHP_OK) {
            worst = r;
        }
    }
    l->tx_seq++;
    return worst;
}

/* ---- receive --------------------------------------------------------- */

static void relay(dhp_link_t *l, dhp_port_t in_port, const dhp_frame_t *f)
{
    const dhp_port_t out = (in_port == DHP_PORT_UP) ? DHP_PORT_DOWN
                                                    : DHP_PORT_UP;

    /* Relay unconditionally, for the same reason send does: the neighbour flag
     * is evidence of past traffic, not of a cable, and relaying into an empty
     * port is harmless while failing to relay is not. */

    /* Relay the body verbatim apart from the hop limit. The MAC never covered
     * the TTL, so the originator's authentication survives the whole journey
     * and every board along the way verifies the same tag. */
    uint8_t body[DHP_BODY_MAX];
    const size_t body_len = f->body_len;
    if (body_len == 0 || body_len > DHP_BODY_MAX) {
        return;
    }
    memcpy(body, f->body, body_len);

    if (dhp_frame_forward_prepare(body, body_len) != DHP_OK) {
        l->n_ttl_drop++;
        return;
    }

    /* Re-stuff for the wire. */
    uint8_t wire[DHP_WIRE_MAX];
    size_t n = 0;
    wire[n++] = DHP_FLAG;
    for (size_t i = 0; i < body_len; i++) {
        const uint8_t b = body[i];
        if (b == DHP_FLAG || b == DHP_ESC) {
            wire[n++] = DHP_ESC;
            wire[n++] = (uint8_t)(b ^ DHP_ESC_XOR);
        } else {
            wire[n++] = b;
        }
    }
    wire[n++] = DHP_FLAG;

    if (l->tx) {
        l->tx(l->tx_ctx, out, wire, n);
    }
    l->port[out].tx_frames++;
    l->port[in_port].forwarded++;
}

dhp_result_t dhp_link_rx_byte(dhp_link_t *l, dhp_port_t port, uint8_t byte,
                              dhp_time_t now, dhp_frame_t *out)
{
    if (port >= DHP_PORT_COUNT) {
        return DHP_ERR_INVAL;
    }
    dhp_link_port_t *p = &l->port[port];

    /* Before pairing there is no chain key, so the MAC cannot be checked.
     * Passing NULL lets pairing traffic through; everything else is filtered
     * out below, so an unpaired board can pair and do nothing else. */
    const uint8_t *key = l->has_key ? l->key : NULL;

    dhp_frame_t f;
    const dhp_result_t r = dhp_framer_push(&p->framer, byte, key, &f);
    if (r != DHP_OK) {
        return r;
    }

    p->rx_frames++;
    p->last_rx = now;
    p->neighbour = true;

    if (!l->has_key && f.type != DHP_MSG_PAIR) {
        /* Unauthenticated and not a pairing frame: refuse to act on it, and
         * refuse to relay it, so an unpaired board cannot be used as a bridge
         * into somebody else's chain. */
        return DHP_ERR_AUTH;
    }

    if (f.src == l->self) {
        return DHP_ERR_AGAIN; /* our own frame came back: ignore */
    }

    if (!seq_is_fresh(l, f.src, f.seq)) {
        l->n_replay++;
        return DHP_ERR_REPLAY;
    }

    const bool for_us = (f.dst == l->self) || (f.dst == DHP_ADDR_BROADCAST);
    const bool relay_on = (f.dst != l->self);

    if (relay_on) {
        relay(l, port, &f);
    }

    if (for_us) {
        *out = f;
        return DHP_OK;
    }
    return DHP_ERR_AGAIN;
}

void dhp_link_tick(dhp_link_t *l, dhp_time_t now)
{
    for (int i = 0; i < DHP_PORT_COUNT; i++) {
        dhp_link_port_t *p = &l->port[i];
        if (p->neighbour &&
            dhp_time_after(now, p->last_rx + l->neighbour_timeout_ms)) {
            p->neighbour = false;
            /* Reset the decoder: whatever comes back may start mid-frame, and
             * a stale partial body must not be spliced onto it. */
            dhp_framer_init(&p->framer);
        }
    }
}
