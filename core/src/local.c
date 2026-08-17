/* SPDX-License-Identifier: GPL-2.0-only */
#include "dhp/local.h"

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

void dhp_local_attach_pack(const dhp_local_attach_t *a, uint8_t *out)
{
    out[0] = DHP_LOCAL_ATTACH;
    put16(&out[1], a->caps);
}

bool dhp_local_attach_unpack(const uint8_t *in, uint8_t len,
                             dhp_local_attach_t *out)
{
    if (len < DHP_LOCAL_ATTACH_BYTES || in[0] != DHP_LOCAL_ATTACH) {
        return false;
    }
    out->caps = get16(&in[1]);
    return true;
}

void dhp_local_status_pack(const dhp_local_status_t *s, uint8_t *out)
{
    out[0] = DHP_LOCAL_STATUS;
    put16(&out[1], s->self);
    put16(&out[3], s->focus);
    out[5] = s->level;
    out[6] = s->boards;
}

bool dhp_local_status_unpack(const uint8_t *in, uint8_t len,
                             dhp_local_status_t *out)
{
    if (len < DHP_LOCAL_STATUS_BYTES || in[0] != DHP_LOCAL_STATUS) {
        return false;
    }
    out->self = get16(&in[1]);
    out->focus = get16(&in[3]);
    out->level = in[5];
    out->boards = in[6];
    return true;
}
