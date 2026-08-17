/* SPDX-License-Identifier: GPL-2.0-only
 *
 * The board-to-client link: what a board and the client on its machine say to
 * each other over USB, as distinct from what travels the chain.
 *
 * Two message types never leave the board:
 *
 *   ATTACH  the client announces what it can do, so the board can advertise
 *           level 3 capability on its behalf. A board with no client attached
 *           must not claim CLIPBOARD, or the chain would report level 3 for a
 *           machine that cannot actually paste anything.
 *
 *   STATUS  the board tells the client its own chain address, the current
 *           focus, and the level. The address matters most: without it the
 *           client would have to be told what to call itself, and two clients
 *           that guessed the same value would each mistake the other's frames
 *           for their own echo.
 *
 * Everything else the client sends is DHP_MSG_DATA and is proxied onto the
 * chain. Nothing else is, which is the whole security argument in
 * docs/client.md -- a client can move clipboard content and cannot claim a
 * role or inject a keystroke.
 *
 * Both are carried in DHP_MSG_CAP payloads, distinguished by a leading byte,
 * because spending two of the sixteen frame types on traffic that never
 * reaches the chain would be poor value.
 */
#ifndef DHP_LOCAL_H
#define DHP_LOCAL_H

#include "dhp/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DHP_LOCAL_ATTACH = 0, /* client -> board */
    DHP_LOCAL_STATUS = 1, /* board  -> client */
} dhp_local_kind_t;

/* ATTACH: [0x00][caps:2] */
#define DHP_LOCAL_ATTACH_BYTES 3

/* STATUS: [0x01][self:2][focus:2][level:1][boards:1] */
#define DHP_LOCAL_STATUS_BYTES 7

typedef struct {
    uint16_t caps;
} dhp_local_attach_t;

typedef struct {
    dhp_addr_t self;   /* the board's chain address; the client adopts it */
    dhp_addr_t focus;  /* which machine currently has the user */
    uint8_t    level;
    uint8_t    boards;
} dhp_local_status_t;

void dhp_local_attach_pack(const dhp_local_attach_t *a, uint8_t *out);
bool dhp_local_attach_unpack(const uint8_t *in, uint8_t len,
                             dhp_local_attach_t *out);

void dhp_local_status_pack(const dhp_local_status_t *s, uint8_t *out);
bool dhp_local_status_unpack(const uint8_t *in, uint8_t len,
                             dhp_local_status_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DHP_LOCAL_H */
