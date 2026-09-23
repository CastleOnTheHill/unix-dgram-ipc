#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include "ipc_internal.h"

/* Control buffer: one SCM_CREDENTIALS.  Deliberately not larger; anything
 * else in the ancillary data is irrelevant to us and MSG_CTRUNC is treated as
 * a rejection. */
#define IPC_CTRL_SIZE CMSG_SPACE(sizeof(struct ucred))

/* ------------------------------------------------------------------ */
/* handing a validated message to the business layer                   */
/* ------------------------------------------------------------------ */

static void prepare_task(ipc_ctx_t *ctx, ipc_task_t *t)
{
    memset(&t->rctx, 0, sizeof(t->rctx));
    t->rctx.ctx           = ctx;
    t->rctx.req_id        = t->hdr.req_id;
    t->rctx.peer_instance = t->hdr.instance_id;
    ipc_strlcpy(t->rctx.dst, t->hdr.src, sizeof(t->rctx.dst));
}

static void build_msg(ipc_task_t *t, ipc_msg_t *m)
{
    memset(m, 0, sizeof(*m));
    m->ns          = t->hdr.ns;
    m->src         = t->hdr.src;
    m->dst         = t->hdr.dst;
    m->event       = t->hdr.event;
    m->type        = (ipc_msg_type_t)t->hdr.type;
    m->req_id      = t->hdr.req_id;
    m->instance_id = t->hdr.instance_id;
    m->data        = t->payload;
    m->len         = t->len;
    m->opaque      = &t->rctx;
}

/* Run the handler for one already-validated message.  Used by both dispatch
 * modes; the only difference is which thread calls it. */
static void invoke_handler(ipc_ctx_t *ctx, ipc_task_t *t)
{
    ipc_msg_t        m;
    ipc_ctx_t       *prev;

    if (ctx->handler == NULL) {
        /* No consumer installed: count it as delivered-but-ignored rather than
         * silently pretending nothing arrived. */
        IPC_STAT_INC(ctx, recv_delivered);
        return;
    }
    prepare_task(ctx, t);
    build_msg(t, &m);
    prev                 = ipc__tls_handler_ctx;
    ipc__tls_handler_ctx = ctx;
    IPC_STAT_INC(ctx, cb_invoked);
    ctx->handler(&m, ctx->handler_user);
    ipc__tls_handler_ctx = prev;
    IPC_STAT_INC(ctx, recv_delivered);
}

/* ------------------------------------------------------------------ */
/* worker pool (IPC_DISPATCH_POOL)                                     */
/* ------------------------------------------------------------------ */

static void *worker_main(void *arg)
{
    ipc_ctx_t  *ctx = arg;
    ipc_task_t  t;

    for (;;) {
        int rc = ipc_queue_pop(&ctx->queue, &t, 1);
        if (rc != IPC_OK) {
            break; /* IPC_ERR_STOPPED */
        }
        invoke_handler(ctx, &t);
        ipc_task_clear(&t);
    }
    return NULL;
}

int ipc_loop_start_workers(ipc_ctx_t *ctx)
{
    int i;

    if (ctx->workers_started) {
        return IPC_OK;
    }
    ctx->workers = calloc((size_t)ctx->opt.workers, sizeof(pthread_t));
    if (ctx->workers == NULL) {
        return IPC_ERR_NOMEM;
    }
    for (i = 0; i < ctx->opt.workers; i++) {
        if (pthread_create(&ctx->workers[i], NULL, worker_main, ctx) != 0) {
            IPC_LOGE("pthread_create(worker %d) failed", i);
            ctx->nworkers        = i;
            ctx->workers_started = 1;
            ipc_loop_shutdown_workers(ctx);
            return IPC_ERR_IO;
        }
    }
    ctx->nworkers        = ctx->opt.workers;
    ctx->workers_started = 1;
    IPC_LOGI("%d callback worker(s) started", ctx->nworkers);
    return IPC_OK;
}

void ipc_loop_shutdown_workers(ipc_ctx_t *ctx)
{
    int i;

    if (ctx == NULL) {
        return;
    }
    ipc_queue_shutdown(&ctx->queue);
    if (ctx->workers_started) {
        for (i = 0; i < ctx->nworkers; i++) {
            pthread_join(ctx->workers[i], NULL);
        }
    }
    free(ctx->workers);
    ctx->workers         = NULL;
    ctx->nworkers        = 0;
    ctx->workers_started = 0;
}

/* ------------------------------------------------------------------ */
/* validation + dispatching of one datagram                            */
/* ------------------------------------------------------------------ */

static int process_datagram(ipc_ctx_t *ctx, const ipc_hdr_t *hdr,
                            const ipc_cred_t *cred, const uint8_t *payload,
                            size_t len)
{
    const ipc_config_entry_t *sender;

    /* --- sender identity ------------------------------------------- */
    sender = ipc_config_lookup(ctx->cfg, ctx->ns, hdr->src);
    if (sender == NULL) {
        IPC_LOGW("rejecting data from unconfigured module '%s'", hdr->src);
        IPC_STAT_INC(ctx, recv_rej_cred);
        return IPC_ERR_CRED;
    }
    if (sender->uid != cred->uid) {
        IPC_LOGW("rejecting data from '%s': claimed uid %u, kernel says %u",
                 hdr->src, (unsigned)sender->uid, (unsigned)cred->uid);
        IPC_STAT_INC(ctx, recv_rej_cred);
        return IPC_ERR_CRED;
    }

    /* --- replies never reach a business handler -------------------- */
    if (hdr->type == IPC_TYPE_REP) {
        /* The echoed instance id must be ours: a reply produced for a
         * previous instance of this process must not complete a request that
         * this instance issued. */
        if (hdr->instance_id != ctx->instance_id) {
            IPC_LOGW("discarding stale reply from '%s' (instance %016llx, "
                     "ours %016llx)", hdr->src,
                     (unsigned long long)hdr->instance_id,
                     (unsigned long long)ctx->instance_id);
            IPC_STAT_INC(ctx, reply_unmatched);
            return IPC_ERR_NOENT;
        }
        if (ipc_pending_complete(&ctx->pend, hdr->req_id, hdr->src, payload,
                                 len) != IPC_OK) {
            IPC_LOGI("discarding unmatched reply from '%s' (req %llu)",
                     hdr->src, (unsigned long long)hdr->req_id);
            IPC_STAT_INC(ctx, reply_unmatched);
            return IPC_ERR_NOENT;
        }
        IPC_STAT_INC(ctx, reply_matched);
        return IPC_OK;
    }

    /* --- business message ------------------------------------------ */
    if (ctx->opt.dispatch == IPC_DISPATCH_POOL) {
        if (ctx->handler == NULL) {
            IPC_STAT_INC(ctx, recv_delivered);
            return IPC_OK;
        }
        if (ipc_queue_push(&ctx->queue, hdr, cred, payload, len) != IPC_OK) {
            /* Non-blocking hand-off by design: blocking the receive thread
             * would stall every peer of this module. */
            IPC_STAT_INC(ctx, cb_dropped);
            IPC_LOGW("callback queue full, dropping event %u from %s",
                     hdr->event, hdr->src);
            return IPC_ERR_AGAIN;
        }
        return IPC_OK;
    }

    {
        /* INLINE: no allocation at all, the payload points straight into the
         * receive buffer. */
        ipc_task_t t;
        memset(&t, 0, sizeof(t));
        t.hdr          = *hdr;
        t.cred         = *cred;
        t.payload      = (uint8_t *)(uintptr_t)payload;
        t.len          = len;
        t.owns_payload = 0;
        invoke_handler(ctx, &t);
    }
    return IPC_OK;
}

/* Drain everything currently queued on the socket.  Returns messages
 * processed, or a negative rc. */
static int drain_socket(ipc_ctx_t *ctx, uint8_t *buf, size_t bufcap,
                        uint8_t *ctrl)
{
    int processed = 0;

    for (;;) {
        struct iovec    iov;
        struct msghdr   mh;
        ssize_t         n;
        ipc_hdr_t       hdr;
        ipc_cred_t      cred;
        size_t          plen;
        int             flags = 0;

        iov.iov_base = buf;
        iov.iov_len  = bufcap;
        memset(&mh, 0, sizeof(mh));
        mh.msg_iov        = &iov;
        mh.msg_iovlen     = 1;
        mh.msg_control    = ctrl;
        mh.msg_controllen = IPC_CTRL_SIZE;

        n = recvmsg(ctx->fd, &mh, MSG_DONTWAIT | MSG_CMSG_CLOEXEC | MSG_TRUNC);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return processed;
            }
            IPC_LOGE("recvmsg: %s", strerror(errno));
            return processed > 0 ? processed : ipc_errno_to_rc(errno);
        }
        flags = mh.msg_flags;
        IPC_STAT_INC(ctx, recv_read);
        processed++;

        if (flags & MSG_CTRUNC) {
            IPC_LOGW("control data truncated, rejecting datagram");
            IPC_STAT_INC(ctx, recv_rejected);
            IPC_STAT_INC(ctx, recv_rej_trunc);
            continue;
        }
        if (flags & MSG_TRUNC) {
            IPC_LOGW("datagram of %zd bytes exceeds our receive buffer (%zu), "
                     "rejecting", n, bufcap);
            IPC_STAT_INC(ctx, recv_rejected);
            IPC_STAT_INC(ctx, recv_rej_trunc);
            continue;
        }
        if (ipc_cred_from_msg(&mh, flags, &cred) != IPC_OK) {
            IPC_LOGW("no usable SCM_CREDENTIALS on incoming datagram");
            IPC_STAT_INC(ctx, recv_rejected);
            IPC_STAT_INC(ctx, recv_rej_cred);
            continue;
        }
        if (ipc_hdr_decode(buf, (size_t)n, &hdr) != IPC_OK) {
            IPC_LOGW("malformed header (%zd bytes)", n);
            IPC_STAT_INC(ctx, recv_rejected);
            IPC_STAT_INC(ctx, recv_rej_proto);
            continue;
        }
        if (strcmp(hdr.dst, ctx->module) != 0 || strcmp(hdr.ns, ctx->ns) != 0) {
            IPC_LOGW("datagram for %s/%s delivered here (%s/%s)", hdr.ns,
                     hdr.dst, ctx->ns, ctx->module);
            IPC_STAT_INC(ctx, recv_rejected);
            IPC_STAT_INC(ctx, recv_rej_proto);
            continue;
        }
        plen = (size_t)n - IPC_HDR_SIZE;
        if (plen != hdr.payload_len) {
            IPC_LOGW("declared payload %u but %zu bytes arrived",
                     hdr.payload_len, plen);
            IPC_STAT_INC(ctx, recv_rejected);
            IPC_STAT_INC(ctx, recv_rej_proto);
            continue;
        }
        process_datagram(ctx, &hdr, &cred, buf + IPC_HDR_SIZE, plen);
    }
}

/* ------------------------------------------------------------------ */
/* the loop itself                                                     */
/* ------------------------------------------------------------------ */

static void *loop_main(void *arg)
{
    ipc_ctx_t *ctx  = arg;
    size_t     cap  = IPC_HDR_SIZE + (size_t)ctx->max_payload;
    uint8_t   *buf  = malloc(cap);
    uint8_t   *ctrl = malloc(IPC_CTRL_SIZE);

    if (buf == NULL || ctrl == NULL) {
        IPC_LOGE("receive buffers: out of memory");
        atomic_store(&ctx->loop_error, IPC_ERR_NOMEM);
    } else {
        IPC_LOGI("receive loop started (buffer %zu bytes)", cap);
        while (!atomic_load(&ctx->stopped)) {
            struct epoll_event ev[4];
            int                n;

            n = epoll_wait(ctx->epoll_fd, ev, 4, -1);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                /* A dead loop is not a graceful stop.  Record why, so
                 * ipc_run() can tell the caller the difference instead of
                 * returning IPC_OK for a context that no longer receives. */
                IPC_LOGE("epoll_wait: %s", strerror(errno));
                atomic_store(&ctx->loop_error, ipc_errno_to_rc(errno));
                break;
            }
            for (int i = 0; i < n; i++) {
                if (ev[i].data.fd == ctx->stop_evfd) {
                    atomic_store(&ctx->stopped, 1);
                    break;
                }
                if (ev[i].data.fd == ctx->fd) {
                    int drc = drain_socket(ctx, buf, cap, ctrl);
                    if (drc < 0 && drc != IPC_ERR_AGAIN) {
                        /* recvmsg() failed for a reason that is not "nothing
                         * to read": the socket is broken.  Same reasoning as
                         * the epoll_wait branch above. */
                        IPC_LOGE("receive loop aborted: %s", ipc_strerror(drc));
                        atomic_store(&ctx->loop_error, drc);
                        atomic_store(&ctx->stopped, 1);
                        break;
                    }
                }
            }
        }
        IPC_LOGI("receive loop stopped");
    }
    free(buf);
    free(ctrl);

    atomic_store(&ctx->stopped, 1);
    pthread_mutex_lock(&ctx->life_lock);
    pthread_cond_broadcast(&ctx->life_cv);
    pthread_mutex_unlock(&ctx->life_lock);
    return NULL;
}

int ipc_run(ipc_ctx_t *ctx)
{
    int rc;

    if (ctx == NULL) {
        return IPC_ERR_INVAL;
    }
    rc = ipc__check_alive(ctx);
    if (rc != IPC_OK) {
        return rc;
    }
    if (ipc__tls_handler_ctx == ctx) {
        return IPC_ERR_DEADLOCK; /* a handler must not own the loop */
    }
    if (atomic_load(&ctx->loop_thread_started)) {
        return IPC_ERR_STATE; /* exactly one loop per context */
    }
    if (atomic_load(&ctx->stopped)) {
        return IPC_OK; /* already stopped: nothing to do */
    }
    if (pthread_create(&ctx->loop_thread, NULL, loop_main, ctx) != 0) {
        IPC_LOGE("pthread_create(loop) failed");
        return IPC_ERR_IO;
    }
    ctx->have_loop_thread = 1;
    atomic_store(&ctx->loop_thread_started, 1);

    /* Wait for the loop to end, then reap our own thread.  Teardown only
     * signals: joining is the job of whoever started it. */
    pthread_mutex_lock(&ctx->life_lock);
    while (!atomic_load(&ctx->stopped)) {
        pthread_cond_wait(&ctx->life_cv, &ctx->life_lock);
    }
    pthread_mutex_unlock(&ctx->life_lock);

    pthread_join(ctx->loop_thread, NULL);
    ctx->have_loop_thread = 0;
    atomic_store(&ctx->loop_thread_started, 0);
    pthread_mutex_lock(&ctx->life_lock);
    pthread_cond_broadcast(&ctx->life_cv);
    pthread_mutex_unlock(&ctx->life_lock);

    /* Distinguish "the caller asked us to stop" from "the receive loop died".
     * Without this the two look identical to the caller, and a
     * `while (running) { ... }` main loop would treat a dead context as a
     * clean shutdown. */
    {
        int lerr = atomic_load(&ctx->loop_error);
        if (lerr != 0) {
            IPC_LOGE("ipc_run() returning %s: the receive loop failed",
                     ipc_strerror(lerr));
            return lerr;
        }
    }
    return IPC_OK;
}

int ipc_poll(ipc_ctx_t *ctx, int timeout_ms)
{
    uint8_t *buf, *ctrl;
    size_t   cap;
    int      rc = 0;

    if (ctx == NULL) {
        return IPC_ERR_INVAL;
    }
    rc = ipc__check_alive(ctx);
    if (rc != IPC_OK) {
        return rc;
    }
    if (atomic_load(&ctx->loop_thread_started)) {
        /* Mixing the two models would mean two readers on one socket. */
        return IPC_ERR_STATE;
    }
    cap  = IPC_HDR_SIZE + (size_t)ctx->max_payload;
    buf  = malloc(cap);
    ctrl = malloc(IPC_CTRL_SIZE);
    if (buf == NULL || ctrl == NULL) {
        free(buf);
        free(ctrl);
        return IPC_ERR_NOMEM;
    }

    /* Always drain first so a poll never reports "nothing" while messages are
     * already waiting. */
    rc = drain_socket(ctx, buf, cap, ctrl);

    if (rc == 0 && timeout_ms != 0 && !atomic_load(&ctx->stopped)) {
        struct epoll_event ev[4];
        int                k = epoll_wait(ctx->epoll_fd, ev, 4, timeout_ms);
        if (k > 0) {
            for (int i = 0; i < k; i++) {
                if (ev[i].data.fd == ctx->stop_evfd) {
                    atomic_store(&ctx->stopped, 1);
                    rc = rc > 0 ? rc : 0;
                    break;
                }
                if (ev[i].data.fd == ctx->fd) {
                    rc = drain_socket(ctx, buf, cap, ctrl);
                }
            }
        } else if (k < 0 && errno != EINTR) {
            rc = ipc_errno_to_rc(errno);
        }
    }
    free(buf);
    free(ctrl);
    return rc;
}

int ipc_stop(ipc_ctx_t *ctx)
{
    uint64_t one = 1;

    if (ctx == NULL) {
        return IPC_ERR_INVAL;
    }
    if (atomic_exchange(&ctx->stopped, 1)) {
        return IPC_OK;
    }
    /* Wake waiters in ipc_send(): if the loop is going away, no reply can
     * ever arrive, and the legacy "wait forever" contract must not turn into
     * an unkillable hang during shutdown. */
    ipc_pending_shutdown(&ctx->pend);
    if (ctx->stop_evfd >= 0) {
        ssize_t n = write(ctx->stop_evfd, &one, sizeof(one));
        (void)n;
    }
    pthread_mutex_lock(&ctx->life_lock);
    pthread_cond_broadcast(&ctx->life_cv);
    pthread_mutex_unlock(&ctx->life_lock);
    return IPC_OK;
}

int ipc_is_stopped(const ipc_ctx_t *ctx)
{
    return ctx ? atomic_load(&ctx->stopped) : 1;
}
