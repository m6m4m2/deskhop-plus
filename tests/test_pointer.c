/* SPDX-License-Identifier: GPL-2.0-only */
#include "dhp/pointer.h"
#include "test_util.h"

/* Move the mouse by `total` counts in `steps` reports, one report per
 * millisecond -- roughly what a 1 kHz mouse produces. Returns the first edge
 * crossed, or NONE. */
static dhp_edge_t move(dhp_pointer_t *p, int total, int steps, dhp_time_t *now)
{
    const int per = total / steps;
    for (int i = 0; i < steps; i++) {
        (*now)++;
        const dhp_edge_t e = dhp_pointer_feed(p, (int16_t)per, 0, *now);
        if (e != DHP_EDGE_NONE) {
            return e;
        }
    }
    return DHP_EDGE_NONE;
}

/* Ordinary movement well inside a screen must never cross. */
static void test_no_spurious_crossing(void)
{
    dhp_pointer_t p;
    dhp_pointer_init(&p, NULL);
    dhp_time_t now = 1000;

    for (int i = 0; i < 40; i++) {
        CHECK_EQ(move(&p, 300, 30, &now), DHP_EDGE_NONE);
        CHECK_EQ(move(&p, -300, 30, &now), DHP_EDGE_NONE);
    }
}

/* Traverse further than any screen, then keep pushing: that is the gesture. */
static void test_push_crosses(void)
{
    dhp_pointer_t p;
    dhp_pointer_init(&p, NULL);
    dhp_time_t now = 1000;

    /* The traverse alone must not be enough -- otherwise a wide screen would
     * switch by itself. */
    CHECK_EQ(move(&p, 8000, 400, &now), DHP_EDGE_NONE);

    /* Now the shove: 20 counts/ms, which is an ordinary firm push. */
    CHECK_EQ(move(&p, 1500, 75, &now), DHP_EDGE_RIGHT);
}

static void test_push_left(void)
{
    dhp_pointer_t p;
    dhp_pointer_init(&p, NULL);
    dhp_time_t now = 1000;
    CHECK_EQ(move(&p, -8000, 400, &now), DHP_EDGE_NONE);
    CHECK_EQ(move(&p, -1500, 75, &now), DHP_EDGE_LEFT);
}

/* "Keep pushing" must genuinely mean keep: resting against the edge, or
 * shoving in slow instalments, must not accumulate a crossing. */
static void test_push_decays(void)
{
    dhp_pointer_t p;
    dhp_pointer_init(&p, NULL);
    dhp_time_t now = 1000;

    CHECK_EQ(move(&p, 8000, 400, &now), DHP_EDGE_NONE);

    /* Nudge, then wait long enough for the accumulator to bleed away. Repeat
     * many times: it must never add up. */
    for (int i = 0; i < 30; i++) {
        CHECK_EQ(move(&p, 200, 10, &now), DHP_EDGE_NONE);
        now += 500; /* hand off the mouse, think, come back */
        CHECK_EQ(dhp_pointer_feed(&p, 0, 0, now), DHP_EDGE_NONE);
    }
}

/* Motion away from the edge abandons the gesture rather than pausing it. */
static void test_reversal_cancels(void)
{
    dhp_pointer_t p;
    dhp_pointer_init(&p, NULL);
    dhp_time_t now = 1000;

    CHECK_EQ(move(&p, 8000, 400, &now), DHP_EDGE_NONE);
    CHECK_EQ(move(&p, 500, 25, &now), DHP_EDGE_NONE);  /* most of a shove */
    CHECK_EQ(move(&p, -50, 5, &now), DHP_EDGE_NONE);   /* changed their mind */
    CHECK_EQ(move(&p, 500, 25, &now), DHP_EDGE_NONE);  /* must restart, not top up */
}

/* After crossing, going straight back must be possible with a short
 * deliberate movement -- but not by accident. */
static void test_reentry_guard(void)
{
    dhp_pointer_t p;
    dhp_pointer_init(&p, NULL);
    dhp_time_t now = 1000;

    CHECK_EQ(move(&p, 8000, 400, &now), DHP_EDGE_NONE);
    CHECK_EQ(move(&p, 1500, 75, &now), DHP_EDGE_RIGHT);

    /* Immediately shoving left must not bounce straight back: the guard band
     * has to be traversed first. */
    CHECK_EQ(move(&p, -900, 45, &now), DHP_EDGE_NONE);

    /* But the total needed is the guard plus a shove, not a whole traverse. */
    const dhp_edge_t e = move(&p, -1600, 80, &now);
    CHECK_EQ(e, DHP_EDGE_LEFT);
}

/* Vertical is off unless asked for, so a chain laid out left-to-right cannot
 * be switched by vertical mouse movement. */
static void test_vertical_disabled_by_default(void)
{
    dhp_pointer_t p;
    dhp_pointer_init(&p, NULL);
    dhp_time_t now = 1000;

    for (int i = 0; i < 500; i++) {
        now++;
        CHECK_EQ(dhp_pointer_feed(&p, 0, 30, now), DHP_EDGE_NONE);
    }
}

static void test_vertical_enabled(void)
{
    dhp_pointer_cfg_t cfg = DHP_POINTER_CFG_DEFAULT;
    cfg.vertical_enabled = true;
    dhp_pointer_t p;
    dhp_pointer_init(&p, &cfg);
    dhp_time_t now = 1000;

    dhp_edge_t got = DHP_EDGE_NONE;
    for (int i = 0; i < 500 && got == DHP_EDGE_NONE; i++) {
        now++;
        got = dhp_pointer_feed(&p, 0, 30, now);
    }
    CHECK_EQ(got, DHP_EDGE_DOWN);
}

void test_pointer_suite(void)
{
    RUN(test_no_spurious_crossing);
    RUN(test_push_crosses);
    RUN(test_push_left);
    RUN(test_push_decays);
    RUN(test_reversal_cancels);
    RUN(test_reentry_guard);
    RUN(test_vertical_disabled_by_default);
    RUN(test_vertical_enabled);
}
