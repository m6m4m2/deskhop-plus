/* SPDX-License-Identifier: GPL-2.0-only
 *
 * UHRP -- the role election, modelled on Cisco HSRP.
 *
 * Boards advertise a priority, the most capable wins, ties break on the unique
 * per-board id, and a backup is pre-elected so that failover is a promotion
 * rather than an election. That last property is what buys the ~150 ms
 * recovery: when the active speaker vanishes there is nothing to decide, only
 * a timer to expire.
 *
 * Two rules HSRP does not need, because HSRP routes packets and this routes
 * keystrokes:
 *
 *   Rule 1 -- never hand over while a key is held.
 *     A planned handover in the middle of a chord would leave the outgoing
 *     board's key-down unmatched by any key-up. So the deferral lives on the
 *     ACTIVE side: a board that has been out-prioritised resigns only once its
 *     held-key count reaches zero. The challenger simply waits. A safety
 *     deadline (handover_defer_max_ms) bounds this, because a genuinely stuck
 *     key must not wedge the handover forever -- past the deadline the active
 *     board releases everything and then resigns.
 *
 *   Rule 2 -- release everything if the active speaker vanished without
 *     warning. A graceful resignation is clean: the outgoing board has already
 *     lifted its keys. A timeout is not: the machine that was being typed on
 *     may be holding a modifier down with nobody left to lift it. So a board
 *     promoted by *timeout* rather than by RESIGN broadcasts a release before
 *     it does anything else.
 *
 * The distinction between those two paths is the entire reason
 * dhp_uhrp_events_t reports `promoted_by_timeout` separately.
 *
 * This module is pure: it never reads a clock, never sends a frame, and never
 * allocates. Time comes in as a parameter and actions come out as events, so
 * a whole chain of boards can be simulated deterministically on a host.
 */
#ifndef DHP_UHRP_H
#define DHP_UHRP_H

#include "dhp/level.h"
#include "dhp/msg.h"
#include "dhp/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t hello_ms;              /* advertisement interval */
    uint16_t hold_ms;               /* peer considered dead after this */
    uint16_t learn_ms;              /* initial listen-before-speak window */
    uint16_t handover_defer_max_ms; /* rule 1 safety deadline */
} dhp_uhrp_timing_t;

/* 50 / 150 gives failover in about 150 ms, as three missed hellos. Faster is
 * possible on a quiet link but three-strikes is what keeps a single dropped
 * frame from causing a spurious role change. */
#define DHP_UHRP_TIMING_DEFAULT                                                \
    ((dhp_uhrp_timing_t){                                                      \
        .hello_ms = 50,                                                        \
        .hold_ms = 150,                                                        \
        .learn_ms = 150,                                                       \
        .handover_defer_max_ms = 2000,                                         \
    })

typedef struct {
    dhp_uid_t  uid;
    dhp_addr_t addr;
    uint8_t    priority;  /* 0 = derive from caps */
    uint16_t   caps;
    bool       preempt;   /* willing to take the role from a weaker holder */
    dhp_uhrp_timing_t timing;
} dhp_uhrp_config_t;

typedef struct {
    bool       used;
    dhp_uid_t  uid;
    dhp_addr_t addr;
    uint8_t    priority;
    uint8_t    state;
    uint8_t    flags;
    uint16_t   caps;
    dhp_time_t last_seen;
} dhp_uhrp_peer_t;

/* Everything the state machine wants done, drained by the caller after each
 * tick or rx. Flags are one-shot: cleared at the start of every entry point. */
typedef struct {
    bool send_hello;
    bool send_resign;
    bool broadcast_release;   /* rule 2 fired */
    bool state_changed;
    bool promoted_by_timeout; /* became active because the old one vanished */
    dhp_uhrp_state_t prev_state;
} dhp_uhrp_events_t;

typedef struct {
    dhp_uhrp_config_t cfg;
    dhp_uhrp_state_t  state;

    dhp_addr_t active;   /* current active speaker, DHP_ADDR_NONE if unknown */
    dhp_addr_t standby;  /* pre-elected backup */
    uint8_t    active_priority;
    dhp_uid_t  active_uid;

    dhp_time_t t_state_entered;
    dhp_time_t t_next_hello;
    dhp_time_t t_active_expiry;

    bool       keys_held;        /* set by the HID layer, drives rule 1 */
    bool       resign_pending;   /* out-prioritised, waiting for keys to clear */
    dhp_time_t t_resign_deadline;

    bool       active_valid;     /* have we ever heard from an active? */
    bool       started;

    /* True when the previous active stood down cleanly (RESIGN), false when
     * it simply stopped answering. Rule 2 keys off this: only an unclean
     * vacancy requires the successor to broadcast a release. */
    bool       vacancy_clean;

    dhp_uhrp_peer_t peers[DHP_MAX_BOARDS];
    dhp_uhrp_events_t ev;
} dhp_uhrp_t;

/* Derive an advertised priority from what a board can contribute. "The most
 * capable wins" is thereby a property of the hardware present, not of a
 * configuration file someone has to keep in sync. */
uint8_t dhp_uhrp_priority_from_caps(uint16_t caps);

void dhp_uhrp_init(dhp_uhrp_t *u, const dhp_uhrp_config_t *cfg);

/* Begin participating. Enters LEARN: listen first, so that a board plugged
 * into a running chain joins it instead of fighting it. */
void dhp_uhrp_start(dhp_uhrp_t *u, dhp_time_t now);

/* Feed a received HELLO. `src` is the frame source address. */
void dhp_uhrp_rx_hello(dhp_uhrp_t *u, dhp_addr_t src, const dhp_hello_t *h,
                       dhp_time_t now);

/* Feed a received RESIGN: the active speaker is standing down cleanly. */
void dhp_uhrp_rx_resign(dhp_uhrp_t *u, dhp_addr_t src, dhp_time_t now);

/* Tell the election whether any key is currently down. Drives rule 1. */
void dhp_uhrp_set_keys_held(dhp_uhrp_t *u, bool held);

/* Advance time. Must be called regularly; every timer lives here. */
void dhp_uhrp_tick(dhp_uhrp_t *u, dhp_time_t now);

/* Drain the pending actions. Returns them and clears the internal set. */
dhp_uhrp_events_t dhp_uhrp_take_events(dhp_uhrp_t *u);

/* Fill a HELLO describing our current position, for transmission. */
void dhp_uhrp_build_hello(const dhp_uhrp_t *u, dhp_hello_t *out);

/* Union of capabilities we can currently reach: ours plus every live peer's.
 * This is the input to dhp_level_of(). */
uint16_t dhp_uhrp_reachable_caps(const dhp_uhrp_t *u);

static inline bool dhp_uhrp_is_active(const dhp_uhrp_t *u)
{
    return u->state == DHP_UHRP_ACTIVE;
}

const char *dhp_uhrp_state_name(dhp_uhrp_state_t s);

#ifdef __cplusplus
}
#endif

#endif /* DHP_UHRP_H */
