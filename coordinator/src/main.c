/* SPDX-License-Identifier: GPL-2.0-only
 *
 * The coordinator daemon: level 2.
 *
 * It is an ordinary participant in the chain that happens to advertise
 * DISPLAY and CONFIG, and to preempt. It runs the same core/ the boards do --
 * the same framing, the same election, the same pointer logic, compiled from
 * the same files. That is the point of core/ being freestanding: a second
 * implementation of a protocol this subtle would drift, and the place it would
 * first drift is the handover, which is the thing the whole design is built
 * around.
 *
 * What it deliberately does NOT do:
 *
 *   - It never claims DHP_CAP_HID_OUT. It is not attached to a machine as a
 *     keyboard, so it is on the chain but is not a screen; the router already
 *     knows to skip it when the pointer crosses.
 *   - It holds no state the chain needs. Kill it and the chain drops to level 1
 *     in about 150 ms and carries on; that is the promise, and it is only true
 *     because nothing here is load-bearing for level 1.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "config.h"
#include "crypto_linux.h"
#include "display.h"
#include "dhp/auth.h"
#include "dhp/link.h"
#include "dhp/router.h"
#include "platform.h"

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* ------------------------------------------------------------------ */

typedef struct {
    coord_config_t cfg;
    serial_t      *ser;
    ssd1306_t     *panel;
    dsp_t          screen;

    dhp_link_t     link;
    dhp_router_t   router;
    dhp_pair_t     pair;
    bool           paired;

    dhp_addr_t     self;
    dhp_uid_t      uid;

    dhp_time_t     started;
    dhp_time_t     next_render;
    char           sas[7];
    char           message[32];
    dhp_time_t     message_until;
    bool           message_active;

    int            ctl_fd;
    bool           verbose;
} coord_t;

static coord_t g_c;

static void notice(coord_t *c, const char *msg)
{
    snprintf(c->message, sizeof(c->message), "%s", msg);
    c->message_until = plat_now_ms() + 5000;
    c->message_active = true;
    if (c->verbose) {
        fprintf(stderr, "deskhop-coord: %s\n", msg);
    }
}

/* ------------------------------------------------------------------ *
 * Identity
 *
 * The coordinator needs a uid that is stable across restarts and distinct from
 * every board's, because the election breaks ties on it and pairing binds the
 * transcript to it. The Pi's serial number provides one.
 * ------------------------------------------------------------------ */

static dhp_uid_t machine_uid(void)
{
    dhp_uid_t uid = 0;

    FILE *f = fopen("/proc/cpuinfo", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (!strncmp(line, "Serial", 6)) {
                const char *colon = strchr(line, ':');
                if (colon) {
                    uid = (dhp_uid_t)strtoull(colon + 1, NULL, 16);
                }
                break;
            }
        }
        fclose(f);
    }

    if (uid == 0) {
        /* Not a Pi, or no serial exposed. /etc/machine-id is stable across
         * restarts, which is what actually matters. */
        f = fopen("/etc/machine-id", "r");
        if (f) {
            char buf[64] = {0};
            if (fgets(buf, sizeof(buf), f)) {
                uid = (dhp_uid_t)strtoull(buf, NULL, 16);
            }
            fclose(f);
        }
    }

    /* Set the top bit so a coordinator never collides with an RP2040 flash id
     * and, on an exact priority tie, sorts above a board. */
    return uid | (1ULL << 63);
}

/* ------------------------------------------------------------------ *
 * Link plumbing
 * ------------------------------------------------------------------ */

static void on_tx(void *ctx, dhp_port_t port, const uint8_t *data, size_t len)
{
    coord_t *c = ctx;

    /* The coordinator has one cable where a board has two. dhp_link_send()
     * emits on both ports because a line topology gives it no way to know
     * which side an address is on; the copy for the port that does not exist
     * is simply dropped here. */
    if (port != serial_port(c->ser)) {
        return;
    }
    serial_write(c->ser, data, len);
}

/* The coordinator is not attached to a machine, so input routed "to" it has
 * nowhere to go. These exist because the router requires them, and their
 * emptiness is correct rather than unfinished. */
static void on_kbd(void *ctx, const dhp_kbd_report_t *r)
{
    (void)ctx;
    (void)r;
}

static void on_mouse(void *ctx, const dhp_mouse_report_t *r)
{
    (void)ctx;
    (void)r;
}

static void on_focus(void *ctx, dhp_addr_t focus, bool is_self)
{
    coord_t *c = ctx;
    (void)is_self;
    if (c->verbose) {
        fprintf(stderr, "deskhop-coord: focus -> board %u\n", focus);
    }
    c->next_render = 0; /* redraw promptly: this is the thing people watch */
}

static void on_level(void *ctx, dhp_level_t level)
{
    coord_t *c = ctx;
    char buf[32];
    snprintf(buf, sizeof(buf), "LEVEL %d", (int)level);
    notice(c, buf);
    c->next_render = 0;
}

/* ------------------------------------------------------------------ *
 * Pairing
 * ------------------------------------------------------------------ */

static void pairing_drain(coord_t *c, dhp_time_t now)
{
    const dhp_pair_events_t ev = dhp_pair_take_events(&c->pair);
    uint8_t buf[64];
    uint8_t n;

    if (ev.send_offer) {
        n = dhp_pair_build_offer(&c->pair, buf, sizeof(buf));
        if (n) {
            dhp_link_send(&c->link, DHP_MSG_PAIR, DHP_ADDR_BROADCAST, buf, n, now);
        }
    }
    if (ev.send_confirm) {
        n = dhp_pair_build_confirm(&c->pair, buf, sizeof(buf));
        if (n) {
            dhp_link_send(&c->link, DHP_MSG_PAIR, DHP_ADDR_BROADCAST, buf, n, now);
        }
        /* The short authentication string becomes meaningful as soon as both
         * offers are in hand, which is exactly now. */
        dhp_pair_sas_string(&c->pair, c->sas);
        c->next_render = 0;
    }
    if (ev.send_abort) {
        n = dhp_pair_build_abort(&c->pair, buf, sizeof(buf));
        if (n) {
            dhp_link_send(&c->link, DHP_MSG_PAIR, DHP_ADDR_BROADCAST, buf, n, now);
        }
        char msg[32];
        snprintf(msg, sizeof(msg), "PAIR FAILED: %s",
                 dhp_pair_abort_name(c->pair.abort_reason));
        notice(c, msg);
        c->sas[0] = '\0';
    }

    if (ev.completed) {
        const uint8_t *key = dhp_pair_chain_key(&c->pair);
        if (key) {
            char err[256];
            if (coord_key_save(c->cfg.key_path, key, err, sizeof(err)) != 0) {
                fprintf(stderr, "deskhop-coord: saving chain key: %s\n", err);
                notice(c, "PAIR OK, SAVE FAILED");
            } else {
                notice(c, "PAIRED");
            }
            dhp_link_set_key(&c->link, key);
            c->paired = true;
            c->sas[0] = '\0';
        }
    }
}

static void start_pairing(coord_t *c)
{
    dhp_pair_begin(&c->pair, plat_now_ms(), 30000);
    c->sas[0] = '\0';
    notice(c, "PAIRING");
}

/* ------------------------------------------------------------------ *
 * Control socket
 *
 * A Unix socket rather than a network one: configuration can move focus
 * between machines and start a pairing, so reachability should be a filesystem
 * permission and not a firewall rule.
 * ------------------------------------------------------------------ */

static int ctl_open(coord_t *c)
{
    unlink(c->cfg.ctl_path);

    const int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        return -1;
    }

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;

    /* sun_path is 108 bytes and the configured path may be longer. Truncating
     * would bind a socket at a path nobody can guess, so refuse instead. */
    if (strlen(c->cfg.ctl_path) >= sizeof(sa.sun_path)) {
        fprintf(stderr, "deskhop-coord: ctl_path too long (max %zu)\n",
                sizeof(sa.sun_path) - 1);
        close(fd);
        return -1;
    }
    memcpy(sa.sun_path, c->cfg.ctl_path, strlen(c->cfg.ctl_path));

    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(fd);
        return -1;
    }
    chmod(c->cfg.ctl_path, 0660);
    return fd;
}

static void ctl_reply(int fd, const struct sockaddr_un *to, socklen_t tolen,
                      const char *msg)
{
    if (tolen > (socklen_t)sizeof(sa_family_t)) {
        sendto(fd, msg, strlen(msg), MSG_NOSIGNAL, (const struct sockaddr *)to,
               tolen);
    }
}

static void ctl_handle(coord_t *c)
{
    char buf[512];
    struct sockaddr_un from;
    socklen_t fromlen = sizeof(from);

    const ssize_t n = recvfrom(c->ctl_fd, buf, sizeof(buf) - 1, 0,
                               (struct sockaddr *)&from, &fromlen);
    if (n <= 0) {
        return;
    }
    buf[n] = '\0';

    char *cmd = strtok(buf, " \t\n");
    if (!cmd) {
        return;
    }

    if (!strcmp(cmd, "status")) {
        char out[1024];
        int o = snprintf(out, sizeof(out),
                         "level=%s active=%s focus=%u boards=%u paired=%s\n",
                         dhp_level_name(dhp_router_level(&c->router)),
                         dhp_router_is_active(&c->router) ? "self" : "board",
                         dhp_router_focus(&c->router),
                         dhp_router_chain_size(&c->router),
                         c->paired ? "yes" : "no");
        for (int i = 0; i < DHP_MAX_BOARDS && o < (int)sizeof(out) - 96; i++) {
            const dhp_chain_entry_t *e = &c->router.chain[i];
            if (!e->used) {
                continue;
            }
            o += snprintf(out + o, sizeof(out) - (size_t)o,
                          "board addr=%u pos=%d caps=0x%04x%s\n", e->addr,
                          e->rel_pos, e->caps,
                          e->addr == dhp_router_focus(&c->router) ? " focus" : "");
        }
        ctl_reply(c->ctl_fd, &from, fromlen, out);
        return;
    }

    if (!strcmp(cmd, "pair")) {
        start_pairing(c);
        ctl_reply(c->ctl_fd, &from, fromlen, "pairing started\n");
        return;
    }

    if (!strcmp(cmd, "focus")) {
        const char *arg = strtok(NULL, " \t\n");
        if (!arg) {
            ctl_reply(c->ctl_fd, &from, fromlen, "usage: focus <addr>\n");
            return;
        }
        const dhp_addr_t target = (dhp_addr_t)strtoul(arg, NULL, 0);
        if (!dhp_router_is_active(&c->router)) {
            /* Only the active speaker owns focus. Saying so is better than
             * sending a request that will be ignored. */
            ctl_reply(c->ctl_fd, &from, fromlen,
                      "not the active speaker; focus is owned elsewhere\n");
            return;
        }
        dhp_router_set_focus(&c->router, target, DHP_FOCUS_R_CFG, plat_now_ms());
        ctl_reply(c->ctl_fd, &from, fromlen, "ok\n");
        return;
    }

    if (!strcmp(cmd, "set")) {
        const char *k = strtok(NULL, " \t\n");
        const char *v = strtok(NULL, " \t\n");
        if (!k || !v) {
            ctl_reply(c->ctl_fd, &from, fromlen, "usage: set <key> <value>\n");
            return;
        }
        char err[256];
        if (coord_config_set(&c->cfg, k, v, err, sizeof(err)) != 0) {
            char out[320];
            snprintf(out, sizeof(out), "error: %s\n", err);
            ctl_reply(c->ctl_fd, &from, fromlen, out);
            return;
        }

        /* Pointer and timing changes take effect immediately; the link and
         * panic settings need a restart, and saying which is which avoids the
         * usual "I changed it and nothing happened". */
        c->router.pointer.cfg = c->cfg.pointer;
        c->router.uhrp.cfg.timing = c->cfg.timing;

        ctl_reply(c->ctl_fd, &from, fromlen,
                  "ok (link and panel settings apply on restart)\n");
        return;
    }

    if (!strcmp(cmd, "save")) {
        char err[256];
        const char *path = strtok(NULL, " \t\n");
        if (!path) {
            path = "/etc/deskhop/coordinator.conf";
        }
        if (coord_config_save(&c->cfg, path, err, sizeof(err)) != 0) {
            char out[320];
            snprintf(out, sizeof(out), "error: %s\n", err);
            ctl_reply(c->ctl_fd, &from, fromlen, out);
        } else {
            ctl_reply(c->ctl_fd, &from, fromlen, "saved\n");
        }
        return;
    }

    ctl_reply(c->ctl_fd, &from, fromlen,
              "commands: status pair focus <addr> set <k> <v> save [path]\n");
}

/* ------------------------------------------------------------------ *
 * Display
 * ------------------------------------------------------------------ */

static void render(coord_t *c, dhp_time_t now)
{
    dsp_status_t st;
    memset(&st, 0, sizeof(st));

    st.level = dhp_router_level(&c->router);
    st.we_are_active = dhp_router_is_active(&c->router);
    st.state_name = dhp_uhrp_state_name(c->router.uhrp.state);
    st.link_baud = c->cfg.link_baud;
    st.crc_errors = c->link.port[serial_port(c->ser)].framer.n_crc_err;
    st.auth_errors = c->link.port[serial_port(c->ser)].framer.n_auth_err;
    st.uptime_s = (now - c->started) / 1000u;

    st.pairing = (c->pair.state == DHP_PAIR_WAITING ||
                  c->pair.state == DHP_PAIR_CONFIRM_WAIT);
    st.sas = c->sas[0] ? c->sas : NULL;

    if (c->message_active) {
        if (dhp_time_after(now, c->message_until)) {
            c->message_active = false;
        } else {
            st.message = c->message;
        }
    }

    const dhp_addr_t focus = dhp_router_focus(&c->router);
    for (int i = 0; i < DHP_MAX_BOARDS && st.n_boards < 16; i++) {
        const dhp_chain_entry_t *e = &c->router.chain[i];
        if (!e->used) {
            continue;
        }
        dsp_board_t *b = &st.board[st.n_boards++];
        b->addr = e->addr;
        b->rel_pos = e->rel_pos;
        b->is_self = (e->addr == c->self);
        b->is_focus = (e->addr == focus);
        b->has_input = (e->caps & DHP_CAP_HID_IN) != 0;
        b->is_coord = (e->caps & DHP_CAP_COORD) != 0;
        b->is_active = false;
    }

    dsp_render_status(&c->screen, &st);

    if (c->panel) {
        ssd1306_blit(c->panel, c->screen.px, sizeof(c->screen.px));
    } else if (c->verbose) {
        dsp_dump(&c->screen, stderr);
    }
}

/* ------------------------------------------------------------------ *
 * Main
 * ------------------------------------------------------------------ */

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [options]\n"
            "  -c, --config PATH   configuration file\n"
            "  -H, --headless      run without a panel\n"
            "  -v, --verbose       log to stderr; dump the screen when headless\n"
            "  -h, --help\n",
            argv0);
}

int main(int argc, char **argv)
{
    coord_t *c = &g_c;
    memset(c, 0, sizeof(*c));

    const char *conf_path = "/etc/deskhop/coordinator.conf";
    bool force_headless = false;

    static const struct option opts[] = {
        {"config", required_argument, 0, 'c'},
        {"headless", no_argument, 0, 'H'},
        {"verbose", no_argument, 0, 'v'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0},
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "c:Hvh", opts, NULL)) != -1) {
        switch (opt) {
        case 'c': conf_path = optarg; break;
        case 'H': force_headless = true; break;
        case 'v': c->verbose = true; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 2;
        }
    }

    coord_config_defaults(&c->cfg);
    char err[256];
    if (coord_config_load(&c->cfg, conf_path, err, sizeof(err)) != 0) {
        fprintf(stderr, "deskhop-coord: %s\n", err);
        return 1;
    }
    if (force_headless) {
        c->cfg.headless = true;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    /* The link is the one thing that must work. */
    const dhp_port_t port = c->cfg.at_head ? DHP_PORT_DOWN : DHP_PORT_UP;
    c->ser = serial_open(c->cfg.serial_dev, c->cfg.link_baud, port, err,
                         sizeof(err));
    if (!c->ser) {
        fprintf(stderr, "deskhop-coord: %s\n", err);
        return 1;
    }

    /* The panel is not. Failing to find it costs the DISPLAY capability and
     * nothing else -- CONFIG is advertised separately, so the chain still
     * reaches level 2 and the daemon still serves its socket. */
    uint16_t caps = DHP_CAP_COORD | DHP_CAP_CONFIG;
    if (!c->cfg.headless) {
        c->panel = ssd1306_open(c->cfg.i2c_dev, c->cfg.i2c_addr, err,
                                sizeof(err));
        if (!c->panel) {
            fprintf(stderr, "deskhop-coord: no panel (%s); continuing\n", err);
        }
    }
    if (c->panel || c->cfg.headless) {
        /* Headless still advertises DISPLAY when asked to: the status is
         * available over the control socket, which is a display of a kind. */
        caps |= DHP_CAP_DISPLAY;
    }

    c->uid = machine_uid();
    c->self = (dhp_addr_t)((c->uid ^ (c->uid >> 16) ^ (c->uid >> 32) ^
                            (c->uid >> 48)) & 0xFFFFu);
    if (c->self == DHP_ADDR_NONE || c->self == DHP_ADDR_BROADCAST) {
        c->self = 0x00C0;
    }

    dhp_link_init(&c->link, c->self, on_tx, c);

    uint8_t key[16];
    c->paired = coord_key_load(c->cfg.key_path, key);
    if (c->paired) {
        dhp_link_set_key(&c->link, key);
    } else {
        fprintf(stderr, "deskhop-coord: not paired; hold the pair button on a "
                        "board and run `deskhop-ctl pair`\n");
    }
    memset(key, 0, sizeof(key));

    dhp_pair_init(&c->pair, crypto_linux(), c->uid);

    const dhp_router_cfg_t rcfg = {
        .self = c->self,
        .uid = c->uid,
        .caps = caps,
        /* The one participant that preempts. Taking over from the board that
         * won the initial election is the whole reason it exists, and the
         * handover waits for any held key by rule 1. */
        .preempt = true,
        .timing = c->cfg.timing,
        .pointer = c->cfg.pointer,
    };
    const dhp_router_hooks_t hooks = {
        .deliver_kbd = on_kbd,
        .deliver_mouse = on_mouse,
        .focus_changed = on_focus,
        .level_changed = on_level,
        .ctx = c,
    };
    dhp_router_init(&c->router, &rcfg, &c->link, &hooks);

    c->ctl_fd = ctl_open(c);
    if (c->ctl_fd < 0) {
        fprintf(stderr, "deskhop-coord: no control socket at %s (%s); "
                        "continuing\n",
                c->cfg.ctl_path, strerror(errno));
    }

    c->started = plat_now_ms();
    dhp_router_start(&c->router, c->started);
    notice(c, "STARTING");

    while (!g_stop) {
        struct pollfd pfd[2];
        int nfd = 0;

        pfd[nfd].fd = serial_fd(c->ser);
        pfd[nfd].events = POLLIN;
        nfd++;

        if (c->ctl_fd >= 0) {
            pfd[nfd].fd = c->ctl_fd;
            pfd[nfd].events = POLLIN;
            nfd++;
        }

        /* Short enough that the hello timer stays accurate: the hold time is
         * 150 ms and three missed hellos is a role change, so the loop must
         * never be the reason one is late. */
        poll(pfd, (nfds_t)nfd, 5);

        const dhp_time_t now = plat_now_ms();

        if (pfd[0].revents & POLLIN) {
            uint8_t buf[1024];
            const int n = serial_read(c->ser, buf, sizeof(buf));
            for (int i = 0; i < n; i++) {
                dhp_frame_t f;
                if (dhp_link_rx_byte(&c->link, port, buf[i], now, &f) != DHP_OK) {
                    continue;
                }
                if (f.type == DHP_MSG_PAIR) {
                    dhp_pair_rx(&c->pair, f.payload, f.len, now);
                } else if (c->paired) {
                    dhp_router_rx(&c->router, &f, port, now);
                }
            }
        }

        if (c->ctl_fd >= 0 && (pfd[nfd - 1].revents & POLLIN)) {
            ctl_handle(c);
        }

        dhp_pair_tick(&c->pair, now);
        pairing_drain(c, now);

        if (c->paired) {
            dhp_router_tick(&c->router, now);
        }

        serial_flush(c->ser);

        /* 4 Hz is plenty for a status panel and keeps the I2C bus quiet;
         * anything that wants to be seen sooner sets next_render to 0. */
        if (dhp_time_after(now, c->next_render)) {
            render(c, now);
            c->next_render = now + 250;
        }
    }

    /* Stand down cleanly rather than simply vanishing. A graceful resignation
     * means the successor does NOT broadcast a release, so shutting the
     * coordinator down does not disturb whatever the user is typing. */
    if (dhp_router_is_active(&c->router)) {
        dhp_link_send(&c->link, DHP_MSG_RESIGN, DHP_ADDR_BROADCAST, NULL, 0,
                      plat_now_ms());
        serial_flush(c->ser);
    }

    if (c->panel) {
        ssd1306_close(c->panel);
    }
    if (c->ctl_fd >= 0) {
        close(c->ctl_fd);
        unlink(c->cfg.ctl_path);
    }
    serial_close(c->ser);
    return 0;
}
