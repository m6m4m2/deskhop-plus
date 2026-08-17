/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Capability levels.
 *
 * The central idea: the system never advertises a capability it cannot
 * currently deliver, and it never waits for a capability it does not have.
 * Levels are therefore derived state, recomputed from what hardware is
 * actually present and answering right now -- not a configured mode, and not
 * a boot sequence with a fixed order.
 *
 *   Level 0  ISOLATED   One board, no chain. It is still a keyboard and mouse
 *                       to its own machine; it just has nowhere to switch to.
 *
 *   Level 1  ELECTED    Two or more boards have agreed an active speaker.
 *                       Keyboard, mouse, switching and pointer crossing all
 *                       work. Reached in milliseconds after power-on, because
 *                       it needs nothing but the boards themselves.
 *
 *   Level 2  COORDINATED A coordinator has booted and taken the active role.
 *                       Adds the status display and configuration. The
 *                       handover into this level is planned, not a restart.
 *
 *   Level 3  RICH       A client is running on at least one machine. Adds
 *                       clipboard, text, files, folders, images, SMB share --
 *                       but only for the machines that actually run a client.
 *
 * Level 3 is per-machine, not global: the system can be at level 3 for one
 * board and level 2 for its neighbour at the same instant. dhp_level_of()
 * answers for one board; dhp_level_system() answers for the chain.
 */
#ifndef DHP_LEVEL_H
#define DHP_LEVEL_H

#include "dhp/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DHP_LEVEL_ISOLATED    = 0,
    DHP_LEVEL_ELECTED     = 1,
    DHP_LEVEL_COORDINATED = 2,
    DHP_LEVEL_RICH        = 3,
} dhp_level_t;

/* What a board can contribute. Advertised in HELLO and TOPO so that every
 * board's view of the system's capability is built from evidence rather than
 * from configuration. */
typedef enum {
    DHP_CAP_HID_OUT   = (1u << 0), /* presents as keyboard+mouse to a machine */
    DHP_CAP_HID_IN    = (1u << 1), /* has the real keyboard/mouse attached */
    DHP_CAP_DISPLAY   = (1u << 2), /* can drive the status display */
    DHP_CAP_CONFIG    = (1u << 3), /* holds and serves configuration */
    DHP_CAP_CLIPBOARD = (1u << 4), /* level 3 client attached, text */
    DHP_CAP_FILES     = (1u << 5), /* level 3 client attached, files/folders */
    DHP_CAP_IMAGES    = (1u << 6),
    DHP_CAP_SHARE     = (1u << 7), /* SMB share */
    DHP_CAP_COORD     = (1u << 8), /* is a coordinator, not a chain board */
} dhp_cap_t;

/* Capability sets that define each level. A level is reached exactly when
 * every capability in its set is present somewhere that can serve it. */
#define DHP_CAPS_LEVEL1 (DHP_CAP_HID_OUT | DHP_CAP_HID_IN)
#define DHP_CAPS_LEVEL2 (DHP_CAP_DISPLAY | DHP_CAP_CONFIG)
#define DHP_CAPS_LEVEL3 (DHP_CAP_CLIPBOARD)

const char *dhp_level_name(dhp_level_t l);

/* Level for one board, given the capabilities it can reach (its own plus
 * whatever the chain currently offers) and whether an election has settled. */
dhp_level_t dhp_level_of(uint16_t reachable_caps, bool elected);

#ifdef __cplusplus
}
#endif

#endif /* DHP_LEVEL_H */
