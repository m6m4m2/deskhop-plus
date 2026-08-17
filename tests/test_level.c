/* SPDX-License-Identifier: GPL-2.0-only */
#include "dhp/level.h"
#include "test_util.h"

/* The rule the whole staged-startup design rests on: capability follows the
 * hardware actually present, so the system never advertises what it cannot
 * currently deliver. */
static void test_levels_follow_capability(void)
{
    const uint16_t l1 = DHP_CAP_HID_OUT | DHP_CAP_HID_IN;
    const uint16_t l2 = l1 | DHP_CAP_DISPLAY | DHP_CAP_CONFIG;
    const uint16_t l3 = l2 | DHP_CAP_CLIPBOARD;

    /* No election settled yet: nothing can be routed, whatever is attached. */
    CHECK_EQ(dhp_level_of(l3, false), DHP_LEVEL_ISOLATED);

    /* A board on its own with no input device cannot reach level 1. */
    CHECK_EQ(dhp_level_of(DHP_CAP_HID_OUT, true), DHP_LEVEL_ISOLATED);

    CHECK_EQ(dhp_level_of(l1, true), DHP_LEVEL_ELECTED);
    CHECK_EQ(dhp_level_of(l2, true), DHP_LEVEL_COORDINATED);
    CHECK_EQ(dhp_level_of(l3, true), DHP_LEVEL_RICH);
}

/* Levels are cumulative. A clipboard client on a chain with no coordinator
 * must not report level 3, because the display and configuration that level 2
 * promises genuinely are not there. */
static void test_levels_are_cumulative(void)
{
    const uint16_t no_coord =
        DHP_CAP_HID_OUT | DHP_CAP_HID_IN | DHP_CAP_CLIPBOARD;
    CHECK_EQ(dhp_level_of(no_coord, true), DHP_LEVEL_ELECTED);
}

/* Losing the coordinator drops the level back rather than stopping. */
static void test_degrades_symmetrically(void)
{
    const uint16_t full = DHP_CAP_HID_OUT | DHP_CAP_HID_IN | DHP_CAP_DISPLAY |
                          DHP_CAP_CONFIG | DHP_CAP_CLIPBOARD;
    CHECK_EQ(dhp_level_of(full, true), DHP_LEVEL_RICH);

    const uint16_t coord_gone = full & ~(uint16_t)(DHP_CAP_DISPLAY | DHP_CAP_CONFIG);
    CHECK_EQ(dhp_level_of(coord_gone, true), DHP_LEVEL_ELECTED);
}

static void test_names(void)
{
    CHECK_STR(dhp_level_name(DHP_LEVEL_ISOLATED), "isolated");
    CHECK_STR(dhp_level_name(DHP_LEVEL_RICH), "rich");
}

void test_level_suite(void)
{
    RUN(test_levels_follow_capability);
    RUN(test_levels_are_cumulative);
    RUN(test_degrades_symmetrically);
    RUN(test_names);
}
