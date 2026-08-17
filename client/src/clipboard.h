/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Clipboard access, via helper commands rather than by linking a display
 * server library.
 *
 * The choice is deliberate. Linking libX11 or libwayland-client would tie one
 * binary to one display stack, and a desktop can be X11, Wayland, both at once
 * through XWayland, or neither if the client is running headless on a server.
 * Shelling out to wl-copy or xclip means the same binary works everywhere the
 * user's own scripts already work, and it makes the whole thing testable:
 * the tests point copy_cmd at a file.
 *
 * The cost is a process spawn per clipboard operation, which is irrelevant at
 * the rate a human copies things.
 */
#ifndef DHP_CLIPBOARD_H
#define DHP_CLIPBOARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    /* Reads the clipboard and writes it to stdout. */
    char paste_cmd[256];
    /* Reads stdin and puts it on the clipboard. */
    char copy_cmd[256];
    /* Same pair for image/png, when the desktop distinguishes them. */
    char paste_image_cmd[256];
    char copy_image_cmd[256];
} clipboard_cfg_t;

/* Picks sensible commands for whatever is running: Wayland first, then X11.
 * Leaves them empty if neither tool is installed, in which case the client
 * runs with clipboard sync disabled and says so once. */
void clipboard_autodetect(clipboard_cfg_t *c);

/* Reads the current clipboard. Returns a malloc'd buffer the caller frees, or
 * NULL. `*len` is set on success. */
uint8_t *clipboard_get(const clipboard_cfg_t *c, bool image, size_t *len);

/* Writes to the clipboard. Returns true on success. */
bool clipboard_set(const clipboard_cfg_t *c, const uint8_t *data, size_t len,
                   bool image);

bool clipboard_available(const clipboard_cfg_t *c);

#endif /* DHP_CLIPBOARD_H */
