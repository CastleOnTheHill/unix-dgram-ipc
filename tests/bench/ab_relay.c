/*
 * ab_relay -- handoff.md 13: the A/B performance measurement.
 *
 * handoff.md asks for a candidate-versus-baseline comparison between
 *
 *   baseline : AF_UNIX SOCK_STREAM through a central forwarding server
 *   candidate: AF_UNIX SOCK_DGRAM, modules talking straight to each other
 *
 * and explicitly requires that the baseline be built honestly:
 *
 *   "The streaming baseline must handle framing, short reads/writes correctly
 *    and keep the same identity verification and buffering policy; it must not
 *    be deliberately constructed to be inefficient.  Synthetic results must
 *    not be passed off as real legacy-framework results."
 *
 * So both arms below are implemented with the same discipline:
 *
 *   - identical wire header, identical payload sizes, identical message rates
 *   - identical concurrency: exactly one request in flight at a time
 *   - identity is verified in both arms, not just in the candidate:
 *       stream arm  -> SO_PEERCRED once per accepted connection, which is how
 *                      a connection-oriented server can identify its clients
 *       datagram arm-> SO_PASSCRED + SCM_CREDENTIALS on every message, which is
 *                      the only per-message identity a connectionless socket
 *                      offers and what libipc actually pays for
 *   - the stream arm does proper length-prefixed framing with read_full() /
 *     write_full() loops, so short reads and partial writes are handled
 *   - the relay arm does an O(1) name lookup into a two-entry table; nothing
 *     in it is artificially slow
 *
 * What this measures is the cost of the *topology*: four socket hops and four
 * thread wakeups per request/response cycle through a relay, versus two of
 * each when the modules are connected directly.
 *
 * It is NOT a measurement of the real legacy frameworks.  No source for any of
 * them was available (handoff.md 4).  These numbers are a synthetic control.
 *
 * Usage:
 *   ab_relay [--size B] [--rounds N] [--repeats R] [--csv] [--keep]
 *
 * Everything runs in one process with threads, so the whole workload's CPU
 * time is accounted by getrusage(RUSAGE_SELF) -- no per-process bookkeeping
 * and no risk of charging a helper process to the wrong arm.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* wire header -- explicit, fixed width, big endian                    */
/* ------------------------------------------------------------------ */

#define AB_MAGIC   0x41425231u /* 'ABR1' */
#define AB_HDR_LEN 24u

#define AB_TYPE_REQ 1u
#define AB_TYPE_REP 2u
#define AB_TYPE_HELLO 3u

#define MOD_A 1u
#define MOD_B 2u

typedef struct {
    uint32_t magic;
    uint32_t type;
    uint32_t from;
    uint32_t to;
    uint32_t seq;
    uint32_t len; /* payload length */
} ab_hdr_t;

static void hdr_encode(unsigned char *out, const ab_hdr_t *h)
{
    uint32_t f[6];

    f[0] = htonl(h->magic);
    f[1] = htonl(h->type);
    f[2] = htonl(h->from);
    f[3] = htonl(h->to);
    f[4] = htonl(h->seq);
    f[5] = htonl(h->len);
    memcpy(out, f, AB_HDR_LEN);
}

static int hdr_decode(const unsigned char *in, ab_hdr_t *h)
{
    uint32_t f[6];
    size_t   i;

    memcpy(f, in, AB_HDR_LEN);
    for (i = 0; i < 6; i++) {
        f[i] = ntohl(f[i]);
    }
    h->magic = f[0];
    h->type  = f[1];
    h->from  = f[2];
    h->to    = f[3];
    h->seq   = f[4];
    h->len   = f[5];
    if (h->magic != AB_MAGIC) {
        return -1;
    }
    if (h->len > (1u << 20)) {
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static uint64_t now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;

    return (x > y) - (x < y);
}

static uint64_t pct_of(const uint64_t *s, size_t n, double p)
{
    size_t idx;

    if (n == 0) {
        return 0;
    }
    idx = (size_t)((p / 100.0) * (double)(n - 1) + 0.5);
    if (idx >= n) {
        idx = n - 1;
    }
    return s[idx];
}

/* Blocking full read.  Returns 0 on success, -1 on EOF or error.  This is the
 * short-read handling handoff.md 13 requires of the streaming baseline. */
static int read_full(int fd, void *buf, size_t n)
{
    unsigned char *p = (unsigned char *)buf;

    while (n > 0) {
        ssize_t r = read(fd, p, n);

        if (r > 0) {
            p += (size_t)r;
            n -= (size_t)r;
            continue;
        }
        if (r < 0 && errno == EINTR) {
            continue;
        }
        return -1;
    }
    return 0;
}

/* Blocking full write.  Same story for partial writes. */
static int write_full(int fd, const void *buf, size_t n)
{
    const unsigned char *p = (const unsigned char *)buf;

    while (n > 0) {
        ssize_t w = write(fd, p, n);

        if (w > 0) {
            p += (size_t)w;
            n -= (size_t)w;
            continue;
        }
        if (w < 0 && errno == EINTR) {
            continue;
        }
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* results                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    double   seconds;
    uint64_t p50, p95, p99;
    long     cpu_user_ms;
    long     cpu_sys_ms;
    long     nvcsw;
    long     nivcsw;
    long     rss_kb;
    long     hwm_kb;
    uint64_t delivered;
    uint64_t rejected;  /* identity checks that failed */
    uint64_t truncated; /* MSG_TRUNC / MSG_CTRUNC seen */
} ab_result_t;

static long proc_kb(const char *key)
{
    FILE *f = fopen("/proc/self/status", "r");
    char  line[256];
    long  v = -1;

    if (f == NULL) {
        return -1;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, key, strlen(key)) == 0) {
            if (sscanf(line + strlen(key), "%ld", &v) != 1) {
                v = -1;
            }
            break;
        }
    }
    fclose(f);
    return v;
}

static void rusage_snapshot(long *u_ms, long *s_ms, long *nv, long *ni)
{
    struct rusage ru;

    memset(&ru, 0, sizeof(ru));
    if (getrusage(RUSAGE_SELF, &ru) != 0) {
        *u_ms = *s_ms = *nv = *ni = -1;
        return;
    }
    *u_ms = ru.ru_utime.tv_sec * 1000L + ru.ru_utime.tv_usec / 1000L;
    *s_ms = ru.ru_stime.tv_sec * 1000L + ru.ru_stime.tv_usec / 1000L;
    *nv    = ru.ru_nvcsw;
    *ni    = ru.ru_nivcsw;
}

/* ------------------------------------------------------------------ */
/* shared scratch paths                                                */
/* ------------------------------------------------------------------ */

static char g_dir[256];
static char g_path_a[300];
static char g_path_b[300];
static char g_path_relay[300];

static int bind_dgram(const char *path)
{
    struct sockaddr_un sun;
    int                fd;
    size_t             plen = strlen(path);

    if (plen >= sizeof(sun.sun_path)) {
        return -1;
    }
    fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    memcpy(sun.sun_path, path, plen + 1);
    if (bind(fd, (struct sockaddr *)&sun,
             (socklen_t)(offsetof(struct sockaddr_un, sun_path) + plen + 1)) !=
        0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int make_sockaddr(const char *path, struct sockaddr_un *sun,
                         socklen_t *len)
{
    size_t plen = strlen(path);

    if (plen >= sizeof(sun->sun_path)) {
        return -1;
    }
    memset(sun, 0, sizeof(*sun));
    sun->sun_family = AF_UNIX;
    memcpy(sun->sun_path, path, plen + 1);
    *len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + plen + 1);
    return 0;
}

/* ------------------------------------------------------------------ */
/* arm 1: datagram, direct -- the candidate topology                   */
/* ------------------------------------------------------------------ */

typedef struct {
    int         fd;      /* the peer's own bound socket */
    int         to_a;    /* socket used to send back (unbound is fine) */
    size_t      payload;
    atomic_int  stop;
    atomic_uint rejected;   /* identity/protocol checks that failed */
    atomic_uint truncated;  /* MSG_TRUNC / MSG_CTRUNC seen */
    atomic_uint delivered;
    uint32_t    expect_uid;
} dgram_peer_t;

static void *dgram_peer_thread(void *arg)
{
    dgram_peer_t      *p = (dgram_peer_t *)arg;
    unsigned char     *req = malloc(AB_HDR_LEN + p->payload);
    unsigned char     *rep = malloc(AB_HDR_LEN + p->payload);
    struct sockaddr_un aaddr;
    socklen_t          alen;
    struct iovec       iov;
    struct msghdr      mh;
    unsigned char      ctrl[CMSG_SPACE(sizeof(struct ucred))];
    ab_hdr_t           h;

    if (req == NULL || rep == NULL || make_sockaddr(g_path_a, &aaddr, &alen) != 0) {
        free(req);
        free(rep);
        return NULL;
    }
    /* SO_PASSCRED is NOT enabled here -- the caller does it before this thread
     * exists, so no datagram can ever be read with the option still off.  A
     * datagram that is already queued when SO_PASSCRED is turned on comes back
     * without SCM_CREDENTIALS, so the ordering matters.  libipc enables it
     * during registration, before the receive loop can read anything. */

    while (!atomic_load(&p->stop)) {
        ssize_t n;
        struct cmsghdr *cm;
        int   cred_ok = 0;

        iov.iov_base    = req;
        iov.iov_len     = AB_HDR_LEN + p->payload;
        memset(&mh, 0, sizeof(mh));
        mh.msg_iov        = &iov;
        mh.msg_iovlen     = 1;
        mh.msg_control    = ctrl;
        mh.msg_controllen = sizeof(ctrl);

        n = recvmsg(p->fd, &mh, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (n < (ssize_t)AB_HDR_LEN) {
            atomic_fetch_add(&p->rejected, 1);
            continue;
        }
        /* Per-message identity: this is the check the candidate design has to
         * pay for on every single datagram. */
        for (cm = CMSG_FIRSTHDR(&mh); cm != NULL; cm = CMSG_NXTHDR(&mh, cm)) {
            if (cm->cmsg_level == SOL_SOCKET &&
                cm->cmsg_type == SCM_CREDENTIALS &&
                cm->cmsg_len >= CMSG_LEN(sizeof(struct ucred))) {
                struct ucred uc;

                memcpy(&uc, CMSG_DATA(cm), sizeof(uc));
                if (uc.uid == (uid_t)p->expect_uid) {
                    cred_ok = 1;
                }
            }
        }
        if (mh.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) {
            atomic_fetch_add(&p->truncated, 1);
        }
        if (hdr_decode(req, &h) != 0 || !cred_ok) {
            atomic_fetch_add(&p->rejected, 1);
            continue;
        }
        if (h.type == AB_TYPE_HELLO) {
            break; /* shutdown sentinel, not a defect */
        }
        if (h.type != AB_TYPE_REQ || h.to != MOD_B) {
            atomic_fetch_add(&p->rejected, 1);
            continue;
        }

        memcpy(rep, req, AB_HDR_LEN);
        hdr_decode(rep, &h);
        h.type = AB_TYPE_REP;
        h.from = MOD_B;
        h.to   = MOD_A;
        hdr_encode(rep, &h);
        if (sendto(p->to_a, rep, AB_HDR_LEN + h.len, 0,
                   (struct sockaddr *)&aaddr, alen) > 0) {
            atomic_fetch_add(&p->delivered, 1);
        }
    }
    free(req);
    free(rep);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* arm 2: stream through a central relay -- the baseline topology      */
/* ------------------------------------------------------------------ */

#define RELAY_MAX_CLIENTS 2

typedef struct {
    int    fd;
    char   name[8];
    pid_t  pid;
    uid_t  uid;
} relay_client_t;

typedef struct {
    relay_client_t cl[RELAY_MAX_CLIENTS];
    int            ncl;
    atomic_int     stop;
    atomic_uint    rejected;
    atomic_uint    delivered;
    uint32_t       expect_uid;
} relay_t;

/* Read one framed message.  Returns 0 on success, -1 on EOF/error. */
static int frame_read(int fd, unsigned char *buf, size_t cap, size_t *out_len)
{
    unsigned char lp[4];
    uint32_t      len;

    if (read_full(fd, lp, 4) != 0) {
        return -1;
    }
    memcpy(&len, lp, 4);
    len = ntohl(len);
    if (len > cap) {
        return -1;
    }
    if (read_full(fd, buf, len) != 0) {
        return -1;
    }
    *out_len = len;
    return 0;
}

static int frame_write(int fd, const unsigned char *buf, size_t len)
{
    unsigned char lp[4];
    uint32_t      be = htonl((uint32_t)len);

    memcpy(lp, &be, 4);
    if (write_full(fd, lp, 4) != 0) {
        return -1;
    }
    return write_full(fd, buf, len);
}

static relay_client_t *relay_find(relay_t *r, uint32_t mod)
{
    const char *want = (mod == MOD_A) ? "A" : "B";
    int         i;

    for (i = 0; i < r->ncl; i++) {
        if (strcmp(r->cl[i].name, want) == 0) {
            return &r->cl[i];
        }
    }
    return NULL;
}

static void *relay_thread(void *arg)
{
    relay_t       *r = (relay_t *)arg;
    struct sockaddr_un sun;
    socklen_t          slen;
    int                lfd;
    unsigned char     *buf;
    size_t             cap = AB_HDR_LEN + (1u << 20);

    buf = malloc(cap);
    if (buf == NULL) {
        return NULL;
    }
    lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (lfd < 0 || make_sockaddr(g_path_relay, &sun, &slen) != 0) {
        free(buf);
        if (lfd >= 0) {
            close(lfd);
        }
        return NULL;
    }
    unlink(g_path_relay);
    if (bind(lfd, (struct sockaddr *)&sun, slen) != 0 || listen(lfd, 4) != 0) {
        fprintf(stderr, "relay: bind/listen: %s\n", strerror(errno));
        close(lfd);
        free(buf);
        return NULL;
    }

    /* Accept exactly two clients, capturing identity ONCE per connection --
     * the connection-oriented equivalent of the candidate's per-message
     * SCM_CREDENTIALS check. */
    while (r->ncl < RELAY_MAX_CLIENTS) {
        struct ucred uc;
        socklen_t    ucl = sizeof(uc);
        int          cfd = accept(lfd, NULL, NULL);
        size_t       n   = 0;
        ab_hdr_t     h;

        if (cfd < 0) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "relay: accept: %s\n", strerror(errno));
            close(lfd);
            free(buf);
            return NULL;
        }
        if (getsockopt(cfd, SOL_SOCKET, SO_PEERCRED, &uc, &ucl) != 0 ||
            uc.uid != (uid_t)r->expect_uid) {
            atomic_fetch_add(&r->rejected, 1);
            close(cfd);
            continue;
        }
        if (frame_read(cfd, buf, cap, &n) != 0 || n < AB_HDR_LEN ||
            hdr_decode(buf, &h) != 0 || h.type != AB_TYPE_HELLO) {
            atomic_fetch_add(&r->rejected, 1);
            close(cfd);
            continue;
        }
        r->cl[r->ncl].fd  = cfd;
        r->cl[r->ncl].uid = uc.uid;
        r->cl[r->ncl].pid = uc.pid;
        snprintf(r->cl[r->ncl].name, sizeof(r->cl[r->ncl].name), "%s",
                 (h.from == MOD_A) ? "A" : "B");
        r->ncl++;
    }
    close(lfd); /* no more clients: stop accepting, keep serving */

    for (;;) {
        struct pollfd pfd[RELAY_MAX_CLIENTS];
        int           i, ready;

        for (i = 0; i < r->ncl; i++) {
            pfd[i].fd      = r->cl[i].fd;
            pfd[i].events  = POLLIN;
            pfd[i].revents = 0;
        }
        ready = poll(pfd, (nfds_t)r->ncl, 50);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        for (i = 0; i < r->ncl; i++) {
            size_t   n = 0;
            ab_hdr_t h;
            relay_client_t *dst;

            if (!(pfd[i].revents & POLLIN)) {
                continue;
            }
            if (frame_read(r->cl[i].fd, buf, cap, &n) != 0) {
                if (atomic_load(&r->stop)) {
                    goto done;
                }
                /* A client went away. */
                atomic_fetch_add(&r->rejected, 1);
                goto done;
            }
            if (n < AB_HDR_LEN || hdr_decode(buf, &h) != 0) {
                atomic_fetch_add(&r->rejected, 1);
                continue;
            }
            /* Identity was established at accept() time; the frame's own
             * "from" field is never trusted for authorisation. */
            if (r->cl[i].uid != (uid_t)r->expect_uid) {
                atomic_fetch_add(&r->rejected, 1);
                continue;
            }
            dst = relay_find(r, h.to);
            if (dst == NULL) {
                atomic_fetch_add(&r->rejected, 1);
                continue;
            }
            if (frame_write(dst->fd, buf, n) != 0) {
                goto done;
            }
            atomic_fetch_add(&r->delivered, 1);
        }
        if (atomic_load(&r->stop)) {
            break;
        }
    }
done:
    free(buf);
    return NULL;
}

typedef struct {
    int           fd;      /* the client's connection to the relay */
    size_t        payload;
    atomic_int    stop;
    atomic_uint   rejected;
    atomic_uint   delivered;
} relay_client_ctx_t;

/* Client B: read a request frame, frame an echo back to A through the relay. */
static void *relay_client_b_thread(void *arg)
{
    relay_client_ctx_t *c = (relay_client_ctx_t *)arg;
    unsigned char      *buf = malloc(AB_HDR_LEN + c->payload);

    if (buf == NULL) {
        return NULL;
    }
    while (!atomic_load(&c->stop)) {
        size_t      n = 0;
        ab_hdr_t    h;
        struct pollfd pfd;

        pfd.fd      = c->fd;
        pfd.events  = POLLIN;
        pfd.revents = 0;
        if (poll(&pfd, 1, 50) <= 0) {
            continue;
        }
        if (frame_read(c->fd, buf, AB_HDR_LEN + c->payload, &n) != 0) {
            break;
        }
        if (n < AB_HDR_LEN || hdr_decode(buf, &h) != 0 || h.type != AB_TYPE_REQ) {
            atomic_fetch_add(&c->rejected, 1);
            continue;
        }
        h.type = AB_TYPE_REP;
        h.from = MOD_B;
        h.to   = MOD_A;
        hdr_encode(buf, &h);
        if (frame_write(c->fd, buf, AB_HDR_LEN + h.len) != 0) {
            break;
        }
        atomic_fetch_add(&c->delivered, 1);
    }
    free(buf);
    return NULL;
}

static int connect_unix(const char *path)
{
    struct sockaddr_un sun;
    socklen_t          slen;
    int                fd;

    if (make_sockaddr(path, &sun, &slen) != 0) {
        return -1;
    }
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&sun, slen) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* ------------------------------------------------------------------ */
/* the two arms                                                        */
/* ------------------------------------------------------------------ */

static int run_direct(size_t payload, uint64_t rounds, uint64_t *lat,
                      ab_result_t *out)
{
    dgram_peer_t       peer;
    pthread_t          tid;
    int                a_fd;
    struct sockaddr_un baddr;
    socklen_t          blen;
    unsigned char     *msg = malloc(AB_HDR_LEN + payload);
    unsigned char     *rbuf = malloc(AB_HDR_LEN + payload);
    struct iovec       iov;
    struct msghdr      mh;
    unsigned char      ctrl[CMSG_SPACE(sizeof(struct ucred))];
    long               u0, s0, nv0, ni0, u1, s1, nv1, ni1;
    uint64_t           t0, t1, i;
    int                one = 1;

    memset(&peer, 0, sizeof(peer));
    if (msg == NULL || rbuf == NULL) {
        free(msg);
        free(rbuf);
        return -1;
    }
    if (make_sockaddr(g_path_b, &baddr, &blen) != 0) {
        free(msg);
        free(rbuf);
        return -1;
    }

    a_fd = bind_dgram(g_path_a);
    peer.fd = bind_dgram(g_path_b);
    if (a_fd < 0 || peer.fd < 0) {
        free(msg);
        free(rbuf);
        if (a_fd >= 0) {
            close(a_fd);
        }
        if (peer.fd >= 0) {
            close(peer.fd);
        }
        return -1;
    }
    peer.to_a        = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    peer.payload     = payload;
    peer.expect_uid  = (uint32_t)getuid();
    atomic_init(&peer.stop, 0);
    atomic_init(&peer.rejected, 0);
    atomic_init(&peer.truncated, 0);
    atomic_init(&peer.delivered, 0);
    if (peer.to_a < 0) {
        close(a_fd);
        close(peer.fd);
        free(msg);
        free(rbuf);
        return -1;
    }
    /* Both ends enable SO_PASSCRED before any datagram can be sent or received.
     * A datagram that is already queued when the option is turned on is
     * delivered without SCM_CREDENTIALS, and it would then be dropped by the
     * check above -- so the enable has to precede the traffic, which is what
     * libipc does at registration time. */
    if (setsockopt(peer.fd, SOL_SOCKET, SO_PASSCRED, &one, sizeof(one)) != 0 ||
        setsockopt(a_fd, SOL_SOCKET, SO_PASSCRED, &one, sizeof(one)) != 0) {
        fprintf(stderr, "ab_relay: SO_PASSCRED: %s\n", strerror(errno));
        close(a_fd);
        close(peer.fd);
        close(peer.to_a);
        free(msg);
        free(rbuf);
        return -1;
    }
    if (pthread_create(&tid, NULL, dgram_peer_thread, &peer) != 0) {
        close(a_fd);
        close(peer.fd);
        close(peer.to_a);
        free(msg);
        free(rbuf);
        return -1;
    }

    rusage_snapshot(&u0, &s0, &nv0, &ni0);
    t0 = now_ns();
    for (i = 0; i < rounds; i++) {
        ab_hdr_t        h;
        struct cmsghdr *cm;
        ssize_t         n;
        int             cred_ok = 0;
        uint64_t        r0, r1;

        h.magic = AB_MAGIC;
        h.type  = AB_TYPE_REQ;
        h.from  = MOD_A;
        h.to    = MOD_B;
        h.seq   = (uint32_t)i;
        h.len   = (uint32_t)payload;
        hdr_encode(msg, &h);

        r0 = now_ns();
        if (sendto(a_fd, msg, AB_HDR_LEN + payload, 0,
                   (struct sockaddr *)&baddr, blen) < 0) {
            break;
        }
        for (;;) {
            iov.iov_base    = rbuf;
            iov.iov_len     = AB_HDR_LEN + payload;
            memset(&mh, 0, sizeof(mh));
            mh.msg_iov        = &iov;
            mh.msg_iovlen     = 1;
            mh.msg_control    = ctrl;
            mh.msg_controllen = sizeof(ctrl);
            n = recvmsg(a_fd, &mh, 0);
            if (n < 0 && errno == EINTR) {
                continue;
            }
            break;
        }
        r1 = now_ns();
        if (n < (ssize_t)AB_HDR_LEN) {
            break;
        }
        for (cm = CMSG_FIRSTHDR(&mh); cm != NULL; cm = CMSG_NXTHDR(&mh, cm)) {
            if (cm->cmsg_level == SOL_SOCKET &&
                cm->cmsg_type == SCM_CREDENTIALS &&
                cm->cmsg_len >= CMSG_LEN(sizeof(struct ucred))) {
                struct ucred uc;

                memcpy(&uc, CMSG_DATA(cm), sizeof(uc));
                if (uc.uid == (uid_t)getuid()) {
                    cred_ok = 1;
                }
            }
        }
        if (!cred_ok) {
            /* Only fires on a real anomaly, so it is worth the print: it tells
             * us *which* round trip lost its credentials instead of leaving a
             * bare counter to be argued about. */
            fprintf(stderr,
                    "ab_relay: reply %" PRIu64 " on the direct arm arrived "
                    "without valid SCM_CREDENTIALS (len=%zd)\n",
                    i, n);
            atomic_fetch_add(&peer.rejected, 1);
        }
        if (mh.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) {
            atomic_fetch_add(&peer.truncated, 1);
        }
        lat[i] = r1 - r0;
    }
    t1 = now_ns();
    rusage_snapshot(&u1, &s1, &nv1, &ni1);

    atomic_store(&peer.stop, 1);
    {
        ab_hdr_t stop;
        unsigned char wire[AB_HDR_LEN];

        /* The sentinel goes through the same encoder as every other message.
         * Sending the host-order struct directly makes hdr_decode() reject it
         * (wrong magic once byte-swapped), which the peer then reports as one
         * protocol rejection per run -- exactly the kind of off-by-one-looking
         * counter that is worth chasing to the ground. */
        memset(&stop, 0, sizeof(stop));
        stop.magic = AB_MAGIC;
        stop.type  = AB_TYPE_HELLO;
        hdr_encode(wire, &stop);
        (void)sendto(a_fd, wire, AB_HDR_LEN, 0, (struct sockaddr *)&baddr, blen);
    }
    pthread_join(tid, NULL);

    out->seconds    = (double)(t1 - t0) / 1e9;
    out->cpu_user_ms = u1 - u0;
    out->cpu_sys_ms  = s1 - s0;
    out->nvcsw      = nv1 - nv0;
    out->nivcsw     = ni1 - ni0;
    out->rss_kb     = proc_kb("VmRSS:");
    out->hwm_kb     = proc_kb("VmHWM:");
    out->delivered  = (uint64_t)atomic_load(&peer.delivered);
    out->rejected   = (uint64_t)atomic_load(&peer.rejected);
    out->truncated  = (uint64_t)atomic_load(&peer.truncated);

    close(a_fd);
    close(peer.fd);
    close(peer.to_a);
    unlink(g_path_a);
    unlink(g_path_b);
    free(msg);
    free(rbuf);
    return 0;
}

static int run_relay(size_t payload, uint64_t rounds, uint64_t *lat,
                     ab_result_t *out)
{
    relay_t            r;
    relay_client_ctx_t cb;
    pthread_t          r_tid, b_tid;
    int                a_fd, b_fd;
    unsigned char     *msg = malloc(AB_HDR_LEN + payload);
    unsigned char     *rbuf = malloc(AB_HDR_LEN + payload);
    ab_hdr_t           hello;
    long               u0, s0, nv0, ni0, u1, s1, nv1, ni1;
    uint64_t           t0, t1, i;

    memset(&r, 0, sizeof(r));
    memset(&cb, 0, sizeof(cb));
    if (msg == NULL || rbuf == NULL) {
        free(msg);
        free(rbuf);
        return -1;
    }
    r.expect_uid = (uint32_t)getuid();
    atomic_init(&r.stop, 0);
    atomic_init(&r.rejected, 0);
    atomic_init(&r.delivered, 0);

    unlink(g_path_relay);
    if (pthread_create(&r_tid, NULL, relay_thread, &r) != 0) {
        free(msg);
        free(rbuf);
        return -1;
    }
    /* Wait for the listening socket to exist. */
    for (i = 0; i < 500; i++) {
        struct stat st;

        if (stat(g_path_relay, &st) == 0) {
            break;
        }
        {
            struct timespec ts = { 0, 2000000 }; /* 2 ms */
            nanosleep(&ts, NULL);
        }
    }

    a_fd = connect_unix(g_path_relay);
    b_fd = connect_unix(g_path_relay);
    if (a_fd < 0 || b_fd < 0) {
        fprintf(stderr, "ab_relay: cannot connect to relay: %s\n",
                strerror(errno));
        free(msg);
        free(rbuf);
        if (a_fd >= 0) {
            close(a_fd);
        }
        if (b_fd >= 0) {
            close(b_fd);
        }
        return -1;
    }

    memset(&hello, 0, sizeof(hello));
    hello.magic = AB_MAGIC;
    hello.type  = AB_TYPE_HELLO;
    hello.from  = MOD_A;
    hdr_encode(msg, &hello);
    if (frame_write(a_fd, msg, AB_HDR_LEN) != 0) {
        return -1;
    }
    hello.from = MOD_B;
    hdr_encode(msg, &hello);
    if (frame_write(b_fd, msg, AB_HDR_LEN) != 0) {
        return -1;
    }

    cb.fd = b_fd;
    cb.payload = payload;
    atomic_init(&cb.stop, 0);
    atomic_init(&cb.rejected, 0);
    atomic_init(&cb.delivered, 0);
    if (pthread_create(&b_tid, NULL, relay_client_b_thread, &cb) != 0) {
        return -1;
    }

    rusage_snapshot(&u0, &s0, &nv0, &ni0);
    t0 = now_ns();
    for (i = 0; i < rounds; i++) {
        ab_hdr_t h;
        size_t   n = 0;
        uint64_t r0, r1;

        h.magic = AB_MAGIC;
        h.type  = AB_TYPE_REQ;
        h.from  = MOD_A;
        h.to    = MOD_B;
        h.seq   = (uint32_t)i;
        h.len   = (uint32_t)payload;
        hdr_encode(msg, &h);

        r0 = now_ns();
        if (frame_write(a_fd, msg, AB_HDR_LEN + payload) != 0) {
            break;
        }
        if (frame_read(a_fd, rbuf, AB_HDR_LEN + payload, &n) != 0) {
            break;
        }
        r1 = now_ns();
        lat[i] = r1 - r0;
    }
    t1 = now_ns();
    rusage_snapshot(&u1, &s1, &nv1, &ni1);

    atomic_store(&cb.stop, 1);
    pthread_join(b_tid, NULL);
    atomic_store(&r.stop, 1);
    close(a_fd);
    close(b_fd);
    pthread_join(r_tid, NULL);

    out->seconds     = (double)(t1 - t0) / 1e9;
    out->cpu_user_ms = u1 - u0;
    out->cpu_sys_ms  = s1 - s0;
    out->nvcsw       = nv1 - nv0;
    out->nivcsw      = ni1 - ni0;
    out->rss_kb      = proc_kb("VmRSS:");
    out->hwm_kb      = proc_kb("VmHWM:");
    out->delivered   = (uint64_t)atomic_load(&cb.delivered) +
                       (uint64_t)atomic_load(&r.delivered);
    out->rejected    = (uint64_t)atomic_load(&cb.rejected) +
                       (uint64_t)atomic_load(&r.rejected);
    out->truncated   = 0; /* framed stream: truncation is impossible by design */
    unlink(g_path_relay);
    free(msg);
    free(rbuf);
    return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    size_t         payload = 256;
    uint64_t       rounds = 20000;
    int            repeats = 5, csv = 0, keep = 0, i;
    char           tmpl[] = "/tmp/ab-relay-XXXXXX";
    uint64_t      *lat = NULL;
    ab_result_t    best_d, best_r;
    static const char *hdr_csv =
        "arm,payload,rounds,sec,p50_ns,p95_ns,p99_ns,cpu_user_ms,"
        "cpu_sys_ms,nvcsw,nivcsw,rss_kb,hwm_kb,hops,rejected";

    memset(&best_d, 0, sizeof(best_d));
    memset(&best_r, 0, sizeof(best_r));

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            payload = (size_t)strtoull(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--rounds") == 0 && i + 1 < argc) {
            rounds = strtoull(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--repeats") == 0 && i + 1 < argc) {
            repeats = (int)strtol(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--csv") == 0) {
            csv = 1;
        } else if (strcmp(argv[i], "--keep") == 0) {
            keep = 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("usage: ab_relay [--size B] [--rounds N] [--repeats R]"
                   " [--csv] [--keep]\n");
            return 0;
        } else {
            fprintf(stderr, "ab_relay: unknown argument '%s'\n", argv[i]);
            return 2;
        }
    }
    if (payload == 0 || payload > (1u << 20) || rounds == 0 || repeats <= 0) {
        fprintf(stderr, "ab_relay: bad parameters\n");
        return 2;
    }

    if (mkdtemp(tmpl) == NULL) {
        fprintf(stderr, "ab_relay: mkdtemp: %s\n", strerror(errno));
        return 1;
    }
    snprintf(g_dir, sizeof(g_dir), "%s", tmpl);
    if (chmod(g_dir, 0700) != 0) {
        fprintf(stderr, "ab_relay: chmod: %s\n", strerror(errno));
    }
    snprintf(g_path_a, sizeof(g_path_a), "%s/a.sock", g_dir);
    snprintf(g_path_b, sizeof(g_path_b), "%s/b.sock", g_dir);
    snprintf(g_path_relay, sizeof(g_path_relay), "%s/relay.sock", g_dir);

    lat = calloc((size_t)rounds, sizeof(uint64_t));
    if (lat == NULL) {
        return 1;
    }

    if (csv) {
        printf("%s\n", hdr_csv);
    } else {
        printf("ab_relay -- synthetic topology A/B (NOT the real legacy "
               "frameworks)\n");
        printf("  payload %zu B, round trips per repetition %" PRIu64
               ", repetitions %d\n", payload, rounds, repeats);
        printf("  direct : A --request--> B --reply--> A        (2 hops, "
               "2 wakeups)\n");
        printf("  relay  : A -> relay -> B -> relay -> A        (4 hops, "
               "4 wakeups)\n\n");
    }

    /* Warm up both arms once so the first repetition is not penalised by
     * page faults and socket-creation costs. */
    {
        uint64_t warm = rounds / 10 ? rounds / 10 : 1;
        ab_result_t scratch;

        if (run_direct(payload, warm, lat, &scratch) != 0 ||
            run_relay(payload, warm, lat, &scratch) != 0) {
            fprintf(stderr, "ab_relay: warmup failed\n");
            free(lat);
            return 1;
        }
    }

    for (i = 0; i < repeats; i++) {
        ab_result_t d, r;

        if (run_direct(payload, rounds, lat, &d) != 0) {
            fprintf(stderr, "ab_relay: direct arm failed\n");
            free(lat);
            return 1;
        }
        qsort(lat, (size_t)rounds, sizeof(uint64_t), cmp_u64);
        d.p50 = pct_of(lat, (size_t)rounds, 50);
        d.p95 = pct_of(lat, (size_t)rounds, 95);
        d.p99 = pct_of(lat, (size_t)rounds, 99);

        if (run_relay(payload, rounds, lat, &r) != 0) {
            fprintf(stderr, "ab_relay: relay arm failed\n");
            free(lat);
            return 1;
        }
        qsort(lat, (size_t)rounds, sizeof(uint64_t), cmp_u64);
        r.p50 = pct_of(lat, (size_t)rounds, 50);
        r.p95 = pct_of(lat, (size_t)rounds, 95);
        r.p99 = pct_of(lat, (size_t)rounds, 99);

        if (csv) {
            printf("direct,%zu,%" PRIu64 ",%.6f,%" PRIu64 ",%" PRIu64
                   ",%" PRIu64 ",%ld,%ld,%ld,%ld,%ld,%ld,2,%" PRIu64 "\n",
                   payload, rounds, d.seconds, d.p50, d.p95, d.p99,
                   d.cpu_user_ms, d.cpu_sys_ms, d.nvcsw, d.nivcsw, d.rss_kb,
                   d.hwm_kb, d.rejected);
            printf("relay,%zu,%" PRIu64 ",%.6f,%" PRIu64 ",%" PRIu64
                   ",%" PRIu64 ",%ld,%ld,%ld,%ld,%ld,%ld,4,%" PRIu64 "\n",
                   payload, rounds, r.seconds, r.p50, r.p95, r.p99,
                   r.cpu_user_ms, r.cpu_sys_ms, r.nvcsw, r.nivcsw, r.rss_kb,
                   r.hwm_kb, r.rejected);
        } else {
            printf("  run %d/%d\n", i + 1, repeats);
            printf("    direct  %7.3f s  %10.0f rt/s   p50 %6" PRIu64
                   " ns  p95 %6" PRIu64 " ns  p99 %6" PRIu64 " ns\n",
                   d.seconds, (double)rounds / d.seconds, d.p50, d.p95, d.p99);
            printf("    relay   %7.3f s  %10.0f rt/s   p50 %6" PRIu64
                   " ns  p95 %6" PRIu64 " ns  p99 %6" PRIu64 " ns\n",
                   r.seconds, (double)rounds / r.seconds, r.p50, r.p95, r.p99);
            printf("    cpu     direct user+sys %ld ms   relay user+sys %ld ms"
                   "   (per round trip: %.1f us vs %.1f us)\n",
                   d.cpu_user_ms + d.cpu_sys_ms, r.cpu_user_ms + r.cpu_sys_ms,
                   (double)(d.cpu_user_ms + d.cpu_sys_ms) * 1000.0 /
                       (double)rounds,
                   (double)(r.cpu_user_ms + r.cpu_sys_ms) * 1000.0 /
                       (double)rounds);
            printf("    ctxsw   direct %ld vol + %ld invol   relay %ld vol + "
                   "%ld invol\n",
                   d.nvcsw, d.nivcsw, r.nvcsw, r.nivcsw);
            printf("    rss     direct %ld KiB (peak %ld)   relay %ld KiB "
                   "(peak %ld)\n",
                   d.rss_kb, d.hwm_kb, r.rss_kb, r.hwm_kb);
            printf("    idcheck direct rejected %" PRIu64 " trunc %" PRIu64
                   " delivered %" PRIu64 "   relay rejected %" PRIu64
                   " trunc %" PRIu64 " delivered %" PRIu64 "\n\n",
                   d.rejected, d.truncated, d.delivered, r.rejected,
                   r.truncated, r.delivered);
        }

        if (i == 0 || d.p50 < best_d.p50) {
            best_d = d;
        }
        if (i == 0 || r.p50 < best_r.p50) {
            best_r = r;
        }
    }

    if (csv) {
        free(lat);
        if (!keep) {
            unlink(g_path_a);
            unlink(g_path_b);
            unlink(g_path_relay);
            rmdir(g_dir);
        }
        return 0;
    }

    printf("-- best-of-%d summary (lowest p50), payload %zu B, %" PRIu64
           " round trips\n", repeats, payload, rounds);
    printf("   %-8s %10s %12s %9s %9s %9s %12s %10s\n", "arm", "seconds",
           "rt/s", "p50 ns", "p95 ns", "p99 ns", "cpu ms", "us/rt");
    printf("   %-8s %10.3f %12.0f %9" PRIu64 " %9" PRIu64 " %9" PRIu64
           " %12ld %10.1f\n",
           "direct", best_d.seconds, (double)rounds / best_d.seconds,
           best_d.p50, best_d.p95, best_d.p99,
           best_d.cpu_user_ms + best_d.cpu_sys_ms,
           (double)(best_d.cpu_user_ms + best_d.cpu_sys_ms) * 1000.0 /
               (double)rounds);
    printf("   %-8s %10.3f %12.0f %9" PRIu64 " %9" PRIu64 " %9" PRIu64
           " %12ld %10.1f\n",
           "relay", best_r.seconds, (double)rounds / best_r.seconds,
           best_r.p50, best_r.p95, best_r.p99,
           best_r.cpu_user_ms + best_r.cpu_sys_ms,
           (double)(best_r.cpu_user_ms + best_r.cpu_sys_ms) * 1000.0 /
               (double)rounds);
    printf("\n   delta  relay/direct: p50 %.2fx   CPU/round-trip %.2fx   "
           "throughput %.2fx\n",
           best_d.p50 ? (double)best_r.p50 / (double)best_d.p50 : 0.0,
           (double)(best_r.cpu_user_ms + best_r.cpu_sys_ms) /
               (double)(best_d.cpu_user_ms + best_d.cpu_sys_ms),
           best_d.seconds ? best_r.seconds / best_d.seconds : 0.0);

    free(lat);
    if (!keep) {
        unlink(g_path_a);
        unlink(g_path_b);
        unlink(g_path_relay);
        rmdir(g_dir);
    } else {
        fprintf(stderr, "ab_relay: keeping scratch dir %s\n", g_dir);
    }
    return 0;
}
