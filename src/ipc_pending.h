/*
 * ipc_pending.h -- bounded table of outstanding synchronous requests.
 *
 * Deliberately simple: a fixed-size array guarded by one mutex and one
 * condition variable, woken with broadcast.  Concurrency is bounded by
 * `cap` (default 64), so broadcast is cheaper than per-slot primitives and
 * the whole structure is allocation-free after init.
 */
#ifndef IPC_PENDING_H
#define IPC_PENDING_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "ipc/ipc.h"

typedef struct {
    int      in_use;
    int      done;
    int      rc;             /* IPC_OK or a negative ipc_err_t */
    size_t   reply_len;      /* bytes written on success, needed size on MSGSIZE */
    void    *reply_buf;      /* caller-owned */
    size_t   reply_cap;
    uint64_t req_id;
    uint64_t deadline_ns;    /* absolute CLOCK_MONOTONIC ns; 0 == no deadline */
    pid_t    owner_pid;      /* only the creating thread may wait on it */
    char     dst[IPC_NAME_MAX];
} ipc_pending_slot_t;

typedef struct {
    pthread_mutex_t     lock;
    pthread_cond_t      cv;
    ipc_pending_slot_t *slots;
    int                 cap;
    int                 used;
    uint64_t            next_req_id;
    int                 shutting_down;
} ipc_pending_t;

int  ipc_pending_init(ipc_pending_t *p, int cap);
void ipc_pending_destroy(ipc_pending_t *p);

/* Reserve a slot.  *out_idx receives the slot index, *out_id the request id.
 * Returns IPC_ERR_TOOMANY when the table is full, IPC_ERR_STOPPED when the
 * table is shutting down. */
int ipc_pending_add(ipc_pending_t *p, const char *dst, int64_t timeout_ms,
                    void *reply_buf, size_t reply_cap,
                    int *out_idx, uint64_t *out_id);

/* Release a slot after the wait finished (success or failure). */
void ipc_pending_release(ipc_pending_t *p, int idx);

/* Complete a request from the receive thread.
 * Matches on (req_id, dst module).  Returns IPC_OK when a live slot was
 * completed, IPC_ERR_NOENT when nothing matched (late / duplicate / wrong
 * source reply). */
int ipc_pending_complete(ipc_pending_t *p, uint64_t req_id, const char *src,
                         const void *data, size_t len);

/* Block until the slot is completed, the deadline expires, or the table is
 * shut down.  `timeout_ms` < 0 means wait forever.
 * Returns IPC_OK / IPC_ERR_TIMEOUT / IPC_ERR_STOPPED / the completion error.
 * On IPC_ERR_MSGSIZE the reply was longer than the caller's buffer and
 * *reply_len_out holds the size that would have been needed. */
int ipc_pending_wait(ipc_pending_t *p, int idx, size_t *reply_len_out);

/* Wake every waiter with IPC_ERR_STOPPED and refuse new reservations. */
void ipc_pending_shutdown(ipc_pending_t *p);

int ipc_pending_used(ipc_pending_t *p);
int ipc_pending_cap(const ipc_pending_t *p);

#endif /* IPC_PENDING_H */
