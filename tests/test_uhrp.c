/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Election behaviour, driven with a virtual clock so that timing claims
 * ("failover in about 150 ms") are actually measured rather than asserted.
 */
#include "dhp/uhrp.h"
#include "test_util.h"

#define MAXN 4

typedef struct {
    dhp_uhrp_t u;
    dhp_addr_t addr;
    bool       alive;
    /* Events accumulated across the run, so a test can ask "did this board
     * ever broadcast a release" without polling every millisecond. */
    bool ever_release;
    bool ever_resign;
    bool ever_promoted_by_timeout;
} node_t;

typedef struct {
    node_t n[MAXN];
    int    count;
    dhp_time_t now;
} net_t;

static void net_add(net_t *net, uint8_t prio, dhp_uid_t uid, uint16_t caps,
                    bool preempt)
{
    node_t *n = &net->n[net->count];
    memset(n, 0, sizeof(*n));
    n->addr = (dhp_addr_t)(net->count + 1);
    n->alive = true;

    const dhp_uhrp_config_t cfg = {
        .uid = uid, .addr = n->addr, .priority = prio, .caps = caps,
        .preempt = preempt, .timing = DHP_UHRP_TIMING_DEFAULT,
    };
    dhp_uhrp_init(&n->u, &cfg);
    net->count++;
}

static void collect(net_t *net, int i)
{
    node_t *n = &net->n[i];
    const dhp_uhrp_events_t ev = dhp_uhrp_take_events(&n->u);

    if (ev.broadcast_release)   n->ever_release = true;
    if (ev.send_resign)         n->ever_resign = true;
    if (ev.promoted_by_timeout) n->ever_promoted_by_timeout = true;

    if (ev.send_hello) {
        dhp_hello_t h;
        dhp_uhrp_build_hello(&n->u, &h);
        for (int j = 0; j < net->count; j++) {
            if (j != i && net->n[j].alive) {
                dhp_uhrp_rx_hello(&net->n[j].u, n->addr, &h, net->now);
            }
        }
    }
    if (ev.send_resign) {
        for (int j = 0; j < net->count; j++) {
            if (j != i && net->n[j].alive) {
                dhp_uhrp_rx_resign(&net->n[j].u, n->addr, net->now);
            }
        }
    }
}

static void net_step(net_t *net, dhp_time_t dt)
{
    net->now += dt;

    for (int i = 0; i < net->count; i++) {
        if (net->n[i].alive) {
            dhp_uhrp_tick(&net->n[i].u, net->now);
        }
    }
    for (int i = 0; i < net->count; i++) {
        if (net->n[i].alive) {
            collect(net, i);
        }
    }
    /* Receiving can itself cause a transition, so drain once more. */
    for (int i = 0; i < net->count; i++) {
        if (net->n[i].alive) {
            collect(net, i);
        }
    }
}

static void net_run(net_t *net, dhp_time_t ms)
{
    for (dhp_time_t t = 0; t < ms; t += 5) {
        net_step(net, 5);
    }
}

static void net_start_all(net_t *net)
{
    for (int i = 0; i < net->count; i++) {
        dhp_uhrp_start(&net->n[i].u, net->now);
    }
}

static int active_count(net_t *net)
{
    int c = 0;
    for (int i = 0; i < net->count; i++) {
        if (net->n[i].alive && dhp_uhrp_is_active(&net->n[i].u)) {
            c++;
        }
    }
    return c;
}

/* ---------------------------------------------------------------------- */

/* A lone board must take the role rather than wait forever for company. */
static void test_single_board_elects_itself(void)
{
    net_t net = {.now = 1000};
    net_add(&net, 100, 0xAAAA, DHP_CAP_HID_OUT, false);
    net_start_all(&net);

    net_run(&net, 400);
    CHECK(dhp_uhrp_is_active(&net.n[0].u));
    /* Nothing preceded it, so there is nothing to clean up. */
    CHECK(!net.n[0].ever_release);
}

/* Level 1 must arrive in milliseconds, not seconds: the user should be able
 * to start typing while the rest of the system is still coming up. */
static void test_level1_arrives_fast(void)
{
    net_t net = {.now = 1000};
    net_add(&net, 100, 0x1111, DHP_CAP_HID_OUT, false);
    net_add(&net, 120, 0x2222, DHP_CAP_HID_OUT | DHP_CAP_HID_IN, false);
    net_start_all(&net);

    const dhp_time_t t0 = net.now;
    while (active_count(&net) == 0 && net.now - t0 < 2000) {
        net_step(&net, 5);
    }
    const uint32_t elapsed = net.now - t0;

    CHECK_EQ(active_count(&net), 1);
    CHECK(elapsed <= 250);
    printf("    level 1 reached in %u ms\n", elapsed);
}

static void test_higher_priority_wins(void)
{
    net_t net = {.now = 1000};
    net_add(&net, 100, 0x1111, 0, false);
    net_add(&net, 200, 0x0001, 0, false); /* lower uid, higher priority */
    net_start_all(&net);
    net_run(&net, 600);

    CHECK_EQ(active_count(&net), 1);
    CHECK(dhp_uhrp_is_active(&net.n[1].u));
    /* The loser is not idle: it has been pre-elected as the backup, which is
     * what makes failover a promotion rather than an election. */
    CHECK_EQ(net.n[0].u.state, DHP_UHRP_STANDBY);
}

static void test_tie_breaks_on_uid(void)
{
    net_t net = {.now = 1000};
    net_add(&net, 150, 0x0000000000000001ULL, 0, false);
    net_add(&net, 150, 0x00000000000000FFULL, 0, false);
    net_start_all(&net);
    net_run(&net, 600);

    CHECK_EQ(active_count(&net), 1);
    CHECK(dhp_uhrp_is_active(&net.n[1].u)); /* larger uid */
}

/* Priority derived from capability, so "the most capable wins" is a property
 * of the hardware present rather than of a config file. */
static void test_priority_from_caps(void)
{
    const uint8_t plain = dhp_uhrp_priority_from_caps(DHP_CAP_HID_OUT);
    const uint8_t with_input =
        dhp_uhrp_priority_from_caps(DHP_CAP_HID_OUT | DHP_CAP_HID_IN);
    const uint8_t coord = dhp_uhrp_priority_from_caps(
        DHP_CAP_COORD | DHP_CAP_DISPLAY | DHP_CAP_CONFIG);

    CHECK(with_input > plain);
    CHECK(coord > with_input); /* a coordinator outranks any chain board */
}

/* Rule 2: an active speaker that vanishes without warning may have left a
 * modifier held on a machine nobody is watching, so its successor must
 * release everything before doing anything else. */
static void test_failover_releases_and_is_timed(void)
{
    net_t net = {.now = 1000};
    net_add(&net, 200, 0x1111, 0, false);
    net_add(&net, 100, 0x2222, 0, false);
    net_start_all(&net);
    net_run(&net, 600);

    CHECK(dhp_uhrp_is_active(&net.n[0].u));
    CHECK_EQ(net.n[1].u.state, DHP_UHRP_STANDBY);

    /* Yank the active board out of the chain. */
    net.n[0].alive = false;
    const dhp_time_t t0 = net.now;

    while (!dhp_uhrp_is_active(&net.n[1].u) && net.now - t0 < 2000) {
        net_step(&net, 5);
    }
    const uint32_t failover_ms = net.now - t0;

    CHECK(dhp_uhrp_is_active(&net.n[1].u));
    CHECK(net.n[1].ever_promoted_by_timeout);
    CHECK(net.n[1].ever_release);

    /* The claim is roughly 150 ms; allow for the 5 ms simulation step and one
     * hello period of jitter. */
    CHECK(failover_ms <= 200);
    printf("    failover in %u ms\n", failover_ms);
}

/* A planned handover is the opposite case: the outgoing board lifted its own
 * keys on the way out, so the successor must NOT blast a release. */
static void test_graceful_handover_is_clean(void)
{
    net_t net = {.now = 1000};
    /* A plain board comes up first; the coordinator boots later and preempts,
     * which is exactly the level 1 -> level 2 transition. */
    net_add(&net, 100, 0x1111, DHP_CAP_HID_OUT, false);
    dhp_uhrp_start(&net.n[0].u, net.now);
    net_run(&net, 400);
    CHECK(dhp_uhrp_is_active(&net.n[0].u));

    net_add(&net, 220, 0x9999, DHP_CAP_COORD | DHP_CAP_DISPLAY, true);
    dhp_uhrp_start(&net.n[1].u, net.now);
    net_run(&net, 800);

    CHECK_EQ(active_count(&net), 1);
    CHECK(dhp_uhrp_is_active(&net.n[1].u));   /* coordinator took over */
    CHECK(net.n[0].ever_resign);              /* by resigning, not by dying */
    CHECK(!net.n[1].ever_promoted_by_timeout);
    CHECK(!net.n[1].ever_release);            /* nothing was left stuck */
}

/* Rule 1: never hand over while a key is held. */
static void test_handover_defers_while_key_held(void)
{
    net_t net = {.now = 1000};
    net_add(&net, 100, 0x1111, DHP_CAP_HID_OUT, false);
    dhp_uhrp_start(&net.n[0].u, net.now);
    net_run(&net, 400);
    CHECK(dhp_uhrp_is_active(&net.n[0].u));

    /* The user is holding a chord on the active board. */
    dhp_uhrp_set_keys_held(&net.n[0].u, true);

    net_add(&net, 220, 0x9999, DHP_CAP_COORD, true);
    dhp_uhrp_start(&net.n[1].u, net.now);
    net_run(&net, 900);

    /* The coordinator wants the role and outranks the board, but the chord is
     * still down, so the handover waits. */
    CHECK(dhp_uhrp_is_active(&net.n[0].u));
    CHECK(!dhp_uhrp_is_active(&net.n[1].u));
    CHECK(!net.n[0].ever_resign);

    /* The user lets go; the handover completes promptly and cleanly. */
    dhp_uhrp_set_keys_held(&net.n[0].u, false);
    net_run(&net, 400);

    CHECK(dhp_uhrp_is_active(&net.n[1].u));
    CHECK(net.n[0].ever_resign);
    CHECK(!net.n[1].ever_release); /* keys were lifted properly, not forced */
}

/* ...but a genuinely stuck key must not wedge the handover forever. Past the
 * deadline the active board releases everything and then stands down. */
static void test_stuck_key_deadline_forces_handover(void)
{
    net_t net = {.now = 1000};
    net_add(&net, 100, 0x1111, DHP_CAP_HID_OUT, false);
    dhp_uhrp_start(&net.n[0].u, net.now);
    net_run(&net, 400);

    dhp_uhrp_set_keys_held(&net.n[0].u, true); /* and never released */

    net_add(&net, 220, 0x9999, DHP_CAP_COORD, true);
    dhp_uhrp_start(&net.n[1].u, net.now);

    /* handover_defer_max_ms defaults to 2000. */
    net_run(&net, 3500);

    CHECK(dhp_uhrp_is_active(&net.n[1].u));
    CHECK(net.n[0].ever_resign);
    /* Forced past the deadline, so the outgoing board had to release for
     * itself rather than trusting the key to come up. */
    CHECK(net.n[0].ever_release);
}

/* Four boards must converge on exactly one active and one standby, and stay
 * there -- no oscillation. */
static void test_four_boards_converge(void)
{
    net_t net = {.now = 1000};
    net_add(&net, 100, 0x1111, 0, false);
    net_add(&net, 130, 0x2222, 0, false);
    net_add(&net, 120, 0x3333, 0, false);
    net_add(&net, 110, 0x4444, 0, false);
    net_start_all(&net);
    net_run(&net, 1000);

    CHECK_EQ(active_count(&net), 1);
    CHECK(dhp_uhrp_is_active(&net.n[1].u));
    CHECK_EQ(net.n[2].u.state, DHP_UHRP_STANDBY); /* second most capable */

    /* Run much longer and confirm nothing changes its mind. */
    const dhp_uhrp_state_t before[MAXN] = {
        net.n[0].u.state, net.n[1].u.state, net.n[2].u.state, net.n[3].u.state};
    net_run(&net, 5000);
    for (int i = 0; i < 4; i++) {
        CHECK_EQ(net.n[i].u.state, before[i]);
    }
}

/* Successive failures must keep promoting down the line. */
static void test_cascading_failover(void)
{
    net_t net = {.now = 1000};
    net_add(&net, 100, 0x1111, 0, false);
    net_add(&net, 130, 0x2222, 0, false);
    net_add(&net, 120, 0x3333, 0, false);
    net_start_all(&net);
    net_run(&net, 800);
    CHECK(dhp_uhrp_is_active(&net.n[1].u));

    net.n[1].alive = false;
    net_run(&net, 600);
    CHECK(dhp_uhrp_is_active(&net.n[2].u));

    net.n[2].alive = false;
    net_run(&net, 600);
    CHECK(dhp_uhrp_is_active(&net.n[0].u));
    CHECK_EQ(active_count(&net), 1);
}

/* A board joining a chain that is already running must join it, not fight it,
 * even if it would have won a fresh election. Without preempt it stands by. */
static void test_late_joiner_does_not_disrupt(void)
{
    net_t net = {.now = 1000};
    net_add(&net, 100, 0x1111, 0, false);
    dhp_uhrp_start(&net.n[0].u, net.now);
    net_run(&net, 500);
    CHECK(dhp_uhrp_is_active(&net.n[0].u));

    net_add(&net, 250, 0xFFFF, 0, false); /* stronger, but not preempting */
    dhp_uhrp_start(&net.n[1].u, net.now);
    net_run(&net, 800);

    CHECK(dhp_uhrp_is_active(&net.n[0].u));
    CHECK_EQ(net.n[1].u.state, DHP_UHRP_STANDBY);
    CHECK_EQ(active_count(&net), 1);
}

void test_uhrp_suite(void)
{
    RUN(test_single_board_elects_itself);
    RUN(test_level1_arrives_fast);
    RUN(test_higher_priority_wins);
    RUN(test_tie_breaks_on_uid);
    RUN(test_priority_from_caps);
    RUN(test_failover_releases_and_is_timed);
    RUN(test_graceful_handover_is_clean);
    RUN(test_handover_defers_while_key_held);
    RUN(test_stuck_key_deadline_forces_handover);
    RUN(test_four_boards_converge);
    RUN(test_cascading_failover);
    RUN(test_late_joiner_does_not_disrupt);
}
