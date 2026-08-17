/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Linux platform layer for the coordinator: the chain link, the panel, the
 * clock and entropy.
 *
 * This is the mirror of firmware/rp2040/src/board.h. The two differ entirely
 * below this line and not at all above it -- both hand the same core/ the same
 * bytes and the same millisecond counter, which is why the coordinator needs
 * no second implementation of the protocol.
 */
#ifndef DHP_PLATFORM_H
#define DHP_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dhp/types.h"

/* ---- clock ---- */

dhp_time_t plat_now_ms(void);

/* ---- chain link ----
 *
 * The coordinator sits at an end of the chain, so it has one port where a
 * board has two. Which one it is depends on which end it was plugged into;
 * see serial_open(). */

typedef struct serial serial_t;

/* `port` is the logical port this cable represents: DHP_PORT_DOWN when the
 * coordinator is at the head of the chain, DHP_PORT_UP when it is at the tail.
 * Getting it wrong does not break anything -- chain position is derived from
 * arrival port and hop count, so the display would simply show the chain
 * mirrored. */
serial_t *serial_open(const char *device, uint32_t baud, dhp_port_t port,
                      char *err, size_t errlen);
void      serial_close(serial_t *s);

int  serial_fd(const serial_t *s);
dhp_port_t serial_port(const serial_t *s);

/* Non-blocking. Returns bytes written, or -1. Short writes are buffered
 * internally and drained by serial_flush(). */
int  serial_write(serial_t *s, const uint8_t *data, size_t len);
int  serial_flush(serial_t *s);

/* Non-blocking read into `buf`. Returns count, 0 if nothing waiting, -1 on
 * error. */
int  serial_read(serial_t *s, uint8_t *buf, size_t cap);

/* ---- panel ---- */

typedef struct ssd1306 ssd1306_t;

/* Returns NULL and fills `err` if the bus or panel is not there, which is not
 * fatal: the daemon runs headless and keeps the chain at level 2 regardless,
 * because CONFIG is advertised separately from DISPLAY. */
ssd1306_t *ssd1306_open(const char *i2c_dev, uint8_t addr, char *err,
                        size_t errlen);
void       ssd1306_close(ssd1306_t *p);
int        ssd1306_blit(ssd1306_t *p, const uint8_t *fb, size_t len);

/* ---- entropy ---- */

/* Fills from the kernel CSPRNG. Aborts on failure rather than returning weak
 * randomness: a pairing nonce that is not random is a pairing that can be
 * predicted, and continuing would be worse than stopping. */
void plat_random(uint8_t *out, size_t len);

#endif /* DHP_PLATFORM_H */
