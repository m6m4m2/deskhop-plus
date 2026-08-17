/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "platform.h"

dhp_time_t plat_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (dhp_time_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

#define TXBUF 8192

struct serial {
    int        fd;
    dhp_port_t port;
    uint8_t    tx[TXBUF];
    size_t     tx_len;
};

/* Only the rates the chain actually uses. A non-standard rate would need
 * termios2/BOTHER, and there is no reason to run this link at one. */
static speed_t baud_constant(uint32_t baud)
{
    switch (baud) {
    case 115200:  return B115200;
    case 230400:  return B230400;
    case 460800:  return B460800;
    case 500000:  return B500000;
    case 921600:  return B921600;
    case 1000000: return B1000000;
    case 1500000: return B1500000;
    case 2000000: return B2000000;
    case 3000000: return B3000000;
    case 4000000: return B4000000;
    default:      return 0;
    }
}

serial_t *serial_open(const char *device, uint32_t baud, dhp_port_t port,
                      char *err, size_t errlen)
{
    const speed_t sp = baud_constant(baud);
    if (sp == 0) {
        snprintf(err, errlen, "unsupported baud %u", baud);
        return NULL;
    }

    const int fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        snprintf(err, errlen, "open %s: %s", device, strerror(errno));
        return NULL;
    }

    struct termios t;
    if (tcgetattr(fd, &t) != 0) {
        snprintf(err, errlen, "tcgetattr: %s", strerror(errno));
        close(fd);
        return NULL;
    }

    /* Raw 8N1, no flow control, no line discipline of any kind. The link
     * carries HDLC frames with their own delimiting and integrity, so any
     * cooking the tty layer does to the byte stream is pure corruption --
     * in particular ICRNL and OPOST would rewrite bytes inside a frame. */
    cfmakeraw(&t);
    t.c_cflag = (tcflag_t)(CS8 | CLOCAL | CREAD);
    t.c_iflag = 0;
    t.c_oflag = 0;
    t.c_lflag = 0;
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;

    cfsetispeed(&t, sp);
    cfsetospeed(&t, sp);

    if (tcsetattr(fd, TCSANOW, &t) != 0) {
        snprintf(err, errlen, "tcsetattr: %s", strerror(errno));
        close(fd);
        return NULL;
    }
    tcflush(fd, TCIOFLUSH);

    serial_t *s = calloc(1, sizeof(*s));
    if (!s) {
        close(fd);
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    s->fd = fd;
    s->port = port;
    return s;
}

void serial_close(serial_t *s)
{
    if (!s) {
        return;
    }
    close(s->fd);
    free(s);
}

int serial_fd(const serial_t *s) { return s->fd; }
dhp_port_t serial_port(const serial_t *s) { return s->port; }

int serial_write(serial_t *s, const uint8_t *data, size_t len)
{
    /* Buffer, then try to drain. The protocol layer calls this from inside
     * frame handling and must never block there: a stalled write would delay
     * the hello timer and could cost the coordinator its role. */
    if (s->tx_len + len > TXBUF) {
        /* Link is not draining. Dropping is the right failure: the frames are
         * hellos and focus updates, all of which are re-sent, and a backlog of
         * stale ones helps nobody. */
        return -1;
    }
    memcpy(s->tx + s->tx_len, data, len);
    s->tx_len += len;
    return serial_flush(s);
}

int serial_flush(serial_t *s)
{
    while (s->tx_len > 0) {
        const ssize_t n = write(s->fd, s->tx, s->tx_len);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0; /* try again next loop */
            }
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if ((size_t)n == s->tx_len) {
            s->tx_len = 0;
            return 0;
        }
        memmove(s->tx, s->tx + n, s->tx_len - (size_t)n);
        s->tx_len -= (size_t)n;
    }
    return 0;
}

int serial_read(serial_t *s, uint8_t *buf, size_t cap)
{
    const ssize_t n = read(s->fd, buf, cap);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return 0;
        }
        return -1;
    }
    return (int)n;
}
