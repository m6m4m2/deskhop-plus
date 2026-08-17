/* SPDX-License-Identifier: GPL-2.0-only */
#include "dhp/pointer.h"

#include <string.h>

const char *dhp_edge_name(dhp_edge_t e)
{
    switch (e) {
    case DHP_EDGE_NONE:  return "none";
    case DHP_EDGE_LEFT:  return "left";
    case DHP_EDGE_RIGHT: return "right";
    case DHP_EDGE_UP:    return "up";
    case DHP_EDGE_DOWN:  return "down";
    default:             return "?";
    }
}

static int32_t clamp(int32_t v, int32_t lo, int32_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

void dhp_pointer_init(dhp_pointer_t *p, const dhp_pointer_cfg_t *cfg)
{
    memset(p, 0, sizeof(*p));
    p->cfg = cfg ? *cfg : DHP_POINTER_CFG_DEFAULT;
    if (p->cfg.span <= 0) {
        p->cfg = DHP_POINTER_CFG_DEFAULT;
    }
}

/* Bleed the push accumulators toward zero. Called once per report with the
 * elapsed time, so a user who stops shoving loses their progress and a user
 * who merely rests the pointer against the edge never accumulates any. */
static void decay(dhp_pointer_t *p, dhp_time_t now)
{
    if (!p->have_time) {
        p->last_update = now;
        p->have_time = true;
        return;
    }

    const int32_t dt = dhp_time_diff(now, p->last_update);
    p->last_update = now;
    if (dt <= 0) {
        return;
    }

    int32_t bleed = dt * p->cfg.push_decay_per_ms;
    if (bleed <= 0) {
        return;
    }

    if (p->push_x > 0) {
        p->push_x = p->push_x > bleed ? p->push_x - bleed : 0;
    } else if (p->push_x < 0) {
        p->push_x = -p->push_x > bleed ? p->push_x + bleed : 0;
    }
    if (p->push_y > 0) {
        p->push_y = p->push_y > bleed ? p->push_y - bleed : 0;
    } else if (p->push_y < 0) {
        p->push_y = -p->push_y > bleed ? p->push_y + bleed : 0;
    }
}

/* Advance one axis. Returns -1 for a crossing at the low edge, +1 at the high
 * edge, 0 for no crossing. */
static int axis_feed(dhp_pointer_t *p, int32_t d, int32_t *travel, int32_t *push)
{
    const int32_t span = p->cfg.span;

    const int32_t before = *travel;
    *travel = clamp(before + d, -span, span);

    /* Push only counts while the integrator is already pinned against the
     * clamp AND the user is still moving further that way. Motion that merely
     * arrives at the clamp is the traverse, not the shove. */
    if (before >= span && d > 0) {
        *push += d;
    } else if (before <= -span && d < 0) {
        *push += d; /* d is negative, so push goes negative */
    } else if (d != 0) {
        /* Any movement away from the edge abandons the gesture immediately,
         * so a shove interrupted by a correction does not silently resume. */
        if ((*push > 0 && d < 0) || (*push < 0 && d > 0)) {
            *push = 0;
        }
    }

    if (*push >= p->cfg.push_threshold) {
        return +1;
    }
    if (*push <= -p->cfg.push_threshold) {
        return -1;
    }
    return 0;
}

/* Set up the integrator for a pointer that has just entered from `from_edge`.
 * Landing saturated against the opposite side, less a guard band, is what
 * makes "I overshot, go straight back" a short deliberate movement rather
 * than a full traverse -- while still costing more than zero, so a crossing
 * cannot immediately bounce back. */
void dhp_pointer_arrive(dhp_pointer_t *p, dhp_edge_t from_edge, dhp_time_t now)
{
    const int32_t span = p->cfg.span;
    const int32_t guard = p->cfg.reentry_guard;

    p->push_x = 0;
    p->push_y = 0;
    p->last_update = now;
    p->have_time = true;

    switch (from_edge) {
    case DHP_EDGE_RIGHT: /* left this machine rightward: arrive at the left */
        p->travel_x = -span + guard;
        p->travel_y = 0;
        break;
    case DHP_EDGE_LEFT:
        p->travel_x = span - guard;
        p->travel_y = 0;
        break;
    case DHP_EDGE_DOWN:
        p->travel_y = -span + guard;
        p->travel_x = 0;
        break;
    case DHP_EDGE_UP:
        p->travel_y = span - guard;
        p->travel_x = 0;
        break;
    default:
        p->travel_x = 0;
        p->travel_y = 0;
        break;
    }
}

dhp_edge_t dhp_pointer_feed(dhp_pointer_t *p, int16_t dx, int16_t dy,
                            dhp_time_t now)
{
    decay(p, now);

    const int hx = axis_feed(p, dx, &p->travel_x, &p->push_x);
    if (hx > 0) {
        dhp_pointer_arrive(p, DHP_EDGE_RIGHT, now);
        return DHP_EDGE_RIGHT;
    }
    if (hx < 0) {
        dhp_pointer_arrive(p, DHP_EDGE_LEFT, now);
        return DHP_EDGE_LEFT;
    }

    if (p->cfg.vertical_enabled) {
        const int hy = axis_feed(p, dy, &p->travel_y, &p->push_y);
        if (hy > 0) {
            dhp_pointer_arrive(p, DHP_EDGE_DOWN, now);
            return DHP_EDGE_DOWN;
        }
        if (hy < 0) {
            dhp_pointer_arrive(p, DHP_EDGE_UP, now);
            return DHP_EDGE_UP;
        }
    } else {
        p->travel_y = clamp(p->travel_y + dy, -p->cfg.span, p->cfg.span);
    }

    return DHP_EDGE_NONE;
}
