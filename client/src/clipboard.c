/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE

#include "clipboard.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Anything bigger than this is not a clipboard, and pushing it through a
 * keyboard cable would be the wrong move anyway -- see DHP_DATA_SHARE. */
#define CLIP_MAX (4u * 1024u * 1024u)

static bool have(const char *tool)
{
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "command -v %s >/dev/null 2>&1", tool);
    return system(cmd) == 0;
}

void clipboard_autodetect(clipboard_cfg_t *c)
{
    memset(c, 0, sizeof(*c));

    const bool wayland = getenv("WAYLAND_DISPLAY") != NULL;

    if (wayland && have("wl-copy")) {
        snprintf(c->paste_cmd, sizeof(c->paste_cmd),
                 "wl-paste --no-newline 2>/dev/null");
        snprintf(c->copy_cmd, sizeof(c->copy_cmd), "wl-copy");
        snprintf(c->paste_image_cmd, sizeof(c->paste_image_cmd),
                 "wl-paste --type image/png 2>/dev/null");
        snprintf(c->copy_image_cmd, sizeof(c->copy_image_cmd),
                 "wl-copy --type image/png");
        return;
    }

    if (have("xclip")) {
        snprintf(c->paste_cmd, sizeof(c->paste_cmd),
                 "xclip -selection clipboard -o 2>/dev/null");
        snprintf(c->copy_cmd, sizeof(c->copy_cmd),
                 "xclip -selection clipboard -i");
        snprintf(c->paste_image_cmd, sizeof(c->paste_image_cmd),
                 "xclip -selection clipboard -t image/png -o 2>/dev/null");
        snprintf(c->copy_image_cmd, sizeof(c->copy_image_cmd),
                 "xclip -selection clipboard -t image/png -i");
        return;
    }

    /* Left empty on purpose: the client is still useful for file and share
     * transfers without a clipboard, so this is not a fatal condition. */
}

bool clipboard_available(const clipboard_cfg_t *c)
{
    return c->paste_cmd[0] != '\0' && c->copy_cmd[0] != '\0';
}

uint8_t *clipboard_get(const clipboard_cfg_t *c, bool image, size_t *len)
{
    const char *cmd = image ? c->paste_image_cmd : c->paste_cmd;
    if (!cmd[0]) {
        return NULL;
    }

    FILE *p = popen(cmd, "r");
    if (!p) {
        return NULL;
    }

    size_t cap = 8192, n = 0;
    uint8_t *buf = malloc(cap);
    if (!buf) {
        pclose(p);
        return NULL;
    }

    for (;;) {
        if (n == cap) {
            if (cap >= CLIP_MAX) {
                /* Stop reading rather than growing without bound: a helper
                 * pointed at the wrong thing could otherwise stream forever. */
                break;
            }
            cap *= 2;
            uint8_t *bigger = realloc(buf, cap);
            if (!bigger) {
                free(buf);
                pclose(p);
                return NULL;
            }
            buf = bigger;
        }
        const size_t got = fread(buf + n, 1, cap - n, p);
        if (got == 0) {
            break;
        }
        n += got;
    }

    pclose(p);

    if (n == 0) {
        free(buf);
        return NULL;
    }
    *len = n;
    return buf;
}

bool clipboard_set(const clipboard_cfg_t *c, const uint8_t *data, size_t len,
                   bool image)
{
    const char *cmd = image ? c->copy_image_cmd : c->copy_cmd;
    if (!cmd[0]) {
        return false;
    }

    FILE *p = popen(cmd, "w");
    if (!p) {
        return false;
    }
    const size_t n = fwrite(data, 1, len, p);
    const int rc = pclose(p);
    return n == len && rc == 0;
}
