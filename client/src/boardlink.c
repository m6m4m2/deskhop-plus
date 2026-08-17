/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE

#include "boardlink.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* A full-speed HID interrupt endpoint carries 64 bytes, which is also
 * DHP_MAX_PAYLOAD -- so one report holds one frame's worth of payload and the
 * transport never has to split a frame across reports. */
#define HID_REPORT_BYTES 64

struct boardlink {
    boardlink_kind_t kind;
    int              fd;
};

static int open_socket(const char *path, char *err, size_t errlen)
{
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        snprintf(err, errlen, "socket: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(sa.sun_path)) {
        snprintf(err, errlen, "socket path too long");
        close(fd);
        return -1;
    }
    memcpy(sa.sun_path, path, strlen(path));

    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 &&
        errno != EINPROGRESS) {
        snprintf(err, errlen, "connect %s: %s", path, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static int open_hidraw(const char *path, char *err, size_t errlen)
{
    const int fd = open(path, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        snprintf(err, errlen, "open %s: %s%s", path, strerror(errno),
                 errno == EACCES ? " (a udev rule is probably missing;"
                                   " see docs/client.md)"
                                 : "");
        return -1;
    }
    return fd;
}

boardlink_t *boardlink_open(boardlink_kind_t kind, const char *path, char *err,
                            size_t errlen)
{
    const int fd = (kind == BOARDLINK_SOCKET) ? open_socket(path, err, errlen)
                                              : open_hidraw(path, err, errlen);
    if (fd < 0) {
        return NULL;
    }

    boardlink_t *b = calloc(1, sizeof(*b));
    if (!b) {
        close(fd);
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    b->kind = kind;
    b->fd = fd;
    return b;
}

void boardlink_close(boardlink_t *b)
{
    if (!b) {
        return;
    }
    close(b->fd);
    free(b);
}

int boardlink_fd(const boardlink_t *b) { return b->fd; }

int boardlink_write(boardlink_t *b, const uint8_t *data, size_t len)
{
    if (b->kind == BOARDLINK_SOCKET) {
        size_t off = 0;
        while (off < len) {
            const ssize_t n = write(b->fd, data + off, len - off);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break; /* the board is not draining; drop the remainder */
                }
                return -1;
            }
            off += (size_t)n;
        }
        return (int)off;
    }

    /* HID moves fixed-size reports. A short frame is padded, because the
     * report length is what the descriptor says and not what we felt like
     * sending; the receiving side finds the frame boundaries from the flag
     * bytes, so padding is invisible above this layer. */
    size_t off = 0;
    while (off < len) {
        uint8_t report[1 + HID_REPORT_BYTES];
        size_t n = len - off;
        if (n > HID_REPORT_BYTES) {
            n = HID_REPORT_BYTES;
        }
        report[0] = 0; /* report id: this interface has only one */
        memset(report + 1, 0x7E, HID_REPORT_BYTES); /* pad with idle flags */
        memcpy(report + 1, data + off, n);

        const ssize_t w = write(b->fd, report, sizeof(report));
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            return (int)off;
        }
        off += n;
    }
    return (int)off;
}

int boardlink_read(boardlink_t *b, uint8_t *buf, size_t cap)
{
    const ssize_t n = read(b->fd, buf, cap);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return 0;
        }
        return -1;
    }
    if (n == 0 && b->kind == BOARDLINK_SOCKET) {
        return -1; /* the board went away */
    }
    return (int)n;
}
