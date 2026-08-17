/* SPDX-License-Identifier: GPL-2.0-only */
#include "dhp/hid.h"
#include "test_util.h"

static void test_tracks_and_releases(void)
{
    dhp_hid_tracker_t t;
    dhp_hid_tracker_init(&t);

    CHECK(!dhp_hid_any_held(&t));

    /* Ctrl down plus the 'c' key on machine 2. */
    const dhp_kbd_report_t down = {.modifiers = 0x01, .keys = {0x06}};
    dhp_hid_note_kbd(&t, 2, &down);

    CHECK(dhp_hid_any_held(&t));
    CHECK(dhp_hid_slot_held(&t, 2));
    CHECK(!dhp_hid_slot_held(&t, 3));
    CHECK_EQ(dhp_hid_key_count(&t, 2), 1);

    dhp_kbd_report_t k;
    dhp_mouse_report_t m;
    CHECK(dhp_hid_build_release(&t, 2, &k, &m));

    /* The release must clear modifiers as well as keys: a stuck Ctrl is worse
     * than a stuck letter, because every later click becomes a ctrl-click. */
    CHECK_EQ(k.modifiers, 0);
    for (int i = 0; i < 6; i++) {
        CHECK_EQ(k.keys[i], 0);
    }
    CHECK_EQ(m.buttons, 0);
    CHECK(!dhp_hid_any_held(&t));

    /* Nothing held any more, so there is nothing to send. */
    CHECK(!dhp_hid_build_release(&t, 2, &k, &m));
}

/* A machine can hold a modifier and a mouse button at once, and releasing
 * only one of them is exactly the bug this guards against. */
static void test_release_covers_both(void)
{
    dhp_hid_tracker_t t;
    dhp_hid_tracker_init(&t);

    const dhp_kbd_report_t shift = {.modifiers = 0x02};
    dhp_hid_note_kbd(&t, 1, &shift);
    dhp_hid_note_mouse(&t, 1, 0x01); /* left button down: a drag */

    CHECK(dhp_hid_slot_held(&t, 1));

    dhp_kbd_report_t k;
    dhp_mouse_report_t m;
    CHECK(dhp_hid_build_release(&t, 1, &k, &m));
    CHECK_EQ(k.modifiers, 0);
    CHECK_EQ(m.buttons, 0);
    CHECK(!dhp_hid_slot_held(&t, 1));
}

static void test_mouse_button_alone_counts_as_held(void)
{
    dhp_hid_tracker_t t;
    dhp_hid_tracker_init(&t);
    dhp_hid_note_mouse(&t, 0, 0x02);
    CHECK(dhp_hid_any_held(&t));
    dhp_hid_note_mouse(&t, 0, 0x00);
    CHECK(!dhp_hid_any_held(&t));
}

static void test_independent_slots(void)
{
    dhp_hid_tracker_t t;
    dhp_hid_tracker_init(&t);

    const dhp_kbd_report_t a = {.modifiers = 0x01};
    const dhp_kbd_report_t b = {.keys = {0x04}};
    dhp_hid_note_kbd(&t, 0, &a);
    dhp_hid_note_kbd(&t, 5, &b);

    dhp_kbd_report_t k;
    dhp_mouse_report_t m;
    CHECK(dhp_hid_build_release(&t, 0, &k, &m));

    /* Releasing one machine must not touch another. */
    CHECK(dhp_hid_slot_held(&t, 5));
    CHECK(dhp_hid_any_held(&t));
}

static void test_forget(void)
{
    dhp_hid_tracker_t t;
    dhp_hid_tracker_init(&t);
    const dhp_kbd_report_t a = {.modifiers = 0x08};
    dhp_hid_note_kbd(&t, 3, &a);
    CHECK(dhp_hid_any_held(&t));
    dhp_hid_forget(&t, 3);
    CHECK(!dhp_hid_any_held(&t));
}

void test_hid_suite(void)
{
    RUN(test_tracks_and_releases);
    RUN(test_release_covers_both);
    RUN(test_mouse_button_alone_counts_as_held);
    RUN(test_independent_slots);
    RUN(test_forget);
}
