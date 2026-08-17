/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Configuration, which is half of what level 2 adds.
 *
 * The chain works with none of this: every value below has a default that a
 * bare chain of boards already uses, and the coordinator's job is to let you
 * change them, not to supply them. That ordering matters -- if the boards
 * depended on configuration they could not reach level 1 before the
 * coordinator booted.
 */
#ifndef DHP_CONFIG_H
#define DHP_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dhp/pointer.h"
#include "dhp/uhrp.h"

typedef struct {
    /* Link */
    char     serial_dev[128];
    uint32_t link_baud;
    bool     at_head; /* coordinator is at the head of the chain, not the tail */

    /* Panel */
    char     i2c_dev[128];
    uint8_t  i2c_addr;
    bool     headless;

    /* Protocol */
    dhp_uhrp_timing_t timing;
    dhp_pointer_cfg_t pointer;

    /* Paths */
    char     key_path[256];
    char     ctl_path[256];
} coord_config_t;

void coord_config_defaults(coord_config_t *c);

/* Parses a `key = value` file, ignoring blank lines and `#` comments.
 * Returns 0 on success. Unknown keys are reported but do not fail the load:
 * a config written for a newer build should not stop an older one starting,
 * because a coordinator that refuses to start takes the display and the
 * configuration with it. */
int coord_config_load(coord_config_t *c, const char *path, char *err,
                      size_t errlen);

/* Writes the current values back, so a change made over the control socket
 * survives a restart. */
int coord_config_save(const coord_config_t *c, const char *path, char *err,
                      size_t errlen);

/* Set one key from a string, as the control socket does. Returns 0 on
 * success. */
int coord_config_set(coord_config_t *c, const char *key, const char *value,
                     char *err, size_t errlen);

/* Chain key persistence. Stored with mode 0600; see docs/protocols/auth.md for
 * what that does and does not protect against. */
bool coord_key_load(const char *path, uint8_t key[16]);
int  coord_key_save(const char *path, const uint8_t key[16], char *err,
                    size_t errlen);

#endif /* DHP_CONFIG_H */
