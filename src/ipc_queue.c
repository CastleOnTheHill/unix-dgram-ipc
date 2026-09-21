#include "ipc_queue.h"

#include <stdlib.h>
#include <string.h>

#include "ipc_util.h"

int ipc_queue_init(ipc_queue_t *q, int cap)
{
    if (q == NULL || cap <= 0) {
        return IPC_ERR_INVAL;
    }
    memset(q, 0, sizeof(*q));
    q->items = calloc((size_t)cap, sizeof(*q->items));
    if (q->items == NULL) {
        return IPC_ERR_NOMEM;
    }
    q->cap = cap;
    if (pthread_mutex_init(&q->lock, NULL) != 0 ||
        pthread_cond_init(&q->cv, NULL) != 0) {
        free(q->items);
        q->items = NULL;
        return IPC_ERR_IO;
    }
    return IPC_OK;
}

void ipc_task_clear(ipc_task_t *t)
{
    if (t == NULL) {
        return;
    }
    if (t->owns_payload) {
        free(t->payload);
    }
    t->payload      = NULL;
    t->len          = 0;
    t->owns_payload = 0;
}

void ipc_queue_destroy(ipc_queue_t *q)
{
    int i;

    if (q == NULL) {
        return;
    }
    /* never leak payloads that were still queued at teardown */
    for (i = 0; i < q->cap; i++) {
        ipc_task_clear(&q->items[i]);
    }
    pthread_cond_destroy(&q->cv);
    pthread_mutex_destroy(&q->lock);
    free(q->items);
    q->items = NULL;
}

int ipc_queue_push(ipc_queue_t *q, const ipc_hdr_t *hdr, const ipc_cred_t *cred,
                   const void *payload, size_t len)
{
    int    slot;
    size_t po = 0;

    if (q == NULL || hdr == NULL || cred == NULL) {
        return IPC_ERR_INVAL;
    }
    if (len > 0 && payload == NULL) {
        return IPC_ERR_INVAL;
    }
    pthread_mutex_lock(&q->lock);
    if (q->shutdown) {
        pthread_mutex_unlock(&q->lock);
        return IPC_ERR_STOPPED;
    }
    if (q->count == q->cap) {
        pthread_mutex_unlock(&q->lock);
        return IPC_ERR_AGAIN;
    }
    slot = (q->head + q->count) % q->cap;
    {
        ipc_task_t *t = &q->items[slot];
        memset(t, 0, sizeof(*t));
        t->hdr  = *hdr;
        t->cred = *cred;
        t->len  = len;
        if (len > 0) {
            t->payload = malloc(len);
            if (t->payload == NULL) {
                pthread_mutex_unlock(&q->lock);
                return IPC_ERR_NOMEM;
            }
            memcpy(t->payload, payload, len);
            t->owns_payload = 1;
        }
        (void)po;
    }
    q->count++;
    pthread_cond_signal(&q->cv);
    pthread_mutex_unlock(&q->lock);
    return IPC_OK;
}

int ipc_queue_pop(ipc_queue_t *q, ipc_task_t *out, int block)
{
    if (q == NULL || out == NULL) {
        return IPC_ERR_INVAL;
    }
    memset(out, 0, sizeof(*out));
    pthread_mutex_lock(&q->lock);
    while (q->count == 0) {
        if (q->shutdown) {
            pthread_mutex_unlock(&q->lock);
            return IPC_ERR_STOPPED;
        }
        if (!block) {
            pthread_mutex_unlock(&q->lock);
            return IPC_ERR_AGAIN;
        }
        pthread_cond_wait(&q->cv, &q->lock);
    }
    *out   = q->items[q->head];
    memset(&q->items[q->head], 0, sizeof(q->items[q->head]));
    q->head = (q->head + 1) % q->cap;
    q->count--;
    pthread_mutex_unlock(&q->lock);
    return IPC_OK;
}

void ipc_queue_shutdown(ipc_queue_t *q)
{
    if (q == NULL) {
        return;
    }
    pthread_mutex_lock(&q->lock);
    q->shutdown = 1;
    pthread_cond_broadcast(&q->cv);
    pthread_mutex_unlock(&q->lock);
}

int ipc_queue_count(ipc_queue_t *q)
{
    int n;

    if (q == NULL) {
        return 0;
    }
    pthread_mutex_lock(&q->lock);
    n = q->count;
    pthread_mutex_unlock(&q->lock);
    return n;
}

int ipc_queue_cap(const ipc_queue_t *q)
{
    return q ? q->cap : 0;
}
