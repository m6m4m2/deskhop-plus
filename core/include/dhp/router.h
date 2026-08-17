/* SPDX-License-Identifier: GPL-2.0-only
 *
 * The router: where the election, the pointer gesture, the sent-state tracker
 * and the chain topology meet, and the only place that decides which machine
 * is currently receiving input.
 *
 * Chain position without a numbering protocol
 * -------------------------------------------
 * To know which board is "the one to the left", boards must agree on an
 * order. Rather than electing a head and running a numbering pass, each board
 * derives everyone's position relative to itself from two facts it already
 * has for free:
 *
 *   - which port a frame arrived on (up = toward the head, down = toward the
 *     tail), which gives the sign;
 *   - how far the hop limit has been decremented, which gives the distance.
 *
 * So a frame arriving on the up port having been relayed twice comes from the
 * board three places to the left. Self is position 0, and no board needs an
 * absolute index at all -- every routing decision is relative anyway. Adding a
 * board to the chain reconfigures this automatically because positions are
 * recomputed from arriving traffic, which is precisely the "nothing to
 * configure when you extend it" property.
 */
#ifndef DHP_ROUTER_H
#define DHP_ROUTER_H

#include "dhp/hid.h"
#include "dhp/level.h"
#include "dhp/link.h"
#include "dhp/msg.h"
#include "dhp/pointer.h"
#include "dhp/types.h"
#include "dhp/uhrp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool       used;
    dhp_addr_t addr;
    dhp_uid_t  uid;
    uint16_t   caps;
    int8_t     rel_pos;  /* negative = up-chain, positive = down-chain */
    dhp_time_t last_seen;
} dhp_chain_entry_t;

/* Delivery of input that belongs to *this* board's own machine, plus status
 * changes worth showing on a display. Everything else leaves as a frame. */
typedef struct {
    void (*deliver_kbd)(void *ctx, const dhp_kbd_report_t *r);
    void (*deliver_mouse)(void *ctx, const dhp_mouse_report_t *r);
    void (*focus_changed)(void *ctx, dhp_addr_t focus, bool is_self);
    void (*level_changed)(void *ctx, dhp_level_t level);
    void  *ctx;
} dhp_router_hooks_t;

typedef struct {
    dhp_addr_t self;
    dhp_uid_t  uid;
    uint16_t   caps;
    bool       preempt;
    dhp_uhrp_timing_t timing;
    dhp_pointer_cfg_t pointer;
} dhp_router_cfg_t;

typedef struct {
    dhp_router_cfg_t  cfg;
    dhp_link_t       *link;
    dhp_router_hooks_t hooks;

    dhp_uhrp_t        uhrp;
    dhp_pointer_t     pointer;
    dhp_hid_tracker_t hid;

    dhp_chain_entry_t chain[DHP_MAX_BOARDS];
    uint8_t           self_slot;

    dhp_addr_t        focus;
    dhp_level_t       level;

    uint32_t          n_switches;
    uint32_t          n_releases_sent;
} dhp_router_t;

void dhp_router_init(dhp_router_t *r, const dhp_router_cfg_t *cfg,
                     dhp_link_t *link, const dhp_router_hooks_t *hooks);

void dhp_router_start(dhp_router_t *r, dhp_time_t now);

/* Feed input captured from the real keyboard/mouse attached to this board.
 * Only meaningful on the board that is currently the active speaker; on any
 * other board these are ignored, which is what stops two input sources from
 * fighting. */
void dhp_router_local_kbd(dhp_router_t *r, const dhp_kbd_report_t *rep,
                          dhp_time_t now);
void dhp_router_local_mouse(dhp_router_t *r, const dhp_mouse_report_t *rep,
                            dhp_time_t now);

/* The switch button: move focus to the next machine along the chain. */
void dhp_router_button(dhp_router_t *r, dhp_time_t now);

/* Move focus explicitly (configuration, client request, hotkey). */
void dhp_router_set_focus(dhp_router_t *r, dhp_addr_t target, uint8_t reason,
                          dhp_time_t now);

/* Handle a frame that dhp_link_rx_byte() delivered to us. */
void dhp_router_rx(dhp_router_t *r, const dhp_frame_t *f, dhp_port_t in_port,
                   dhp_time_t now);

void dhp_router_tick(dhp_router_t *r, dhp_time_t now);

static inline dhp_level_t dhp_router_level(const dhp_router_t *r)
{
    return r->level;
}

static inline dhp_addr_t dhp_router_focus(const dhp_router_t *r)
{
    return r->focus;
}

static inline bool dhp_router_is_active(const dhp_router_t *r)
{
    return dhp_uhrp_is_active(&r->uhrp);
}

/* Number of live boards known, including this one. */
uint8_t dhp_router_chain_size(const dhp_router_t *r);

#ifdef __cplusplus
}
#endif

#endif /* DHP_ROUTER_H */
