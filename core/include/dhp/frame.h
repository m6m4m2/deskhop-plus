/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Link framing: HDLC-style flag delimiting with byte stuffing.
 *
 * Why HDLC rather than a length-prefixed format: a length prefix requires the
 * receiver to already be in sync to know where a frame ends, so a receiver
 * that joins a running link has no way to find the boundary except by luck.
 * Flag delimiting resynchronises with no state at all -- you scan for the next
 * 0x7E and you are aligned. That is the property that lets a board be plugged
 * into a chain that is already running and mid-frame, and find its place
 * immediately.
 *
 * Wire layout of one frame:
 *
 *     0x7E  <stuffed body>  0x7E
 *
 * where the body, before stuffing, is:
 *
 *     off  size  field
 *     0    1     ver:4 | type:4
 *     1    1     ttl                  (hop limit, decremented on forward)
 *     2    2     src   (LE)
 *     4    2     dst   (LE)           0xFFFF = broadcast
 *     6    2     seq   (LE)
 *     8    1     len                  payload length, 0..DHP_MAX_PAYLOAD
 *     9    len   payload
 *     ..   4     mac   (LE)           SipHash-2-4, truncated to 32 bits
 *     ..   2     crc16 (LE)           CRC-16/CCITT-FALSE over all of the above
 *
 * Two integrity fields, deliberately:
 *
 *   - crc16 covers the whole body including ttl. It is the cheap line-noise
 *     check, evaluated first so that garbage costs almost nothing to discard.
 *   - mac covers the body EXCLUDING ttl. It is the authenticity check.
 *
 * ttl is excluded from the MAC because every hop decrements it. If it were
 * covered, a forwarded frame's MAC would fail at the second board and the
 * chain could not relay anything. This is the same reason IPsec AH excludes
 * the IP TTL field. crc16 is recomputed by each forwarding board, which is
 * cheap; the MAC is computed once by the originator and verified unchanged by
 * every board along the way, which is the point.
 */
#ifndef DHP_FRAME_H
#define DHP_FRAME_H

#include "dhp/siphash.h"
#include "dhp/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DHP_FLAG 0x7Eu
#define DHP_ESC  0x7Du
#define DHP_ESC_XOR 0x20u

#define DHP_PROTO_VERSION 1u

/* Byte offsets within the unstuffed body. */
#define DHP_OFF_VER_TYPE 0
#define DHP_OFF_TTL      1
#define DHP_OFF_SRC      2
#define DHP_OFF_DST      4
#define DHP_OFF_SEQ      6
#define DHP_OFF_LEN      8
#define DHP_HDR_BYTES    9
#define DHP_MAC_BYTES    4
#define DHP_CRC_BYTES    2
#define DHP_FRAME_OVERHEAD (DHP_HDR_BYTES + DHP_MAC_BYTES + DHP_CRC_BYTES)

/* Largest unstuffed body. */
#define DHP_BODY_MAX (DHP_FRAME_OVERHEAD + DHP_MAX_PAYLOAD)

/* Worst case on the wire: every body byte stuffed to two, plus two flags. */
#define DHP_WIRE_MAX (2 * DHP_BODY_MAX + 2)

/* Default hop limit. A frame may legitimately traverse the whole chain, so
 * this must exceed DHP_MAX_BOARDS; the margin catches loops. */
#define DHP_TTL_DEFAULT 20

typedef struct {
    uint8_t    type;
    uint8_t    ttl;
    dhp_addr_t src;
    dhp_addr_t dst;
    uint16_t   seq;
    uint8_t    len;
    const uint8_t *payload; /* points into the decoder buffer, or caller data */

    /* Decoder output only (NULL/0 when you are encoding): the raw unstuffed
     * body, which is what forwarding needs. The decoder resets its own
     * body_len as soon as a frame completes, so a relay must take the length
     * from here rather than from the framer. */
    const uint8_t *body;
    uint16_t       body_len;
} dhp_frame_t;

/* Encode one frame, stuffed and flag-delimited, into `out`.
 * `key` is the 16-byte chain key; see dhp/auth.h for how it is established.
 * On success writes the wire length to *out_len. */
dhp_result_t dhp_frame_encode(const dhp_frame_t *f,
                              const uint8_t key[DHP_SIPHASH_KEY_BYTES],
                              uint8_t *out, size_t out_cap, size_t *out_len);

/* Streaming decoder. One instance per link port. */
typedef struct {
    uint8_t body[DHP_BODY_MAX];
    uint16_t body_len;
    bool in_frame;   /* we have seen an opening flag */
    bool escaped;    /* previous byte was ESC */
    bool overflow;   /* body exceeded max; discard until next flag */
    /* Counters, useful on the status display and for diagnosing a bad link. */
    uint32_t n_ok;
    uint32_t n_crc_err;
    uint32_t n_auth_err;
    uint32_t n_overflow;
} dhp_framer_t;

void dhp_framer_init(dhp_framer_t *fr);

/* Feed one received byte.
 *
 * Returns DHP_OK and fills *out when a complete, CRC-valid, MAC-valid frame
 * is available. `out->payload` then points into `fr->body` and stays valid
 * until the next call to this function.
 *
 * Returns DHP_ERR_AGAIN when more bytes are needed (the common case).
 * Returns DHP_ERR_CRC / DHP_ERR_AUTH / DHP_ERR_TRUNC / DHP_ERR_VERSION when a
 * frame completed but was rejected; the decoder has already resynchronised and
 * the caller can simply continue feeding bytes.
 *
 * If `key` is NULL the MAC is not checked. That is only legal before a chain
 * key exists -- i.e. during pairing -- and dhp_link only permits it for
 * DHP_MSG_PAIR frames. */
dhp_result_t dhp_framer_push(dhp_framer_t *fr, uint8_t byte,
                             const uint8_t key[DHP_SIPHASH_KEY_BYTES],
                             dhp_frame_t *out);

/* Recompute the MAC over an already-formed body and compare, constant time. */
bool dhp_frame_mac_valid(const uint8_t *body, size_t body_len,
                         const uint8_t key[DHP_SIPHASH_KEY_BYTES]);

/* Decrement ttl in-place and repair the crc, for forwarding a frame onward
 * without re-encoding it. Returns DHP_ERR_TTL if the hop limit is exhausted,
 * in which case the frame must be dropped. */
dhp_result_t dhp_frame_forward_prepare(uint8_t *body, size_t body_len);

#ifdef __cplusplus
}
#endif

#endif /* DHP_FRAME_H */
