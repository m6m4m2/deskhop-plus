/* SPDX-License-Identifier: GPL-2.0-only */
#include "dhp/frame.h"

#include <string.h>

#include "dhp/crc16.h"
#include "dhp/siphash.h"

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* The MAC covers the body with ttl removed. Rather than allocate a scratch
 * copy, feed SipHash a small stack buffer holding the covered bytes: the
 * header is only 9 bytes, so the copy is 8 + len and bounded by 72. */
static uint64_t mac_compute(const uint8_t *body, size_t covered_len,
                            const uint8_t key[DHP_SIPHASH_KEY_BYTES])
{
    /* covered_len is DHP_HDR_BYTES + payload len. */
    uint8_t scratch[DHP_HDR_BYTES - 1 + DHP_MAX_PAYLOAD];

    scratch[0] = body[DHP_OFF_VER_TYPE];
    /* skip body[DHP_OFF_TTL] */
    memcpy(&scratch[1], &body[DHP_OFF_SRC], covered_len - (DHP_OFF_TTL + 1));

    return dhp_siphash24(scratch, covered_len - 1, key);
}

bool dhp_frame_mac_valid(const uint8_t *body, size_t body_len,
                         const uint8_t key[DHP_SIPHASH_KEY_BYTES])
{
    if (body_len < DHP_FRAME_OVERHEAD) {
        return false;
    }
    const size_t covered = body_len - DHP_MAC_BYTES - DHP_CRC_BYTES;
    const uint64_t want = mac_compute(body, covered, key);

    uint8_t want_bytes[DHP_MAC_BYTES];
    for (int i = 0; i < DHP_MAC_BYTES; i++) {
        want_bytes[i] = (uint8_t)(want >> (8 * i));
    }
    return dhp_ct_equal(want_bytes, &body[covered], DHP_MAC_BYTES);
}

static dhp_result_t emit(uint8_t *out, size_t cap, size_t *n, uint8_t b)
{
    /* Stuff on the way out so the caller never sees a bare flag inside a
     * frame; this is what makes the opening flag unambiguous. */
    if (b == DHP_FLAG || b == DHP_ESC) {
        if (*n + 2 > cap) {
            return DHP_ERR_NOSPACE;
        }
        out[(*n)++] = DHP_ESC;
        out[(*n)++] = (uint8_t)(b ^ DHP_ESC_XOR);
    } else {
        if (*n + 1 > cap) {
            return DHP_ERR_NOSPACE;
        }
        out[(*n)++] = b;
    }
    return DHP_OK;
}

dhp_result_t dhp_frame_encode(const dhp_frame_t *f,
                              const uint8_t key[DHP_SIPHASH_KEY_BYTES],
                              uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (!f || !key || !out || !out_len) {
        return DHP_ERR_INVAL;
    }
    if (f->len > DHP_MAX_PAYLOAD) {
        return DHP_ERR_INVAL;
    }
    if (f->type > 0x0Fu) {
        return DHP_ERR_INVAL;
    }

    uint8_t body[DHP_BODY_MAX];
    const size_t covered = DHP_HDR_BYTES + f->len;

    body[DHP_OFF_VER_TYPE] = (uint8_t)((DHP_PROTO_VERSION << 4) | f->type);
    body[DHP_OFF_TTL] = f->ttl ? f->ttl : DHP_TTL_DEFAULT;
    put16(&body[DHP_OFF_SRC], f->src);
    put16(&body[DHP_OFF_DST], f->dst);
    put16(&body[DHP_OFF_SEQ], f->seq);
    body[DHP_OFF_LEN] = f->len;
    if (f->len && f->payload) {
        memcpy(&body[DHP_HDR_BYTES], f->payload, f->len);
    } else if (f->len) {
        return DHP_ERR_INVAL;
    }

    const uint64_t mac = mac_compute(body, covered, key);
    for (int i = 0; i < DHP_MAC_BYTES; i++) {
        body[covered + i] = (uint8_t)(mac >> (8 * i));
    }

    const size_t crc_off = covered + DHP_MAC_BYTES;
    put16(&body[crc_off], dhp_crc16(body, crc_off));
    const size_t body_len = crc_off + DHP_CRC_BYTES;

    size_t n = 0;
    if (out_cap < 1) {
        return DHP_ERR_NOSPACE;
    }
    out[n++] = DHP_FLAG;
    for (size_t i = 0; i < body_len; i++) {
        const dhp_result_t r = emit(out, out_cap, &n, body[i]);
        if (r != DHP_OK) {
            return r;
        }
    }
    if (n + 1 > out_cap) {
        return DHP_ERR_NOSPACE;
    }
    out[n++] = DHP_FLAG;

    *out_len = n;
    return DHP_OK;
}

void dhp_framer_init(dhp_framer_t *fr)
{
    memset(fr, 0, sizeof(*fr));
}

/* Validate a completed body. Split out so the flag handler stays readable. */
static dhp_result_t finish(dhp_framer_t *fr,
                           const uint8_t key[DHP_SIPHASH_KEY_BYTES],
                           dhp_frame_t *out)
{
    const uint8_t *body = fr->body;
    const size_t body_len = fr->body_len;

    if (body_len < DHP_FRAME_OVERHEAD) {
        return DHP_ERR_TRUNC;
    }

    const size_t crc_off = body_len - DHP_CRC_BYTES;
    if (get16(&body[crc_off]) != dhp_crc16(body, crc_off)) {
        fr->n_crc_err++;
        return DHP_ERR_CRC;
    }

    /* Only now is the length field trustworthy enough to cross-check. */
    const uint8_t len = body[DHP_OFF_LEN];
    if (len > DHP_MAX_PAYLOAD ||
        (size_t)DHP_HDR_BYTES + len + DHP_MAC_BYTES + DHP_CRC_BYTES != body_len) {
        return DHP_ERR_TRUNC;
    }

    if ((body[DHP_OFF_VER_TYPE] >> 4) != DHP_PROTO_VERSION) {
        return DHP_ERR_VERSION;
    }

    if (key && !dhp_frame_mac_valid(body, body_len, key)) {
        fr->n_auth_err++;
        return DHP_ERR_AUTH;
    }

    out->type = body[DHP_OFF_VER_TYPE] & 0x0Fu;
    out->ttl = body[DHP_OFF_TTL];
    out->src = get16(&body[DHP_OFF_SRC]);
    out->dst = get16(&body[DHP_OFF_DST]);
    out->seq = get16(&body[DHP_OFF_SEQ]);
    out->len = len;
    out->payload = len ? &body[DHP_HDR_BYTES] : NULL;
    out->body = body;
    out->body_len = (uint16_t)body_len;

    fr->n_ok++;
    return DHP_OK;
}

dhp_result_t dhp_framer_push(dhp_framer_t *fr, uint8_t byte,
                             const uint8_t key[DHP_SIPHASH_KEY_BYTES],
                             dhp_frame_t *out)
{
    if (byte == DHP_FLAG) {
        /* A flag both closes the frame in progress and opens the next one.
         * Back-to-back frames therefore share a flag, and a stream of flags on
         * an idle line costs nothing. */
        dhp_result_t r = DHP_ERR_AGAIN;

        if (fr->in_frame && !fr->overflow && fr->body_len > 0) {
            r = finish(fr, key, out);
        } else if (fr->overflow) {
            fr->n_overflow++;
        }

        fr->in_frame = true;
        fr->escaped = false;
        fr->overflow = false;
        fr->body_len = 0;
        return r;
    }

    if (!fr->in_frame || fr->overflow) {
        /* Not yet synchronised, or discarding a runaway frame: ignore until
         * the next flag. This is the stateless-resync property. */
        return DHP_ERR_AGAIN;
    }

    if (byte == DHP_ESC) {
        fr->escaped = true;
        return DHP_ERR_AGAIN;
    }

    if (fr->escaped) {
        fr->escaped = false;
        byte ^= DHP_ESC_XOR;
    }

    if (fr->body_len >= DHP_BODY_MAX) {
        fr->overflow = true;
        return DHP_ERR_AGAIN;
    }

    fr->body[fr->body_len++] = byte;
    return DHP_ERR_AGAIN;
}

dhp_result_t dhp_frame_forward_prepare(uint8_t *body, size_t body_len)
{
    if (body_len < DHP_FRAME_OVERHEAD) {
        return DHP_ERR_TRUNC;
    }
    if (body[DHP_OFF_TTL] == 0) {
        return DHP_ERR_TTL;
    }
    body[DHP_OFF_TTL]--;
    if (body[DHP_OFF_TTL] == 0) {
        return DHP_ERR_TTL;
    }

    /* The MAC is untouched -- it never covered ttl -- so only the crc needs
     * repairing. */
    const size_t crc_off = body_len - DHP_CRC_BYTES;
    put16(&body[crc_off], dhp_crc16(body, crc_off));
    return DHP_OK;
}
