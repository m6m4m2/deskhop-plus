/* SPDX-License-Identifier: GPL-2.0-only
 *
 * The level 3 client: clipboard, text, files, images.
 *
 * It runs on a machine, talks to the board plugged into that machine, and
 * gives that one machine the data features. Machines without a client carry on
 * exactly as before -- which is why level 3 is per-machine and not a property
 * of the chain.
 *
 * The client is not a chain member; see boardlink.h for why that matters and
 * what the board will and will not proxy on its behalf.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdarg.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "boardlink.h"
#include "clipboard.h"
#include "dhp/data.h"
#include "dhp/link.h"

static volatile sig_atomic_t g_stop;
static void on_signal(int s) { (void)s; g_stop = 1; }

#define MAX_INFLIGHT (4u * 1024u * 1024u)

typedef struct {
    boardlink_t    *board;
    dhp_link_t      link;
    dhp_data_t      data;
    clipboard_cfg_t clip;

    dhp_addr_t      self;
    char            outdir[256];
    char            ctl_path[256];
    int             ctl_fd;
    bool            verbose;
    bool            accept_files;

    /* Outbound: dhp_data references this until on_sent, so it must not be
     * freed or reused before then. */
    uint8_t        *tx_buf;
    size_t          tx_len;
    bool            tx_busy;

    /* Inbound, reassembled here because only the endpoint has anywhere to put
     * it. Core streams chunks and holds nothing. */
    uint8_t        *rx_buf;
    uint32_t        rx_cap;
    uint32_t        rx_len;
    uint8_t         rx_kind;
    char            rx_name[64];

    /* Clipboard watch */
    uint8_t        *last_clip;
    size_t          last_clip_len;
    dhp_time_t      next_clip_poll;
    dhp_addr_t      peer; /* who to send to; 0 = broadcast to the chain */
} client_t;

static client_t g_c;

static dhp_time_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (dhp_time_t)((uint64_t)ts.tv_sec * 1000u +
                        (uint64_t)ts.tv_nsec / 1000000u);
}

static void logv(client_t *c, const char *fmt, ...)
{
    if (!c->verbose) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "deskhop-client: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* ------------------------------------------------------------------ */

static void on_tx(void *ctx, dhp_port_t port, const uint8_t *data, size_t len)
{
    client_t *c = ctx;
    (void)port; /* the client has exactly one link, to its own board */
    boardlink_write(c->board, data, len);
}

/* ---- receiving ---------------------------------------------------- */

static void on_offer(void *ctx, uint8_t stream, dhp_addr_t from, uint8_t kind,
                     uint32_t total, const char *name)
{
    client_t *c = ctx;

    if (total > MAX_INFLIGHT) {
        logv(c, "refusing %u bytes from %u: too large", total, from);
        dhp_data_reject(&c->data, stream, now_ms());
        return;
    }
    if (kind == DHP_DATA_FILE && !c->accept_files) {
        /* Text and images are what a clipboard is for and arrive because the
         * user copied something. A file lands on disk, so it is opt-in. */
        logv(c, "refusing file '%s' from %u: files not enabled", name, from);
        dhp_data_reject(&c->data, stream, now_ms());
        return;
    }

    if (c->rx_cap < total) {
        uint8_t *b = realloc(c->rx_buf, total);
        if (!b) {
            dhp_data_reject(&c->data, stream, now_ms());
            return;
        }
        c->rx_buf = b;
        c->rx_cap = total;
    }
    c->rx_len = 0;
    c->rx_kind = kind;
    snprintf(c->rx_name, sizeof(c->rx_name), "%s", name ? name : "");

    logv(c, "accepting %u bytes of kind %u ('%s') from %u", total, kind,
         c->rx_name, from);
    dhp_data_accept(&c->data, stream, now_ms());
}

static void on_chunk(void *ctx, uint8_t stream, uint32_t offset,
                     const uint8_t *d, uint8_t len)
{
    client_t *c = ctx;
    (void)stream;
    if (offset + len <= c->rx_cap) {
        memcpy(c->rx_buf + offset, d, len);
        if (offset + len > c->rx_len) {
            c->rx_len = offset + len;
        }
    }
}

/* Names arrive from another machine, so they are attacker-influenced input.
 * Anything that could escape the output directory is replaced rather than
 * rejected, so a hostile name degrades to an ugly filename instead of a path
 * traversal. */
static void safe_name(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 1 < cap; i++) {
        const char ch = in[i];
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                        (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' ||
                        ch == '_';
        out[o++] = ok ? ch : '_';
    }
    out[o] = '\0';

    /* "..", "." and the empty string are all still dangerous or useless after
     * character filtering. */
    if (o == 0 || !strcmp(out, ".") || !strcmp(out, "..")) {
        snprintf(out, cap, "received");
    }
}

static void on_received(void *ctx, uint8_t stream, uint8_t result)
{
    client_t *c = ctx;
    (void)stream;

    if (result != DHP_DATA_OK) {
        logv(c, "receive failed: %s", dhp_data_result_name(result));
        return;
    }

    switch (c->rx_kind) {
    case DHP_DATA_TEXT:
        if (clipboard_set(&c->clip, c->rx_buf, c->rx_len, false)) {
            /* Remember it as our own, or the watcher below will see the new
             * clipboard, think the user copied it, and send it straight back
             * -- which with two clients is an endless loop. */
            free(c->last_clip);
            c->last_clip = malloc(c->rx_len);
            if (c->last_clip) {
                memcpy(c->last_clip, c->rx_buf, c->rx_len);
                c->last_clip_len = c->rx_len;
            }
            logv(c, "clipboard updated, %u bytes", c->rx_len);
        }
        break;

    case DHP_DATA_IMAGE:
        clipboard_set(&c->clip, c->rx_buf, c->rx_len, true);
        logv(c, "clipboard image updated, %u bytes", c->rx_len);
        break;

    case DHP_DATA_SHARE:
        /* The bytes never crossed the chain; this is a location. */
        printf("%.*s\n", (int)c->rx_len, (const char *)c->rx_buf);
        fflush(stdout);
        logv(c, "share offer: %.*s", (int)c->rx_len, (const char *)c->rx_buf);
        break;

    case DHP_DATA_FILE:
    case DHP_DATA_LIST:
    default: {
        char clean[64];
        safe_name(c->rx_name[0] ? c->rx_name : "received", clean, sizeof(clean));

        char path[512];
        snprintf(path, sizeof(path), "%s/%s", c->outdir, clean);

        /* O_EXCL: never follow a symlink or clobber something already there. */
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
        for (int i = 1; fd < 0 && errno == EEXIST && i < 1000; i++) {
            snprintf(path, sizeof(path), "%s/%s.%d", c->outdir, clean, i);
            fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
        }
        if (fd < 0) {
            logv(c, "cannot write %s: %s", path, strerror(errno));
            break;
        }
        const ssize_t w = write(fd, c->rx_buf, c->rx_len);
        close(fd);
        if (w == (ssize_t)c->rx_len) {
            printf("%s\n", path);
            fflush(stdout);
            logv(c, "wrote %s (%u bytes)", path, c->rx_len);
        }
        break;
    }
    }
}

static void on_sent(void *ctx, uint8_t stream, uint8_t result)
{
    client_t *c = ctx;
    (void)stream;
    logv(c, "send finished: %s", dhp_data_result_name(result));

    free(c->tx_buf);
    c->tx_buf = NULL;
    c->tx_len = 0;
    c->tx_busy = false;
}

/* ---- sending ------------------------------------------------------- */

/* Takes ownership of `data`, which dhp_data references until on_sent. */
static bool send_owned(client_t *c, uint8_t kind, const char *name,
                       uint8_t *data, size_t len)
{
    if (c->tx_busy) {
        free(data);
        return false;
    }
    c->tx_buf = data;
    c->tx_len = len;
    c->tx_busy = true;

    const int h = dhp_data_send(&c->data, c->peer, kind, name, data,
                                (uint32_t)len, now_ms());
    if (h < 0) {
        logv(c, "send refused: %s", dhp_data_result_name((uint8_t)-h));
        free(c->tx_buf);
        c->tx_buf = NULL;
        c->tx_busy = false;
        return false;
    }
    return true;
}

/* ---- clipboard watch ----------------------------------------------- */

static void poll_clipboard(client_t *c, dhp_time_t now)
{
    if (!clipboard_available(&c->clip) || c->tx_busy) {
        return;
    }
    if (!dhp_time_after(now, c->next_clip_poll)) {
        return;
    }
    /* Polling, because neither X11 nor Wayland offers a portable way to be
     * notified without owning a window. Twice a second is imperceptible for
     * copy-and-paste and costs one process spawn. */
    c->next_clip_poll = now + 500;

    size_t len = 0;
    uint8_t *cur = clipboard_get(&c->clip, false, &len);
    if (!cur) {
        return;
    }

    if (c->last_clip && len == c->last_clip_len &&
        memcmp(cur, c->last_clip, len) == 0) {
        free(cur);
        return; /* unchanged, or it is something we ourselves just pasted in */
    }

    free(c->last_clip);
    c->last_clip = malloc(len);
    if (c->last_clip) {
        memcpy(c->last_clip, cur, len);
        c->last_clip_len = len;
    }

    logv(c, "clipboard changed, %zu bytes -> peer %u", len, c->peer);
    send_owned(c, DHP_DATA_TEXT, "clipboard", cur, len);
}

/* ---- control socket ------------------------------------------------ */

static int ctl_open(client_t *c)
{
    unlink(c->ctl_path);
    const int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        return -1;
    }
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    if (strlen(c->ctl_path) >= sizeof(sa.sun_path)) {
        close(fd);
        return -1;
    }
    memcpy(sa.sun_path, c->ctl_path, strlen(c->ctl_path));
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(fd);
        return -1;
    }
    chmod(c->ctl_path, 0600); /* the clipboard is the user's, not the system's */
    return fd;
}

static void ctl_reply(int fd, const struct sockaddr_un *to, socklen_t len,
                      const char *msg)
{
    if (len > (socklen_t)sizeof(sa_family_t)) {
        sendto(fd, msg, strlen(msg), MSG_NOSIGNAL, (const struct sockaddr *)to,
               len);
    }
}

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || (unsigned long)n > MAX_INFLIGHT) {
        fclose(f);
        return NULL;
    }
    uint8_t *b = malloc((size_t)n);
    if (!b) {
        fclose(f);
        return NULL;
    }
    const size_t got = fread(b, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) {
        free(b);
        return NULL;
    }
    *len = got;
    return b;
}

/* strtok(NULL, "\n") returns everything after the command word INCLUDING the
 * space that separated them, so an argument taken that way arrives with a
 * leading blank -- which turns a path into one that does not exist and a
 * string into one with a stray space at the front. */
static const char *skip_blanks(const char *s)
{
    while (s && (*s == ' ' || *s == '\t')) {
        s++;
    }
    return s;
}

static void ctl_handle(client_t *c)
{
    char buf[4096];
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
        char out[256];
        snprintf(out, sizeof(out),
                 "peer=%u sending=%s clipboard=%s files=%s streams=%u\n"
                 "sent=%u received=%u retransmits=%u\n",
                 c->peer, c->tx_busy ? "yes" : "no",
                 clipboard_available(&c->clip) ? "yes" : "no",
                 c->accept_files ? "accept" : "refuse",
                 dhp_data_active(&c->data), c->data.n_sent, c->data.n_received,
                 c->data.n_retransmits);
        ctl_reply(c->ctl_fd, &from, fromlen, out);
        return;
    }

    if (!strcmp(cmd, "peer")) {
        const char *a = strtok(NULL, " \t\n");
        if (a) {
            c->peer = (dhp_addr_t)strtoul(a, NULL, 0);
        }
        char out[64];
        snprintf(out, sizeof(out), "peer=%u\n", c->peer);
        ctl_reply(c->ctl_fd, &from, fromlen, out);
        return;
    }

    if (!strcmp(cmd, "send")) {
        const char *path = skip_blanks(strtok(NULL, "\n"));
        if (!path || !*path) {
            ctl_reply(c->ctl_fd, &from, fromlen, "usage: send <path>\n");
            return;
        }
        size_t len = 0;
        uint8_t *data = read_file(path, &len);
        if (!data) {
            ctl_reply(c->ctl_fd, &from, fromlen,
                      "cannot read that, or it is too large -- use `share` "
                      "for anything big\n");
            return;
        }
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        ctl_reply(c->ctl_fd, &from, fromlen,
                  send_owned(c, DHP_DATA_FILE, base, data, len)
                      ? "sending\n"
                      : "busy\n");
        return;
    }

    if (!strcmp(cmd, "text")) {
        const char *rest = skip_blanks(strtok(NULL, "\n"));
        if (!rest || !*rest) {
            ctl_reply(c->ctl_fd, &from, fromlen, "usage: text <string>\n");
            return;
        }
        const size_t len = strlen(rest);
        uint8_t *copy = malloc(len);
        if (!copy) {
            return;
        }
        memcpy(copy, rest, len);
        ctl_reply(c->ctl_fd, &from, fromlen,
                  send_owned(c, DHP_DATA_TEXT, "text", copy, len) ? "sending\n"
                                                                  : "busy\n");
        return;
    }

    if (!strcmp(cmd, "share")) {
        const char *loc = skip_blanks(strtok(NULL, "\n"));
        if (!loc || !*loc) {
            ctl_reply(c->ctl_fd, &from, fromlen, "usage: share <smb-url>\n");
            return;
        }
        /* Anything large goes this way: the chain carries the location and the
         * bytes go over the network. See docs/protocols/data.md. */
        const size_t len = strlen(loc);
        uint8_t *copy = malloc(len);
        if (!copy) {
            return;
        }
        memcpy(copy, loc, len);
        const char *base = strrchr(loc, '/');
        base = base ? base + 1 : loc;
        ctl_reply(c->ctl_fd, &from, fromlen,
                  send_owned(c, DHP_DATA_SHARE, base, copy, len) ? "sending\n"
                                                                 : "busy\n");
        return;
    }

    ctl_reply(c->ctl_fd, &from, fromlen,
              "commands: status peer <addr> text <s> send <path> share <url>\n");
}

/* ------------------------------------------------------------------ */

static void usage(const char *a0)
{
    fprintf(stderr,
            "usage: %s [options]\n"
            "  -d, --device PATH    board hidraw node (default /dev/deskhop0)\n"
            "  -s, --socket PATH    connect to a socket instead (development)\n"
            "  -p, --peer ADDR      board address to send to\n"
            "  -S, --self ADDR      this board's address (the board supplies\n"
            "                       this on real hardware; for socket dev)\n"
            "  -o, --outdir DIR     where received files land\n"
            "  -c, --ctl PATH       control socket\n"
            "      --copy-cmd CMD   override the clipboard write helper\n"
            "      --paste-cmd CMD  override the clipboard read helper\n"
            "      --accept-files   write received files to --outdir\n"
            "  -v, --verbose\n",
            a0);
}

int main(int argc, char **argv)
{
    client_t *c = &g_c;
    memset(c, 0, sizeof(*c));

    const char *device = "/dev/deskhop0";
    const char *sock = NULL;
    snprintf(c->outdir, sizeof(c->outdir), "%s/Downloads",
             getenv("HOME") ? getenv("HOME") : "/tmp");
    snprintf(c->ctl_path, sizeof(c->ctl_path), "%s/.deskhop-client.sock",
             getenv("HOME") ? getenv("HOME") : "/tmp");

    clipboard_autodetect(&c->clip);

    enum { OPT_COPY = 1000, OPT_PASTE, OPT_FILES };
    static const struct option opts[] = {
        {"device", required_argument, 0, 'd'},
        {"socket", required_argument, 0, 's'},
        {"peer", required_argument, 0, 'p'},
        {"self", required_argument, 0, 'S'},
        {"outdir", required_argument, 0, 'o'},
        {"ctl", required_argument, 0, 'c'},
        {"copy-cmd", required_argument, 0, OPT_COPY},
        {"paste-cmd", required_argument, 0, OPT_PASTE},
        {"accept-files", no_argument, 0, OPT_FILES},
        {"verbose", no_argument, 0, 'v'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0},
    };

    int o;
    while ((o = getopt_long(argc, argv, "d:s:p:S:o:c:vh", opts, NULL)) != -1) {
        switch (o) {
        case 'd': device = optarg; break;
        case 's': sock = optarg; break;
        case 'p': c->peer = (dhp_addr_t)strtoul(optarg, NULL, 0); break;
        case 'S': c->self = (dhp_addr_t)strtoul(optarg, NULL, 0); break;
        case 'o': snprintf(c->outdir, sizeof(c->outdir), "%s", optarg); break;
        case 'c': snprintf(c->ctl_path, sizeof(c->ctl_path), "%s", optarg); break;
        case OPT_COPY:
            snprintf(c->clip.copy_cmd, sizeof(c->clip.copy_cmd), "%s", optarg);
            break;
        case OPT_PASTE:
            snprintf(c->clip.paste_cmd, sizeof(c->clip.paste_cmd), "%s", optarg);
            break;
        case OPT_FILES: c->accept_files = true; break;
        case 'v': c->verbose = true; break;
        case 'h': usage(argv[0]); return 0;
        default: usage(argv[0]); return 2;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    char err[256];
    c->board = boardlink_open(sock ? BOARDLINK_SOCKET : BOARDLINK_HIDRAW,
                              sock ? sock : device, err, sizeof(err));
    if (!c->board) {
        fprintf(stderr, "deskhop-client: %s\n", err);
        return 1;
    }

    if (!clipboard_available(&c->clip)) {
        fprintf(stderr, "deskhop-client: no clipboard helper found "
                        "(install wl-clipboard or xclip); "
                        "file and share transfers still work\n");
    }

    /* The client answers on its board's chain address.
     *
     * It must not invent one: two clients that picked the same value would
     * each treat the other's frames as their own echo and silently ignore
     * them. On real hardware the board supplies this when the client attaches;
     * --self exists for development against a socket, where there is no board
     * to ask. */
    if (c->self == DHP_ADDR_NONE) {
        c->self = 0x00FF;
    }
    dhp_link_init(&c->link, c->self, on_tx, c);

    const uint8_t local_key[16] = DHP_LOCAL_KEY;
    dhp_link_set_key(&c->link, local_key);

    const dhp_data_hooks_t hooks = {
        .on_offer = on_offer, .on_chunk = on_chunk,
        .on_received = on_received, .on_sent = on_sent, .ctx = c,
    };
    dhp_data_init(&c->data, &c->link, &hooks);

    c->ctl_fd = ctl_open(c);
    if (c->ctl_fd < 0) {
        fprintf(stderr, "deskhop-client: no control socket at %s\n",
                c->ctl_path);
    }

    logv(c, "up; peer %u, outdir %s", c->peer, c->outdir);

    while (!g_stop) {
        struct pollfd pfd[2];
        int nfd = 0;
        pfd[nfd].fd = boardlink_fd(c->board);
        pfd[nfd].events = POLLIN;
        nfd++;
        if (c->ctl_fd >= 0) {
            pfd[nfd].fd = c->ctl_fd;
            pfd[nfd].events = POLLIN;
            nfd++;
        }

        poll(pfd, (nfds_t)nfd, 20);
        const dhp_time_t now = now_ms();

        if (pfd[0].revents & POLLIN) {
            uint8_t b[1024];
            const int n = boardlink_read(c->board, b, sizeof(b));
            if (n < 0) {
                fprintf(stderr, "deskhop-client: board went away\n");
                break;
            }
            for (int i = 0; i < n; i++) {
                dhp_frame_t f;
                if (dhp_link_rx_byte(&c->link, DHP_PORT_UP, b[i], now, &f) ==
                    DHP_OK) {
                    dhp_data_rx(&c->data, &f, now);
                }
            }
        }

        if (c->ctl_fd >= 0 && (pfd[nfd - 1].revents & POLLIN)) {
            ctl_handle(c);
        }

        dhp_data_tick(&c->data, now);
        poll_clipboard(c, now);
    }

    if (c->ctl_fd >= 0) {
        close(c->ctl_fd);
        unlink(c->ctl_path);
    }
    free(c->tx_buf);
    free(c->rx_buf);
    free(c->last_clip);
    boardlink_close(c->board);
    return 0;
}
