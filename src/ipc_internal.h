/*
 * ipc_internal.h -- internal context layout shared by the implementation
 * translation units.  Not installed; tests may include it directly.
 */
#ifndef IPC_INTERNAL_H
#define IPC_INTERNAL_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sys/types.h>

#include "ipc/ipc.h"
#include "ipc_config.h"
#include "ipc_pending.h"
#include "ipc_proto.h"
#include "ipc_queue.h"
#include "ipc_util.h"

/* One definition of the counter set: used to build both the atomic storage
 * and the snapshot function, so the two can never drift apart. */
#define IPC_STAT_FIELDS(X)                                                     \
    X(send_attempts)                                                           \
    X(send_enqueued)                                                           \
    X(send_failed)                                                             \
    X(broadcast_targets)                                                       \
    X(broadcast_skipped)                                                       \
    X(recv_read)                                                               \
    X(recv_rejected)                                                           \
    X(recv_rej_cred)                                                           \
    X(recv_rej_proto)                                                          \
    X(recv_rej_trunc)                                                          \
    X(recv_delivered)                                                          \
    X(cb_invoked)                                                              \
    X(cb_dropped)                                                              \
    X(reply_sent)                                                              \
    X(reply_matched)                                                           \
    X(reply_unmatched)                                                         \
    X(pending_rejected)                                                        \
    X(eagain_count)                                                            \
    X(deadlock_probes)

typedef struct {
#define IPC_X(f) _Atomic uint64_t f;
    IPC_STAT_FIELDS(IPC_X)
#undef IPC_X
} ipc_stats_atomic_t;

struct ipc_ctx {
    /* identity */
    char     module[IPC_NAME_MAX];
    char     ns[IPC_NS_MAX];
    char     path[IPC_SUN_PATH_MAX];
    char     lock_path[IPC_SUN_PATH_MAX + 8];
    uid_t    uid;
    gid_t    gid;
    uint64_t instance_id;
    pid_t    owner_pid;

    /* transport */
    int fd;
    int lock_fd;
    int epoll_fd;
    int stop_evfd;
    int bounds_created; /* the socket path was created by us, so teardown
                         * unlinks it.  Does NOT cover the lock file: that one
                         * is deliberately never removed (see
                         * ctx_release_resources()). */

    /* config + options */
    ipc_config_t        *cfg;
    ipc_register_opts_t  opt;
    uint32_t             max_payload;

    /* dispatch */
    ipc_handler_fn handler;
    void          *handler_user;
    _Atomic int    loop_thread_started;
    _Atomic int    stopped;
    _Atomic int    teardown;      /* ipc_unregister() was entered          */
    _Atomic int    teardown_done; /* ipc_unregister() finished; the context
                                   * is drained and only ipc_ctx_free() is
                                   * still allowed to touch it            */
    _Atomic int    loop_error;    /* fatal error that killed the receive
                                   * loop; 0 while it is healthy.  Reported
                                   * by ipc_run() so a dead loop cannot look
                                   * like a normal ipc_stop().             */
    pthread_t      loop_thread;
    int            have_loop_thread;
    pthread_t     *workers;
    int            nworkers;
    int            workers_started;

    ipc_pending_t pend;
    ipc_queue_t   queue;

    pthread_mutex_t  life_lock;
    pthread_cond_t   life_cv;

    ipc_stats_atomic_t st;
};

/* Thread-local marker set while a business handler runs.  Used to turn the
 * "ipc_send() from a handler" mistake into a clean error instead of a
 * silent deadlock in INLINE mode. */
extern __thread ipc_ctx_t *ipc__tls_handler_ctx;

#define IPC_STAT_INC(ctx, field)                                               \
    atomic_fetch_add_explicit(&(ctx)->st.field, 1u, memory_order_relaxed)
#define IPC_STAT_ADD(ctx, field, v)                                            \
    atomic_fetch_add_explicit(&(ctx)->st.field, (uint64_t)(v),                 \
                              memory_order_relaxed)

/* ipc_loop.c */
void ipc_loop_shutdown_workers(ipc_ctx_t *ctx);
int  ipc_loop_start_workers(ipc_ctx_t *ctx);

/* ipc_io.c -- shared low-level send used by post/send/broadcast/reply.
 * `dst_path` is the peer's socket path.  Returns IPC_OK or a negative rc. */
int ipc__sendto(ipc_ctx_t *ctx, const char *dst_path, const ipc_hdr_t *hdr,
                const void *payload, size_t len);

/* ipc_ctx.c -- helpers used by io/loop */
const char *ipc__peer_path(ipc_ctx_t *ctx, const char *dst, uid_t *uid_out);
int         ipc__check_alive(ipc_ctx_t *ctx);

#endif /* IPC_INTERNAL_H */
