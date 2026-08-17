/* SPDX-License-Identifier: GPL-2.0-only
 *
 * CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection, no final xor).
 *
 * This is the cheap line-error check, evaluated before the MAC so that noise
 * on the wire is discarded without paying for a SipHash. It is not a security
 * primitive and is not relied on as one.
 */
#ifndef DHP_CRC16_H
#define DHP_CRC16_H

#include "dhp/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DHP_CRC16_INIT ((uint16_t)0xFFFFu)

uint16_t dhp_crc16_update(uint16_t crc, const uint8_t *data, size_t len);

static inline uint16_t dhp_crc16(const uint8_t *data, size_t len)
{
    return dhp_crc16_update(DHP_CRC16_INIT, data, len);
}

#ifdef __cplusplus
}
#endif

#endif /* DHP_CRC16_H */
