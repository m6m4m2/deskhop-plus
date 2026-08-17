/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void coord_config_defaults(coord_config_t *c)
{
    memset(c, 0, sizeof(*c));

    /* serial0 rather than ttyAMA0: on a Pi it is the symlink that always
     * points at whichever UART is actually on the GPIO header, which differs
     * between models and with dtoverlay=disable-bt. */
    snprintf(c->serial_dev, sizeof(c->serial_dev), "/dev/serial0");
    c->link_baud = 2000000;
    c->at_head = false; /* plugged into the tail of the chain by default */

    snprintf(c->i2c_dev, sizeof(c->i2c_dev), "/dev/i2c-1");
    c->i2c_addr = 0x3C;
    c->headless = false;

    c->timing = DHP_UHRP_TIMING_DEFAULT;
    c->pointer = DHP_POINTER_CFG_DEFAULT;

    snprintf(c->key_path, sizeof(c->key_path),
             "/var/lib/deskhop/chain.key");
    snprintf(c->ctl_path, sizeof(c->ctl_path), "/run/deskhop-coord.sock");
}

static void trim(char *s)
{
    char *p = s;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (p != s) {
        memmove(s, p, strlen(p) + 1);
    }
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n' ||
                     s[n - 1] == '\r')) {
        s[--n] = '\0';
    }
}

static bool parse_bool(const char *v, bool *out)
{
    if (!strcmp(v, "1") || !strcasecmp(v, "true") || !strcasecmp(v, "yes")) {
        *out = true;
        return true;
    }
    if (!strcmp(v, "0") || !strcasecmp(v, "false") || !strcasecmp(v, "no")) {
        *out = false;
        return true;
    }
    return false;
}

int coord_config_set(coord_config_t *c, const char *key, const char *value,
                     char *err, size_t errlen)
{
#define STR_KEY(name, field)                                                   \
    if (!strcmp(key, name)) {                                                  \
        snprintf(c->field, sizeof(c->field), "%s", value);                     \
        return 0;                                                              \
    }
#define U32_KEY(name, field, lo, hi)                                           \
    if (!strcmp(key, name)) {                                                  \
        char *end;                                                             \
        const long v = strtol(value, &end, 0);                                 \
        if (*end || v < (lo) || v > (hi)) {                                    \
            snprintf(err, errlen, "%s must be %ld..%ld", name, (long)(lo),     \
                     (long)(hi));                                              \
            return -1;                                                         \
        }                                                                      \
        c->field = (typeof(c->field))v;                                        \
        return 0;                                                              \
    }
#define BOOL_KEY(name, field)                                                  \
    if (!strcmp(key, name)) {                                                  \
        if (!parse_bool(value, &c->field)) {                                   \
            snprintf(err, errlen, "%s must be true or false", name);           \
            return -1;                                                         \
        }                                                                      \
        return 0;                                                              \
    }

    STR_KEY("serial_dev", serial_dev)
    STR_KEY("i2c_dev", i2c_dev)
    STR_KEY("key_path", key_path)
    STR_KEY("ctl_path", ctl_path)

    U32_KEY("link_baud", link_baud, 115200, 4000000)
    U32_KEY("i2c_addr", i2c_addr, 0x03, 0x77)

    BOOL_KEY("at_head", at_head)
    BOOL_KEY("headless", headless)

    /* Election timing. The bounds are not arbitrary: a hold time below two
     * hello intervals turns a single dropped frame into a role change, and
     * above about a second the failover stops feeling immediate. */
    U32_KEY("hello_ms", timing.hello_ms, 10, 1000)
    U32_KEY("hold_ms", timing.hold_ms, 30, 3000)
    U32_KEY("handover_defer_max_ms", timing.handover_defer_max_ms, 100, 30000)

    U32_KEY("pointer_span", pointer.span, 500, 100000)
    U32_KEY("pointer_push", pointer.push_threshold, 50, 10000)
    U32_KEY("pointer_decay", pointer.push_decay_per_ms, 0, 100)
    U32_KEY("pointer_guard", pointer.reentry_guard, 0, 50000)
    BOOL_KEY("pointer_vertical", pointer.vertical_enabled)

#undef STR_KEY
#undef U32_KEY
#undef BOOL_KEY

    snprintf(err, errlen, "unknown key '%s'", key);
    return -1;
}

int coord_config_load(coord_config_t *c, const char *path, char *err,
                      size_t errlen)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        /* Absent is fine: the defaults are a working configuration, and this
         * file exists to change them rather than to supply them. */
        if (errno == ENOENT) {
            return 0;
        }
        snprintf(err, errlen, "open %s: %s", path, strerror(errno));
        return -1;
    }

    char line[512];
    int lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;

        char *hash = strchr(line, '#');
        if (hash) {
            *hash = '\0';
        }
        trim(line);
        if (line[0] == '\0') {
            continue;
        }

        char *eq = strchr(line, '=');
        if (!eq) {
            fprintf(stderr, "%s:%d: expected key = value\n", path, lineno);
            continue;
        }
        *eq = '\0';
        char *k = line, *v = eq + 1;
        trim(k);
        trim(v);

        char kerr[128];
        if (coord_config_set(c, k, v, kerr, sizeof(kerr)) != 0) {
            /* Reported, not fatal. A coordinator that refuses to start takes
             * the display and the configuration down with it, which is a much
             * worse outcome than one stale key. */
            fprintf(stderr, "%s:%d: %s (ignored)\n", path, lineno, kerr);
        }
    }

    fclose(f);
    return 0;
}

int coord_config_save(const coord_config_t *c, const char *path, char *err,
                      size_t errlen)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    FILE *f = fopen(tmp, "w");
    if (!f) {
        snprintf(err, errlen, "open %s: %s", tmp, strerror(errno));
        return -1;
    }

    fprintf(f, "# deskhop coordinator configuration\n");
    fprintf(f, "# written automatically; edits are preserved on next save\n\n");
    fprintf(f, "serial_dev = %s\n", c->serial_dev);
    fprintf(f, "link_baud = %u\n", c->link_baud);
    fprintf(f, "at_head = %s\n", c->at_head ? "true" : "false");
    fprintf(f, "\ni2c_dev = %s\n", c->i2c_dev);
    fprintf(f, "i2c_addr = 0x%02x\n", c->i2c_addr);
    fprintf(f, "headless = %s\n", c->headless ? "true" : "false");
    fprintf(f, "\nhello_ms = %u\n", c->timing.hello_ms);
    fprintf(f, "hold_ms = %u\n", c->timing.hold_ms);
    fprintf(f, "handover_defer_max_ms = %u\n", c->timing.handover_defer_max_ms);
    fprintf(f, "\npointer_span = %d\n", c->pointer.span);
    fprintf(f, "pointer_push = %d\n", c->pointer.push_threshold);
    fprintf(f, "pointer_decay = %d\n", c->pointer.push_decay_per_ms);
    fprintf(f, "pointer_guard = %d\n", c->pointer.reentry_guard);
    fprintf(f, "pointer_vertical = %s\n",
            c->pointer.vertical_enabled ? "true" : "false");
    fprintf(f, "\nkey_path = %s\n", c->key_path);
    fprintf(f, "ctl_path = %s\n", c->ctl_path);

    if (fclose(f) != 0) {
        snprintf(err, errlen, "write %s: %s", tmp, strerror(errno));
        return -1;
    }

    /* Rename over the original, so a crash mid-write cannot leave a truncated
     * config that the next start would silently accept. */
    if (rename(tmp, path) != 0) {
        snprintf(err, errlen, "rename: %s", strerror(errno));
        return -1;
    }
    return 0;
}

bool coord_key_load(const char *path, uint8_t key[16])
{
    const int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return false;
    }
    const ssize_t n = read(fd, key, 16);
    close(fd);
    return n == 16;
}

int coord_key_save(const char *path, const uint8_t key[16], char *err,
                   size_t errlen)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    /* 0600 from the moment it exists: creating it readable and chmod-ing
     * afterwards leaves a window in which the chain key is world-readable. */
    const int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        snprintf(err, errlen, "open %s: %s", tmp, strerror(errno));
        return -1;
    }
    const ssize_t n = write(fd, key, 16);
    if (n != 16) {
        snprintf(err, errlen, "write: %s", strerror(errno));
        close(fd);
        unlink(tmp);
        return -1;
    }
    fsync(fd);
    close(fd);

    if (rename(tmp, path) != 0) {
        snprintf(err, errlen, "rename: %s", strerror(errno));
        unlink(tmp);
        return -1;
    }
    return 0;
}
