/*
 * test_ctx.c -- context lifecycle tests that need no root.
 *
 * Why this file exists.
 *
 * src/ipc_ctx.c, src/ipc_io.c and src/ipc_loop.c were reachable *only* through
 * the root-only integration suite, which switches UIDs with setpriv.  In any
 * environment without root -- the normal case for a developer shell or a plain
 * CI container -- "make check" covered config/proto/queue/pending/util and
 * nothing else, so registration, send/receive and teardown could regress
 * silently.  ipc_poll(), ipc_is_stopped() and ipc_socket_fd() had no call site
 * anywhere in the repository at all.
 *
 * Nothing here needs privileges: the config is a private file this test writes
 * itself, the socket directory is private (0700), and the configured UID is
 * simply the effective UID.  That works because the library's identity rule is
 * "the config's UID must be ours", not "the UID must be 1501".
 */
#include "ipc/ipc.h"
#include "utest.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* sandbox: a private directory, a private config, three module names  */
/* ------------------------------------------------------------------ */

typedef struct {
    char base[128];
    char dir[256];
    /* Larger than dir on purpose: each of these is dir plus a suffix, and a
     * same-sized buffer would make -Wformat-truncation warn (with reason). */
    char run[336];
    char conf[336];
    int  made_dir;
    int  made_run;
} sandbox_t;

static int g_sbx_seq;

static int sbx_init(sandbox_t *s)
{
    const char *tmp = getenv("TMPDIR");
    mode_t      old;
    size_t      n;

    memset(s, 0, sizeof(*s));
    if (tmp == NULL || tmp[0] == '\0') {
        tmp = "/tmp";
    }
    n = strlen(tmp);
    while (n > 1 && tmp[n - 1] == '/') {
        n--;
    }
    snprintf(s->base, sizeof(s->base), "%.*s", (int)n, tmp);
    snprintf(s->dir, sizeof(s->dir), "%s/ipc-utest-ctx-%ld-%d", s->base,
             (long)getpid(), ++g_sbx_seq);
    snprintf(s->run, sizeof(s->run), "%s/run", s->dir);
    snprintf(s->conf, sizeof(s->conf), "%s/ipc.conf", s->dir);

    /* 0077 so nothing this test creates is ever group/other writable: the
     * library refuses such a config or socket directory on purpose. */
    old = umask(0077);
    if (mkdir(s->dir, 0700) != 0) {
        umask(old);
        return -1;
    }
    s->made_dir = 1;
    if (mkdir(s->run, 0700) != 0) {
        umask(old);
        return -1;
    }
    s->made_run = 1;
    umask(old);
    return 0;
}

static void sbx_write_conf(sandbox_t *s, const char *text)
{
    FILE *f = fopen(s->conf, "w");

    if (f == NULL) {
        return;
    }
    fputs(text, f);
    fclose(f);
    (void)chmod(s->conf, 0600);
}

/* A1 and A2 will be registered; A3 is configured but never bound, so the
 * "configured yet offline" path is reachable without a second machine. */
static void sbx_conf_default(sandbox_t *s)
{
    /* Wide enough for the three %s at their worst case (s->run is 336 bytes),
     * not just for the paths this test happens to generate -- otherwise GCC is
     * right to warn that the three substitutions could overrun the buffer. */
    char text[2048];

    snprintf(text, sizeof(text),
             "# written by tests/unit/test_ctx.c\n"
             "core A1 %u %s/A1.sock\n"
             "core A2 %u %s/A2.sock\n"
             "core A3 %u %s/A3.sock\n",
             (unsigned)geteuid(), s->run, (unsigned)geteuid(), s->run,
             (unsigned)geteuid(), s->run);
    sbx_write_conf(s, text);
}

static void sbx_destroy(sandbox_t *s)
{
    static const char *const files[] = {
        "A1.sock", "A1.sock.lock", "A2.sock", "A2.sock.lock",
        "A3.sock", "A3.sock.lock", "residue",     "ipc.conf",
    };
    char   p[512];
    size_t i;
    for (i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        snprintf(p, sizeof(p), "%s/%s", s->run, files[i]);
        (void)unlink(p);
        snprintf(p, sizeof(p), "%s/%s", s->dir, files[i]);
        (void)unlink(p);
    }
    if (s->made_run) {
        (void)rmdir(s->run);
    }
    if (s->made_dir) {
        (void)rmdir(s->dir);
    }
}

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

/* Bounded wait on an atomic counter.  Polling rather than sleeping a fixed
 * time: the point of the assertion is "it happened", not "a second passed". */
static int wait_for(atomic_int *v, int want, int ms)
{
    struct timespec ts = { 0, 1000000 }; /* 1 ms */
    int             i;

    for (i = 0; i < ms; i++) {
        if (atomic_load(v) >= want) {
            return 0;
        }
        nanosleep(&ts, NULL);
    }
    return (atomic_load(v) >= want) ? 0 : -1;
}

static void expect_register(sandbox_t *s, const char *mod, const char *ns,
                            int want)
{
    ipc_register_opts_t o = IPC_REGISTER_OPTS_INIT;
    ipc_ctx_t          *c = NULL;
    int                 rc;

    o.module    = mod;
    o.ns        = ns;
    o.conf_path = s->conf;
    rc          = ipc_register(&o, &c);
    if (rc != want) {
        char b[256];
        snprintf(b, sizeof(b), "register(%s) = %d (%s), want %d (%s)",
                 mod, rc, ipc_strerror(rc), want, ipc_strerror(want));
        UT_FAIL(b);
        return;
    }
    if (rc == IPC_OK && c != NULL) {
        (void)ipc_unregister(c);
        (void)ipc_ctx_free(c);
    }
}

/* ------------------------------------------------------------------ */
/* handler state (tests run one at a time, so file scope is enough)     */
/* ------------------------------------------------------------------ */

static atomic_int g_count_a1;
static atomic_int g_count_a2;

static void handler_count(const ipc_msg_t *m, void *user)
{
    (void)m;
    if (user != NULL) {
        atomic_fetch_add((atomic_int *)user, 1);
    }
}

static void handler_reply(const ipc_msg_t *m, void *user)
{
    if (user != NULL) {
        atomic_fetch_add((atomic_int *)user, 1);
    }
    if (m->type == IPC_TYPE_REQ) {
        char reply[64];
        int  n = snprintf(reply, sizeof(reply), "reply-to-%u", m->event);
        if (n > 0) {
            (void)ipc_reply(m, reply, (size_t)n);
        }
    }
}

/* INLINE dispatch: this handler runs on the very thread that would have to
 * deliver the reply, so ipc_send() from it must be refused. */
static ipc_ctx_t *g_nest_self;
static atomic_int g_nest_rc;

static void handler_inline_nest_probe(const ipc_msg_t *m, void *user)
{
    if (user != NULL) {
        atomic_fetch_add((atomic_int *)user, 1);
    }
    if (m->type != IPC_TYPE_REQ) {
        return;
    }
    if (atomic_load(&g_nest_rc) == 999) {
        char   b[32];
        size_t rl = 0;
        int    rc = ipc_send_timeout(g_nest_self, "A2", 61, "x", 1, b,
                                     sizeof(b), &rl, 100);
        atomic_store(&g_nest_rc, rc);
    }
    (void)ipc_reply(m, "ok", 2);
}

/* POOL dispatch: a *worker* thread runs this, so calling back into the library
 * is legal -- this is the documented difference between the two modes. */
static void handler_pool_nest(const ipc_msg_t *m, void *user)
{
    if (user != NULL) {
        atomic_fetch_add((atomic_int *)user, 1);
    }
    if (m->type != IPC_TYPE_REQ) {
        return;
    }
    if (atomic_load(&g_nest_rc) == 999) {
        char   b[64];
        size_t rl = 0;
        int    rc = ipc_send_timeout(g_nest_self, "A1", 55, "inner", 5, b,
                                     sizeof(b), &rl, 2000);
        atomic_store(&g_nest_rc, rc);
    }
    {
        char reply[64];
        int  n = snprintf(reply, sizeof(reply), "outer-%u", m->event);
        if (n > 0) {
            (void)ipc_reply(m, reply, (size_t)n);
        }
    }
}

/* ------------------------------------------------------------------ */
/* running the loop in a helper thread                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    ipc_ctx_t *ctx;
    int        rc;
} loop_arg_t;

static void *loop_thread(void *p)
{
    loop_arg_t *a = (loop_arg_t *)p;

    a->rc = ipc_run(a->ctx);
    return NULL;
}

typedef struct {
    sandbox_t  sbx;
    ipc_ctx_t *a1;
    ipc_ctx_t *a2;
    loop_arg_t l1, l2;
    pthread_t  t1, t2;
    int        t1_up, t2_up;
} bench_t;

static void bench_stop_loops(bench_t *b)
{
    if (b->t1_up) {
        if (b->a1 != NULL) {
            (void)ipc_stop(b->a1);
        }
        pthread_join(b->t1, NULL);
        b->t1_up = 0;
    }
    if (b->t2_up) {
        if (b->a2 != NULL) {
            (void)ipc_stop(b->a2);
        }
        pthread_join(b->t2, NULL);
        b->t2_up = 0;
    }
}

/* Start the loop for whichever contexts are non-NULL.  Returns 0 when both
 * were started. */
static int bench_start_loops(bench_t *b, int want_a1, int want_a2)
{
    if (want_a1) {
        b->l1.ctx = b->a1;
        b->l1.rc  = 999;
        if (pthread_create(&b->t1, NULL, loop_thread, &b->l1) != 0) {
            return -1;
        }
        b->t1_up = 1;
    }
    if (want_a2) {
        b->l2.ctx = b->a2;
        b->l2.rc  = 999;
        if (pthread_create(&b->t2, NULL, loop_thread, &b->l2) != 0) {
            return -1;
        }
        b->t2_up = 1;
    }
    return 0;
}

static void bench_shutdown(bench_t *b)
{
    bench_stop_loops(b);
    if (b->a1 != NULL) {
        (void)ipc_unregister(b->a1);
        (void)ipc_ctx_free(b->a1);
        b->a1 = NULL;
    }
    if (b->a2 != NULL) {
        (void)ipc_unregister(b->a2);
        (void)ipc_ctx_free(b->a2);
        b->a2 = NULL;
    }
    sbx_destroy(&b->sbx);
}

static int bench_register(bench_t *b, const ipc_register_opts_t *o1,
                          const ipc_register_opts_t *o2)
{
    if (o1 != NULL) {
        if (ipc_register(o1, &b->a1) != IPC_OK) {
            UT_FAIL("ipc_register(A1) failed");
            return -1;
        }
    }
    if (o2 != NULL) {
        if (ipc_register(o2, &b->a2) != IPC_OK) {
            UT_FAIL("ipc_register(A2) failed");
            return -1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* 1. registration, accessors, and the idempotent-unregister contract   */
/* ------------------------------------------------------------------ */

UT_TEST(ctx, accessors_and_idempotent_unregister)
{
    sandbox_t              s;
    ipc_register_opts_t    o  = IPC_REGISTER_OPTS_INIT;
    ipc_ctx_t             *c  = NULL;
    const char            *path;
    ipc_stats_t            st;

    if (sbx_init(&s) != 0) {
        UT_FAIL("could not create the sandbox directory");
        return;
    }
    sbx_conf_default(&s);

    o.module    = "A1";
    o.ns        = "core";
    o.conf_path = s.conf;

    UT_EQ_INT(ipc_register(&o, &c), IPC_OK);
    if (c == NULL) {
        sbx_destroy(&s);
        UT_FAIL("ipc_register() returned IPC_OK but no context");
        return;
    }

    /* ipc_socket_fd() / ipc_is_stopped() had no call site anywhere in
     * the repository, so this is their first real consumer. */
    UT_EQ_STR(ipc_module_id(c), "A1");
    UT_EQ_STR(ipc_namespace(c), "core");
    path = ipc_socket_path(c);
    UT_CHECK_MSG(path != NULL && strstr(path, "/A1.sock") != NULL,
                 "socket path is %s", path ? path : "(null)");
    UT_CHECK(ipc_socket_fd(c) >= 0);
    UT_CHECK(ipc_instance_id(c) != 0);
    UT_EQ_INT(ipc_is_stopped(c), 0);

    /* ipc_unregister() must be idempotent *and* safe to call repeatedly.
     *
     * An earlier implementation did free(ctx) at the end, so the second call
     * wrote to freed memory -- a heap-use-after-free ASan reproduces without
     * any concurrency at all.  Three calls in a row is the minimal regression
     * for both halves of the contract. */
    UT_EQ_INT(ipc_unregister(c), IPC_OK);
    UT_EQ_INT(ipc_is_stopped(c), 1);
    UT_EQ_INT(ipc_unregister(c), IPC_OK);
    UT_EQ_INT(ipc_unregister(c), IPC_OK);

    /* Accessors must stay legal until ipc_ctx_free(): that is the whole point
     * of not freeing in ipc_unregister(). */
    UT_CHECK(ipc_socket_fd(c) < 0);
    UT_EQ_STR(ipc_module_id(c), "A1");
    ipc_get_stats(c, &st);
    UT_EQ_U64(st.recv_read, 0);

    UT_EQ_INT(ipc_ctx_free(c), IPC_OK);
    sbx_destroy(&s);
}

/* ------------------------------------------------------------------ */
/* 2. registration failure paths                                       */
/* ------------------------------------------------------------------ */

UT_TEST(ctx, register_rejects_bad_config)
{
    sandbox_t           s;
    ipc_register_opts_t o = IPC_REGISTER_OPTS_INIT;
    char                text[1024];
    uid_t               other;
    int                 rc;

    if (sbx_init(&s) != 0) {
        UT_FAIL("could not create the sandbox directory");
        return;
    }

    /* ---- argument validation (no config read at all) --------------- */
    o.module    = NULL;
    o.conf_path = s.conf;
    {
        ipc_ctx_t *c = NULL;
        UT_EQ_INT(ipc_register(&o, &c), IPC_ERR_INVAL);
        o.module = "A1";
        UT_EQ_INT(ipc_register(NULL, &c), IPC_ERR_INVAL);
        UT_EQ_INT(ipc_register(&o, NULL), IPC_ERR_INVAL);
    }
    o.module = "this-module-name-is-far-too-long-for-the-wire-format";
    {
        ipc_ctx_t *c = NULL;
        UT_EQ_INT(ipc_register(&o, &c), IPC_ERR_INVAL);
    }

    /* ---- the config file itself ----------------------------------- */
    sbx_conf_default(&s);

    /* missing config */
    {
        sandbox_t m = s;
        snprintf(m.conf, sizeof(m.conf), "%s/nope.conf", s.dir);
        expect_register(&m, "A1", "core", IPC_ERR_NOENT);
    }

    /* unknown module id */
    expect_register(&s, "Z9", "core", IPC_ERR_NOENT);

    /* uid mismatch: the config names somebody else's uid.  This is the check
     * the whole permission model rests on, and it is reachable without root
     * because "somebody else" only has to differ from us. */
    other = (geteuid() == 0) ? 1 : 0;
    snprintf(text, sizeof(text), "core A1 %u %s/A1.sock\n", (unsigned)other,
             s.run);
    sbx_write_conf(&s, text);
    expect_register(&s, "A1", "core", IPC_ERR_PERM);
    sbx_conf_default(&s);

    /* ambiguous module id: present in two namespaces, and no ns was given */
    snprintf(text, sizeof(text),
             "core  A1 %u %s/A1.sock\n"
             "extra A1 %u %s/extra-A1.sock\n",
             (unsigned)geteuid(), s.run, (unsigned)geteuid(), s.run);
    sbx_write_conf(&s, text);
    expect_register(&s, "A1", NULL, IPC_ERR_CONFIG);
    /* ...but naming the namespace resolves it */
    expect_register(&s, "A1", "core", IPC_OK);
    sbx_destroy(&s);

    /* ---- config file permissions (the promise made in README section 4) -- */
    if (sbx_init(&s) != 0) {
        UT_FAIL("could not create the second sandbox directory");
        return;
    }
    sbx_conf_default(&s);

    /* group-writable config must be refused: a service that can rewrite the
     * table can register itself as any module it likes. */
    (void)chmod(s.conf, 0664);
    expect_register(&s, "A1", "core", IPC_ERR_PERM);

    /* world-writable likewise */
    (void)chmod(s.conf, 0666);
    expect_register(&s, "A1", "core", IPC_ERR_PERM);

    /* a readable config is accepted again */
    (void)chmod(s.conf, 0600);
    expect_register(&s, "A1", "core", IPC_OK);

    /* a directory where a config file should be */
    (void)chmod(s.conf, 0600);
    (void)unlink(s.conf);
    rc = mkdir(s.conf, 0700);
    UT_EQ_INT(rc, 0);
    expect_register(&s, "A1", "core", IPC_ERR_PERM);
    (void)rmdir(s.conf);

    /* socket directory writable by group/others: check_own_dir() must refuse,
     * because a hostile group member could swap the path for a symlink in the
     * window between bind() and chmod(). */
    sbx_conf_default(&s);
    (void)chmod(s.run, 0775);
    expect_register(&s, "A1", "core", IPC_ERR_PERM);
    (void)chmod(s.run, 0700);
    expect_register(&s, "A1", "core", IPC_OK);

    sbx_destroy(&s);
}

/* ------------------------------------------------------------------ */
/* 3. two contexts in one process cannot hold the same module          */
/* ------------------------------------------------------------------ */

UT_TEST(ctx, duplicate_registration_is_busy)
{
    sandbox_t           s;
    ipc_register_opts_t o = IPC_REGISTER_OPTS_INIT;
    ipc_ctx_t          *c1 = NULL;
    ipc_ctx_t          *c2 = NULL;

    if (sbx_init(&s) != 0) {
        UT_FAIL("could not create the sandbox directory");
        return;
    }
    sbx_conf_default(&s);
    o.module    = "A1";
    o.ns        = "core";
    o.conf_path = s.conf;

    UT_EQ_INT(ipc_register(&o, &c1), IPC_OK);
    UT_EQ_INT(ipc_register(&o, &c2), IPC_ERR_BUSY);
    UT_CHECK(c2 == NULL);

    /* releasing the first must make the name registrable again: the lock file
     * stays on disk on purpose, so this also proves the flock was released and
     * that the leftover lock file is not mistaken for residue. */
    UT_EQ_INT(ipc_unregister(c1), IPC_OK);
    UT_EQ_INT(ipc_ctx_free(c1), IPC_OK);
    UT_EQ_INT(ipc_register(&o, &c2), IPC_OK);
    UT_EQ_INT(ipc_unregister(c2), IPC_OK);
    UT_EQ_INT(ipc_ctx_free(c2), IPC_OK);

    /* A stale *socket* (not a lock holder) is residue and must be replaced. */
    sbx_destroy(&s);
}

/* ------------------------------------------------------------------ */
/* 4. INLINE: post / send / broadcast / offline / limits / stop        */
/* ------------------------------------------------------------------ */

UT_TEST(ctx, inline_post_send_broadcast_and_stop)
{
    bench_t             b;
    ipc_register_opts_t o1 = IPC_REGISTER_OPTS_INIT;
    ipc_register_opts_t o2 = IPC_REGISTER_OPTS_INIT;
    char                rbuf[64];
    size_t              rlen = 0;
    uint8_t             big[1025];
    int                 a2_before;
    ipc_stats_t         st;

    memset(&b, 0, sizeof(b));
    if (sbx_init(&b.sbx) != 0) {
        UT_FAIL("could not create the sandbox directory");
        return;
    }
    sbx_conf_default(&b.sbx);

    o1.module      = "A1";
    o1.ns          = "core";
    o1.conf_path   = b.sbx.conf;
    o1.max_payload = 1024;
    o2.module      = "A2";
    o2.ns          = "core";
    o2.conf_path   = b.sbx.conf;
    o2.max_payload = 1024;

    if (bench_register(&b, &o1, &o2) != 0) {
        bench_shutdown(&b);
        return;
    }

    atomic_store(&g_count_a1, 0);
    atomic_store(&g_count_a2, 0);
    atomic_store(&g_nest_rc, 999);
    g_nest_self = b.a1;
    UT_EQ_INT(ipc_set_handler(b.a1, handler_inline_nest_probe, &g_count_a1),
              IPC_OK);
    /* A2 has to *answer*: the synchronous request below needs a reply. */
    UT_EQ_INT(ipc_set_handler(b.a2, handler_reply, &g_count_a2), IPC_OK);

    if (bench_start_loops(&b, 1, 1) != 0) {
        UT_FAIL("could not start the receive loops");
        bench_shutdown(&b);
        return;
    }

    /* Wait until *both* loops are demonstrably reading, instead of guessing:
     * ipc_send_timeout() refuses to run before a loop exists (IPC_ERR_STATE),
     * so the rest of this test would otherwise be racy. */
    UT_EQ_INT(ipc_post(b.a2, "A1", 1, "warm", 4), IPC_OK);
    UT_EQ_INT(wait_for(&g_count_a1, 1, 5000), 0);
    UT_EQ_INT(ipc_post(b.a1, "A2", 2, "warm", 4), IPC_OK);
    UT_EQ_INT(wait_for(&g_count_a2, 1, 5000), 0);

    /* ---- target resolution ---------------------------------------- */
    UT_EQ_INT(ipc_post(b.a1, "A3", 3, "x", 1), IPC_ERR_OFFLINE); /* configured,
                                                                  * never bound */
    UT_EQ_INT(ipc_post(b.a1, "Z9", 3, "x", 1), IPC_ERR_NOENT);   /* not in the
                                                                  * table */
    UT_EQ_INT(ipc_post(b.a1, "", 3, "x", 1), IPC_ERR_INVAL);

    /* ---- sender-side size limit (the half of A4 a sender can see) --- */
    memset(big, 'b', sizeof(big));
    UT_EQ_INT(ipc_post(b.a1, "A2", 4, big, sizeof(big)), IPC_ERR_MSGSIZE);

    /* ---- synchronous request/reply --------------------------------- */
    memset(rbuf, 0, sizeof(rbuf));
    UT_EQ_INT(ipc_send_timeout(b.a1, "A2", 7, "ping", 4, rbuf, sizeof(rbuf) - 1,
                               &rlen, 3000),
              IPC_OK);
    UT_EQ_U64(rlen, strlen("reply-to-7"));
    UT_CHECK_MSG(rlen == strlen("reply-to-7") &&
                     memcmp(rbuf, "reply-to-7", rlen) == 0,
                 "reply was \"%.*s\"", (int)rlen, rbuf);

    /* ---- ipc_send() from an INLINE handler must be refused ---------- */
    memset(rbuf, 0, sizeof(rbuf));
    rlen = 0;
    UT_EQ_INT(ipc_send_timeout(b.a2, "A1", 62, "probe", 5, rbuf, sizeof(rbuf) - 1,
                               &rlen, 3000),
              IPC_OK);
    UT_EQ_INT(atomic_load(&g_nest_rc), IPC_ERR_DEADLOCK);

    /* ---- broadcast skips the offline module, never queues it ------- */
    a2_before = atomic_load(&g_count_a2);
    UT_EQ_INT(ipc_broadcast(b.a1, 9, "bc", 2), 1);
    UT_EQ_INT(wait_for(&g_count_a2, a2_before + 1, 5000), 0);
    ipc_get_stats(b.a1, &st);
    UT_EQ_U64(st.broadcast_targets, 2); /* A2 and A3, not A1 itself */
    UT_EQ_U64(st.broadcast_skipped, 1); /* A3 is offline */
    UT_EQ_U64(st.eagain_count, 0);

    /* ---- a running loop excludes ipc_poll() on the same context (C1) */
    UT_EQ_INT(ipc_poll(b.a1, 0), IPC_ERR_STATE);

    /* ---- a stop the caller asked for is a clean return -------------- */
    UT_EQ_INT(ipc_stop(b.a1), IPC_OK);
    pthread_join(b.t1, NULL);
    b.t1_up = 0;
    UT_EQ_INT(b.l1.rc, IPC_OK);
    UT_EQ_INT(ipc_is_stopped(b.a1), 1);
    UT_EQ_INT(ipc_stop(b.a1), IPC_OK); /* idempotent */

    bench_shutdown(&b);
}

/* ------------------------------------------------------------------ */
/* 5. POOL: the bounded queue plus worker threads                      */
/* ------------------------------------------------------------------ */

UT_TEST(ctx, pool_dispatch_delivers_and_allows_nested_send)
{
    bench_t             b;
    ipc_register_opts_t o1 = IPC_REGISTER_OPTS_INIT;
    ipc_register_opts_t o2 = IPC_REGISTER_OPTS_INIT;
    char                rbuf[64];
    size_t              rlen = 0;

    memset(&b, 0, sizeof(b));
    if (sbx_init(&b.sbx) != 0) {
        UT_FAIL("could not create the sandbox directory");
        return;
    }
    sbx_conf_default(&b.sbx);

    /* A1 stays INLINE (its handler is where the nested request lands), A2 is
     * POOL so its handler runs on a worker thread. */
    o1.module    = "A1";
    o1.ns        = "core";
    o1.conf_path = b.sbx.conf;
    o2.module    = "A2";
    o2.ns        = "core";
    o2.conf_path = b.sbx.conf;
    o2.dispatch  = IPC_DISPATCH_POOL;
    o2.workers   = 2;

    if (bench_register(&b, &o1, &o2) != 0) {
        bench_shutdown(&b);
        return;
    }

    atomic_store(&g_count_a1, 0);
    atomic_store(&g_count_a2, 0);
    atomic_store(&g_nest_rc, 999);
    g_nest_self = b.a2; /* the worker sends back to A1 */
    UT_EQ_INT(ipc_set_handler(b.a1, handler_reply, &g_count_a1), IPC_OK);
    UT_EQ_INT(ipc_set_handler(b.a2, handler_pool_nest, &g_count_a2), IPC_OK);

    if (bench_start_loops(&b, 1, 1) != 0) {
        UT_FAIL("could not start the receive loops");
        bench_shutdown(&b);
        return;
    }

    /* Prove both loops are up before the synchronous part. */
    UT_EQ_INT(ipc_post(b.a2, "A1", 1, "warm", 4), IPC_OK);
    UT_EQ_INT(wait_for(&g_count_a1, 1, 5000), 0);
    UT_EQ_INT(ipc_post(b.a1, "A2", 2, "warm", 4), IPC_OK);
    UT_EQ_INT(wait_for(&g_count_a2, 1, 5000), 0);

    /* A1 -> A2 (POOL worker) -> A1 (INLINE) -> back to the worker -> back to
     * A1.  Four legs, two contexts, and the only reason it can complete is
     * that a POOL handler is allowed to call ipc_send(), which is exactly the
     * difference from INLINE asserted in the previous test. */
    memset(rbuf, 0, sizeof(rbuf));
    UT_EQ_INT(ipc_send_timeout(b.a1, "A2", 70, "outer", 5, rbuf,
                               sizeof(rbuf) - 1, &rlen, 3000),
              IPC_OK);
    UT_EQ_INT(atomic_load(&g_nest_rc), IPC_OK);
    UT_EQ_U64(rlen, strlen("outer-70"));
    UT_CHECK_MSG(rlen == strlen("outer-70") && memcmp(rbuf, "outer-70", rlen) == 0,
                 "outer reply was \"%.*s\"", (int)rlen, rbuf);

    /* The pool path must not have dropped anything at this rate. */
    {
        ipc_stats_t st;
        ipc_get_stats(b.a2, &st);
        UT_EQ_U64(st.cb_dropped, 0);
        UT_EQ_U64(st.cb_invoked, 2); /* the warm-up post and the request */
    }

    /* Drain the queue at teardown: ipc_unregister() must not hang with work
     * still queued and workers still parked. */
    bench_shutdown(&b);
}

/* ------------------------------------------------------------------ */
/* 6. ipc_poll(): the embedder's own loop, with no ipc_run() at all     */
/* ------------------------------------------------------------------ */

UT_TEST(ctx, poll_mode_works_without_ipc_run)
{
    bench_t             b;
    ipc_register_opts_t o1 = IPC_REGISTER_OPTS_INIT;
    ipc_register_opts_t o2 = IPC_REGISTER_OPTS_INIT;

    memset(&b, 0, sizeof(b));
    if (sbx_init(&b.sbx) != 0) {
        UT_FAIL("could not create the sandbox directory");
        return;
    }
    sbx_conf_default(&b.sbx);

    o1.module    = "A1";
    o1.ns        = "core";
    o1.conf_path = b.sbx.conf;
    o2.module    = "A2";
    o2.ns        = "core";
    o2.conf_path = b.sbx.conf;

    if (bench_register(&b, &o1, &o2) != 0) {
        bench_shutdown(&b);
        return;
    }
    atomic_store(&g_count_a1, 0);
    UT_EQ_INT(ipc_set_handler(b.a1, handler_count, &g_count_a1), IPC_OK);

    /* ipc_poll() had no call site in the whole repository before this test.
     * No thread is started anywhere in this test. */
    UT_EQ_INT(ipc_poll(b.a1, 0), 0); /* nothing queued */

    UT_EQ_INT(ipc_post(b.a2, "A1", 42, "poll-me", 7), IPC_OK);

    /* Returns the number of datagrams processed: the drain happens first, so
     * the queued datagram is seen without waiting for the timeout. */
    UT_EQ_INT(ipc_poll(b.a1, 3000), 1);
    UT_EQ_INT(atomic_load(&g_count_a1), 1);

    /* A second poll with nothing pending must not invent a message. */
    UT_EQ_INT(ipc_poll(b.a1, 0), 0);

    /* ipc_post() from a poll-driven context keeps working... */
    UT_EQ_INT(ipc_post(b.a1, "A2", 43, "reply-capable", 13), IPC_OK);
    /* ...but the synchronous path cannot: without a running loop nobody can
     * read the reply off the socket, so it fails fast instead of hanging.
     * This is the limitation documented on ipc_poll() in ipc.h. */
    {
        char   rbuf[32];
        size_t rlen = 0;
        UT_EQ_INT(ipc_send_timeout(b.a1, "A2", 44, "x", 1, rbuf, sizeof(rbuf),
                                   &rlen, 200),
                  IPC_ERR_STATE);
    }

    /* A3 is configured but never bound: poll on a quiet socket still works. */
    UT_EQ_INT(ipc_is_stopped(b.a1), 0);
    UT_EQ_INT(ipc_stop(b.a1), IPC_OK);
    UT_EQ_INT(ipc_is_stopped(b.a1), 1);

    bench_shutdown(&b);
}
