/* SPDX-License-Identifier: GPL-2.0-only
 *
 * A whole chain of boards, in one process, on a virtual clock.
 *
 * Nodes are wired exactly as the hardware is: node i's down port carries bytes
 * to node i+1's up port and vice versa, through byte queues that stand in for
 * the UARTs. Because core/ never reads a clock and never touches hardware, the
 * code under test here is bit-for-bit the code that runs on the boards -- the
 * simulation replaces only the wires and the passage of time.
 */
#ifndef DHP_SIM_H
#define DHP_SIM_H

#include "dhp/link.h"
#include "dhp/router.h"

#define SIM_MAX_NODES 8
#define SIM_WIRE_CAP 16384

typedef struct {
    uint8_t buf[SIM_WIRE_CAP];
    int head, tail;
    bool cut;          /* cable pulled */
    uint32_t dropped;  /* bytes lost to a cut or an overrun */
} sim_wire_t;

struct sim;

typedef struct {
    struct sim  *owner;
    int          idx;
    dhp_link_t   link;
    dhp_router_t router;
    bool         powered;

    /* Observations, for assertions. */
    dhp_kbd_report_t   last_kbd;
    dhp_mouse_report_t last_mouse;
    int          kbd_count;
    int          mouse_count;
    int          kbd_release_count;   /* all-zero keyboard reports received */
    uint8_t      held_mods;           /* what this machine believes is down */
    uint8_t      held_keys[6];
    uint8_t      held_buttons;

    dhp_addr_t   focus;
    int          focus_changes;
    dhp_level_t  level;
    int          level_changes;
} sim_node_t;

typedef struct sim {
    sim_node_t node[SIM_MAX_NODES];
    int        n;
    /* down[i]: bytes in flight from node i toward node i+1.
     * up[i]:   bytes in flight from node i toward node i-1. */
    sim_wire_t down[SIM_MAX_NODES];
    sim_wire_t up[SIM_MAX_NODES];
    dhp_time_t now;
    uint8_t    key[DHP_SIPHASH_KEY_BYTES];
} sim_t;

void sim_init(sim_t *s, dhp_time_t start);

/* Append a board to the tail of the chain. Returns its index. */
int sim_add(sim_t *s, uint16_t caps, bool preempt, dhp_uid_t uid);

/* Power a board up (it begins participating) or pull it out entirely. */
void sim_power(sim_t *s, int idx, bool on);

/* Cut or restore the cable between idx and idx+1. */
void sim_cut(sim_t *s, int idx, bool cut);

/* Advance the world by one millisecond step. */
void sim_step(sim_t *s);
void sim_run(sim_t *s, uint32_t ms);

/* Inject input as though the real keyboard/mouse were attached to node idx. */
void sim_kbd(sim_t *s, int idx, uint8_t mods, uint8_t key1);
void sim_mouse_move(sim_t *s, int idx, int16_t dx, int16_t dy);
void sim_mouse_buttons(sim_t *s, int idx, uint8_t buttons);

/* Push the pointer through an edge: traverse, then keep shoving. Returns true
 * if focus changed within the allowed time. */
bool sim_push_edge(sim_t *s, int idx, int direction, uint32_t max_ms);

/* Which node is the active speaker, or -1. */
int sim_active(const sim_t *s);
int sim_active_count(const sim_t *s);

/* Index of the node currently receiving input, or -1. */
int sim_focus_idx(const sim_t *s);

#endif /* DHP_SIM_H */
