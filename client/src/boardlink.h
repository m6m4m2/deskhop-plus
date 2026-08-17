/* SPDX-License-Identifier: GPL-2.0-only
 *
 * The client's link to the board plugged into this machine.
 *
 * Why the client is not a chain member
 * -----------------------------------
 * The obvious design is to hand the client the chain key and let it speak the
 * chain protocol directly. That is wrong, and the reason is worth stating: the
 * chain key authenticates *role advertisements*. A process holding it could
 * claim to be the most capable board, win the election, and become the thing
 * that decides where every keystroke goes -- so any unprivileged program on any
 * one machine could take over input for all of them. Levels 1 and 2 would then
 * be only as trustworthy as the least trustworthy desktop on the desk.
 *
 * So the board stays the chain endpoint and *proxies* for its client. It
 * accepts local frames over USB, and re-emits onto the chain only
 * DHP_MSG_DATA. A compromised client can therefore move clipboard content
 * around -- which it could do anyway, being on the machine -- and cannot
 * advertise a priority, claim a role, or inject a keystroke.
 *
 * The local key
 * -------------
 * dhp_link wants a key, and this link is a USB cable inside a single machine.
 * A secret there would protect nothing: anything that can open the device can
 * already read the clipboard it is carrying, and the OS is the trust boundary.
 * So DHP_LOCAL_KEY below is a fixed, published value, present so the framing
 * layer is unchanged rather than to keep anything secret. Do not mistake it
 * for one.
 */
#ifndef DHP_BOARDLINK_H
#define DHP_BOARDLINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dhp/types.h"

/* Deliberately not a secret. See above. */
#define DHP_LOCAL_KEY                                                          \
    {                                                                          \
        'd', 'h', 'p', '-', 'l', 'o', 'c', 'a', 'l', '-', 'v', '1', 0, 0, 0, 0 \
    }

typedef enum {
    BOARDLINK_HIDRAW = 0, /* a real board, over its vendor HID interface */
    BOARDLINK_SOCKET = 1, /* a Unix socket: development and tests */
} boardlink_kind_t;

typedef struct boardlink boardlink_t;

boardlink_t *boardlink_open(boardlink_kind_t kind, const char *path, char *err,
                            size_t errlen);
void         boardlink_close(boardlink_t *b);

int  boardlink_fd(const boardlink_t *b);

/* Both non-blocking. A HID transport moves fixed-size reports and a socket
 * moves a byte stream; both are hidden here, so the caller only ever sees the
 * framed byte stream that dhp_link expects. */
int  boardlink_write(boardlink_t *b, const uint8_t *data, size_t len);
int  boardlink_read(boardlink_t *b, uint8_t *buf, size_t cap);

#endif /* DHP_BOARDLINK_H */
