/* SPDX-License-Identifier: GPL-2.0-only
 *
 * DeskHop+ core types.
 *
 * Everything in core/ is freestanding C11: no libc beyond <string.h>, no
 * allocation, no I/O, no clock access. Time is always passed in by the caller
 * as a monotonic millisecond counter, which is what makes the whole protocol
 * stack testable on a host machine with a simulated clock.
 */
#ifndef DHP_TYPES_H
#define DHP_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Monotonic milliseconds. Wraps every ~49 days; all comparisons in core use
 * dhp_time_after() so wrap is handled correctly. */
typedef uint32_t dhp_time_t;

/* Signed difference that is correct across wrap, for uint32 monotonic time. */
static inline int32_t dhp_time_diff(dhp_time_t a, dhp_time_t b)
{
    return (int32_t)(a - b);
}

/* true if a is at or after b */
static inline bool dhp_time_after(dhp_time_t a, dhp_time_t b)
{
    return dhp_time_diff(a, b) >= 0;
}

/* Short board address used on the wire. Derived from the 64-bit UID. */
typedef uint16_t dhp_addr_t;

#define DHP_ADDR_BROADCAST ((dhp_addr_t)0xFFFFu)
#define DHP_ADDR_NONE      ((dhp_addr_t)0x0000u)

/* Permanent per-board identity, from RP2040 flash unique ID. Ties in the
 * election break on this, so it must be globally unique per board. */
typedef uint64_t dhp_uid_t;

/* Maximum boards in one chain. Bounded so every table in core is a fixed
 * array: no allocation anywhere in the protocol stack. */
#define DHP_MAX_BOARDS 16

/* Maximum payload bytes in a single link frame. Sized so that a worst-case
 * fully byte-stuffed frame still fits comfortably in a UART DMA buffer. */
#define DHP_MAX_PAYLOAD 64

typedef enum {
    DHP_OK = 0,
    DHP_ERR_INVAL = -1,     /* caller passed something nonsensical */
    DHP_ERR_NOSPACE = -2,   /* output buffer too small */
    DHP_ERR_CRC = -3,       /* frame failed CRC: line noise */
    DHP_ERR_AUTH = -4,      /* frame failed MAC: not one of ours */
    DHP_ERR_REPLAY = -5,    /* sequence number already seen */
    DHP_ERR_TTL = -6,       /* hop limit exhausted */
    DHP_ERR_TRUNC = -7,     /* frame ended early / length field disagrees */
    DHP_ERR_VERSION = -8,   /* protocol version we do not speak */
    DHP_ERR_AGAIN = -9,     /* no complete frame available yet */
    DHP_ERR_STATE = -10,    /* operation not legal in the current state */
} dhp_result_t;

/* Which end of the chain a link port faces. A board has at most one of each,
 * which is exactly the two UARTs an RP2040 provides. */
typedef enum {
    DHP_PORT_UP = 0,   /* toward the head of the chain */
    DHP_PORT_DOWN = 1, /* toward the tail of the chain */
    DHP_PORT_COUNT = 2,
    DHP_PORT_LOCAL = 0xFF, /* frame originated on this board */
} dhp_port_t;

#ifdef __cplusplus
}
#endif

#endif /* DHP_TYPES_H */
