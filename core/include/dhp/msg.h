/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Message types carried in the frame payload.
 *
 * The type field is 4 bits, so there are sixteen and no more. That is a
 * deliberate constraint: it forces the protocol to stay small enough to reason
 * about, and anything richer (configuration, clipboard, file transfer) is
 * layered inside DHP_MSG_CFG / DHP_MSG_DATA rather than spending a type.
 */
#ifndef DHP_MSG_H
#define DHP_MSG_H

#include "dhp/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DHP_MSG_HELLO      = 0,  /* UHRP election advertisement */
    DHP_MSG_RESIGN     = 1,  /* graceful handover of the active role */
    DHP_MSG_KBD        = 2,  /* keyboard report toward the focused board */
    DHP_MSG_MOUSE      = 3,  /* mouse report toward the focused board */
    DHP_MSG_FOCUS      = 4,  /* "machine N now has the input devices" */
    DHP_MSG_RELEASE    = 5,  /* release every held key/button on a board */
    DHP_MSG_TOPO       = 6,  /* chain position discovery */
    DHP_MSG_CAP        = 7,  /* capability / level advertisement */
    DHP_MSG_PAIR       = 8,  /* trust establishment; see dhp/auth.h */
    DHP_MSG_CFG        = 9,  /* configuration, available at level 2 */
    DHP_MSG_DATA       = 10, /* level 3 bulk: clipboard, files, images */
    DHP_MSG_PING       = 11,
    DHP_MSG_PONG       = 12,
    /* Raw input captured on a board that holds the physical keyboard/mouse
     * but is not itself the active speaker, forwarded to whoever is.
     *
     * This exists because input capture and focus authority are not the same
     * job. The keyboard is plugged into one particular board, but once the
     * coordinator boots and takes the active role it is the coordinator that
     * decides where input goes -- and it has no keyboard of its own. Rather
     * than let two boards both believe they own routing, the input board
     * hands its reports to the active speaker and the active speaker remains
     * the single authority. It is also why the pointer integrator can live in
     * exactly one place: crossing decisions need the same single owner. */
    DHP_MSG_INPUT      = 13,
    /* 14..15 reserved */
} dhp_msg_type_t;

/* DHP_MSG_INPUT payload: a one-byte kind followed by the report. */
#define DHP_INPUT_KIND_KBD   0
#define DHP_INPUT_KIND_MOUSE 1

/* ---- DHP_MSG_HELLO --------------------------------------------------- */

/* UHRP speaker state, on the wire. */
typedef enum {
    DHP_UHRP_INIT    = 0,
    DHP_UHRP_LEARN   = 1,
    DHP_UHRP_LISTEN  = 2,
    DHP_UHRP_SPEAK   = 3,
    DHP_UHRP_STANDBY = 4,
    DHP_UHRP_ACTIVE  = 5,
} dhp_uhrp_state_t;

/* 16 bytes. Sent every hello interval by every board that is not LISTEN-only,
 * so it is worth keeping tight. */
typedef struct {
    uint8_t  state;      /* dhp_uhrp_state_t */
    uint8_t  priority;   /* higher wins; see dhp/uhrp.h for how it is derived */
    uint8_t  flags;      /* DHP_HELLO_F_* */
    uint8_t  hold_cs;    /* hold time in units of 10 ms (max 2550 ms) */
    uint64_t uid;        /* tiebreak, and identity for the trust store */
    uint16_t caps;       /* dhp_cap_t bitmap this board can contribute */
    uint16_t reserved;
} dhp_hello_t;

#define DHP_HELLO_BYTES 16

#define DHP_HELLO_F_PREEMPT   (1u << 0) /* willing to take the role by force */
#define DHP_HELLO_F_KEYS_HELD (1u << 1) /* a key is down right now */
#define DHP_HELLO_F_HAS_INPUT (1u << 2) /* keyboard/mouse attached here */
#define DHP_HELLO_F_COORD     (1u << 3) /* this speaker is the coordinator */

void dhp_hello_pack(const dhp_hello_t *h, uint8_t out[DHP_HELLO_BYTES]);
bool dhp_hello_unpack(const uint8_t *in, uint8_t len, dhp_hello_t *out);

/* ---- DHP_MSG_KBD ----------------------------------------------------- */

/* Boot-protocol-shaped keyboard report: modifier bitmap plus six keycodes.
 * Deliberately the BIOS-compatible shape, because working before an operating
 * system exists is the whole point. */
typedef struct {
    uint8_t modifiers;
    uint8_t keys[6];
} dhp_kbd_report_t;

#define DHP_KBD_BYTES 7

void dhp_kbd_pack(const dhp_kbd_report_t *r, uint8_t out[DHP_KBD_BYTES]);
bool dhp_kbd_unpack(const uint8_t *in, uint8_t len, dhp_kbd_report_t *out);

/* ---- DHP_MSG_MOUSE --------------------------------------------------- */

typedef struct {
    int16_t dx;
    int16_t dy;
    int8_t  wheel;
    int8_t  pan;
    uint8_t buttons;
} dhp_mouse_report_t;

#define DHP_MOUSE_BYTES 7

void dhp_mouse_pack(const dhp_mouse_report_t *r, uint8_t out[DHP_MOUSE_BYTES]);
bool dhp_mouse_unpack(const uint8_t *in, uint8_t len, dhp_mouse_report_t *out);

/* ---- DHP_MSG_FOCUS --------------------------------------------------- */

typedef struct {
    dhp_addr_t target;  /* board that now owns the input devices */
    uint8_t    reason;  /* DHP_FOCUS_R_* */
    uint8_t    edge;    /* which edge the pointer left by, dhp_edge_t */
} dhp_focus_t;

#define DHP_FOCUS_BYTES 4

#define DHP_FOCUS_R_BUTTON   0 /* user pressed the switch button */
#define DHP_FOCUS_R_EDGE     1 /* pointer pushed through a screen edge */
#define DHP_FOCUS_R_HOTKEY   2
#define DHP_FOCUS_R_CFG      3 /* coordinator or client asked for it */
#define DHP_FOCUS_R_FAILOVER 4 /* forced during a role change */

void dhp_focus_pack(const dhp_focus_t *f, uint8_t out[DHP_FOCUS_BYTES]);
bool dhp_focus_unpack(const uint8_t *in, uint8_t len, dhp_focus_t *out);

/* ---- DHP_MSG_TOPO ---------------------------------------------------- */

typedef struct {
    uint64_t uid;
    uint8_t  hops_from_head; /* filled in as the frame is relayed */
    uint8_t  caps_lo;
    uint8_t  caps_hi;
    uint8_t  flags;
} dhp_topo_t;

#define DHP_TOPO_BYTES 12

#define DHP_TOPO_F_HEAD (1u << 0) /* no neighbour on the up port */
#define DHP_TOPO_F_TAIL (1u << 1) /* no neighbour on the down port */

void dhp_topo_pack(const dhp_topo_t *t, uint8_t out[DHP_TOPO_BYTES]);
bool dhp_topo_unpack(const uint8_t *in, uint8_t len, dhp_topo_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DHP_MSG_H */
