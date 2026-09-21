/*
 * ipc_queue.h -- bounded hand-off queue between the receive thread and the
 * optional worker pool (IPC_DISPATCH_POOL).
 *
 * The queue is used exclusively in POOL mode.  INLINE mode hands the message
 * straight to the handler and makes no allocation at all, which is the
 * cheaper option and the default.
 *
 * Push never blocks: when the queue is full the message is dropped and
 * counted, because blocking the receive thread would stall every peer.
 */
#ifndef IPC_QUEUE_H
#define IPC_QUEUE_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include "ipc/ipc.h"
#include "ipc_proto.h"

typedef struct {
    ipc_hdr_t       hdr;
    ipc_cred_t      cred;
    uint8_t        *payload;  /* owned when owns_payload */
    size_t          len;
    int             owns_payload;
    ipc_reply_ctx_t rctx;     /* pre-filled so ipc_reply() can answer */
} ipc_task_t;

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t  cv;
    ipc_task_t     *items;
    int             cap;
    int             head;
    int             count;
    int             shutdown;
} ipc_queue_t;

int  ipc_queue_init(ipc_queue_t *q, int cap);
void ipc_queue_destroy(ipc_queue_t *q);

/* Deep-copies the payload.  Returns IPC_ERR_AGAIN when full (drop),
 * IPC_ERR_STOPPED after shutdown, IPC_ERR_NOMEM on allocation failure. */
int ipc_queue_push(ipc_queue_t *q, const ipc_hdr_t *hdr, const ipc_cred_t *cred,
                   const void *payload, size_t len);

/* Pop one task.  Returns IPC_OK, IPC_ERR_AGAIN when empty and non-blocking,
 * or IPC_ERR_STOPPED when shut down (also when the queue drains and the
 * shutdown flag is set, so workers always terminate). */
int ipc_queue_pop(ipc_queue_t *q, ipc_task_t *out, int block);

/* Frees a task's payload, leaving the struct zeroed. */
void ipc_task_clear(ipc_task_t *t);

void ipc_queue_shutdown(ipc_queue_t *q);
int  ipc_queue_count(ipc_queue_t *q);
int  ipc_queue_cap(const ipc_queue_t *q);

#endif /* IPC_QUEUE_H */
