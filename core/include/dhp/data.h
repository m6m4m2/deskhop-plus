/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Level 3 bulk transport: clipboard, text, images, file handoff.
 *
 * The problem
 * -----------
 * A link frame carries 64 payload bytes. Anything larger than a short
 * clipboard string therefore needs segmentation, reassembly, ordering, flow
 * control and a way to fail cleanly -- none of which the frame layer provides,
 * because the frame layer exists to move keystrokes and would be worse at that
 * job if it also had to move megabytes.
 *
 * Go-back-N, and why
 * ------------------
 * The receiver accepts only the next expected chunk and discards anything out
 * of order; the sender retransmits from the last acknowledged point. That is
 * less efficient than selective acknowledgement, and it is chosen deliberately:
 *
 *   - It needs no receive-side reordering buffer. Core allocates nothing, and a
 *     reassembly buffer would either bound transfers to whatever fits in a
 *     fixed array or force an allocator into the protocol layer. Go-back-N lets
 *     a receiver stream chunks straight to disk as they arrive.
 *   - The link underneath is a short point-to-point UART with a CRC and a
 *     replay window. Loss is rare and almost always a burst, which is the case
 *     where selective acknowledgement wins least.
 *
 * What this is NOT for
 * --------------------
 * Large files. The chain runs at 2 Mbps, and after framing overhead a transfer
 * moves roughly 100 KB/s -- fine for a clipboard, a screenshot or a document,
 * and about three hours for a 1 GB video. Pushing bulk file data through a
 * keyboard cable is the wrong shape.
 *
 * So DHP_DATA_SHARE exists: instead of the bytes, the sender offers a location
 * on the coordinator's SMB share, and the receiving machine fetches it over the
 * network at network speed. The chain carries the handoff -- which is small,
 * and needs the trust and the machine-to-machine addressing the chain already
 * has -- and not the payload. See docs/protocols/data.md.
 */
#ifndef DHP_DATA_H
#define DHP_DATA_H

#include "dhp/link.h"
#include "dhp/msg.h"
#include "dhp/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Concurrent transfers in each direction. Four is enough for "copy some text
 * while a screenshot is still going" without making the state table large. */
#define DHP_DATA_MAX_STREAMS 4

/* CHUNK spends 4 payload bytes on its header, leaving this for content. */
#define DHP_DATA_CHUNK_BYTES (DHP_MAX_PAYLOAD - 4)

/* Longest name carried in an OFFER. Names are advisory -- a receiver must
 * never use one to build a path unchecked. */
#define DHP_DATA_NAME_MAX 48

typedef enum {
    DHP_DATA_TEXT   = 0, /* clipboard text, UTF-8 */
    DHP_DATA_IMAGE  = 1, /* PNG bytes */
    DHP_DATA_FILE   = 2, /* small file, inline */
    DHP_DATA_LIST   = 3, /* newline-separated names: a folder listing */
    DHP_DATA_SHARE  = 4, /* an SMB location, not the content */
} dhp_data_kind_t;

typedef enum {
    DHP_DATA_OK        = 0,
    DHP_DATA_REFUSED   = 1, /* the far side declined */
    DHP_DATA_TIMEOUT   = 2,
    DHP_DATA_CORRUPT   = 3, /* end-to-end check failed */
    DHP_DATA_TOO_BIG   = 4,
    DHP_DATA_CANCELLED = 5,
    DHP_DATA_NO_STREAM = 6, /* no free slot */
} dhp_data_result_t;

typedef struct {
    /* An offer has arrived. Call dhp_data_accept() or dhp_data_reject().
     * Nothing is received until you do -- a machine should not be able to
     * push a megabyte at another machine unasked. */
    void (*on_offer)(void *ctx, uint8_t stream, dhp_addr_t from, uint8_t kind,
                     uint32_t total, const char *name);

    /* A chunk, in order, exactly once. `offset` is the byte position, so a
     * receiver can write straight to a file without tracking position. */
    void (*on_chunk)(void *ctx, uint8_t stream, uint32_t offset,
                     const uint8_t *data, uint8_t len);

    /* Receive finished. `result` is DHP_DATA_OK or why it failed. */
    void (*on_received)(void *ctx, uint8_t stream, uint8_t result);

    /* Send finished, and the caller's buffer is no longer referenced. */
    void (*on_sent)(void *ctx, uint8_t stream, uint8_t result);

    void *ctx;
} dhp_data_hooks_t;

typedef enum {
    DHP_ST_FREE = 0,
    DHP_ST_OFFERING,  /* sender: offer sent, waiting for accept */
    DHP_ST_SENDING,
    DHP_ST_DRAINING,  /* sender: all chunks out, waiting for final ack */
    DHP_ST_OFFERED,   /* receiver: offer in, waiting for the app to decide */
    DHP_ST_RECEIVING,
} dhp_data_state_t;

typedef struct {
    uint8_t    state;
    uint8_t    id;
    bool       outbound;
    dhp_addr_t peer;
    uint8_t    kind;
    char       name[DHP_DATA_NAME_MAX + 1];

    uint32_t   total;
    uint32_t   crc;       /* end-to-end, declared in the offer */
    uint32_t   running;   /* receiver: computed as chunks pass through */

    uint16_t   next_seq;  /* sender: next to transmit. receiver: next expected */
    uint16_t   acked;      /* sender: chunks confirmed */
    uint16_t   rewound_to; /* sender: loss already responded to, see data.c */
    uint8_t    window;

    const uint8_t *src;   /* sender: caller memory, valid until on_sent */

    dhp_time_t last_rx;
    dhp_time_t last_tx;
    uint8_t    retries;
} dhp_data_stream_t;

typedef struct {
    dhp_link_t       *link;
    dhp_data_hooks_t  hooks;
    dhp_data_stream_t s[DHP_DATA_MAX_STREAMS * 2]; /* both directions */

    uint8_t  next_id;
    uint8_t  window;          /* chunks in flight before waiting for an ack */
    uint16_t ack_timeout_ms;
    /* The offer phase waits longer than the chunk phase, and separately.
     * A missing chunk ack means a dropped frame on a wire that is otherwise
     * working, so retrying quickly is right. An offer going unanswered can
     * mean the far machine's client is still starting, or that a human is
     * being asked whether to accept -- neither of which resolves in 250 ms,
     * and giving up on them looks to the user like the feature is broken. */
    uint16_t offer_timeout_ms;
    uint8_t  offer_retries;
    uint16_t idle_timeout_ms;
    uint8_t  max_retries;
    uint32_t max_inbound;     /* refuse offers larger than this */

    uint32_t n_sent, n_received, n_retransmits, n_refused;
} dhp_data_t;

void dhp_data_init(dhp_data_t *d, dhp_link_t *link,
                   const dhp_data_hooks_t *hooks);

/* Begin sending. `data` must stay valid and unchanged until on_sent fires.
 * Returns the stream id, or negative dhp_data_result_t. */
int dhp_data_send(dhp_data_t *d, dhp_addr_t to, uint8_t kind, const char *name,
                  const uint8_t *data, uint32_t len, dhp_time_t now);

void dhp_data_accept(dhp_data_t *d, uint8_t stream, dhp_time_t now);
void dhp_data_reject(dhp_data_t *d, uint8_t stream, dhp_time_t now);
void dhp_data_cancel(dhp_data_t *d, uint8_t stream, dhp_time_t now);

/* Feed a DHP_MSG_DATA frame. */
void dhp_data_rx(dhp_data_t *d, const dhp_frame_t *f, dhp_time_t now);

/* Drives retransmission and timeouts, and pushes the send window along. */
void dhp_data_tick(dhp_data_t *d, dhp_time_t now);

/* How many streams are doing anything, for the display and for backpressure. */
uint8_t dhp_data_active(const dhp_data_t *d);

const char *dhp_data_result_name(uint8_t r);

#ifdef __cplusplus
}
#endif

#endif /* DHP_DATA_H */
