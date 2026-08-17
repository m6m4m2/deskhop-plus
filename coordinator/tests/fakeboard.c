/* SPDX-License-Identifier: GPL-2.0-only
 *
 * End-to-end test for the coordinator daemon.
 *
 * Everything else in this tree is tested either in-process (core/) or by
 * compiling (firmware/). This is the one place the daemon is actually run:
 * a pseudo-terminal stands in for the UART, this program plays a chain board
 * on one end using the real core/, and the real deskhop-coord binary is
 * exec'd on the other.
 *
 * That exercises what unit tests cannot -- the termios setup, the serial
 * read/write path, frame assembly across arbitrary read boundaries, and the
 * daemon's whole event loop -- against the behaviour that matters:
 *
 *   1. a lone board reaches level 1 by itself, before the coordinator exists;
 *   2. the coordinator preempts and takes the routing role;
 *   3. the level rises to 2 without the board being disturbed;
 *   4. killing the coordinator drops it back to level 1 within a hold time.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "dhp/link.h"
#include "dhp/router.h"

static int failures;
static int checks;

#define CHECK(cond, what)                                                      \
    do {                                                                       \
        checks++;                                                              \
        if (!(cond)) {                                                         \
            failures++;                                                        \
            printf("  FAIL %s\n", what);                                       \
        } else {                                                               \
            printf("  ok   %s\n", what);                                       \
        }                                                                      \
    } while (0)

/* The board and the daemon must already share a chain key; pairing itself is
 * covered by tests/test_auth.c and is not what this test is about. */
static const uint8_t CHAIN_KEY[16] = {
    0x5A, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
    0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xF0,
};

static int g_pty;

static dhp_time_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (dhp_time_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

static void board_tx(void *ctx, dhp_port_t port, const uint8_t *data, size_t len)
{
    (void)ctx;
    /* This board is at the head; the coordinator hangs off its down port. */
    if (port != DHP_PORT_DOWN) {
        return;
    }
    ssize_t off = 0;
    while (off < (ssize_t)len) {
        const ssize_t n = write(g_pty, data + off, len - (size_t)off);
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                continue;
            }
            return;
        }
        off += n;
    }
}

static void nop_kbd(void *ctx, const dhp_kbd_report_t *r) { (void)ctx; (void)r; }
static void nop_mouse(void *ctx, const dhp_mouse_report_t *r) { (void)ctx; (void)r; }

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <path-to-deskhop-coord>\n", argv[0]);
        return 2;
    }
    const char *coord_bin = argv[1];

    /* --- the virtual null-modem --- */
    int master, slave;
    char slave_name[128];
    if (openpty(&master, &slave, slave_name, NULL, NULL) != 0) {
        fprintf(stderr, "openpty: %s\n", strerror(errno));
        return 2;
    }

    /* Raw on both ends: the link carries HDLC frames with their own framing
     * and integrity, so any line discipline is pure corruption. */
    struct termios t;
    tcgetattr(master, &t);
    cfmakeraw(&t);
    tcsetattr(master, TCSANOW, &t);
    tcgetattr(slave, &t);
    cfmakeraw(&t);
    tcsetattr(slave, TCSANOW, &t);

    fcntl(master, F_SETFL, O_NONBLOCK);
    g_pty = master;

    /* --- scratch state for the daemon --- */
    char dir[] = "/tmp/dhpcoordXXXXXX";
    if (!mkdtemp(dir)) {
        fprintf(stderr, "mkdtemp: %s\n", strerror(errno));
        return 2;
    }

    char keypath[256], confpath[256], ctlpath[256];
    snprintf(keypath, sizeof(keypath), "%s/chain.key", dir);
    snprintf(confpath, sizeof(confpath), "%s/coord.conf", dir);
    snprintf(ctlpath, sizeof(ctlpath), "%s/ctl.sock", dir);

    FILE *f = fopen(keypath, "w");
    fwrite(CHAIN_KEY, 1, sizeof(CHAIN_KEY), f);
    fclose(f);

    f = fopen(confpath, "w");
    fprintf(f, "serial_dev = %s\n", slave_name);
    fprintf(f, "link_baud = 2000000\n");
    fprintf(f, "at_head = false\n");   /* coordinator is at the tail */
    fprintf(f, "headless = true\n");
    fprintf(f, "key_path = %s\n", keypath);
    fprintf(f, "ctl_path = %s\n", ctlpath);
    fclose(f);

    /* --- the board --- */
    dhp_link_t link;
    dhp_router_t router;
    dhp_link_init(&link, 0x0001, board_tx, NULL);
    dhp_link_set_key(&link, CHAIN_KEY);

    const dhp_router_cfg_t cfg = {
        .self = 0x0001,
        .uid = 0x0000000000001111ULL,
        .caps = DHP_CAP_HID_OUT | DHP_CAP_HID_IN,
        .preempt = false,
        .timing = DHP_UHRP_TIMING_DEFAULT,
        .pointer = DHP_POINTER_CFG_DEFAULT,
    };
    const dhp_router_hooks_t hooks = {
        .deliver_kbd = nop_kbd, .deliver_mouse = nop_mouse,
    };
    dhp_router_init(&router, &cfg, &link, &hooks);
    dhp_router_start(&router, now_ms());

    /* Service the pty and advance the board for `ms`. A macro rather than a
     * function so it can close over the link and router without threading a
     * context struct through a test. */
#define PUMP(ms)                                                               \
    do {                                                                       \
        const dhp_time_t _end = now_ms() + (ms);                               \
        while (!dhp_time_after(now_ms(), _end)) {                              \
            uint8_t _b[512];                                                   \
            const ssize_t _n = read(master, _b, sizeof(_b));                   \
            for (ssize_t _i = 0; _i < _n; _i++) {                              \
                dhp_frame_t _f;                                                \
                if (dhp_link_rx_byte(&link, DHP_PORT_DOWN, _b[_i], now_ms(),   \
                                     &_f) == DHP_OK) {                         \
                    dhp_router_rx(&router, &_f, DHP_PORT_DOWN, now_ms());      \
                }                                                              \
            }                                                                  \
            dhp_router_tick(&router, now_ms());                                \
            usleep(1000);                                                      \
        }                                                                      \
    } while (0)

    printf("== coordinator end-to-end ==\n");

    /* 1. The board must reach level 1 on its own, with no coordinator running
     *    at all. This is the property the whole staged-startup design rests
     *    on, so it is checked before the daemon is even started. */
    PUMP(400);
    CHECK(dhp_router_is_active(&router),
          "board alone takes the routing role (level 1, no coordinator)");
    CHECK(dhp_router_level(&router) == DHP_LEVEL_ELECTED,
          "level 1 reached without the coordinator");

    /* 2. Start the real daemon. */
    const pid_t pid = fork();
    if (pid == 0) {
        close(master);
        execl(coord_bin, coord_bin, "--config", confpath, "--headless",
              (char *)NULL);
        fprintf(stderr, "exec %s: %s\n", coord_bin, strerror(errno));
        _exit(127);
    }
    close(slave);

    /* Give it time to open the port, elect, and preempt. */
    const dhp_time_t t0 = now_ms();
    while (dhp_router_is_active(&router) && now_ms() - t0 < 5000) {
        PUMP(20);
    }
    const uint32_t takeover_ms = now_ms() - t0;

    CHECK(!dhp_router_is_active(&router),
          "coordinator preempted and took the routing role");
    printf("       takeover in %u ms\n", takeover_ms);

    PUMP(400);
    CHECK(dhp_router_level(&router) == DHP_LEVEL_COORDINATED,
          "board sees level 2 once the coordinator is present");
    CHECK(router.uhrp.active != DHP_ADDR_NONE,
          "board tracks the coordinator as the active speaker");

    /* The board must still know where the user is: a handover inherits focus
     * rather than resetting it. */
    CHECK(dhp_router_focus(&router) != DHP_ADDR_NONE,
          "focus survives the handover");

    /* 3. Kill it. The chain must drop back to level 1 rather than stopping,
     *    and inside roughly one hold time. */
    kill(pid, SIGKILL);
    const dhp_time_t t1 = now_ms();
    while (!dhp_router_is_active(&router) && now_ms() - t1 < 5000) {
        PUMP(10);
    }
    const uint32_t recover_ms = now_ms() - t1;

    CHECK(dhp_router_is_active(&router),
          "board retook the role after the coordinator was killed");
    printf("       recovery in %u ms\n", recover_ms);
    CHECK(recover_ms < 1000, "recovery within a hold time");

    PUMP(300);
    CHECK(dhp_router_level(&router) == DHP_LEVEL_ELECTED,
          "degraded back to level 1 rather than stopping");

    int status = 0;
    waitpid(pid, &status, 0);

    unlink(keypath);
    unlink(confpath);
    unlink(ctlpath);
    rmdir(dir);
    close(master);

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
