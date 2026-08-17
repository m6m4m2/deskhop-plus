/* SPDX-License-Identifier: GPL-2.0-only */
#include "dhp/crc16.h"

/* Bitwise rather than table-driven: 256 entries of flash is a poor trade on a
 * board where the CRC is never the bottleneck, and it keeps core free of
 * generated data. */
uint16_t dhp_crc16_update(uint16_t crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int bit = 0; bit < 8; bit++) {
            if (crc & 0x8000u) {
                crc = (uint16_t)((crc << 1) ^ 0x1021u);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}
