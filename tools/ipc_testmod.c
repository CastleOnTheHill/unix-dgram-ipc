/*
 * ipc_testmod.c -- scriptable module used by the integration tests.
 *
 * The binary registers as a module, then reads a command stream from a FIFO
 * and appends an observation journal.  Every assertion in the shell tests is
 * made against the journal, so a test waits for a specific line (with a
 * deadline) instead of sleeping and hoping.
 *
 * Commands (one per line on the FIFO):
 *   behave <event> <action>         how to handle that event (see below)
 *   post <dst> <event> <payload>    asynchronous send
 *   send <dst> <event> <payload>    synchronous send, waits forever (legacy)
 *   sendt <ms> <dst> <event> <pl>   synchronous send with a timeout
 *   sendmany <n> <ms> <dst> <ev>    n concurrent synchronous senders, each
 *                                   carrying its index, each verifying that the
 *                                   reply it received belongs to its request
 *   blast <n> <dst> <event> <pl>    post n times as fast as possible and report
 *                                   the OK/AGAIN/offline split
 *   soak <n> <dst> <event> <pl>     post n times, retrying on AGAIN, so that a
 *                                   known number of messages is really delivered
 *   bcast <event> <payload>         broadcast
 *   stats                           dump counters to the journal
 *   note <text>                     journal marker
 *   del <event>                     forget a behaviour
 *   re-register                     try ipc_register() a second time
 *   close-fifo                      stop reading commands
 *   crash                           _exit() with no cleanup (simulate SIGKILL)
 *   exit                            _exit() without unregistering
 *   stop                            graceful unregister and exit
 *
 * Actions:
 *   echo                 reply with the request payload (default for REQ)
 *   reply:<text>         reply with a literal string
 *   replysize:<n>        reply with n 'R' bytes
 *   replyevent           reply with the decimal event id
 *   drop                 stay silent (for the "sent but no reply" tests)
 *   journal              record and answer nothing
 *   block:<ms>           sleep, then reply echo
 *   nested:<mod>:<ev>    ipc_send() from inside the handler
 *   deadlock             ipc_send() from inside an INLINE handler
 *   double-reply         call ipc_reply() twice
 *   stop-now             ipc_stop() from inside the handler
 *   forkprobe:<sec>      fork a child that inherits the fds and stays alive
 *
 * Environment: IPC_JOURNAL (required), IPC_FIFO (optional), IPC_CONF (used by
 * re-register).
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ipc/ipc.h"
#include "ipc_proto.h" /* ipc_payload_to_cstr: journal rendering only */

#define MAX_BEHAVIOURS 64
#define MAX_CMD        4096
#define MAX_ACTION     96

typedef struct {
    uint32_t event;
    char     action[MAX_ACTION];
} behaviour_t;

static behaviour_t     g_beh[MAX_BEHAVIOURS];
static int             g_nbeh;
static ipc_ctx_t      *g_ctx;
static pthread_mutex_t g_jlock = PTHREAD_MUTEX_INITIALIZER;
static FILE           *g_jf;
static volatile int    g_stop;

/* ------------------------------------------------------------------ */
/* journal                                                             */
/* ------------------------------------------------------------------ */

static void J(const char *fmt, ...)
{
    va_list ap;

    pthread_mutex_lock(&g_jlock);
    if (g_jf != NULL) {
        va_start(ap, fmt);
        vfprintf(g_jf, fmt, ap);
        va_end(ap);
        fputc('\n', g_jf);
        fflush(g_jf);
    }
    pthread_mutex_unlock(&g_jlock);
}

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static behaviour_t *behaviour_for(uint32_t event)
{
    int i;

    for (i = 0; i < g_nbeh; i++) {
        if (g_beh[i].event == event) {
            return &g_beh[i];
        }
    }
    return NULL;
}

static void sleep_ms(long ms)
{
    struct timespec ts;

    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static int starts_with(const char *s, const char *p)
{
    return strncmp(s, p, strlen(p)) == 0;
}

/* Copy the next whitespace-delimited token from *pp into out.  Returns 1 when
 * a token was found, 0 at end of string. */
static int next_token(const char **pp, char *out, size_t cap)
{
    const char *p = *pp;
    size_t      n = 0;

    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p == '\0') {
        *pp = p;
        return 0;
    }
    while (*p != '\0' && *p != ' ' && *p != '\t' && n < cap - 1) {
        out[n++] = *p++;
    }
    out[n] = '\0';
    /* If the token was longer than cap-1, skip the rest of it. */
    while (*p != '\0' && *p != ' ' && *p != '\t') {
        p++;
    }
    *pp = p;
    return 1;
}

static void payload_line(const char *tag, const void *data, size_t len,
                         const ipc_msg_t *m)
{
    char buf[200];

    ipc_payload_to_cstr(data, len, buf, sizeof(buf));
    if (m != NULL) {
        J("%s src=%s dst=%s ns=%s event=%u type=%d len=%zu instance=0x%016llx "
          "data=%s",
          tag, m->src, m->dst, m->ns, m->event, (int)m->type, len,
          (unsigned long long)m->instance_id, buf);
    } else {
        J("%s len=%zu data=%s", tag, len, buf);
    }
}

/* ------------------------------------------------------------------ */
/* handler actions                                                     */
/* ------------------------------------------------------------------ */

static void do_reply(const ipc_msg_t *m, const void *data, size_t len)
{
    int rc = ipc_reply(m, data, len);

    J("REPLY event=%u len=%zu rc=%d", m->event, len, rc);
}

static void run_forkprobe(const ipc_msg_t *m, long seconds)
{
    pid_t pid = fork();

    if (pid < 0) {
        J("FORKPROBE rc=-1 errno=%d", errno);
        return;
    }
    if (pid == 0) {
        /* The child inherits the socket, the lock and the FIFO.  It must not
         * use the parent's context, and it reports through its own file so the
         * journal stays a single-writer artifact. */
        char  path[512];
        const char *env = getenv("IPC_JOURNAL");
        int   fd;
        int   rc;
        char  line[256];
        int   n;

        snprintf(path, sizeof(path), "%s.child", env ? env : "/tmp/ipc");
        rc = ipc_post(g_ctx, m->src, 99, "child", 5);
        n  = snprintf(line, sizeof(line),
                      "CHILD pid=%ld post_rc=%d expect_rc=%d\n", (long)getpid(),
                      rc, IPC_ERR_STATE);
        fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (fd >= 0) {
            ssize_t w = write(fd, line, (size_t)n);
            (void)w;
            close(fd);
        }
        sleep_ms(seconds * 1000);
        _exit(0);
    }
    J("FORKPROBE child=%ld seconds=%ld", (long)pid, seconds);
}

static void handle(const ipc_msg_t *m, void *user)
{
    behaviour_t *b   = behaviour_for(m->event);
    const char  *act = b ? b->action
                         : (m->type == IPC_TYPE_REQ ? "echo" : "journal");

    (void)user;
    payload_line("RECV", m->data, m->len, m);

    if (strcmp(act, "journal") == 0) {
        return;
    }
    if (strcmp(act, "echo") == 0) {
        if (m->type == IPC_TYPE_REQ) {
            do_reply(m, m->data, m->len);
        }
        return;
    }
    if (starts_with(act, "reply:")) {
        if (m->type == IPC_TYPE_REQ) {
            do_reply(m, act + 6, strlen(act + 6));
        }
        return;
    }
    if (starts_with(act, "replysize:")) {
        static char big[131072];
        size_t      n = (size_t)strtoul(act + 10, NULL, 10);

        if (n > sizeof(big)) {
            n = sizeof(big);
        }
        memset(big, 'R', n);
        if (m->type == IPC_TYPE_REQ) {
            do_reply(m, big, n);
        }
        return;
    }
    if (strcmp(act, "replyevent") == 0) {
        char num[32];

        if (m->type == IPC_TYPE_REQ) {
            snprintf(num, sizeof(num), "%u", m->event);
            do_reply(m, num, strlen(num));
        }
        return;
    }
    if (strcmp(act, "drop") == 0) {
        J("ACTION event=%u drop", m->event);
        return;
    }
    if (starts_with(act, "block:")) {
        long ms = strtol(act + 6, NULL, 10);

        J("ACTION event=%u block_ms=%ld begin", m->event, ms);
        sleep_ms(ms);
        if (m->type == IPC_TYPE_REQ) {
            do_reply(m, m->data, m->len);
        }
        J("ACTION event=%u block_ms=%ld end", m->event, ms);
        return;
    }
    if (starts_with(act, "nested:")) {
        char        spec[128];
        char       *colon;
        const char *dst;
        uint32_t    ev;
        char        buf[64];
        size_t      rlen = 0;
        int         rc;

        strncpy(spec, act + 7, sizeof(spec) - 1);
        spec[sizeof(spec) - 1] = '\0';
        colon                  = strchr(spec, ':');
        if (colon == NULL) {
            J("NESTED rc=%d bad_spec", IPC_ERR_INVAL);
            return;
        }
        *colon = '\0';
        dst    = spec;
        ev     = (uint32_t)strtoul(colon + 1, NULL, 10);
        rc     = ipc_send(g_ctx, dst, ev, "nested", 6, buf, sizeof(buf), &rlen);
        J("NESTED dst=%s event=%u rc=%d reply_len=%zu", dst, ev, rc, rlen);
        if (m->type == IPC_TYPE_REQ) {
            char out[64];
            snprintf(out, sizeof(out), "nested-rc=%d", rc);
            do_reply(m, out, strlen(out));
        }
        return;
    }
    if (strcmp(act, "deadlock") == 0) {
        int rc = ipc_send(g_ctx, m->src, 1234, "x", 1, NULL, 0, NULL);

        J("DEADLOCKPROBE rc=%d", rc);
        if (m->type == IPC_TYPE_REQ) {
            char out[64];
            snprintf(out, sizeof(out), "dl-rc=%d", rc);
            do_reply(m, out, strlen(out));
        }
        return;
    }
    if (strcmp(act, "double-reply") == 0) {
        int rc1 = ipc_reply(m, "one", 3);
        int rc2 = ipc_reply(m, "two", 3);

        J("DOUBLEREPLY rc1=%d rc2=%d", rc1, rc2);
        return;
    }
    if (strcmp(act, "stop-now") == 0) {
        J("ACTION event=%u stop_now", m->event);
        if (m->type == IPC_TYPE_REQ) {
            do_reply(m, "stopping", 8);
        }
        ipc_stop(g_ctx);
        return;
    }
    if (starts_with(act, "forkprobe:")) {
        long sec = strtol(act + 10, NULL, 10);

        run_forkprobe(m, sec);
        if (m->type == IPC_TYPE_REQ) {
            do_reply(m, "forked", 6);
        }
        return;
    }
    J("ACTION event=%u unknown_action=%s", m->event, act);
}

/* ------------------------------------------------------------------ */
/* commands                                                            */
/* ------------------------------------------------------------------ */

static void journal_stats(void)
{
    ipc_stats_t s;

    ipc_get_stats(g_ctx, &s);
    J("STATS attempts=%llu enqueued=%llu failed=%llu eagain=%llu "
      "bcast_targets=%llu bcast_skipped=%llu recv_read=%llu rejected=%llu "
      "rej_cred=%llu rej_proto=%llu rej_trunc=%llu delivered=%llu "
      "cb_invoked=%llu cb_dropped=%llu replies=%llu reply_matched=%llu "
      "reply_unmatched=%llu pending_rejected=%llu deadlock_probes=%llu",
      (unsigned long long)s.send_attempts,
      (unsigned long long)s.send_enqueued, (unsigned long long)s.send_failed,
      (unsigned long long)s.eagain_count,
      (unsigned long long)s.broadcast_targets,
      (unsigned long long)s.broadcast_skipped,
      (unsigned long long)s.recv_read, (unsigned long long)s.recv_rejected,
      (unsigned long long)s.recv_rej_cred, (unsigned long long)s.recv_rej_proto,
      (unsigned long long)s.recv_rej_trunc,
      (unsigned long long)s.recv_delivered, (unsigned long long)s.cb_invoked,
      (unsigned long long)s.cb_dropped, (unsigned long long)s.reply_sent,
      (unsigned long long)s.reply_matched,
      (unsigned long long)s.reply_unmatched,
      (unsigned long long)s.pending_rejected,
      (unsigned long long)s.deadlock_probes);
}

static void cmd_behave(const char *rest)
{
    char       evtok[32];
    uint32_t   ev;
    const char *p = rest;

    if (!next_token(&p, evtok, sizeof(evtok))) {
        J("CMD behave rc=%d missing_event", IPC_ERR_INVAL);
        return;
    }
    ev = (uint32_t)strtoul(evtok, NULL, 10);
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p == '\0') {
        J("CMD behave rc=%d missing_action", IPC_ERR_INVAL);
        return;
    }
    if (g_nbeh >= MAX_BEHAVIOURS) {
        J("CMD behave rc=%d too_many", IPC_ERR_TOOMANY);
        return;
    }
    g_beh[g_nbeh].event = ev;
    strncpy(g_beh[g_nbeh].action, p, MAX_ACTION - 1);
    g_beh[g_nbeh].action[MAX_ACTION - 1] = '\0';
    g_nbeh++;
    J("CMD behave event=%u action=%s", ev, g_beh[g_nbeh - 1].action);
}

static void cmd_post(const char *rest)
{
    char        dst[64], evtok[32];
    const char *p = rest;
    uint32_t    ev;
    const char *payload;
    int         rc;

    if (!next_token(&p, dst, sizeof(dst)) ||
        !next_token(&p, evtok, sizeof(evtok))) {
        J("CMD post rc=%d bad_args", IPC_ERR_INVAL);
        return;
    }
    ev = (uint32_t)strtoul(evtok, NULL, 10);
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    payload = p;
    rc      = ipc_post(g_ctx, dst, ev, payload, strlen(payload));
    J("POST dst=%s event=%u rc=%d payload=%s", dst, ev, rc, payload);
}

static void cmd_bcast(const char *rest)
{
    char        evtok[32];
    const char *p = rest;
    uint32_t    ev;
    const char *payload;
    int         rc;

    if (!next_token(&p, evtok, sizeof(evtok))) {
        J("CMD bcast rc=%d bad_args", IPC_ERR_INVAL);
        return;
    }
    ev = (uint32_t)strtoul(evtok, NULL, 10);
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    payload = p;
    rc      = ipc_broadcast(g_ctx, ev, payload, strlen(payload));
    J("BCAST event=%u rc=%d payload=%s", ev, rc, payload);
}

static void cmd_send(const char *rest, int with_timeout)
{
    char        dst[64], evtok[32];
    const char *p = rest;
    uint32_t    ev;
    int         timeout = -1;
    const char *payload;
    char        reply[131072];
    size_t      rlen = 0;
    int         rc;
    char        safe[200];

    if (with_timeout) {
        char mstok[32];
        if (!next_token(&p, mstok, sizeof(mstok))) {
            J("CMD sendt rc=%d bad_args", IPC_ERR_INVAL);
            return;
        }
        timeout = (int)strtol(mstok, NULL, 10);
    }
    if (!next_token(&p, dst, sizeof(dst)) || !next_token(&p, evtok, sizeof(evtok))) {
        J("CMD send rc=%d bad_args", IPC_ERR_INVAL);
        return;
    }
    ev = (uint32_t)strtoul(evtok, NULL, 10);
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    payload = p;

    J("SEND begin op=%s dst=%s event=%u timeout=%d payload=%s",
      with_timeout ? "sendt" : "send", dst, ev, timeout, payload);
    rc = ipc_send_timeout(g_ctx, dst, ev, payload, strlen(payload), reply,
                          sizeof(reply), &rlen, timeout);
    ipc_payload_to_cstr(reply, rlen, safe, sizeof(safe));
    J("SEND done op=%s dst=%s event=%u rc=%d reply_len=%zu reply=%s",
      with_timeout ? "sendt" : "send", dst, ev, rc, rlen, safe);
}

/*
 * Return codes from handle_command():
 *   0  keep reading commands
 *   1  stop the process (graceful unregister)
 *   2  close the command FIFO but keep receiving (the caller then waits for a
 *      signal, because nothing can reach the command loop again)
 */
#define CMD_CONTINUE   0
#define CMD_STOP       1
#define CMD_CLOSE_FIFO 2

/* ------------------------------------------------------------------ */
/* concurrent synchronous senders                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *dst;
    uint32_t    event;
    int         timeout_ms;
    int         index;
} many_arg_t;

/* Each thread issues one request carrying its own index, and records whether
 * the reply it got back belonged to *its* request.  A cross-delivered reply is
 * exactly the failure this test exists to catch, so the payload is compared
 * instead of just the return code. */
static void *many_thread(void *arg)
{
    many_arg_t *a = (many_arg_t *)arg;
    char        req[64];
    char        reply[256];
    char        safe[200];
    size_t      rlen = 0;
    int         rc;
    int         match;

    snprintf(req, sizeof(req), "T%d", a->index);
    memset(reply, 0, sizeof(reply));
    rc = ipc_send_timeout(g_ctx, a->dst, a->event, req, strlen(req), reply,
                          sizeof(reply), &rlen, a->timeout_ms);
    ipc_payload_to_cstr(reply, rlen, safe, sizeof(safe));
    match = (rc == IPC_OK && strcmp(safe, req) == 0);
    J("SENDMANY n=%d rc=%d reply=%s match=%d", a->index, rc, safe, match);
    return NULL;
}

static void cmd_sendmany(const char *rest)
{
    char       ntok[32], ttok[32], dst[64], evtok[32];
    const char *p = rest;
    int         n, timeout;
    uint32_t    ev;
    pthread_t  *th = NULL;
    many_arg_t *ar = NULL;
    int         made = 0, i;

    if (!next_token(&p, ntok, sizeof(ntok)) ||
        !next_token(&p, ttok, sizeof(ttok)) ||
        !next_token(&p, dst, sizeof(dst)) ||
        !next_token(&p, evtok, sizeof(evtok))) {
        J("CMD sendmany rc=%d bad_args", IPC_ERR_INVAL);
        return;
    }
    n       = (int)strtol(ntok, NULL, 10);
    timeout = (int)strtol(ttok, NULL, 10);
    ev      = (uint32_t)strtoul(evtok, NULL, 10);
    if (n <= 0 || n > 256) {
        J("CMD sendmany rc=%d bad_count", IPC_ERR_INVAL);
        return;
    }

    th = calloc((size_t)n, sizeof(*th));
    ar = calloc((size_t)n, sizeof(*ar));
    if (th == NULL || ar == NULL) {
        J("CMD sendmany rc=%d nomem", IPC_ERR_NOMEM);
        free(th);
        free(ar);
        return;
    }
    for (i = 0; i < n; i++) {
        ar[i].dst        = dst;
        ar[i].event      = ev;
        ar[i].timeout_ms = timeout;
        ar[i].index      = i;
        if (pthread_create(&th[i], NULL, many_thread, &ar[i]) != 0) {
            break;
        }
        made++;
    }
    for (i = 0; i < made; i++) {
        pthread_join(th[i], NULL);
    }
    /* The per-thread SENDMANY lines carry the verdict; this line only proves
     * the whole batch finished, so the test can stop waiting on a boundary
     * instead of on a timeout. */
    J("SENDMANY_DONE started=%d of %d", made, n);
    free(th);
    free(ar);
}

/* Fire N datagrams as fast as possible and report how the send path behaved.
 * The expected congestion contract is: IPC_OK until the peer's queue fills,
 * then IPC_ERR_AGAIN, never a block and never a silent drop. */
static void cmd_blast(const char *rest)
{
    char        ntok[32], dst[64], evtok[32];
    const char *p = rest;
    long        n;
    uint32_t    ev;
    const char *payload;
    long        ok = 0, again = 0, offline = 0, other = 0, i;
    int         first_bad = 0;
    size_t      plen;

    if (!next_token(&p, ntok, sizeof(ntok)) ||
        !next_token(&p, dst, sizeof(dst)) ||
        !next_token(&p, evtok, sizeof(evtok))) {
        J("CMD blast rc=%d bad_args", IPC_ERR_INVAL);
        return;
    }
    n   = strtol(ntok, NULL, 10);
    ev  = (uint32_t)strtoul(evtok, NULL, 10);
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    payload = p;
    plen    = strlen(payload);

    for (i = 0; i < n; i++) {
        int rc = ipc_post(g_ctx, dst, ev, payload, plen);

        if (rc == IPC_OK) {
            ok++;
        } else if (rc == IPC_ERR_AGAIN) {
            again++;
        } else if (rc == IPC_ERR_OFFLINE || rc == IPC_ERR_NOENT) {
            offline++;
        } else {
            other++;
            if (first_bad == 0) {
                first_bad = rc;
            }
        }
    }
    J("BLAST n=%ld ok=%ld again=%ld offline=%ld other=%ld first_bad=%d",
      n, ok, again, offline, other, first_bad);
}

/* Post N datagrams with retry-on-AGAIN until all N have been accepted.
 *
 * blast() measures the no-retry behaviour; soak() is for the leak and memory
 * tests, which need a *known* number of messages actually delivered rather than
 * "as many as happened to fit".  It never gives up: the watchdog is the test's
 * timeout, not a counter. */
static void cmd_soak(const char *rest)
{
    char        ntok[32], dst[64], evtok[32];
    const char *p = rest;
    long        n, sent = 0, again = 0, other = 0;
    uint32_t    ev;
    const char *payload;
    size_t      plen;

    if (!next_token(&p, ntok, sizeof(ntok)) ||
        !next_token(&p, dst, sizeof(dst)) ||
        !next_token(&p, evtok, sizeof(evtok))) {
        J("CMD soak rc=%d bad_args", IPC_ERR_INVAL);
        return;
    }
    n   = strtol(ntok, NULL, 10);
    ev  = (uint32_t)strtoul(evtok, NULL, 10);
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    payload = p;
    plen    = strlen(payload);

    while (sent < n) {
        int rc = ipc_post(g_ctx, dst, ev, payload, plen);

        if (rc == IPC_OK) {
            sent++;
        } else if (rc == IPC_ERR_AGAIN) {
            again++;
            sched_yield();
        } else {
            other++;
            J("SOAK aborted sent=%ld again=%ld rc=%d", sent, again, rc);
            return;
        }
    }
    J("SOAK n=%ld sent=%ld again=%ld other=%ld", n, sent, again, other);
}

static int handle_command(char *line)
{
    char        cmd[64];
    const char *rest;

    /* line is NUL terminated; cmd is the first token, rest the remainder. */
    {
        const char *p = line;
        if (!next_token(&p, cmd, sizeof(cmd))) {
            return CMD_CONTINUE;
        }
        rest = p;
    }

    if (strcmp(cmd, "note") == 0) {
        while (*rest == ' ' || *rest == '\t') {
            rest++;
        }
        J("NOTE %s", rest);
        return CMD_CONTINUE;
    }
    if (strcmp(cmd, "behave") == 0) {
        cmd_behave(rest);
        return CMD_CONTINUE;
    }
    if (strcmp(cmd, "post") == 0) {
        cmd_post(rest);
        return CMD_CONTINUE;
    }
    if (strcmp(cmd, "bcast") == 0) {
        cmd_bcast(rest);
        return CMD_CONTINUE;
    }
    if (strcmp(cmd, "send") == 0) {
        cmd_send(rest, 0);
        return CMD_CONTINUE;
    }
    if (strcmp(cmd, "sendt") == 0) {
        cmd_send(rest, 1);
        return CMD_CONTINUE;
    }
    if (strcmp(cmd, "sendmany") == 0) {
        cmd_sendmany(rest);
        return CMD_CONTINUE;
    }
    if (strcmp(cmd, "blast") == 0) {
        cmd_blast(rest);
        return CMD_CONTINUE;
    }
    if (strcmp(cmd, "soak") == 0) {
        cmd_soak(rest);
        return CMD_CONTINUE;
    }
    if (strcmp(cmd, "stats") == 0) {
        journal_stats();
        return CMD_CONTINUE;
    }
    if (strcmp(cmd, "del") == 0) {
        char     evtok[32];
        uint32_t ev;
        int      i;

        if (!next_token(&rest, evtok, sizeof(evtok))) {
            return CMD_CONTINUE;
        }
        ev = (uint32_t)strtoul(evtok, NULL, 10);
        for (i = 0; i < g_nbeh; i++) {
            if (g_beh[i].event == ev) {
                g_beh[i] = g_beh[g_nbeh - 1];
                g_nbeh--;
                break;
            }
        }
        J("CMD del event=%u", ev);
        return CMD_CONTINUE;
    }
    if (strcmp(cmd, "re-register") == 0) {
        ipc_register_opts_t o      = IPC_REGISTER_OPTS_INIT;
        ipc_ctx_t          *second = NULL;
        int                 rc;

        o.module    = ipc_module_id(g_ctx);
        o.ns        = ipc_namespace(g_ctx);
        o.conf_path = getenv("IPC_CONF");
        rc          = ipc_register(&o, &second);
        J("REREGISTER rc=%d", rc);
        if (rc == IPC_OK) {
            J("REREGISTER unexpected_success");
            ipc_unregister(second);
        }
        return CMD_CONTINUE;
    }
    if (strcmp(cmd, "close-fifo") == 0) {
        J("CMD close_fifo");
        return CMD_CLOSE_FIFO;
    }
    if (strcmp(cmd, "crash") == 0) {
        J("CMD crash pid=%ld", (long)getpid());
        fflush(g_jf);
        _exit(99);
    }
    if (strcmp(cmd, "exit") == 0) {
        J("CMD exit pid=%ld", (long)getpid());
        fflush(g_jf);
        _exit(0);
    }
    if (strcmp(cmd, "stop") == 0) {
        J("CMD stop");
        g_stop = 1;
        return CMD_STOP;
    }
    J("CMD unknown=%s", cmd);
    return CMD_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static void *run_thread(void *arg)
{
    ipc_run((ipc_ctx_t *)arg);
    return NULL;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s --module ID --conf FILE [options]\n"
            "  --ns NAME                namespace (default: the only match)\n"
            "  --no-ns                  leave the namespace unset (ambiguity test)\n"
            "  --group NAME             shared group for the socket\n"
            "  --dispatch inline|pool   default inline\n"
            "  --workers N              pool workers, default 2\n"
            "  --queue N                callback queue depth, default 64\n"
            "  --pending N              concurrent sync requests, default 64\n"
            "  --max-payload N          payload cap, default 8192\n"
            "  --include-self           include self in broadcasts\n"
            "  --allow-uid-split        relax the real/effective UID check\n"
            "  --no-loop                register but never receive\n"
            "  --sndbuf N --rcvbuf N    socket buffer overrides\n"
            "Env: IPC_JOURNAL (journal path), IPC_FIFO (command fifo),\n"
            "     IPC_CONF (config path, used by the re-register command).\n",
            argv0);
}

int main(int argc, char **argv)
{
    ipc_register_opts_t o = IPC_REGISTER_OPTS_INIT;
    const char         *journal;
    const char         *fifo;
    pthread_t           rt;
    char                line[MAX_CMD];
    size_t              used    = 0;
    int                 no_loop = 0;
    int                 exit_after_register = 0;
    int                 started = 0;
    int                 i, rc;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        int         takes_value = 1;

        if (strcmp(a, "--module") == 0 && v) {
            o.module = v;
        } else if (strcmp(a, "--ns") == 0 && v) {
            o.ns = v;
        } else if (strcmp(a, "--no-ns") == 0) {
            /* Leave opts.ns NULL on purpose: the library must then refuse an
             * ambiguous module id instead of picking a namespace. */
            o.ns        = NULL;
            takes_value = 0;
        } else if (strcmp(a, "--conf") == 0 && v) {
            o.conf_path = v;
        } else if (strcmp(a, "--group") == 0 && v) {
            o.group = v;
        } else if (strcmp(a, "--dispatch") == 0 && v) {
            o.dispatch = (strcmp(v, "pool") == 0) ? IPC_DISPATCH_POOL
                                                  : IPC_DISPATCH_INLINE;
        } else if (strcmp(a, "--workers") == 0 && v) {
            o.workers = atoi(v);
        } else if (strcmp(a, "--queue") == 0 && v) {
            o.cb_queue_max = atoi(v);
        } else if (strcmp(a, "--pending") == 0 && v) {
            o.max_pending = atoi(v);
        } else if (strcmp(a, "--max-payload") == 0 && v) {
            o.max_payload = (uint32_t)strtoul(v, NULL, 10);
        } else if (strcmp(a, "--sndbuf") == 0 && v) {
            o.sndbuf = atoi(v);
        } else if (strcmp(a, "--rcvbuf") == 0 && v) {
            o.rcvbuf = atoi(v);
        } else if (strcmp(a, "--include-self") == 0) {
            o.broadcast_include_self = 1;
            takes_value              = 0;
        } else if (strcmp(a, "--allow-uid-split") == 0) {
            o.allow_uid_split = 1;
            takes_value       = 0;
        } else if (strcmp(a, "--no-loop") == 0) {
            no_loop     = 1;
            takes_value = 0;
        } else if (strcmp(a, "--exit-after-register") == 0) {
            /* Register, journal the outcome, then _exit() without any cleanup.
             * Used for registration-race and lifecycle-churn tests, where the
             * point is exactly that no graceful teardown happens. */
            exit_after_register = 1;
            takes_value         = 0;
        } else if (strcmp(a, "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "%s: unknown option %s\n", argv[0], a);
            usage(argv[0]);
            return 2;
        }
        i += takes_value;
    }

    journal = getenv("IPC_JOURNAL");
    fifo    = getenv("IPC_FIFO");
    if (o.module == NULL || journal == NULL) {
        usage(argv[0]);
        return 2;
    }
    g_jf = fopen(journal, "a");
    if (g_jf == NULL) {
        fprintf(stderr, "cannot open journal %s: %s\n", journal,
                strerror(errno));
        return 2;
    }
    setvbuf(g_jf, NULL, _IOLBF, 0);

    rc = ipc_register(&o, &g_ctx);
    J("BOOT module=%s rc=%d uid=%ld pid=%ld dispatch=%s instance=0x%016llx",
      o.module, rc, (long)getuid(), (long)getpid(),
      o.dispatch == IPC_DISPATCH_POOL ? "pool" : "inline",
      (unsigned long long)(rc == IPC_OK ? ipc_instance_id(g_ctx) : 0));
    if (rc != IPC_OK) {
        J("BOOT_FAIL err=%s", ipc_strerror(rc));
        fclose(g_jf);
        return 1;
    }
    J("SOCK path=%s", ipc_socket_path(g_ctx));

    if (exit_after_register) {
        J("EXIT after_register rc=0");
        fflush(g_jf);
        _exit(0);
    }

    ipc_set_handler(g_ctx, handle, g_ctx);

    if (no_loop) {
        J("LOOP disabled");
    } else {
        if (pthread_create(&rt, NULL, run_thread, g_ctx) != 0) {
            J("LOOP rc=-1 pthread_create_failed");
            return 1;
        }
        started = 1;
        J("LOOP started");
    }

    if (fifo != NULL) {
        /* O_RDWR on a FIFO keeps a writer end open, so read() never reports a
         * spurious EOF between test steps. */
        int fd      = open(fifo, O_RDWR | O_NONBLOCK);
        int fifo_gone = 0;

        if (fd < 0) {
            J("FIFO rc=-1 errno=%d", errno);
            return 1;
        }
        while (!g_stop && !fifo_gone) {
            struct pollfd pfd;
            ssize_t       n;

            pfd.fd     = fd;
            pfd.events = POLLIN;
            if (poll(&pfd, 1, 200) <= 0) {
                continue;
            }
            n = read(fd, line + used, sizeof(line) - used - 1);
            if (n <= 0) {
                continue; /* EAGAIN, EINTR, or writer not attached yet */
            }
            used += (size_t)n;
            line[used] = '\0';
            for (;;) {
                char  *nl = strchr(line, '\n');
                size_t keep;
                int    crc;

                if (nl == NULL) {
                    break;
                }
                *nl = '\0';
                crc = handle_command(line);
                if (crc == CMD_STOP) {
                    break;
                }
                if (crc == CMD_CLOSE_FIFO) {
                    fifo_gone = 1;
                    break;
                }
                keep = used - (size_t)(nl + 1 - line);
                memmove(line, nl + 1, keep);
                used       = keep;
                line[used] = '\0';
            }
            if (used >= sizeof(line) - 1) {
                J("FIFO line_too_long discarding");
                used = 0;
            }
        }
        close(fd);
        if (fifo_gone && !g_stop) {
            /* The command channel is intentionally released: the module keeps
             * its socket and keeps serving peers, and is only stopped by a
             * signal from the test.  That is the point of the test that uses
             * this command. */
            J("FIFO closed_keeps_serving");
            while (!g_stop) {
                sleep_ms(50);
            }
        }
    } else {
        J("FIFO not configured");
    }

    J("STOPPING");
    ipc_stop(g_ctx);
    if (started) {
        pthread_join(rt, NULL);
    }
    ipc_unregister(g_ctx);
    ipc_ctx_free(g_ctx);
    J("EXIT");
    fclose(g_jf);
    return 0;
}
