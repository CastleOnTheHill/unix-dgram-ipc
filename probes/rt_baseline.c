/*
 * rt_baseline -- round-trip latency of a *bare* AF_UNIX SOCK_DGRAM socketpair,
 * with no framework on top, so the numbers in the ipc_bench report can be read
 * as "framework cost above the kernel primitive" rather than as raw numbers.
 *
 * Two modes, because they answer two different questions:
 *
 *   same-thread   send and receive on one thread.  No scheduler involvement,
 *                 so this is the pure syscall path: sendto() -> queue ->
 *                 recvfrom().  Lower bound for anything built on datagrams.
 *
 *   two-thread    sender and echoer on separate threads, single message in
 *                 flight.  Adds one cross-thread wakeup per direction, which
 *                 is what a real module-to-module hop pays.
 *
 * Usage: rt_baseline [--rounds N] [--size B]
 */

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

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

static uint64_t pct_of(const uint64_t *sorted, size_t n, double p)
{
    size_t idx = (size_t)((p / 100.0) * (double)(n - 1) + 0.5);
    if (idx >= n) {
        idx = n - 1;
    }
    return sorted[idx];
}

static void report(const char *label, uint64_t *s, size_t n, size_t planned)
{
    uint64_t sum = 0;
    size_t   i;

    if (n == 0) {
        /* Error paths `break` out of the measurement loop.  Saying so beats
         * feeding the statistics a tail of zeros, which would drag min/p50
         * down and make a failed run look like a fast one. */
        printf("  %-12s no samples\n", label);
        return;
    }
    qsort(s, n, sizeof(uint64_t), cmp_u64);
    for (i = 0; i < n; i++) {
        sum += s[i];
    }
    printf("  %-12s min %7" PRIu64 "  p50 %7" PRIu64 "  p90 %7" PRIu64
           "  p99 %7" PRIu64 "  max %8" PRIu64 "  mean %9.1f  ns\n",
           label, s[0], pct_of(s, n, 50), pct_of(s, n, 90), pct_of(s, n, 99),
           s[n - 1], (double)sum / (double)n);
    if (n != planned) {
        printf("  %-12s (NOTE: only %zu of %zu planned samples were collected)\n",
               label, n, planned);
    }
}

/* ---------------------------------------------------------------- */

typedef struct {
    int    fd;
    size_t payload;
    uint64_t rounds;
    int    go;
} echo_arg_t;

/* Echo everything that arrives until the peer closes. */
static void *echo_thread(void *arg)
{
    echo_arg_t *a = (echo_arg_t *)arg;
    char       *buf = malloc(a->payload + 16);

    if (buf == NULL) {
        return NULL;
    }
    for (;;) {
        ssize_t n = recv(a->fd, buf, a->payload + 16, 0);
        if (n <= 0) {
            break;
        }
        if (send(a->fd, buf, (size_t)n, 0) < 0) {
            break;
        }
    }
    free(buf);
    return NULL;
}

int main(int argc, char **argv)
{
    uint64_t  rounds = 20000;
    size_t    payload = 256;
    int       sv[2];
    char     *buf, *rbuf;
    uint64_t *same, *two;
    size_t    same_n, two_n;
    int       i;
    uint64_t  k;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--rounds") == 0 && i + 1 < argc) {
            rounds = strtoull(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            payload = (size_t)strtoull(argv[++i], NULL, 10);
        } else {
            fprintf(stderr, "usage: rt_baseline [--rounds N] [--size B]\n");
            return 2;
        }
    }
    if (rounds == 0 || payload == 0) {
        return 2;
    }

    buf  = malloc(payload);
    rbuf = malloc(payload);
    same = calloc((size_t)rounds, sizeof(uint64_t));
    two  = calloc((size_t)rounds, sizeof(uint64_t));
    if (buf == NULL || rbuf == NULL || same == NULL || two == NULL) {
        fprintf(stderr, "rt_baseline: out of memory\n");
        return 1;
    }
    memset(buf, 'x', payload);

    printf("rt_baseline  payload=%zu B  rounds=%" PRIu64 "\n", payload, rounds);

    /* -- same thread: pure syscall round trip ---------------------- */
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) != 0) {
        fprintf(stderr, "rt_baseline: socketpair: %s\n", strerror(errno));
        return 1;
    }
    for (k = 0; k < rounds; k++) {
        uint64_t t0 = now_ns();
        if (send(sv[0], buf, payload, 0) < 0) {
            fprintf(stderr, "rt_baseline: send: %s\n", strerror(errno));
            return 1;
        }
        if (recv(sv[1], rbuf, payload, 0) < 0) {
            fprintf(stderr, "rt_baseline: recv: %s\n", strerror(errno));
            return 1;
        }
        same[k] = now_ns() - t0;
    }
    same_n = (size_t)rounds;
    close(sv[0]);
    close(sv[1]);

    /* -- two threads: adds one wakeup per direction ---------------- */
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) != 0) {
        fprintf(stderr, "rt_baseline: socketpair: %s\n", strerror(errno));
        return 1;
    }
    {
        echo_arg_t      a = { sv[1], payload, rounds, 0 };
        pthread_t       t;
        struct timespec ts = { 0, 200000 };

        if (pthread_create(&t, NULL, echo_thread, &a) != 0) {
            fprintf(stderr, "rt_baseline: pthread_create failed\n");
            return 1;
        }
        /* Let the echo thread reach recv() and block before we start, so the
         * first round trip is not charged for thread startup. */
        nanosleep(&ts, NULL);

        for (k = 0; k < rounds; k++) {
            uint64_t t0 = now_ns();
            if (send(sv[0], buf, payload, 0) < 0) {
                fprintf(stderr, "rt_baseline: two-thread send: %s\n",
                        strerror(errno));
                break;
            }
            if (recv(sv[0], rbuf, payload, 0) < 0) {
                fprintf(stderr, "rt_baseline: two-thread recv: %s\n",
                        strerror(errno));
                break;
            }
            two[k] = now_ns() - t0;
        }
        /* Only the slots actually filled are statistics; the rest are still
         * zero from calloc() and must not be counted. */
        two_n = (size_t)k;
        close(sv[0]);
        shutdown(sv[1], SHUT_RDWR);
        pthread_join(t, NULL);
        close(sv[1]);
    }

    report("same-thread", same, same_n, (size_t)rounds);
    report("two-thread", two, two_n, (size_t)rounds);

    free(buf);
    free(rbuf);
    free(same);
    free(two);
    return 0;
}
