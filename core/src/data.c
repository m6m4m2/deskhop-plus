/* SPDX-License-Identifier: GPL-2.0-only */
#include "dhp/data.h"

#include <string.h>

#include "dhp/crc16.h"

/* ---- wire ------------------------------------------------------------ *
 *
 * Every DATA payload starts with a one-byte opcode and a one-byte stream id.
 * Stream ids are chosen by the sender and are unique only per peer per
 * direction, which is why inbound and outbound slots are kept apart below.
 */
enum {
    OP_OFFER  = 0, /* kind:1 total:4 crc:4 name_len:1 name[] */
    OP_ACCEPT = 1, /* window:1 */
    OP_REJECT = 2, /* reason:1 */
    OP_CHUNK  = 3, /* seq:2 data[] */
    OP_ACK    = 4, /* next_seq:2 window:1 */
    OP_DONE   = 5, /* result:1 -- receiver's verdict */
    OP_ABORT  = 6, /* reason:1 */
};

#define OFFER_FIXED 11 /* op, id, kind, total(4), crc(4), name_len */

/* Sentinel for "no rewind outstanding"; a real sequence never reaches it. */
#define NO_REWIND 0xFFFFu

const char *dhp_data_result_name(uint8_t r)
{
    switch (r) {
    case DHP_DATA_OK:        return "ok";
    case DHP_DATA_REFUSED:   return "refused";
    case DHP_DATA_TIMEOUT:   return "timed out";
    case DHP_DATA_CORRUPT:   return "corrupt";
    case DHP_DATA_TOO_BIG:   return "too big";
    case DHP_DATA_CANCELLED: return "cancelled";
    case DHP_DATA_NO_STREAM: return "no free stream";
    default:                 return "?";
    }
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

static uint32_t get32(const uint8_t *p)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        v |= ((uint32_t)p[i]) << (8 * i);
    }
    return v;
}

/* End-to-end integrity, distinct from the per-hop CRC and MAC on every frame.
 * Those catch a corrupted wire; this catches a transfer reassembled wrongly --
 * a chunk duplicated, dropped or transposed -- which is the failure this layer
 * could itself cause and the one nothing below it would notice.
 *
 * Adler-32 rather than a CRC because it must be computed INCREMENTALLY. The
 * receiver streams chunks straight out to the application and never holds the
 * reassembled content, so any check needing a second pass over a complete
 * buffer is unavailable to it by construction. Adler-32 folds in one byte at a
 * time, costs no table, and is amply strong for detecting a misordered chunk. */
#define ADLER_INIT 1u
#define ADLER_MOD 65521u

static uint32_t adler32_update(uint32_t state, const uint8_t *data, uint32_t len)
{
    uint32_t a = state & 0xFFFFu;
    uint32_t b = (state >> 16) & 0xFFFFu;

    for (uint32_t i = 0; i < len; i++) {
        a += data[i];
        if (a >= ADLER_MOD) {
            a -= ADLER_MOD;
        }
        b += a;
        if (b >= ADLER_MOD) {
            b -= ADLER_MOD;
        }
    }
    return (b << 16) | a;
}

static uint32_t content_crc(const uint8_t *data, uint32_t len)
{
    return adler32_update(ADLER_INIT, data, len);
}

/* ---- streams --------------------------------------------------------- */

static dhp_data_stream_t *slot_free(dhp_data_t *d, bool outbound)
{
    /* Inbound and outbound are given separate halves of the table so that a
     * peer flooding offers cannot starve this machine's ability to send. */
    const int base = outbound ? 0 : DHP_DATA_MAX_STREAMS;
    for (int i = base; i < base + DHP_DATA_MAX_STREAMS; i++) {
        if (d->s[i].state == DHP_ST_FREE) {
            return &d->s[i];
        }
    }
    return NULL;
}

static dhp_data_stream_t *slot_find(dhp_data_t *d, dhp_addr_t peer, uint8_t id,
                                    bool outbound)
{
    const int base = outbound ? 0 : DHP_DATA_MAX_STREAMS;
    for (int i = base; i < base + DHP_DATA_MAX_STREAMS; i++) {
        dhp_data_stream_t *s = &d->s[i];
        if (s->state != DHP_ST_FREE && s->id == id && s->peer == peer) {
            return s;
        }
    }
    return NULL;
}

/* The public stream handle is the table index, so the app can refer to a
 * stream without knowing about peers or direction. */
static uint8_t handle_of(const dhp_data_t *d, const dhp_data_stream_t *s)
{
    return (uint8_t)(s - d->s);
}

static dhp_data_stream_t *by_handle(dhp_data_t *d, uint8_t h)
{
    if (h >= DHP_DATA_MAX_STREAMS * 2 || d->s[h].state == DHP_ST_FREE) {
        return NULL;
    }
    return &d->s[h];
}

static uint16_t chunks_for(uint32_t total)
{
    return (uint16_t)((total + DHP_DATA_CHUNK_BYTES - 1) / DHP_DATA_CHUNK_BYTES);
}

static void send_op(dhp_data_t *d, dhp_data_stream_t *s, uint8_t op,
                    const uint8_t *extra, uint8_t extra_len, dhp_time_t now)
{
    uint8_t buf[DHP_MAX_PAYLOAD];
    buf[0] = op;
    buf[1] = s->id;
    if (extra_len) {
        memcpy(&buf[2], extra, extra_len);
    }
    dhp_link_send(d->link, DHP_MSG_DATA, s->peer, buf, (uint8_t)(2 + extra_len),
                  now);
    s->last_tx = now;
}

static void finish(dhp_data_t *d, dhp_data_stream_t *s, uint8_t result)
{
    const uint8_t h = handle_of(d, s);
    const bool outbound = s->outbound;

    /* Clear before the callback: the app is allowed to start a new transfer
     * from inside it, and that must be able to reuse this slot. */
    memset(s, 0, sizeof(*s));

    if (outbound) {
        if (result == DHP_DATA_OK) {
            d->n_sent++;
        }
        if (d->hooks.on_sent) {
            d->hooks.on_sent(d->hooks.ctx, h, result);
        }
    } else {
        if (result == DHP_DATA_OK) {
            d->n_received++;
        }
        if (d->hooks.on_received) {
            d->hooks.on_received(d->hooks.ctx, h, result);
        }
    }
}

/* ---- init ------------------------------------------------------------ */

void dhp_data_init(dhp_data_t *d, dhp_link_t *link,
                   const dhp_data_hooks_t *hooks)
{
    memset(d, 0, sizeof(*d));
    d->link = link;
    if (hooks) {
        d->hooks = *hooks;
    }
    d->next_id = 1;

    /* Eight chunks in flight is about 480 bytes, which at 2 Mbps is roughly
     * 3 ms of wire -- enough to keep the link busy across one ack round trip
     * without letting a transfer monopolise it against keystrokes. */
    d->window = 8;
    d->ack_timeout_ms = 250;
    d->max_retries = 6;
    d->offer_timeout_ms = 1000;
    d->offer_retries = 8;
    /* Longer than the offer budget above, so a slow acceptance is decided by
     * the offer path rather than being cut short by the idle sweep. */
    d->idle_timeout_ms = 15000;
    d->max_inbound = 8u * 1024u * 1024u;
}

/* ---- sending --------------------------------------------------------- */

int dhp_data_send(dhp_data_t *d, dhp_addr_t to, uint8_t kind, const char *name,
                  const uint8_t *data, uint32_t len, dhp_time_t now)
{
    if (!data || len == 0) {
        return -DHP_DATA_CORRUPT;
    }

    dhp_data_stream_t *s = slot_free(d, true);
    if (!s) {
        return -DHP_DATA_NO_STREAM;
    }

    memset(s, 0, sizeof(*s));
    s->state = DHP_ST_OFFERING;
    s->outbound = true;
    s->id = d->next_id++;
    if (d->next_id == 0) {
        d->next_id = 1;
    }
    s->peer = to;
    s->kind = kind;
    s->total = len;
    s->src = data;
    s->crc = content_crc(data, len);
    s->window = d->window;
    s->rewound_to = NO_REWIND;
    s->last_rx = now;
    s->retries = 0;

    if (name) {
        size_t n = strlen(name);
        if (n > DHP_DATA_NAME_MAX) {
            n = DHP_DATA_NAME_MAX;
        }
        memcpy(s->name, name, n);
        s->name[n] = '\0';
    }

    uint8_t extra[1 + 4 + 4 + 1 + DHP_DATA_NAME_MAX];
    const uint8_t nl = (uint8_t)strlen(s->name);
    extra[0] = kind;
    put32(&extra[1], s->total);
    put32(&extra[5], s->crc);
    extra[9] = nl;
    memcpy(&extra[10], s->name, nl);

    send_op(d, s, OP_OFFER, extra, (uint8_t)(10 + nl), now);
    return handle_of(d, s);
}

/* Push chunks until the window is full or the content runs out. */
static void pump_sender(dhp_data_t *d, dhp_data_stream_t *s, dhp_time_t now)
{
    const uint16_t total_chunks = chunks_for(s->total);

    while (s->next_seq < total_chunks &&
           (uint16_t)(s->next_seq - s->acked) < s->window) {

        const uint32_t off = (uint32_t)s->next_seq * DHP_DATA_CHUNK_BYTES;
        uint32_t n = s->total - off;
        if (n > DHP_DATA_CHUNK_BYTES) {
            n = DHP_DATA_CHUNK_BYTES;
        }

        uint8_t extra[2 + DHP_DATA_CHUNK_BYTES];
        put16(&extra[0], s->next_seq);
        memcpy(&extra[2], s->src + off, n);

        send_op(d, s, OP_CHUNK, extra, (uint8_t)(2 + n), now);
        s->next_seq++;
    }

    if (s->next_seq >= total_chunks && s->state == DHP_ST_SENDING) {
        s->state = DHP_ST_DRAINING;
    }
}

/* ---- receiving ------------------------------------------------------- */

void dhp_data_accept(dhp_data_t *d, uint8_t stream, dhp_time_t now)
{
    dhp_data_stream_t *s = by_handle(d, stream);
    if (!s || s->state != DHP_ST_OFFERED) {
        return;
    }
    s->state = DHP_ST_RECEIVING;
    s->last_rx = now;

    uint8_t extra[1] = {d->window};
    send_op(d, s, OP_ACCEPT, extra, 1, now);
}

void dhp_data_reject(dhp_data_t *d, uint8_t stream, dhp_time_t now)
{
    dhp_data_stream_t *s = by_handle(d, stream);
    if (!s || s->state != DHP_ST_OFFERED) {
        return;
    }
    uint8_t extra[1] = {DHP_DATA_REFUSED};
    send_op(d, s, OP_REJECT, extra, 1, now);
    d->n_refused++;
    memset(s, 0, sizeof(*s));
}

void dhp_data_cancel(dhp_data_t *d, uint8_t stream, dhp_time_t now)
{
    dhp_data_stream_t *s = by_handle(d, stream);
    if (!s) {
        return;
    }
    uint8_t extra[1] = {DHP_DATA_CANCELLED};
    send_op(d, s, OP_ABORT, extra, 1, now);
    finish(d, s, DHP_DATA_CANCELLED);
}

static void send_ack(dhp_data_t *d, dhp_data_stream_t *s, dhp_time_t now)
{
    uint8_t extra[3];
    put16(&extra[0], s->next_seq);
    extra[2] = s->window;
    send_op(d, s, OP_ACK, extra, 3, now);
}

/* ---- receive path ---------------------------------------------------- */

static void rx_offer(dhp_data_t *d, dhp_addr_t from, uint8_t id,
                     const uint8_t *p, uint8_t len, dhp_time_t now)
{
    if (len < OFFER_FIXED - 2) {
        return;
    }

    const uint8_t kind = p[0];
    const uint32_t total = get32(&p[1]);
    const uint32_t crc = get32(&p[5]);
    uint8_t nl = p[9];
    if (nl > DHP_DATA_NAME_MAX || (uint32_t)10 + nl > len) {
        nl = 0;
    }

    dhp_data_stream_t *existing = slot_find(d, from, id, false);
    if (existing) {
        /* A repeated offer means our accept or reject was lost. Re-send the
         * answer rather than opening a second stream for the same transfer. */
        if (existing->state == DHP_ST_RECEIVING) {
            send_ack(d, existing, now);
        }
        return;
    }

    dhp_data_stream_t *s = slot_free(d, false);
    if (!s) {
        /* Refuse explicitly. Silence would leave the sender retrying an offer
         * that can never be accepted until its own timeout. */
        dhp_data_stream_t tmp;
        memset(&tmp, 0, sizeof(tmp));
        tmp.id = id;
        tmp.peer = from;
        uint8_t extra[1] = {DHP_DATA_NO_STREAM};
        send_op(d, &tmp, OP_REJECT, extra, 1, now);
        return;
    }

    memset(s, 0, sizeof(*s));
    s->state = DHP_ST_OFFERED;
    s->outbound = false;
    s->id = id;
    s->peer = from;
    s->kind = kind;
    s->total = total;
    s->crc = crc;
    s->window = d->window;
    s->running = ADLER_INIT;
    s->last_rx = now;
    memcpy(s->name, &p[10], nl);
    s->name[nl] = '\0';

    if (total == 0 || total > d->max_inbound) {
        uint8_t extra[1] = {DHP_DATA_TOO_BIG};
        send_op(d, s, OP_REJECT, extra, 1, now);
        memset(s, 0, sizeof(*s));
        return;
    }

    if (d->hooks.on_offer) {
        d->hooks.on_offer(d->hooks.ctx, handle_of(d, s), from, kind, total,
                          s->name);
    }
}

static void rx_chunk(dhp_data_t *d, dhp_data_stream_t *s, const uint8_t *p,
                     uint8_t len, dhp_time_t now)
{
    if (len < 2 || s->state != DHP_ST_RECEIVING) {
        return;
    }

    const uint16_t seq = get16(&p[0]);
    const uint8_t n = (uint8_t)(len - 2);
    s->last_rx = now;

    if (seq != s->next_seq) {
        /* Go-back-N: anything out of order is discarded and the sender is told
         * where to resume. Nothing is buffered, which is the whole point.
         *
         * Rate-limited, though: after one lost chunk the whole rest of the
         * window arrives out of order, and answering each one would send a
         * burst of identical resync acks down a link that is also carrying
         * keystrokes. One is enough to restart the sender. */
        if (dhp_time_after(now, s->last_tx + 20)) {
            send_ack(d, s, now);
        }
        return;
    }

    const uint32_t off = (uint32_t)seq * DHP_DATA_CHUNK_BYTES;
    if (off + n > s->total) {
        uint8_t extra[1] = {DHP_DATA_CORRUPT};
        send_op(d, s, OP_ABORT, extra, 1, now);
        finish(d, s, DHP_DATA_CORRUPT);
        return;
    }

    /* Fold into the running checksum before handing the chunk on, so the
     * verdict below is over exactly the bytes the application was given. */
    s->running = adler32_update(s->running, &p[2], n);

    if (d->hooks.on_chunk) {
        d->hooks.on_chunk(d->hooks.ctx, handle_of(d, s), off, &p[2], n);
    }
    s->next_seq++;
    s->acked = s->next_seq;

    const uint16_t total_chunks = chunks_for(s->total);
    if (s->next_seq >= total_chunks) {
        /* Only the receiver has seen every byte, so only it can pass the
         * verdict -- and it reports that verdict to the sender rather than
         * merely reporting arrival, so a corrupt transfer fails on both sides
         * rather than looking successful to whoever sent it. */
        const uint8_t verdict =
            (s->running == s->crc) ? DHP_DATA_OK : DHP_DATA_CORRUPT;
        uint8_t extra[1] = {verdict};
        send_op(d, s, OP_DONE, extra, 1, now);
        finish(d, s, verdict);
        return;
    }

    /* Acknowledge once per window rather than per chunk: an ack for every
     * chunk would roughly double the frames this transfer puts on a link that
     * is also carrying keystrokes. */
    if ((s->next_seq % (s->window / 2 ? s->window / 2 : 1)) == 0) {
        send_ack(d, s, now);
    }
}

void dhp_data_rx(dhp_data_t *d, const dhp_frame_t *f, dhp_time_t now)
{
    if (f->type != DHP_MSG_DATA || f->len < 2) {
        return;
    }

    const uint8_t op = f->payload[0];
    const uint8_t id = f->payload[1];
    const uint8_t *p = &f->payload[2];
    const uint8_t len = (uint8_t)(f->len - 2);

    if (op == OP_OFFER) {
        rx_offer(d, f->src, id, p, len, now);
        return;
    }

    /* ACCEPT, ACK and DONE answer something we sent; REJECT and ABORT can go
     * either way, so try both halves of the table. */
    const bool answers_our_send =
        (op == OP_ACCEPT || op == OP_ACK || op == OP_DONE);
    dhp_data_stream_t *s = slot_find(d, f->src, id, answers_our_send);
    if (!s && !answers_our_send) {
        s = slot_find(d, f->src, id, false);
        if (!s) {
            s = slot_find(d, f->src, id, true);
        }
    }
    if (!s) {
        return;
    }

    s->last_rx = now;
    s->retries = 0;

    switch (op) {
    case OP_ACCEPT:
        if (s->state == DHP_ST_OFFERING) {
            if (len >= 1 && p[0] > 0) {
                /* Respect the receiver's window: it knows what it can absorb
                 * and we do not. */
                s->window = p[0];
            }
            s->state = DHP_ST_SENDING;
            pump_sender(d, s, now);
        }
        break;

    case OP_REJECT:
        finish(d, s, len >= 1 ? p[0] : DHP_DATA_REFUSED);
        break;

    case OP_CHUNK:
        rx_chunk(d, s, p, len, now);
        break;

    case OP_ACK:
        if (len >= 2 && (s->state == DHP_ST_SENDING ||
                         s->state == DHP_ST_DRAINING)) {
            const uint16_t next = get16(&p[0]);
            if (next > s->acked) {
                s->acked = next;
                /* Forward progress, so a later loss is a new event and should
                 * rewind again. */
                s->rewound_to = NO_REWIND;
            }
            if (next < s->next_seq) {
                /* The receiver is behind: it discarded everything after
                 * `next`, so resume from there.
                 *
                 * Only once per loss. Every subsequent chunk of the window in
                 * flight draws another ack at the same position, and rewinding
                 * for each of them re-sends the window once per duplicate --
                 * which turns a single dropped chunk into a retransmission
                 * storm that never converges. */
                if (s->rewound_to != next) {
                    s->next_seq = next;
                    s->rewound_to = next;
                    d->n_retransmits++;
                }
            }
            if (len >= 3 && p[2] > 0) {
                s->window = p[2];
            }
            s->state = DHP_ST_SENDING;
            pump_sender(d, s, now);
        }
        break;

    case OP_DONE:
        finish(d, s, len >= 1 ? p[0] : DHP_DATA_OK);
        break;

    case OP_ABORT:
        finish(d, s, len >= 1 ? p[0] : DHP_DATA_CANCELLED);
        break;

    default:
        break;
    }
}

/* ---- time ------------------------------------------------------------ */

void dhp_data_tick(dhp_data_t *d, dhp_time_t now)
{
    for (int i = 0; i < DHP_DATA_MAX_STREAMS * 2; i++) {
        dhp_data_stream_t *s = &d->s[i];
        if (s->state == DHP_ST_FREE) {
            continue;
        }

        /* A stream that has heard nothing at all for the idle timeout is
         * abandoned, whichever side it is. Otherwise a peer that vanishes
         * mid-transfer holds a slot forever. */
        if (dhp_time_after(now, s->last_rx + d->idle_timeout_ms)) {
            finish(d, s, DHP_DATA_TIMEOUT);
            continue;
        }

        if (!s->outbound) {
            continue; /* receivers are driven entirely by arriving chunks */
        }

        const bool offering = (s->state == DHP_ST_OFFERING);
        const uint16_t patience =
            offering ? d->offer_timeout_ms : d->ack_timeout_ms;
        const uint8_t budget = offering ? d->offer_retries : d->max_retries;

        if (!dhp_time_after(now, s->last_tx + patience)) {
            continue;
        }

        if (++s->retries > budget) {
            uint8_t extra[1] = {DHP_DATA_TIMEOUT};
            send_op(d, s, OP_ABORT, extra, 1, now);
            finish(d, s, DHP_DATA_TIMEOUT);
            continue;
        }

        switch (s->state) {
        case DHP_ST_OFFERING: {
            /* Re-offer: the first one may have been lost, or the far side may
             * not have had a client attached yet. */
            uint8_t extra[1 + 4 + 4 + 1 + DHP_DATA_NAME_MAX];
            const uint8_t nl = (uint8_t)strlen(s->name);
            extra[0] = s->kind;
            put32(&extra[1], s->total);
            put32(&extra[5], s->crc);
            extra[9] = nl;
            memcpy(&extra[10], s->name, nl);
            send_op(d, s, OP_OFFER, extra, (uint8_t)(10 + nl), now);
            break;
        }
        case DHP_ST_SENDING:
        case DHP_ST_DRAINING:
            /* Nothing acknowledged within the timeout: rewind to the last
             * confirmed chunk and resend from there. */
            s->next_seq = s->acked;
            s->rewound_to = NO_REWIND;
            s->state = DHP_ST_SENDING;
            d->n_retransmits++;
            pump_sender(d, s, now);
            break;
        default:
            break;
        }
    }
}

uint8_t dhp_data_active(const dhp_data_t *d)
{
    uint8_t n = 0;
    for (int i = 0; i < DHP_DATA_MAX_STREAMS * 2; i++) {
        if (d->s[i].state != DHP_ST_FREE) {
            n++;
        }
    }
    return n;
}
