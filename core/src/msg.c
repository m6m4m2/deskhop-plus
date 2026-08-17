/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Explicit little-endian pack/unpack for every payload.
 *
 * Nothing here casts a struct onto a byte buffer. All boards happen to be
 * little-endian ARM today, but the coordinator is a separate machine and the
 * level 3 clients are ordinary desktop software, so the wire format is defined
 * by these functions rather than by a compiler's struct layout.
 */
#include "dhp/msg.h"

#include <string.h>

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void put64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

static uint64_t get64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v |= ((uint64_t)p[i]) << (8 * i);
    }
    return v;
}

/* ---- HELLO ----------------------------------------------------------- */

void dhp_hello_pack(const dhp_hello_t *h, uint8_t out[DHP_HELLO_BYTES])
{
    out[0] = h->state;
    out[1] = h->priority;
    out[2] = h->flags;
    out[3] = h->hold_cs;
    put64(&out[4], h->uid);
    put16(&out[12], h->caps);
    put16(&out[14], h->reserved);
}

bool dhp_hello_unpack(const uint8_t *in, uint8_t len, dhp_hello_t *out)
{
    if (len < DHP_HELLO_BYTES) {
        return false;
    }
    out->state = in[0];
    out->priority = in[1];
    out->flags = in[2];
    out->hold_cs = in[3];
    out->uid = get64(&in[4]);
    out->caps = get16(&in[12]);
    out->reserved = get16(&in[14]);
    return true;
}

/* ---- KBD ------------------------------------------------------------- */

void dhp_kbd_pack(const dhp_kbd_report_t *r, uint8_t out[DHP_KBD_BYTES])
{
    out[0] = r->modifiers;
    memcpy(&out[1], r->keys, 6);
}

bool dhp_kbd_unpack(const uint8_t *in, uint8_t len, dhp_kbd_report_t *out)
{
    if (len < DHP_KBD_BYTES) {
        return false;
    }
    out->modifiers = in[0];
    memcpy(out->keys, &in[1], 6);
    return true;
}

/* ---- MOUSE ----------------------------------------------------------- */

void dhp_mouse_pack(const dhp_mouse_report_t *r, uint8_t out[DHP_MOUSE_BYTES])
{
    put16(&out[0], (uint16_t)r->dx);
    put16(&out[2], (uint16_t)r->dy);
    out[4] = (uint8_t)r->wheel;
    out[5] = (uint8_t)r->pan;
    out[6] = r->buttons;
}

bool dhp_mouse_unpack(const uint8_t *in, uint8_t len, dhp_mouse_report_t *out)
{
    if (len < DHP_MOUSE_BYTES) {
        return false;
    }
    out->dx = (int16_t)get16(&in[0]);
    out->dy = (int16_t)get16(&in[2]);
    out->wheel = (int8_t)in[4];
    out->pan = (int8_t)in[5];
    out->buttons = in[6];
    return true;
}

/* ---- FOCUS ----------------------------------------------------------- */

void dhp_focus_pack(const dhp_focus_t *f, uint8_t out[DHP_FOCUS_BYTES])
{
    put16(&out[0], f->target);
    out[2] = f->reason;
    out[3] = f->edge;
}

bool dhp_focus_unpack(const uint8_t *in, uint8_t len, dhp_focus_t *out)
{
    if (len < DHP_FOCUS_BYTES) {
        return false;
    }
    out->target = get16(&in[0]);
    out->reason = in[2];
    out->edge = in[3];
    return true;
}

/* ---- TOPO ------------------------------------------------------------ */

void dhp_topo_pack(const dhp_topo_t *t, uint8_t out[DHP_TOPO_BYTES])
{
    put64(&out[0], t->uid);
    out[8] = t->hops_from_head;
    out[9] = t->caps_lo;
    out[10] = t->caps_hi;
    out[11] = t->flags;
}

bool dhp_topo_unpack(const uint8_t *in, uint8_t len, dhp_topo_t *out)
{
    if (len < DHP_TOPO_BYTES) {
        return false;
    }
    out->uid = get64(&in[0]);
    out->hops_from_head = in[8];
    out->caps_lo = in[9];
    out->caps_hi = in[10];
    out->flags = in[11];
    return true;
}
