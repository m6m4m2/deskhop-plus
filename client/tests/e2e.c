/* SPDX-License-Identifier: GPL-2.0-only
 *
 * End-to-end test for the level 3 client.
 *
 * Two real deskhop-client processes are started, each connected to a Unix
 * socket standing in for its board's vendor HID interface. This program plays
 * the pair of boards in between: it accepts both connections and relays
 * DHP_MSG_DATA frames from one to the other, which is exactly the proxy role a
 * real board performs -- and, importantly, it relays *only* DATA, so the test
 * also demonstrates that a client cannot get anything else onto the chain.
 *
 * The clipboard helpers are pointed at files, so the whole path is observable:
 * write a file on one side, and the content must appear in a file on the
 * other having been segmented, framed, relayed and reassembled.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "dhp/frame.h"
#include "dhp/link.h"
#include "dhp/local.h"
#include "dhp/msg.h"

static int failures, checks;

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

static const uint8_t LOCAL_KEY[16] = {'d', 'h', 'p', '-', 'l', 'o',
                                      'c', 'a', 'l', '-', 'v', '1',
                                      0,   0,   0,   0};

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int listen_unix(const char *path)
{
    unlink(path);
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    memcpy(sa.sun_path, path, strlen(path));
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 ||
        listen(fd, 4) != 0) {
        perror("bind/listen");
        exit(2);
    }
    return fd;
}

static void write_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        perror("fopen");
        exit(2);
    }
    fputs(content, f);
    fclose(f);
}

static char *read_all(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) {
        fclose(f);
        return NULL;
    }
    char *b = malloc((size_t)n + 1);
    const size_t got = fread(b, 1, (size_t)n, f);
    fclose(f);
    b[got] = '\0';
    if (len) {
        *len = got;
    }
    return b;
}

/* One side of the proxy: a decoder, and where its output goes. */
typedef struct {
    int          fd;
    dhp_framer_t framer;
    int          peer_index;
    dhp_addr_t   addr;      /* the chain address this board would have */
    bool         attached;  /* the client announced itself */
    uint16_t     seq;
} side_t;

/* What a real board sends its client so it learns who it is. */
static void send_status(side_t *sd)
{
    const dhp_local_status_t st = {
        .self = sd->addr, .focus = sd->addr, .level = 3, .boards = 2,
    };
    uint8_t body[DHP_LOCAL_STATUS_BYTES];
    dhp_local_status_pack(&st, body);

    const dhp_frame_t f = {
        .type = DHP_MSG_CAP, .ttl = DHP_TTL_DEFAULT, .src = sd->addr,
        .dst = 0x00FF, .seq = sd->seq++, .len = sizeof(body), .payload = body,
    };
    uint8_t wire[DHP_WIRE_MAX];
    size_t n = 0;
    if (dhp_frame_encode(&f, LOCAL_KEY, wire, sizeof(wire), &n) == DHP_OK) {
        ssize_t o = 0;
        while (o < (ssize_t)n) {
            const ssize_t w = write(sd->fd, wire + o, n - (size_t)o);
            if (w <= 0) break;
            o += w;
        }
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <path-to-deskhop-client>\n", argv[0]);
        return 2;
    }
    const char *client_bin = argv[1];

    signal(SIGPIPE, SIG_IGN);

    char dir[] = "/tmp/dhpclientXXXXXX";
    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 2;
    }

    char sockA[256], sockB[256], clipA[256], clipB[256], outA[256], outB[256];
    char ctlA[256], ctlB[256];
    snprintf(sockA, sizeof(sockA), "%s/a.sock", dir);
    snprintf(sockB, sizeof(sockB), "%s/b.sock", dir);
    snprintf(clipA, sizeof(clipA), "%s/clip-a.txt", dir);
    snprintf(clipB, sizeof(clipB), "%s/clip-b.txt", dir);
    snprintf(outA, sizeof(outA), "%s/out-a", dir);
    snprintf(outB, sizeof(outB), "%s/out-b", dir);
    snprintf(ctlA, sizeof(ctlA), "%s/ctl-a.sock", dir);
    snprintf(ctlB, sizeof(ctlB), "%s/ctl-b.sock", dir);
    mkdir(outA, 0700);
    mkdir(outB, 0700);

    /* A's clipboard starts empty, B's too. The helpers are plain files, so the
     * test can both set and observe what each desktop "has". */
    write_file(clipA, "");
    write_file(clipB, "");

    const int lsnA = listen_unix(sockA);
    const int lsnB = listen_unix(sockB);

    printf("== level 3 client end-to-end ==\n");

    /* Client A: address 1, sends to 2. Client B: address 2, sends to 1. */
    char pasteA[512], copyA[512], pasteB[512], copyB[512];
    snprintf(pasteA, sizeof(pasteA), "cat %s", clipA);
    snprintf(copyA, sizeof(copyA), "cat > %s", clipA);
    snprintf(pasteB, sizeof(pasteB), "cat %s", clipB);
    snprintf(copyB, sizeof(copyB), "cat > %s", clipB);

    const pid_t pidA = fork();
    if (pidA == 0) {
        execl(client_bin, client_bin, "--socket", sockA, "--peer", "2",
              "--outdir", outA, "--ctl", ctlA, "--paste-cmd", pasteA,
              "--copy-cmd", copyA, "--accept-files",
              getenv("DHP_E2E_VERBOSE") ? "-v" : "--accept-files", (char *)NULL);
        _exit(127);
    }
    const pid_t pidB = fork();
    if (pidB == 0) {
        execl(client_bin, client_bin, "--socket", sockB, "--peer", "1",
              "--outdir", outB, "--ctl", ctlB, "--paste-cmd", pasteB,
              "--copy-cmd", copyB, "--accept-files",
              getenv("DHP_E2E_VERBOSE") ? "-v" : "--accept-files", (char *)NULL);
        _exit(127);
    }

    /* Bounded: a client that fails to start would otherwise leave this
     * blocked in accept() forever, and a test that hangs is worse than one
     * that fails. */
    side_t s[2];
    memset(s, 0, sizeof(s));
    s[0].fd = -1;
    s[1].fd = -1;

    const uint64_t deadline = now_ms() + 5000;
    while ((s[0].fd < 0 || s[1].fd < 0) && now_ms() < deadline) {
        struct pollfd lp[2] = {
            {.fd = lsnA, .events = POLLIN},
            {.fd = lsnB, .events = POLLIN},
        };
        poll(lp, 2, 200);
        if (s[0].fd < 0 && (lp[0].revents & POLLIN)) {
            s[0].fd = accept(lsnA, NULL, NULL);
        }
        if (s[1].fd < 0 && (lp[1].revents & POLLIN)) {
            s[1].fd = accept(lsnB, NULL, NULL);
        }
    }
    if (s[0].fd < 0 || s[1].fd < 0) {
        printf("  FAIL clients did not connect within 5 s\n");
        kill(pidA, SIGKILL);
        kill(pidB, SIGKILL);
        return 1;
    }
    s[0].peer_index = 1;
    s[1].peer_index = 0;
    s[0].addr = 1;
    s[1].addr = 2;
    dhp_framer_init(&s[0].framer);
    dhp_framer_init(&s[1].framer);
    fcntl(s[0].fd, F_SETFL, O_NONBLOCK);
    fcntl(s[1].fd, F_SETFL, O_NONBLOCK);

    CHECK(true, "both clients connected to their boards");

    int relayed_data = 0;
    int refused_non_data = 0;
    uint64_t _next_status = 0;

    /* Pump the proxy for `ms`, relaying DATA and nothing else. */
#define PUMP(ms)                                                               \
    do {                                                                       \
        const uint64_t _end = now_ms() + (ms);                                 \
        while (now_ms() < _end) {                                              \
            for (int _k = 0; _k < 2; _k++) {                                   \
                uint8_t _b[1024];                                              \
                const ssize_t _n = read(s[_k].fd, _b, sizeof(_b));             \
                for (ssize_t _i = 0; _i < _n; _i++) {                          \
                    dhp_frame_t _f;                                            \
                    if (dhp_framer_push(&s[_k].framer, _b[_i], LOCAL_KEY,      \
                                        &_f) != DHP_OK) {                      \
                        continue;                                              \
                    }                                                          \
                    if (_f.type == DHP_MSG_CAP) {                             \
                        /* The client announcing itself. A real board consumes \
                         * this locally and never puts it on the chain. */     \
                        s[_k].attached = true;                                 \
                        continue;                                              \
                    }                                                          \
                    if (_f.type != DHP_MSG_DATA) {                             \
                        /* A real board proxies DATA only. Anything else from  \
                         * a client is dropped rather than put on the chain. */\
                        refused_non_data++;                                    \
                        continue;                                              \
                    }                                                          \
                    relayed_data++;                                            \
                    uint8_t _w[DHP_WIRE_MAX];                                  \
                    size_t _wl = 0;                                            \
                    dhp_frame_t _out = _f;                                     \
                    _out.ttl = DHP_TTL_DEFAULT;                                \
                    if (dhp_frame_encode(&_out, LOCAL_KEY, _w, sizeof(_w),     \
                                         &_wl) == DHP_OK) {                    \
                        ssize_t _o = 0;                                        \
                        while (_o < (ssize_t)_wl) {                            \
                            const ssize_t _x = write(s[s[_k].peer_index].fd,   \
                                                     _w + _o, _wl - (size_t)_o);\
                            if (_x <= 0) break;                                \
                            _o += _x;                                          \
                        }                                                      \
                    }                                                          \
                }                                                              \
            }                                                                  \
            /* Boards announce themselves once a second. */                    \
            if (now_ms() >= _next_status) {                                    \
                send_status(&s[0]);                                            \
                send_status(&s[1]);                                            \
                _next_status = now_ms() + 500;                                 \
            }                                                                  \
            usleep(2000);                                                      \
        }                                                                      \
    } while (0)

    PUMP(600);

    /* --- clipboard: A copies something, B must end up with it --- */
    const char *msg = "the quick brown fox jumps over the lazy dog";
    write_file(clipA, msg);
    PUMP(3000);

    size_t got_len = 0;
    char *got = read_all(clipB, &got_len);
    CHECK(got && strcmp(got, msg) == 0, "clipboard text reached the other machine");
    if (got && strcmp(got, msg) != 0) {
        printf("       got \"%s\"\n", got);
    }
    free(got);

    /* --- a payload spanning many chunks --- */
    char *big = malloc(20001);
    for (int i = 0; i < 20000; i++) {
        big[i] = (char)('a' + (i % 26));
    }
    big[20000] = '\0';
    write_file(clipA, big);
    PUMP(6000);

    got = read_all(clipB, &got_len);
    CHECK(got && got_len == 20000 && memcmp(got, big, 20000) == 0,
          "20 KB clipboard survived segmentation and reassembly");
    if (got) {
        printf("       received %zu bytes\n", got_len);
    }
    free(got);
    free(big);

    /* --- a file, sent over the control socket --- */
    char srcfile[256];
    snprintf(srcfile, sizeof(srcfile), "%s/note.txt", dir);
    write_file(srcfile, "file content travelling over the chain\n");

    /* Sent directly rather than by shelling out to socat. system() would
     * block this process -- which is also the proxy relaying every frame --
     * for as long as the helper runs, starving the very transfer being
     * tested. */
    char req[512];
    snprintf(req, sizeof(req), "send %s", srcfile);
    const int cfd = socket(AF_UNIX, SOCK_DGRAM, 0);
    struct sockaddr_un ca;
    memset(&ca, 0, sizeof(ca));
    ca.sun_family = AF_UNIX;
    memcpy(ca.sun_path, ctlA, strlen(ctlA));
    if (sendto(cfd, req, strlen(req), 0, (struct sockaddr *)&ca,
               sizeof(ca)) < 0) {
        printf("       (could not reach the client control socket)\n");
    }
    close(cfd);
    PUMP(4000);

    char landed[512];
    snprintf(landed, sizeof(landed), "%s/note.txt", outB);
    got = read_all(landed, &got_len);
    CHECK(got && strstr(got, "file content travelling") != NULL,
          "file arrived on the other machine and was written to disk");
    free(got);

    /* --- the proxy saw only DATA --- */
    CHECK(s[0].attached && s[1].attached,
          "both clients announced themselves to their boards");
    CHECK(relayed_data > 0, "DATA frames were relayed");
    CHECK(refused_non_data == 0,
          "the client emitted nothing but DATA (it cannot claim a role)");
    printf("       relayed %d DATA frames, %d non-DATA\n", relayed_data,
           refused_non_data);

    kill(pidA, SIGTERM);
    kill(pidB, SIGTERM);
    waitpid(pidA, NULL, 0);
    waitpid(pidB, NULL, 0);

    close(s[0].fd);
    close(s[1].fd);
    close(lsnA);
    close(lsnB);

    char rm[512];
    snprintf(rm, sizeof(rm), "rm -rf %s", dir);
    if (system(rm) != 0) {
        /* best effort */
    }

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
