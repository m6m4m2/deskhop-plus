/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Router behaviour that is easier to pin down on a small chain than in the
 * full simulation: chain position, input authority, and focus bookkeeping.
 */
#include "sim.h"
#include "test_util.h"

#define BOARD (DHP_CAP_HID_OUT)
#define BOARD_WITH_KEYBOARD (DHP_CAP_HID_OUT | DHP_CAP_HID_IN)

static void build(sim_t *s, int n, int keyboard_at)
{
    sim_init(s, 5000);
    for (int i = 0; i < n; i++) {
        sim_add(s, i == keyboard_at ? BOARD_WITH_KEYBOARD : BOARD, false,
                (dhp_uid_t)(0x2000 + i));
    }
    for (int i = 0; i < n; i++) {
        sim_power(s, i, true);
    }
    sim_run(s, 500);
}

/* Chain position is derived from the port a frame arrives on and how far the
 * hop limit has been decremented -- no numbering protocol, no configuration. */
static void test_chain_positions_are_derived(void)
{
    sim_t s;
    build(&s, 4, 0);

    /* From board 0 at the head, the others must be at +1, +2, +3. */
    const dhp_router_t *r = &s.node[0].router;
    int found[4] = {0, 0, 0, 0};
    for (int i = 0; i < DHP_MAX_BOARDS; i++) {
        if (!r->chain[i].used || r->chain[i].addr == 1) {
            continue;
        }
        const int expect = (int)r->chain[i].addr - 1;
        CHECK_EQ(r->chain[i].rel_pos, expect);
        if (expect >= 0 && expect < 4) {
            found[expect] = 1;
        }
    }
    CHECK(found[1] && found[2] && found[3]);

    /* From the middle, boards must appear on both sides with the right sign. */
    const dhp_router_t *m = &s.node[2].router;
    for (int i = 0; i < DHP_MAX_BOARDS; i++) {
        if (!m->chain[i].used || m->chain[i].addr == 3) {
            continue;
        }
        const int expect = (int)m->chain[i].addr - 3;
        CHECK_EQ(m->chain[i].rel_pos, expect);
    }
}

/* Only the board that actually holds the input devices may inject input. */
static void test_only_input_board_can_inject(void)
{
    sim_t s;
    build(&s, 3, 1);

    const int a = sim_active(&s);
    CHECK_EQ(a, 1); /* the keyboard board is the most capable */

    const int f = sim_focus_idx(&s);
    CHECK(f >= 0);
    const int before = s.node[f].kbd_count;

    /* Board 2 has no keyboard attached; anything it claims to have captured
     * must be ignored rather than routed. */
    sim_kbd(&s, 2, 0x01, 0x04);
    sim_run(&s, 50);
    CHECK_EQ(s.node[f].kbd_count, before);

    /* Board 1 does, and its input is routed. */
    sim_kbd(&s, 1, 0x01, 0x04);
    sim_run(&s, 50);
    CHECK(s.node[f].kbd_count > before);
}

/* Every board tracks who has focus, so a display on any of them is correct
 * and a promotion inherits the right target. */
static void test_all_boards_track_focus(void)
{
    sim_t s;
    build(&s, 3, 1);

    const int a = sim_active(&s);
    dhp_router_set_focus(&s.node[a].router, 3, DHP_FOCUS_R_CFG, s.now);
    sim_run(&s, 100);

    for (int i = 0; i < 3; i++) {
        CHECK_EQ(dhp_router_focus(&s.node[i].router), 3);
    }
}

/* Pushing past the last machine in the chain must do nothing rather than
 * wrapping or losing the pointer. */
static void test_edge_of_chain_does_not_wrap(void)
{
    sim_t s;
    build(&s, 2, 0);

    const int a = sim_active(&s);
    dhp_router_set_focus(&s.node[a].router, 2, DHP_FOCUS_R_CFG, s.now);
    sim_run(&s, 50);
    CHECK_EQ(sim_focus_idx(&s), 1);

    /* Board 1 is the tail; shoving further right has nowhere to go. */
    for (int i = 0; i < 2000; i++) {
        sim_mouse_move(&s, 0, 20, 0);
        sim_step(&s);
    }
    CHECK_EQ(sim_focus_idx(&s), 1);
}

/* A mouse drag must not be broken by a switch leaving the button stuck. */
static void test_switch_releases_mouse_button(void)
{
    sim_t s;
    build(&s, 3, 1);

    const int a = sim_active(&s);
    dhp_router_set_focus(&s.node[a].router, 2, DHP_FOCUS_R_CFG, s.now);
    sim_run(&s, 50);
    const int from = sim_focus_idx(&s);
    CHECK_EQ(from, 1);

    sim_mouse_buttons(&s, 1, 0x01); /* left button down */
    sim_run(&s, 20);
    CHECK_EQ(s.node[from].held_buttons, 0x01);

    dhp_router_button(&s.node[a].router, s.now);
    sim_run(&s, 50);

    CHECK(sim_focus_idx(&s) != from);
    CHECK_EQ(s.node[from].held_buttons, 0x00);
}

/* A single board with nothing else attached is still a keyboard and mouse to
 * its own machine -- level 0 is useful, not broken. */
static void test_lone_board_still_works(void)
{
    sim_t s;
    sim_init(&s, 1000);
    sim_add(&s, BOARD_WITH_KEYBOARD, false, 0x3001);
    sim_power(&s, 0, true);
    sim_run(&s, 400);

    CHECK_EQ(sim_active(&s), 0);
    CHECK_EQ(sim_focus_idx(&s), 0);

    const int before = s.node[0].kbd_count;
    sim_kbd(&s, 0, 0x00, 0x04);
    sim_run(&s, 20);
    CHECK(s.node[0].kbd_count > before);
}

/* Level 0: a board with no chain key is still a keyboard and mouse to its own
 * machine. It cannot join a chain, but the machine it is plugged into must not
 * notice that -- this is the state every board is in before it has ever been
 * paired, and the first thing anyone tests on a bench. */
static void test_unpaired_board_still_serves_its_machine(void)
{
    sim_t s;
    sim_init(&s, 1000);
    sim_add(&s, BOARD_WITH_KEYBOARD, false, 0x4001);

    /* No key: exactly as a board comes out of the box. */
    dhp_link_clear_key(&s.node[0].link);

    sim_power(&s, 0, true);
    sim_run(&s, 400);

    /* It elects itself, because it is the only candidate. */
    CHECK_EQ(sim_active(&s), 0);
    CHECK_EQ(sim_focus_idx(&s), 0);

    /* And typing on it reaches its own machine. */
    const int before = s.node[0].kbd_count;
    sim_kbd(&s, 0, 0x00, 0x04);
    sim_run(&s, 20);
    CHECK(s.node[0].kbd_count > before);
    CHECK_EQ(s.node[0].last_kbd.keys[0], 0x04);

    /* The mouse too. */
    const int mbefore = s.node[0].mouse_count;
    sim_mouse_move(&s, 0, 10, 0);
    sim_run(&s, 20);
    CHECK(s.node[0].mouse_count > mbefore);
}

void test_router_suite(void)
{
    RUN(test_unpaired_board_still_serves_its_machine);
    RUN(test_chain_positions_are_derived);
    RUN(test_only_input_board_can_inject);
    RUN(test_all_boards_track_focus);
    RUN(test_edge_of_chain_does_not_wrap);
    RUN(test_switch_releases_mouse_button);
    RUN(test_lone_board_still_works);
}
