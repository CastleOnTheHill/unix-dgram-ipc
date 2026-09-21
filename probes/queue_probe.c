/*
 * queue_probe.c -- measure the real queue-full condition of AF_UNIX datagrams.
 *
 * handoff.md warns that SO_RCVBUF is not the knob that controls a Unix socket
 * receive buffer and that "message limit and queue-full condition must be
 * established against the actual kernel".  This probe does exactly that: it
 * varies SO_RCVBUF, SO_SNDBUF and the payload size, fills a socket whose
 * reader never reads, and reports where EAGAIN appears.
 *
 * Run: ./build/bin/queue_probe
 */
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* Both ends are non-blocking on purpose.  With a blocking sender, a full
 * receiver queue makes sendto() *sleep* instead of returning EAGAIN, and a
 * probe that never returns is useless.  The library's send path uses
 * MSG_DONTWAIT for the same reason; this probe documents what that choice
 * means: the failure mode is EAGAIN, not an unbounded stall. */
static void bind_dgram(const char *path, int *out_fd)
{
    int                fd;
    struct sockaddr_un sun;

    unlink(path);
    fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        perror("socket");
        exit(1);
    }
    /* memcpy rather than snprintf("%s"): the fortify pass cannot prove the
     * argument non-NULL and emits a spurious -Wformat-truncation. */
    if (strlen(path) >= sizeof(sun.sun_path)) {
        fprintf(stderr, "socket path too long: %s\n", path);
        exit(1);
    }
    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    memcpy(sun.sun_path, path, strlen(path) + 1);
    if (bind(fd, (struct sockaddr *)&sun,
             (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
                         strlen(path) + 1)) != 0) {
        perror("bind");
        exit(1);
    }
    *out_fd = fd;
}

static int sockopt(int fd, int opt, int val)
{
    if (val > 0 && setsockopt(fd, SOL_SOCKET, opt, &val, sizeof(val)) != 0) {
        return -1;
    }
    {
        int         cur = 0;
        socklen_t   l   = sizeof(cur);
        if (getsockopt(fd, SOL_SOCKET, opt, &cur, &l) != 0) {
            return -1;
        }
        return cur;
    }
}

/* 2 MiB is comfortably above the largest datagram this probe ever tries, so
 * the measurement is never silently clamped by the probe's own buffer. */
#define PROBE_BUF (2u << 20)

/* Fill the receiver's queue; returns how many datagrams were accepted. */
static long fill(int sender_fd, const char *dst, size_t payload,
                 long max_msgs, int *out_errno)
{
    static char buf[PROBE_BUF];
    long        n = 0;
    struct sockaddr_un sun;
    socklen_t   sl;

    if (payload > sizeof(buf)) {
        payload = sizeof(buf);
    }
    memset(buf, 'q', payload);
    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    memcpy(sun.sun_path, dst, strlen(dst) + 1);
    sl = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strlen(dst) + 1);

    while (n < max_msgs) {
        ssize_t w = sendto(sender_fd, buf, payload, 0, (struct sockaddr *)&sun,
                           sl);
        if (w < 0) {
            *out_errno = errno;
            break;
        }
        n++;
    }
    return n;
}

static void drain(int fd)
{
    static char buf[PROBE_BUF];

    for (;;) {
        ssize_t n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (n < 0) {
            break;
        }
    }
}

int main(void)
{
    static const int    rcvbufs[] = { 0, 4096, 16384, 65536, 1 << 20 };
    static const int    sndbufs[] = { 0, 4096, 65536, 1 << 20 };
    static const size_t payloads[] = { 64, 1024, 8192 };
    char                rp[128], sp[128];
    size_t              i, j, k;

    printf("== AF_UNIX SOCK_DGRAM queue behaviour (kernel queue depth, "
           "unread receiver)\n");
    printf("== wmem_default/max are read from /proc by the caller\n\n");
    printf("%-8s %-8s %-9s %-9s %-8s %-8s\n", "payload", "SO_RCVBUF", "eff_rbuf",
           "eff_sbuf", "accepted", "errno");

    for (i = 0; i < sizeof(payloads) / sizeof(payloads[0]); i++) {
        for (j = 0; j < sizeof(rcvbufs) / sizeof(rcvbufs[0]); j++) {
            for (k = 0; k < sizeof(sndbufs) / sizeof(sndbufs[0]); k++) {
                int   rfd, sfd;
                int   effr, effs;
                long  accepted;
                int   err = 0;
                char  rcvs[16], snds[16];

                snprintf(rp, sizeof(rp), "/tmp/ipc_qp_r_%ld", (long)getpid());
                snprintf(sp, sizeof(sp), "/tmp/ipc_qp_s_%ld", (long)getpid());
                bind_dgram(rp, &rfd);
                bind_dgram(sp, &sfd);

                effr = sockopt(rfd, SO_RCVBUF, rcvbufs[j]);
                effs = sockopt(sfd, SO_SNDBUF, sndbufs[k]);

                accepted = fill(sfd, rp, payloads[i], 100000, &err);

                snprintf(rcvs, sizeof(rcvs), "%d", rcvbufs[j]);
                snprintf(snds, sizeof(snds), "%d", sndbufs[k]);
                printf("%-8zu %-8s %-9d %-9d %-8ld %-8d\n", payloads[i],
                       rcvbufs[j] ? rcvs : "default", effr, effs, accepted,
                       err);

                drain(rfd);
                close(rfd);
                close(sfd);
                unlink(rp);
                unlink(sp);

                if (err != EAGAIN && err != ENOBUFS && err != 0) {
                    printf("   (stopped early with errno=%d)\n", err);
                }
            }
        }
    }

    /* ---- largest single datagram, receiver drained ------------------ */
    printf("\n== largest single datagram accepted with an EMPTY receiver queue\n");
    printf("%-12s %-10s %-8s %s\n", "payload", "sndbuf", "accepted", "errno");
    {
        static const size_t probes[] = { 65536, 200000, 212000, 262144,
                                         425984, 1048576 };
        int                 rfd, sfd;
        size_t              pi;

        snprintf(rp, sizeof(rp), "/tmp/ipc_qp_r_%ld", (long)getpid());
        snprintf(sp, sizeof(sp), "/tmp/ipc_qp_s_%ld", (long)getpid());
        bind_dgram(rp, &rfd);
        bind_dgram(sp, &sfd);
        {
            int effr = sockopt(rfd, SO_RCVBUF, 1 << 20);
            int effs = sockopt(sfd, SO_SNDBUF, 1 << 20);

            printf("   (requested 1 MiB both sides; kernel reports sndbuf=%d "
                   "rcvbuf=%d)\n", effs, effr);
        }
        for (pi = 0; pi < sizeof(probes) / sizeof(probes[0]); pi++) {
            int  err = 0;
            long acc;

            drain(rfd);
            acc = fill(sfd, rp, probes[pi], 1, &err);
            printf("%-12zu %-10d %-8ld %d%s\n", probes[pi], 1 << 20, acc, err,
                   (err == EMSGSIZE) ? "  (EMSGSIZE)" : "");
        }
        drain(rfd);
        close(rfd);
        close(sfd);
        unlink(rp);
        unlink(sp);
    }

    /* ---- and the same thing with a 212992-byte sender buffer -------- */
    {
        static const size_t probes[] = { 65536, 200000, 212000, 262144 };
        int                 rfd, sfd;
        size_t              pi;

        snprintf(rp, sizeof(rp), "/tmp/ipc_qp_r2_%ld", (long)getpid());
        snprintf(sp, sizeof(sp), "/tmp/ipc_qp_s2_%ld", (long)getpid());
        bind_dgram(rp, &rfd);
        bind_dgram(sp, &sfd);
        {
            int effr = sockopt(rfd, SO_RCVBUF, 0);
            int effs = sockopt(sfd, SO_SNDBUF, 0);

            printf("\n   (kernel defaults; sndbuf=%d rcvbuf=%d)\n", effs, effr);
        }
        for (pi = 0; pi < sizeof(probes) / sizeof(probes[0]); pi++) {
            int  err = 0;
            long acc;

            drain(rfd);
            acc = fill(sfd, rp, probes[pi], 1, &err);
            printf("%-12zu %-10s %-8ld %d%s\n", probes[pi], "default", acc, err,
                   (err == EMSGSIZE) ? "  (EMSGSIZE)" : "");
        }
        drain(rfd);
        close(rfd);
        close(sfd);
        unlink(rp);
        unlink(sp);
    }

    printf("\nNOTE: EAGAIN is the observable queue-full condition.  It is not "
           "governed by SO_RCVBUF on AF_UNIX sockets;\n"
           "      the capacity scales with the SENDER's SO_SNDBUF.  A datagram "
           "larger than the sender's buffer\n"
           "      fails with EMSGSIZE on a non-blocking socket (and would block "
           "on a blocking one).\n");
    return 0;
}
