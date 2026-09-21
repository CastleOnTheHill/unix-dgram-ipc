#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>

#include "ipc_internal.h"

/* ------------------------------------------------------------------ */
/* one place that actually writes to a peer's socket                   */
/* ------------------------------------------------------------------ */

int ipc__sendto(ipc_ctx_t *ctx, const char *dst_path, const ipc_hdr_t *hdr,
                const void *payload, size_t len)
{
    uint8_t         hb[IPC_HDR_SIZE];
    struct sockaddr_un sun;
    struct iovec    iov[2];
    struct msghdr   mh;
    ssize_t         n;
    size_t          want;

    if (ctx == NULL || dst_path == NULL || hdr == NULL) {
        return IPC_ERR_INVAL;
    }
    if (len > 0 && payload == NULL) {
        return IPC_ERR_INVAL;
    }
    if (len > ctx->max_payload) {
        return IPC_ERR_MSGSIZE;
    }
    if (ipc_hdr_encode(hdr, hb, sizeof(hb)) != IPC_HDR_SIZE) {
        return IPC_ERR_PROTO;
    }

    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    memcpy(sun.sun_path, dst_path, strlen(dst_path) + 1);

    iov[0].iov_base = hb;
    iov[0].iov_len  = IPC_HDR_SIZE;
    iov[1].iov_base = (void *)(uintptr_t)payload;
    iov[1].iov_len  = len;

    memset(&mh, 0, sizeof(mh));
    mh.msg_name    = &sun;
    mh.msg_namelen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
                                 strlen(dst_path) + 1);
    mh.msg_iov     = iov;
    mh.msg_iovlen  = (len > 0 && payload != NULL) ? 2 : 1;

    /* MSG_DONTWAIT: never block, even if a caller made the fd blocking.
     * MSG_NOSIGNAL: no SIGPIPE on a closed peer (relevant for SOCK_STREAM
     * peers and harmless here). */
    n = sendmsg(ctx->fd, &mh, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (n < 0) {
        int rc = ipc_errno_to_rc(errno);
        if (rc == IPC_ERR_AGAIN) {
            IPC_STAT_INC(ctx, eagain_count);
        }
        if (rc == IPC_ERR_NOENT || rc == IPC_ERR_OFFLINE) {
            /* Both mean "this module has no live socket right now":
             *   ENOENT       - path gone (never registered, or removed)
             *   ECONNREFUSED - path present but nobody is bound to it
             * Neither is a hard error for the caller's bookkeeping. */
            rc = IPC_ERR_OFFLINE;
        }
        IPC_LOGD("sendmsg to %s failed: %s", dst_path,
                 rc == IPC_ERR_AGAIN ? "queue full" : strerror(errno));
        return rc;
    }
    want = IPC_HDR_SIZE + (mh.msg_iovlen == 2 ? len : 0);
    if ((size_t)n != want) {
        /* Datagram sockets are all-or-nothing; a short write means something
         * is very wrong (e.g. the path was swapped for a stream socket). */
        IPC_LOGE("short sendmsg to %s: %zd of %zu bytes", dst_path, n, want);
        return IPC_ERR_IO;
    }
    return IPC_OK;
}

static void fill_hdr(ipc_ctx_t *ctx, ipc_hdr_t *h, ipc_msg_type_t type,
                     const char *dst, uint32_t event, uint32_t payload_len,
                     uint64_t req_id)
{
    memset(h, 0, sizeof(*h));
    h->version     = IPC_PROTO_VERSION;
    h->type        = (uint8_t)type;
    h->flags       = 0;
    ipc_strlcpy(h->ns, ctx->ns, sizeof(h->ns));
    ipc_strlcpy(h->src, ctx->module, sizeof(h->src));
    ipc_strlcpy(h->dst, dst, sizeof(h->dst));
    h->event       = event;
    h->payload_len = payload_len;
    h->req_id      = req_id;
    h->instance_id = ctx->instance_id;
}

/* ------------------------------------------------------------------ */
/* legacy interface #2: post                                           */
/* ------------------------------------------------------------------ */

int ipc_post(ipc_ctx_t *ctx, const char *dst, uint32_t event, const void *data,
             size_t len)
{
    ipc_hdr_t   h;
    const char *path;
    int         rc;

    if (dst == NULL || dst[0] == '\0') {
        return IPC_ERR_INVAL;
    }
    rc = ipc__check_alive(ctx);
    if (rc != IPC_OK) {
        return rc;
    }
    if (len > ctx->max_payload) {
        IPC_STAT_INC(ctx, send_failed);
        return IPC_ERR_MSGSIZE;
    }
    path = ipc__peer_path(ctx, dst, NULL);
    if (path == NULL) {
        IPC_STAT_INC(ctx, send_failed);
        return IPC_ERR_NOENT;
    }
    fill_hdr(ctx, &h, IPC_TYPE_POST, dst, event, (uint32_t)len, 0);
    IPC_STAT_INC(ctx, send_attempts);
    rc = ipc__sendto(ctx, path, &h, data, len);
    if (rc == IPC_OK) {
        IPC_STAT_INC(ctx, send_enqueued);
    } else {
        IPC_STAT_INC(ctx, send_failed);
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* legacy interface #3: send (synchronous)                             */
/* ------------------------------------------------------------------ */

int ipc_send_timeout(ipc_ctx_t *ctx, const char *dst, uint32_t event,
                     const void *data, size_t len, void *reply_buf,
                     size_t reply_cap, size_t *out_len, int timeout_ms)
{
    ipc_hdr_t   h;
    const char *path;
    int         idx    = -1;
    uint64_t    req_id = 0;
    size_t      rlen   = 0;
    int         rc;

    if (out_len != NULL) {
        *out_len = 0;
    }
    if (dst == NULL || dst[0] == '\0') {
        return IPC_ERR_INVAL;
    }
    if (reply_buf == NULL && reply_cap != 0) {
        return IPC_ERR_INVAL;
    }
    rc = ipc__check_alive(ctx);
    if (rc != IPC_OK) {
        return rc;
    }
    if (len > ctx->max_payload) {
        IPC_STAT_INC(ctx, send_failed);
        return IPC_ERR_MSGSIZE;
    }
    if (!atomic_load(&ctx->loop_thread_started)) {
        /* Without a receive loop in another thread nobody can ever complete
         * this request; failing fast beats hanging forever. */
        IPC_LOGE("ipc_send() requires a running ipc_run() loop");
        return IPC_ERR_STATE;
    }
    if (ipc__tls_handler_ctx == ctx &&
        ctx->opt.dispatch == IPC_DISPATCH_INLINE) {
        /* The handler runs on the only thread that could deliver the reply. */
        IPC_STAT_INC(ctx, deadlock_probes);
        return IPC_ERR_DEADLOCK;
    }
    path = ipc__peer_path(ctx, dst, NULL);
    if (path == NULL) {
        IPC_STAT_INC(ctx, send_failed);
        return IPC_ERR_NOENT;
    }

    /* Register the slot BEFORE sending: a fast peer must never be able to
     * answer while the requester is not yet listening. */
    rc = ipc_pending_add(&ctx->pend, dst, timeout_ms, reply_buf, reply_cap,
                         &idx, &req_id);
    if (rc != IPC_OK) {
        IPC_STAT_INC(ctx, pending_rejected);
        return rc;
    }

    fill_hdr(ctx, &h, IPC_TYPE_REQ, dst, event, (uint32_t)len, req_id);
    IPC_STAT_INC(ctx, send_attempts);
    rc = ipc__sendto(ctx, path, &h, data, len);
    if (rc != IPC_OK) {
        IPC_STAT_INC(ctx, send_failed);
        ipc_pending_release(&ctx->pend, idx); /* never entered the wait */
        return rc;
    }
    IPC_STAT_INC(ctx, send_enqueued);

    rc = ipc_pending_wait(&ctx->pend, idx, &rlen);
    ipc_pending_release(&ctx->pend, idx);
    if (out_len != NULL) {
        *out_len = rlen;
    }
    return rc;
}

int ipc_send(ipc_ctx_t *ctx, const char *dst, uint32_t event, const void *data,
             size_t len, void *reply_buf, size_t reply_cap, size_t *out_len)
{
    /* Legacy contract: wait indefinitely for the reply. */
    return ipc_send_timeout(ctx, dst, event, data, len, reply_buf, reply_cap,
                            out_len, -1);
}

/* ------------------------------------------------------------------ */
/* legacy interface #4: broadcast                                      */
/* ------------------------------------------------------------------ */

int ipc_broadcast(ipc_ctx_t *ctx, uint32_t event, const void *data, size_t len)
{
    ipc_hdr_t h;
    int       i;
    int       sent = 0;
    int       rc;

    rc = ipc__check_alive(ctx);
    if (rc != IPC_OK) {
        return rc;
    }
    if (len > ctx->max_payload) {
        return IPC_ERR_MSGSIZE;
    }

    fill_hdr(ctx, &h, IPC_TYPE_POST, "", event, (uint32_t)len, 0);

    /* Fixed target set: the configured modules of our own namespace.  A
     * module that is not running is skipped, never queued for later.  This is
     * not atomic and does not promise that all targets succeed; a partial
     * success is not an error. */
    for (i = 0; i < ipc_config_count(ctx->cfg); i++) {
        const ipc_config_entry_t *e = ipc_config_at(ctx->cfg, i);

        if (strcmp(e->ns, ctx->ns) != 0) {
            continue;
        }
        if (!ctx->opt.broadcast_include_self &&
            strcmp(e->module, ctx->module) == 0) {
            continue;
        }
        ipc_strlcpy(h.dst, e->module, sizeof(h.dst));
        IPC_STAT_INC(ctx, broadcast_targets);
        IPC_STAT_INC(ctx, send_attempts);
        rc = ipc__sendto(ctx, e->path, &h, data, len);
        if (rc == IPC_OK) {
            IPC_STAT_INC(ctx, send_enqueued);
            sent++;
        } else if (rc == IPC_ERR_OFFLINE) {
            IPC_STAT_INC(ctx, broadcast_skipped);
        } else {
            IPC_STAT_INC(ctx, send_failed);
        }
    }
    return sent;
}

/* ------------------------------------------------------------------ */
/* reply                                                               */
/* ------------------------------------------------------------------ */

int ipc_reply(const ipc_msg_t *msg, const void *data, size_t len)
{
    ipc_reply_ctx_t *r;
    ipc_ctx_t       *ctx;
    ipc_hdr_t        h;
    const char      *path;
    int              rc;

    if (msg == NULL || msg->opaque == NULL) {
        return IPC_ERR_INVAL;
    }
    if (msg->type != IPC_TYPE_REQ) {
        return IPC_ERR_INVAL; /* only requests have a reply to give */
    }
    r   = (ipc_reply_ctx_t *)msg->opaque;
    ctx = r->ctx;
    if (ctx == NULL) {
        return IPC_ERR_INVAL;
    }
    if (r->replied) {
        return IPC_ERR_STATE; /* at most one reply per request */
    }
    rc = ipc__check_alive(ctx);
    if (rc != IPC_OK) {
        return rc;
    }
    if (len > ctx->max_payload) {
        return IPC_ERR_MSGSIZE;
    }
    path = ipc__peer_path(ctx, r->dst, NULL);
    if (path == NULL) {
        return IPC_ERR_NOENT;
    }
    fill_hdr(ctx, &h, IPC_TYPE_REP, r->dst, msg->event, (uint32_t)len, r->req_id);
    /* Echo the *requester's* instance id: a restarted requester must reject a
     * reply addressed to the previous instance. */
    h.instance_id = r->peer_instance;
    rc = ipc__sendto(ctx, path, &h, data, len);
    if (rc == IPC_OK) {
        r->replied = 1;
        IPC_STAT_INC(ctx, reply_sent);
    }
    return rc;
}
