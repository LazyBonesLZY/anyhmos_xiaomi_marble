/* SPDX-License-Identifier: MIT
 * HarmonyOS hisysevent datagram sink for marble.
 *
 * Upstream the hisysevent broker is `hiview`, which init starts with two
 * AF_UNIX SOCK_DGRAM sockets.  On marble the broker never starts (its service
 * has device nodes and permissions that this kernel/userland mix does not
 * provide), so every process that reports an event fails with
 *
 *   E C02D08/samgr/HISYSEVENT_TRANSPORT: /dev/unix/socket/hisysevent
 *   write failed, errno=2, msg=No such file or directory
 *
 * This program is the pragmatic replacement: it owns the two socket paths,
 * drains every datagram, and keeps a bounded counter file.  Clients only need
 * `sendto()` to succeed; nothing here interprets the payload, so a malformed
 * event cannot take the sink down.  It is deliberately dependency-free (plain
 * POSIX) so it can start before render/service and stay up.
 *
 * Usage: hmos_hisysevent_sink [-q] [--socket-dir DIR] [--stats FILE]
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define MAX_SOCKETS 4
#define RECV_BUDGET 4096
/* One summary line every this many datagrams; keeps the log tiny while still
 * proving the sink is alive when a session is inspected hours later. */
#define SUMMARY_INTERVAL 2000

static const char *kSockets[] = {"hisysevent", "hisysevent_fast"};
static const char *socket_dir = "/dev/unix/socket";
static const char *stats_path = "/data/local/tmp/hisysevent-sink.log";
static volatile sig_atomic_t stopping;

static void on_signal(int sig)
{
    (void)sig;
    stopping = 1;
}

static void note(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(line) - 2) n = (int)sizeof(line) - 2;
    line[n++] = '\n';
    (void)write(STDOUT_FILENO, line, (size_t)n);
}

static int ensure_dir(const char *path)
{
    char tmp[256];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(tmp)) return -1;
    memcpy(tmp, path, len + 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
        *p = '/';
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int open_sink(const char *dir, const char *name)
{
    char path[256];
    if (snprintf(path, sizeof(path), "%s/%s", dir, name) >= (int)sizeof(path)) return -1;
    /* Recreate unconditionally: a stale socket from a previous boot would make
     * bind() fail with EADDRINUSE and leave every client unable to report. */
    (void)unlink(path);
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path)) {
        close(fd);
        return -1;
    }
    strcpy(addr.sun_path, path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    /* Client processes run as a dozen different uids; the datagram has to be
     * writable for all of them or the errno=2 storm simply becomes EACCES. */
    (void)chmod(path, 0666);
    int rcv = 1 << 20;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));
    return fd;
}

static void write_stats(unsigned long long total, unsigned long long dropped)
{
    char buf[192];
    int n = snprintf(buf, sizeof(buf), "HISYSEVENT_SINK events=%llu dropped=%llu\n",
                     total, dropped);
    if (n <= 0) return;
    int fd = open(stats_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return;
    (void)write(fd, buf, (size_t)n);
    close(fd);
}

int main(int argc, char **argv)
{
    bool quiet = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-q")) quiet = true;
        else if (!strcmp(argv[i], "--socket-dir") && i + 1 < argc) socket_dir = argv[++i];
        else if (!strcmp(argv[i], "--stats") && i + 1 < argc) stats_path = argv[++i];
        else if (!strcmp(argv[i], "--help")) {
            note("usage: %s [-q] [--socket-dir DIR] [--stats FILE]", argv[0]);
            return 0;
        }
    }
    if (ensure_dir(socket_dir) != 0) {
        note("HISYSEVENT_SINK_FAIL mkdir %s errno=%d", socket_dir, errno);
        return 3;
    }
    struct pollfd fds[MAX_SOCKETS];
    int nfd = 0;
    for (size_t i = 0; i < sizeof(kSockets) / sizeof(kSockets[0]); i++) {
        int fd = open_sink(socket_dir, kSockets[i]);
        if (fd < 0) {
            note("HISYSEVENT_SINK_FAIL bind %s errno=%d", kSockets[i], errno);
            return 4;
        }
        fds[nfd].fd = fd;
        fds[nfd].events = POLLIN;
        fds[nfd].revents = 0;
        nfd++;
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    (void)sigaction(SIGTERM, &sa, NULL);
    (void)sigaction(SIGINT, &sa, NULL);
    (void)signal(SIGPIPE, SIG_IGN);

    note("HISYSEVENT_SINK_READY dir=%s sockets=%d mode=0666", socket_dir, nfd);
    unsigned long long total = 0;
    unsigned long long dropped = 0;
    unsigned char buf[RECV_BUDGET];
    while (!stopping) {
        int rc = poll(fds, (nfds_t)nfd, 2000);
        if (rc < 0) {
            if (errno == EINTR) continue;
            note("HISYSEVENT_SINK_FAIL poll errno=%d", errno);
            break;
        }
        if (rc == 0) continue;
        for (int i = 0; i < nfd; i++) {
            if (!(fds[i].revents & POLLIN)) continue;
            for (;;) {
                ssize_t n = recv(fds[i].fd, buf, sizeof(buf), 0);
                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    dropped++;
                    break;
                }
                if (n == 0) break;
                total++;
                if (!quiet && total % SUMMARY_INTERVAL == 0) {
                    note("HISYSEVENT_SINK events=%llu dropped=%llu", total, dropped);
                    write_stats(total, dropped);
                }
            }
        }
    }
    for (int i = 0; i < nfd; i++) close(fds[i].fd);
    write_stats(total, dropped);
    note("HISYSEVENT_SINK_STOP events=%llu dropped=%llu", total, dropped);
    return 0;
}
