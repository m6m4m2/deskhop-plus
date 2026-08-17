/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Whole-system behaviour: a chain of boards on simulated UARTs, with the
 * coordinator arriving late and leaving abruptly. These are the claims the
 * design is actually built around, measured end to end.
 */
#include "sim.h"
#include "test_util.h"

#define BOARD (DHP_CAP_HID_OUT)
#define BOARD_WITH_KEYBOARD (DHP_CAP_HID_OUT | DHP_CAP_HID_IN)
#define COORDINATOR (DHP_CAP_COORD | DHP_CAP_DISPLAY | DHP_CAP_CONFIG)

/* Three boards, keyboard on the middle one. */
static void build_chain(sim_t *s)
{
    sim_init(s, 1000);
    sim_add(s, BOARD, false, 0x1001);
    sim_add(s, BOARD_WITH_KEYBOARD, false, 0x1002);
    sim_add(s, BOARD, false, 0x1003);
    for (int i = 0; i < 3; i++) {
        sim_power(s, i, true);
    }
}

/* Level 1 must be usable in milliseconds -- the whole point of not waiting
 * for the coordinator. */
static void test_level1_comes_up_immediately(void)
{
    sim_t s;
    build_chain(&s);

    const dhp_time_t t0 = s.now;
    while (sim_active(&s) < 0 && s.now - t0 < 1000) {
        sim_step(&s);
    }
    const uint32_t ms = s.now - t0;

    CHECK_EQ(sim_active_count(&s), 1);
    CHECK(ms <= 250);
    printf("    level 1 usable after %u ms\n", ms);

    /* The board holding the keyboard is the most capable, so it wins. */
    CHECK_EQ(sim_active(&s), 1);

    sim_run(&s, 200);
    CHECK_EQ(s.node[1].router.level, DHP_LEVEL_ELECTED);
}

/* Typing reaches the focused machine and nothing else. */
static void test_typing_reaches_focused_machine(void)
{
    sim_t s;
    build_chain(&s);
    sim_run(&s, 400);

    const int f = sim_focus_idx(&s);
    CHECK(f >= 0);

    const int before_other = s.node[(f + 1) % 3].kbd_count;

    sim_kbd(&s, 1, 0x00, 0x04); /* 'a' down */
    sim_run(&s, 20);
    sim_kbd(&s, 1, 0x00, 0x00); /* up */
    sim_run(&s, 20);

    CHECK(s.node[f].kbd_count >= 2);
    CHECK_EQ(s.node[(f + 1) % 3].kbd_count, before_other);
}

/* Pushing the pointer into the screen edge moves to the next machine, with no
 * screen geometry configured anywhere. */
static void test_pointer_crossing_switches(void)
{
    sim_t s;
    build_chain(&s);
    sim_run(&s, 400);

    const int before = sim_focus_idx(&s);
    CHECK(before >= 0);

    CHECK(sim_push_edge(&s, 1, +1, 3000));
    sim_run(&s, 50);

    const int after = sim_focus_idx(&s);
    CHECK(after != before);
    CHECK_EQ(after, before + 1);
}

/* The feature that stops a machine being abandoned mid-chord: switching away
 * while a modifier is down must release it on the machine being left. */
static void test_switch_releases_held_keys(void)
{
    sim_t s;
    build_chain(&s);
    sim_run(&s, 400);

    const int from = sim_focus_idx(&s);
    CHECK(from >= 0);

    /* Hold Ctrl down and leave it down. */
    sim_kbd(&s, 1, 0x01, 0x00);
    sim_run(&s, 20);
    CHECK_EQ(s.node[from].held_mods, 0x01);

    /* Now switch by pushing the pointer through the edge. */
    CHECK(sim_push_edge(&s, 1, +1, 3000));
    sim_run(&s, 50);

    const int to = sim_focus_idx(&s);
    CHECK(to != from);

    /* The abandoned machine must no longer believe Ctrl is down. */
    CHECK_EQ(s.node[from].held_mods, 0x00);
    CHECK(s.node[from].kbd_release_count > 0);
}

/* Level 2: the coordinator boots long after everything else is working, takes
 * the role by a planned handover, and interrupts nothing. */
static void test_coordinator_takes_over_cleanly(void)
{
    sim_t s;
    build_chain(&s);
    sim_run(&s, 400);

    CHECK_EQ(sim_active(&s), 1);
    CHECK_EQ(s.node[1].router.level, DHP_LEVEL_ELECTED);

    /* Focus somewhere specific so we can prove it survives the handover. */
    dhp_router_set_focus(&s.node[1].router, 3, DHP_FOCUS_R_CFG, s.now);
    sim_run(&s, 50);
    CHECK_EQ(sim_focus_idx(&s), 2);

    /* The coordinator finishes its fifteen-to-twenty second boot and joins at
     * the tail of the chain. */
    const int c = sim_add(&s, COORDINATOR, true, 0x9000);
    sim_power(&s, c, true);
    sim_run(&s, 1000);

    /* It now holds the role... */
    CHECK_EQ(sim_active(&s), c);
    CHECK_EQ(sim_active_count(&s), 1);

    /* ...the system has reached level 2... */
    CHECK_EQ(s.node[c].router.level, DHP_LEVEL_COORDINATED);
    CHECK_EQ(s.node[1].router.level, DHP_LEVEL_COORDINATED);

    /* ...the user is still on the same machine... */
    CHECK_EQ(sim_focus_idx(&s), 2);

    /* ...and nothing was left stuck, because this was a handover and not a
     * failure: no board received a forced release. */
    for (int i = 0; i < 3; i++) {
        CHECK_EQ(s.node[i].kbd_release_count, 0);
    }
}

/* With the coordinator active, the keyboard is attached to a different board.
 * Input must still work: capture and authority are separate jobs. */
static void test_typing_works_while_coordinator_is_active(void)
{
    sim_t s;
    build_chain(&s);
    sim_run(&s, 400);

    const int c = sim_add(&s, COORDINATOR, true, 0x9000);
    sim_power(&s, c, true);
    sim_run(&s, 1000);
    CHECK_EQ(sim_active(&s), c);

    const int f = sim_focus_idx(&s);
    CHECK(f >= 0);
    const int before = s.node[f].kbd_count;

    /* The keyboard is on board 1, the authority is the coordinator. */
    sim_kbd(&s, 1, 0x00, 0x07);
    sim_run(&s, 40);
    sim_kbd(&s, 1, 0x00, 0x00);
    sim_run(&s, 40);

    CHECK(s.node[f].kbd_count > before);
    CHECK_EQ(s.node[f].last_kbd.keys[0], 0x00); /* the key-up arrived */
}

/* Unplug the coordinator: the system must drop back to level 1 rather than
 * stop, and do it inside roughly 150 ms. */
static void test_coordinator_loss_degrades_to_level1(void)
{
    sim_t s;
    build_chain(&s);
    sim_run(&s, 400);

    const int c = sim_add(&s, COORDINATOR, true, 0x9000);
    sim_power(&s, c, true);
    sim_run(&s, 1000);
    CHECK_EQ(sim_active(&s), c);
    CHECK_EQ(s.node[1].router.level, DHP_LEVEL_COORDINATED);

    /* Yank it. */
    sim_power(&s, c, false);
    const dhp_time_t t0 = s.now;

    while (sim_active(&s) < 0 && s.now - t0 < 2000) {
        sim_step(&s);
    }
    const uint32_t recovery_ms = s.now - t0;

    CHECK(sim_active(&s) >= 0);
    CHECK(sim_active(&s) != c);
    CHECK(recovery_ms <= 250);
    printf("    dropped back to level 1 in %u ms\n", recovery_ms);

    sim_run(&s, 300);
    CHECK_EQ(s.node[1].router.level, DHP_LEVEL_ELECTED);
    CHECK_EQ(sim_active_count(&s), 1);

    /* And typing still works afterwards. */
    const int f = sim_focus_idx(&s);
    CHECK(f >= 0);
    const int before = s.node[f].kbd_count;
    sim_kbd(&s, 1, 0x00, 0x05);
    sim_run(&s, 40);
    CHECK(s.node[f].kbd_count > before);
}

/* Rule 2 end to end: the active speaker dies while a key is held, and every
 * machine must be let go of rather than left holding it. */
static void test_abrupt_loss_releases_everything(void)
{
    sim_t s;
    build_chain(&s);
    sim_run(&s, 400);

    const int c = sim_add(&s, COORDINATOR, true, 0x9000);
    sim_power(&s, c, true);
    sim_run(&s, 1000);
    CHECK_EQ(sim_active(&s), c);

    const int f = sim_focus_idx(&s);
    CHECK(f >= 0);

    /* Hold Ctrl, then kill the authority without warning. */
    sim_kbd(&s, 1, 0x01, 0x00);
    sim_run(&s, 30);
    CHECK_EQ(s.node[f].held_mods, 0x01);

    sim_power(&s, c, false);
    sim_run(&s, 600);

    /* Somebody took over, and nothing is still held anywhere. */
    CHECK(sim_active(&s) >= 0);
    for (int i = 0; i < 3; i++) {
        CHECK_EQ(s.node[i].held_mods, 0x00);
        CHECK_EQ(s.node[i].held_buttons, 0x00);
    }
}

/* Extending the chain requires no configuration: a new board joins a running
 * system and becomes reachable by pointer crossing. */
static void test_adding_a_board_needs_no_configuration(void)
{
    sim_t s;
    build_chain(&s);
    sim_run(&s, 400);
    CHECK_EQ(dhp_router_chain_size(&s.node[1].router), 3);

    /* Plug in a fourth board at the tail. */
    const int d = sim_add(&s, BOARD, false, 0x1004);
    sim_power(&s, d, true);
    sim_run(&s, 500);

    /* Every board now knows about it, with no configuration step. */
    CHECK_EQ(dhp_router_chain_size(&s.node[1].router), 4);
    CHECK_EQ(sim_active_count(&s), 1);

    /* And the user can reach it by pushing through the edges. */
    CHECK(sim_push_edge(&s, 1, +1, 3000));
    sim_run(&s, 50);
    CHECK_EQ(sim_focus_idx(&s), 2);

    CHECK(sim_push_edge(&s, 1, +1, 3000));
    sim_run(&s, 50);
    CHECK_EQ(sim_focus_idx(&s), 3);
}

/* The button walks the chain and wraps. */
static void test_button_cycles(void)
{
    sim_t s;
    build_chain(&s);
    sim_run(&s, 400);

    const int a = sim_active(&s);
    int seen[3] = {0, 0, 0};

    for (int i = 0; i < 6; i++) {
        const int f = sim_focus_idx(&s);
        CHECK(f >= 0 && f < 3);
        seen[f] = 1;
        dhp_router_button(&s.node[a].router, s.now);
        sim_run(&s, 30);
    }
    CHECK(seen[0] && seen[1] && seen[2]);
}

/* Cutting the chain in half must leave each fragment working rather than
 * leaving the system dead. */
static void test_chain_cut_leaves_both_halves_working(void)
{
    sim_t s;
    build_chain(&s);
    sim_run(&s, 400);
    CHECK_EQ(sim_active_count(&s), 1);

    /* Cut between board 1 and board 2. */
    sim_cut(&s, 1, true);
    sim_run(&s, 800);

    /* Each side now has its own authority: two active speakers, because they
     * are genuinely two separate systems now. */
    CHECK_EQ(sim_active_count(&s), 2);
    CHECK(dhp_router_is_active(&s.node[1].router)); /* keeps the keyboard side */
    CHECK(dhp_router_is_active(&s.node[2].router));

    /* Reconnect: they must merge back to a single authority. */
    sim_cut(&s, 1, false);
    sim_run(&s, 1500);
    CHECK_EQ(sim_active_count(&s), 1);
}

void test_sim_suite(void)
{
    RUN(test_level1_comes_up_immediately);
    RUN(test_typing_reaches_focused_machine);
    RUN(test_pointer_crossing_switches);
    RUN(test_switch_releases_held_keys);
    RUN(test_coordinator_takes_over_cleanly);
    RUN(test_typing_works_while_coordinator_is_active);
    RUN(test_coordinator_loss_degrades_to_level1);
    RUN(test_abrupt_loss_releases_everything);
    RUN(test_adding_a_board_needs_no_configuration);
    RUN(test_button_cycles);
    RUN(test_chain_cut_leaves_both_halves_working);
}
