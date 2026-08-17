/* SPDX-License-Identifier: GPL-2.0-only
 *
 * SSD1306 over Linux I2C. Transport only -- everything about what the screen
 * says lives in display.c, which needs no hardware to exercise.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "display.h"
#include "platform.h"

struct ssd1306 {
    int     fd;
    uint8_t addr;
};

/* Control byte: 0x00 introduces commands, 0x40 introduces pixel data. */
#define CTRL_CMD  0x00
#define CTRL_DATA 0x40

static int write_cmds(ssd1306_t *p, const uint8_t *cmds, size_t n)
{
    uint8_t buf[64];
    if (n + 1 > sizeof(buf)) {
        return -1;
    }
    buf[0] = CTRL_CMD;
    memcpy(buf + 1, cmds, n);
    return write(p->fd, buf, n + 1) == (ssize_t)(n + 1) ? 0 : -1;
}

ssd1306_t *ssd1306_open(const char *i2c_dev, uint8_t addr, char *err,
                        size_t errlen)
{
    const int fd = open(i2c_dev, O_RDWR);
    if (fd < 0) {
        snprintf(err, errlen, "open %s: %s", i2c_dev, strerror(errno));
        return NULL;
    }
    if (ioctl(fd, I2C_SLAVE, addr) < 0) {
        snprintf(err, errlen, "i2c addr 0x%02x: %s", addr, strerror(errno));
        close(fd);
        return NULL;
    }

    ssd1306_t *p = calloc(1, sizeof(*p));
    if (!p) {
        close(fd);
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    p->fd = fd;
    p->addr = addr;

    static const uint8_t init[] = {
        0xAE,             /* display off while we reconfigure */
        0xD5, 0x80,       /* clock: default divide, ~370 kHz oscillator */
        0xA8, 0x3F,       /* multiplex ratio: 64 rows */
        0xD3, 0x00,       /* no display offset */
        0x40,             /* start line 0 */
        0x8D, 0x14,       /* charge pump on -- without this the panel is dark
                           * even though every other register is correct, which
                           * is the classic "it initialised but shows nothing" */
        0x20, 0x00,       /* horizontal addressing: one blit covers the frame */
        0xA1,             /* segment remap, so column 0 is on the left */
        0xC8,             /* scan direction reversed, so row 0 is at the top */
        0xDA, 0x12,       /* COM pins: alternating, for a 128x64 part */
        0x81, 0x7F,       /* contrast */
        0xD9, 0xF1,       /* precharge */
        0xDB, 0x40,       /* VCOM deselect */
        0xA4,             /* follow RAM, not all-on */
        0xA6,             /* not inverted */
        0x2E,             /* scrolling off */
        0xAF,             /* display on */
    };
    if (write_cmds(p, init, sizeof(init)) != 0) {
        snprintf(err, errlen, "panel did not acknowledge: %s", strerror(errno));
        close(fd);
        free(p);
        return NULL;
    }
    return p;
}

void ssd1306_close(ssd1306_t *p)
{
    if (!p) {
        return;
    }
    const uint8_t off[] = {0xAE};
    write_cmds(p, off, sizeof(off));
    close(p->fd);
    free(p);
}

int ssd1306_blit(ssd1306_t *p, const uint8_t *fb, size_t len)
{
    if (len != DSP_FB_BYTES) {
        return -1;
    }

    const uint8_t window[] = {
        0x21, 0x00, (uint8_t)(DSP_W - 1),      /* column range */
        0x22, 0x00, (uint8_t)(DSP_PAGES - 1),  /* page range */
    };
    if (write_cmds(p, window, sizeof(window)) != 0) {
        return -1;
    }

    /* Chunked: many I2C adapters cap a single transfer well below 1 KiB, and
     * a 1024-byte write that silently truncates shows up as a screen with its
     * bottom half stale, which is a confusing thing to debug. */
    const size_t chunk = 128;
    uint8_t buf[1 + 128];
    for (size_t off = 0; off < len; off += chunk) {
        const size_t n = (len - off) < chunk ? (len - off) : chunk;
        buf[0] = CTRL_DATA;
        memcpy(buf + 1, fb + off, n);
        if (write(p->fd, buf, n + 1) != (ssize_t)(n + 1)) {
            return -1;
        }
    }
    return 0;
}
