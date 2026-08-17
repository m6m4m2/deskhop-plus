/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Pointer crossing by relative dead-reckoning.
 *
 * Upstream DeskHop rewrites the mouse HID descriptor to report *absolute*
 * coordinates, tracks the cursor in pixel space, and switches when it reaches
 * a screen boundary. That works, but it obliges you to tell it how big each
 * screen is and how the screens are arranged -- and it re-breaks whenever a
 * monitor changes, a resolution changes, or a machine reorders its displays.
 *
 * This module takes the opposite approach: stay relative, never model the
 * screen at all, and define crossing as a *gesture* rather than a position.
 *
 * The reasoning
 * -------------
 * A device sitting on the USB wire cannot see where the pointer actually is.
 * With relative reports it can only integrate what the user does. So consider
 * what physically happens when someone pushes off the side of a screen:
 *
 *   - they move the mouse right; the pointer moves right;
 *   - the pointer reaches the real screen edge and stops;
 *   - they keep moving the mouse right, and the pointer stays pinned.
 *
 * The device sees continued rightward motion in both the third phase and the
 * middle of a very wide screen. The only thing that separates them is *how far
 * the user has already travelled in that direction* -- so cumulative travel,
 * saturated at a span wider than any plausible screen, is the discriminator.
 * Nothing here needs to know pixels.
 *
 * Consequently:
 *
 *   travel   integrates dx, clamped to +/- span. Reaching the clamp means
 *            "you have moved further in this direction than any screen is
 *            wide, so the pointer is certainly against the edge by now".
 *   push     accumulates only while travel is saturated. It decays with time,
 *            which is what turns the rule into "keep pushing" rather than
 *            "happen to end up at the edge". Resting the mouse at the edge
 *            never crosses; a deliberate shove does.
 *
 * The cost of needing no configuration
 * ------------------------------------
 * The integrator is deliberately decoupled from the real pointer position, and
 * it must be: the OS applies its own acceleration curve, the real edge clamps
 * the pointer while the integrator keeps counting, and applications warp the
 * cursor whenever they like. Any attempt to keep the two in agreement would be
 * exactly the fragile screen model this design is avoiding. Because the
 * crossing test is a gesture and not a coordinate, that drift is harmless --
 * it is the reason the approach is robust rather than a defect in it.
 *
 * After a crossing the integrator resets to the far side minus a small guard
 * band, so pushing straight back is possible with a short deliberate movement
 * but cannot happen by accident.
 */
#ifndef DHP_POINTER_H
#define DHP_POINTER_H

#include "dhp/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DHP_EDGE_NONE  = 0,
    DHP_EDGE_LEFT  = 1,
    DHP_EDGE_RIGHT = 2,
    DHP_EDGE_UP    = 3,
    DHP_EDGE_DOWN  = 4,
} dhp_edge_t;

typedef struct {
    /* Travel that certainly exceeds any screen traverse, in mouse counts.
     * 8000 counts is 8 inches at 1000 dpi, which no single screen requires. */
    int32_t span;

    /* Push required to commit to a crossing, in mouse counts, once travel is
     * saturated. Large enough that brushing the edge does nothing. */
    int32_t push_threshold;

    /* Push bleeds away at this many counts per millisecond when the user is
     * not actively pushing. This is what makes it "keep pushing".
     *
     * It also sets a speed floor, which is the more important effect: a mouse
     * moving slower than this many counts per millisecond can never
     * accumulate anything, however long it is held against the edge. At 1000
     * dpi the default of 4 is 4 inches per second, so a slow drift or a hand
     * resting on the mouse cannot switch machines, while a deliberate shove
     * at a normal 20 counts/ms nets 16/ms and crosses in under 40 ms. */
    int32_t push_decay_per_ms;

    /* After crossing, how far back the user must move before the reverse
     * crossing becomes available. Small: going straight back is a legitimate
     * and common correction. */
    int32_t reentry_guard;

    /* Whether vertical crossing is considered at all. */
    bool    vertical_enabled;
} dhp_pointer_cfg_t;

#define DHP_POINTER_CFG_DEFAULT                                                \
    ((dhp_pointer_cfg_t){                                                      \
        .span = 8000,                                                          \
        .push_threshold = 600,                                                 \
        .push_decay_per_ms = 4,                                                \
        .reentry_guard = 1500,                                                 \
        .vertical_enabled = false,                                             \
    })

typedef struct {
    dhp_pointer_cfg_t cfg;
    int32_t travel_x, travel_y;
    int32_t push_x, push_y; /* signed: positive means pushing right/down */
    dhp_time_t last_update;
    bool have_time;
} dhp_pointer_t;

void dhp_pointer_init(dhp_pointer_t *p, const dhp_pointer_cfg_t *cfg);

/* Feed one relative mouse delta. Returns the edge the user has just pushed
 * through, or DHP_EDGE_NONE. When it returns an edge, the integrator has
 * already been reset for arrival on the neighbouring machine. */
dhp_edge_t dhp_pointer_feed(dhp_pointer_t *p, int16_t dx, int16_t dy,
                            dhp_time_t now);

/* Reset as if the pointer had just arrived from `from_edge` -- used when a
 * switch happens for some other reason (button, hotkey, failover) so that the
 * gesture state matches the machine the user is now on. */
void dhp_pointer_arrive(dhp_pointer_t *p, dhp_edge_t from_edge, dhp_time_t now);

const char *dhp_edge_name(dhp_edge_t e);

#ifdef __cplusplus
}
#endif

#endif /* DHP_POINTER_H */
