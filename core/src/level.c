/* SPDX-License-Identifier: GPL-2.0-only */
#include "dhp/level.h"

const char *dhp_level_name(dhp_level_t l)
{
    switch (l) {
    case DHP_LEVEL_ISOLATED:    return "isolated";
    case DHP_LEVEL_ELECTED:     return "elected";
    case DHP_LEVEL_COORDINATED: return "coordinated";
    case DHP_LEVEL_RICH:        return "rich";
    default:                    return "?";
    }
}

dhp_level_t dhp_level_of(uint16_t caps, bool elected)
{
    /* Levels are cumulative: you cannot be at level 3 without the level 1
     * capabilities, because clipboard without a shared keyboard is not this
     * product. Each test therefore builds on the one below it. */
    if (!elected || (caps & DHP_CAPS_LEVEL1) != DHP_CAPS_LEVEL1) {
        return DHP_LEVEL_ISOLATED;
    }
    if ((caps & DHP_CAPS_LEVEL2) != DHP_CAPS_LEVEL2) {
        return DHP_LEVEL_ELECTED;
    }
    if ((caps & DHP_CAPS_LEVEL3) != DHP_CAPS_LEVEL3) {
        return DHP_LEVEL_COORDINATED;
    }
    return DHP_LEVEL_RICH;
}
