/*
 * ipc.h -- AF_UNIX SOCK_DGRAM direct-connect IPC framework (public API).
 *
 * Prototype implementation of the design described in handoff.md:
 * modules talk straight to each other over Unix domain datagram sockets;
 * there is no central forwarding server.
 *
 * NOTE ON COMPATIBILITY: no source of the three legacy C IPC frameworks was
 * available when this prototype was written.  The four functions below
 * (register / post / send / broadcast) mirror the *documented* legacy
 * semantics only.  Everything else -- exact signatures, return codes,
 * callback threading, data ownership -- is an explicitly stated assumption
 * recorded in this header and in README.md.  Legacy compatibility is
 * therefore "not verified".
 */
#ifndef IPC_IPC_H
#define IPC_IPC_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Compile-time limits                                                 */
/* ------------------------------------------------------------------ */

/* Module identifier: 31 chars + NUL (fixed-width field in the wire header). */
#define IPC_NAME_MAX 32
/* Framework namespace: 15 chars + NUL. */
#define IPC_NS_MAX 16
/* Transport path length (sizeof(((struct sockaddr_un *)0)->sun_path) is 108). */
#define IPC_SUN_PATH_MAX 108
/* Serialised header size.  Fixed width, no padding, big-endian. */
#define IPC_HDR_SIZE 112
/* Absolute hard cap for a payload; enforced even if config asks for more. */
#define IPC_PAYLOAD_HARD_MAX 65536u
/* Default runtime payload cap (bytes). */
#define IPC_PAYLOAD_DEFAULT 8192u

#define IPC_CONF_DEFAULT "/etc/ipc-modules.conf"

/* ------------------------------------------------------------------ */
/* Error codes                                                         */
/* ------------------------------------------------------------------ */

typedef enum {
    IPC_OK               = 0,
    IPC_ERR_INVAL        = -1,  /* bad argument */
    IPC_ERR_NOMEM        = -2,
    IPC_ERR_IO           = -3,
    IPC_ERR_AGAIN        = -4,  /* peer queue full / would block */
    IPC_ERR_NOENT        = -5,  /* module not present in the static table */
    IPC_ERR_OFFLINE      = -6,  /* module known but has no live socket */
    IPC_ERR_PERM         = -7,
    IPC_ERR_BUSY         = -8,  /* module already registered (lock held) */
    IPC_ERR_CRED         = -9,  /* missing / mismatching SCM_CREDENTIALS */
    IPC_ERR_PROTO        = -10, /* malformed datagram */
    IPC_ERR_TIMEOUT      = -11,
    IPC_ERR_STOPPED      = -12,
    IPC_ERR_DEADLOCK     = -13, /* ipc_send() re-entered from a handler */
    IPC_ERR_MSGSIZE      = -14,
    IPC_ERR_CONFIG       = -15,
    IPC_ERR_TOOMANY      = -16, /* pending-request table exhausted */
    IPC_ERR_STATE        = -17  /* API used in the wrong lifecycle state */
} ipc_err_t;

const char *ipc_strerror(int rc);

/* ------------------------------------------------------------------ */
/* Message types and delivery classes                                  */
/* ------------------------------------------------------------------ */

typedef enum {
    IPC_TYPE_POST = 1, /* fire and forget, no reply expected */
    IPC_TYPE_REQ  = 2, /* synchronous request, reply expected */
    IPC_TYPE_REP  = 3  /* reply to IPC_TYPE_REQ */
} ipc_msg_type_t;

typedef enum {
    /* Handler runs on the single receive thread.  Cheapest in memory and
     * preserves per-source ordering.  ipc_send() MUST NOT be called from a
     * handler: it is detected and fails with IPC_ERR_DEADLOCK instead of
     * deadlocking.  This is the default. */
    IPC_DISPATCH_INLINE = 0,
    /* Handler runs on one of `workers` bounded worker threads pulling from a
     * bounded queue.  ipc_send() is allowed from a handler.  A full queue
     * drops the message and bumps stats.cb_dropped. */
    IPC_DISPATCH_POOL   = 1
} ipc_dispatch_t;

/* ------------------------------------------------------------------ */
/* Static module table (config)                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    char     ns[IPC_NS_MAX];
    char     module[IPC_NAME_MAX];
    uid_t    uid;          /* UID authorised to register/service this module */
    char     path[IPC_SUN_PATH_MAX];
} ipc_config_entry_t;

typedef struct ipc_config ipc_config_t;

/* Parse "<ns> <module> <uid> <path>" lines, '#' comments, blank lines.
 * Duplicate (ns, module) or duplicate path => IPC_ERR_CONFIG. */
int  ipc_config_load(const char *path, ipc_config_t **out);
int  ipc_config_parse(const char *text, ipc_config_t **out); /* for tests */
void ipc_config_free(ipc_config_t *cfg);

int  ipc_config_count(const ipc_config_t *cfg);
const ipc_config_entry_t *ipc_config_at(const ipc_config_t *cfg, int idx);
const ipc_config_entry_t *ipc_config_lookup(const ipc_config_t *cfg,
                                            const char *ns, const char *module);
/* Reverse lookup: which module owns this socket path?  NULL if none. */
const ipc_config_entry_t *ipc_config_lookup_path(const ipc_config_t *cfg,
                                                 const char *path);

/* ------------------------------------------------------------------ */
/* Handler / message                                                   */
/* ------------------------------------------------------------------ */

typedef struct ipc_msg ipc_msg_t;

struct ipc_msg {
    const char    *ns;
    const char    *src;        /* sender module id */
    const char    *dst;        /* target module id (== our own id) */
    uint32_t       event;
    ipc_msg_type_t type;
    uint64_t       req_id;
    uint64_t       instance_id;/* opaque generation id of the *sender* process */
    const void    *data;       /* valid only for the duration of the call */
    size_t         len;
    void          *opaque;     /* internal; do not touch */
};

/* Runs on the receive thread (INLINE) or a worker thread (POOL).
 * `data` is owned by the library and is only valid inside the call; copy it
 * if the handler needs to keep it.  Calling ipc_reply() at most once, and
 * only for messages with type == IPC_TYPE_REQ, is allowed. */
typedef void (*ipc_handler_fn)(const ipc_msg_t *msg, void *user);

/* ------------------------------------------------------------------ */
/* Statistics                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t send_attempts;    /* post + send + broadcast target attempts  */
    uint64_t send_enqueued;    /* sendto() succeeded                      */
    uint64_t send_failed;      /* sendto() failed for a reason other than "peer offline" */
    uint64_t broadcast_targets;
    uint64_t broadcast_skipped;/* target offline / unreachable            */
    uint64_t recv_read;        /* datagrams read off the socket           */
    uint64_t recv_rejected;    /* dropped before the handler (see reasons) */
    uint64_t recv_rej_cred;    /*   SCM_CREDENTIALS missing or mismatched */
    uint64_t recv_rej_proto;   /*   malformed header / size              */
    uint64_t recv_rej_trunc;   /*   MSG_TRUNC or MSG_CTRUNC               */
    uint64_t recv_delivered;   /* handed to a business handler            */
    uint64_t cb_invoked;       /* handler calls started                   */
    uint64_t cb_dropped;       /* dropped because the callback queue was full */
    uint64_t reply_sent;
    uint64_t reply_matched;    /* REP completed a pending request         */
    uint64_t reply_unmatched;  /* REP with no (matching) pending request  */
    uint64_t pending_rejected; /* ipc_send() refused: table full          */
    uint64_t eagain_count;     /* EAGAIN/ENOBUFS observed on send         */
    uint64_t deadlock_probes;  /* ipc_send() refused with IPC_ERR_DEADLOCK */
} ipc_stats_t;

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    const char    *module;        /* this process' module id (required)      */
    const char    *ns;            /* framework namespace; NULL means "the only
                                   * namespace in which this module name
                                   * occurs".  Ambiguous => IPC_ERR_CONFIG.  */
    const char    *conf_path;     /* NULL => IPC_CONF_DEFAULT                */
    const char    *group;         /* shared group name for the socket file,
                                   * e.g. "ipc-members".  NULL => leave the
                                   * group as the process default.            */
    int            allow_uid_split;/* 0 (default) => real UID, effective UID and
                                   * the configured UID must all agree.  1 =>
                                   * check the effective UID only.          */
    ipc_dispatch_t dispatch;      /* default IPC_DISPATCH_INLINE             */
    int            workers;       /* POOL only, clamp 1..8, default 2        */
    int            cb_queue_max;  /* POOL only, default 64                   */
    int            max_pending;   /* concurrent ipc_send() slots, default 64 */
    uint32_t       max_payload;   /* default IPC_PAYLOAD_DEFAULT             */
    int            broadcast_include_self; /* default 0                       */
    int            sndbuf;        /* SO_SNDBUF, 0 => kernel default          */
    int            rcvbuf;        /* SO_RCVBUF, 0 => kernel default          */
} ipc_register_opts_t;

#define IPC_REGISTER_OPTS_INIT { 0 }

typedef struct ipc_ctx ipc_ctx_t;

/* Legacy interface #1: become `module`.
 * Full lifecycle: config lookup, identity check, non-blocking exclusive lock,
 * residue handling, socket creation/bind/permission, epoll registration.
 * Any failure is rolled back completely. */
int ipc_register(const ipc_register_opts_t *opts, ipc_ctx_t **out);

/* Graceful teardown: stop accepting, resolve pending, close, unlink while
 * still holding the lock, release the lock.  Idempotent. */
int ipc_unregister(ipc_ctx_t *ctx);

int ipc_set_handler(ipc_ctx_t *ctx, ipc_handler_fn fn, void *user);

const char *ipc_module_id(const ipc_ctx_t *ctx);
const char *ipc_namespace(const ipc_ctx_t *ctx);
const char *ipc_socket_path(const ipc_ctx_t *ctx);
int         ipc_socket_fd(const ipc_ctx_t *ctx);
uint64_t    ipc_instance_id(const ipc_ctx_t *ctx);

/* ------------------------------------------------------------------ */
/* Legacy interfaces #2..#4                                            */
/* ------------------------------------------------------------------ */

/* Legacy interface #2: asynchronous send, no business reply awaited.
 * Non-blocking by contract: never blocks.  IPC_ERR_AGAIN means the peer queue
 * was full and the message was NOT enqueued; IPC_ERR_OFFLINE means the peer
 * has no live socket.  Message length is checked BEFORE anything is sent. */
int ipc_post(ipc_ctx_t *ctx, const char *dst, uint32_t event,
             const void *data, size_t len);

/* Legacy interface #3: synchronous send, waits for the reply.
 * Preserves the legacy contract of waiting indefinitely by default; use
 * ipc_send_timeout() for a bounded variant.  `reply_buf`/`reply_cap` receive
 * the reply payload; pass NULL to discard it (the reply is still counted).
 * `out_len` is optional.
 *
 * Contract details:
 *  - The request is registered in the pending table BEFORE sendto(), so a
 *    fast peer can never lose the reply.  If sendto() fails the slot is
 *    released and no wait happens.
 *  - A reply is accepted only if its source module AND echoed instance id
 *    match the request; stale replies from a previous instance of the peer,
 *    or replies from a wrong module, are rejected.
 *  - If the context is stopped while waiting, IPC_ERR_STOPPED is returned.
 *  - Called from an INLINE handler thread => IPC_ERR_DEADLOCK. */
int ipc_send(ipc_ctx_t *ctx, const char *dst, uint32_t event,
             const void *data, size_t len,
             void *reply_buf, size_t reply_cap, size_t *out_len);

/* Same, but bounded.  timeout_ms < 0 behaves like ipc_send(). */
int ipc_send_timeout(ipc_ctx_t *ctx, const char *dst, uint32_t event,
                     const void *data, size_t len,
                     void *reply_buf, size_t reply_cap, size_t *out_len,
                     int timeout_ms);

/* Legacy interface #4: send to every module in our namespace.
 * Not atomic: modules that are offline are skipped and counted, they are
 * never queued for later.  Returns the number of targets that accepted the
 * datagram, or a negative error if the arguments are invalid.  A partial
 * success is NOT an error.  Use ipc_get_stats() for the skip count. */
int ipc_broadcast(ipc_ctx_t *ctx, uint32_t event, const void *data, size_t len);

/* Reply to a IPC_TYPE_REQ received by our handler.  May be called once per
 * message, from the handler.  Returns 0 on success. */
int ipc_reply(const ipc_msg_t *msg, const void *data, size_t len);

/* ------------------------------------------------------------------ */
/* Event loop                                                          */
/* ------------------------------------------------------------------ */

/* Blocking receive loop until ipc_stop()/ipc_unregister().  Spawns one
 * receive thread and (POOL) `workers` worker threads.  Must not be combined
 * with ipc_poll() on the same context. */
int ipc_run(ipc_ctx_t *ctx);

/* Single-process drain step: process everything currently queued without
 * blocking, plus at most `timeout_ms` of waiting if nothing was queued.
 * Returns the number of datagrams processed, or a negative error.
 * Intended for embedders and tests that own their own loop. */
int ipc_poll(ipc_ctx_t *ctx, int timeout_ms);

int ipc_stop(ipc_ctx_t *ctx);   /* wakes ipc_run(); safe from any thread */
int ipc_is_stopped(const ipc_ctx_t *ctx);

void ipc_get_stats(const ipc_ctx_t *ctx, ipc_stats_t *out);

#ifdef __cplusplus
}
#endif
#endif /* IPC_IPC_H */
