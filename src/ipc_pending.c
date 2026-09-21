#include "ipc_pending.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ipc_util.h"

int ipc_pending_init(ipc_pending_t *p, int cap)
{
    if (p == NULL || cap <= 0) {
        return IPC_ERR_INVAL;
    }
    memset(p, 0, sizeof(*p));
    p->slots = calloc((size_t)cap, sizeof(*p->slots));
    if (p->slots == NULL) {
        return IPC_ERR_NOMEM;
    }
    p->cap = cap;
    if (pthread_mutex_init(&p->lock, NULL) != 0 ||
        pthread_cond_init(&p->cv, NULL) != 0) {
        free(p->slots);
        p->slots = NULL;
        return IPC_ERR_IO;
    }
    return IPC_OK;
}

void ipc_pending_destroy(ipc_pending_t *p)
{
    if (p == NULL) {
        return;
    }
    pthread_cond_destroy(&p->cv);
    pthread_mutex_destroy(&p->lock);
    free(p->slots);
    p->slots = NULL;
}

int ipc_pending_add(ipc_pending_t *p, const char *dst, int64_t timeout_ms,
                    void *reply_buf, size_t reply_cap,
                    int *out_idx, uint64_t *out_id)
{
    int    i;
    int    rc = IPC_OK;

    if (p == NULL || dst == NULL || out_idx == NULL || out_id == NULL) {
        return IPC_ERR_INVAL;
    }
    pthread_mutex_lock(&p->lock);
    if (p->shutting_down) {
        rc = IPC_ERR_STOPPED;
        goto out;
    }
    for (i = 0; i < p->cap; i++) {
        if (!p->slots[i].in_use) {
            break;
        }
    }
    if (i == p->cap) {
        rc = IPC_ERR_TOOMANY;
        goto out;
    }
    {
        ipc_pending_slot_t *s = &p->slots[i];
        memset(s, 0, sizeof(*s));
        s->in_use      = 1;
        s->reply_buf   = reply_buf;
        s->reply_cap   = reply_cap;
        s->req_id      = ++p->next_req_id;
        s->owner_pid   = getpid();
        s->deadline_ns = 0;
        ipc_strlcpy(s->dst, dst, sizeof(s->dst));
        if (timeout_ms >= 0) {
            s->deadline_ns = ipc_mono_ns() + (uint64_t)timeout_ms * 1000000ull;
        }
        *out_idx = i;
        *out_id  = s->req_id;
        p->used++;
    }
out:
    pthread_mutex_unlock(&p->lock);
    return rc;
}

void ipc_pending_release(ipc_pending_t *p, int idx)
{
    if (p == NULL || idx < 0 || idx >= p->cap) {
        return;
    }
    pthread_mutex_lock(&p->lock);
    if (p->slots[idx].in_use) {
        p->slots[idx].in_use = 0;
        p->used--;
    }
    pthread_mutex_unlock(&p->lock);
}

int ipc_pending_complete(ipc_pending_t *p, uint64_t req_id, const char *src,
                         const void *data, size_t len)
{
    int i;
    int rc = IPC_ERR_NOENT;

    if (p == NULL || src == NULL) {
        return IPC_ERR_INVAL;
    }
    pthread_mutex_lock(&p->lock);
    for (i = 0; i < p->cap; i++) {
        ipc_pending_slot_t *s = &p->slots[i];
        if (!s->in_use || s->done) {
            continue;
        }
        if (s->req_id != req_id) {
            continue;
        }
        if (strcmp(s->dst, src) != 0) {
            continue; /* reply from the wrong module: not ours */
        }
        if (s->reply_buf != NULL && s->reply_cap > 0) {
            if (len > s->reply_cap) {
                s->rc        = IPC_ERR_MSGSIZE;
                s->reply_len = len; /* tell the caller how much is needed */
            } else {
                if (len > 0 && data != NULL) {
                    memcpy(s->reply_buf, data, len);
                }
                s->rc        = IPC_OK;
                s->reply_len = len;
            }
        } else {
            s->rc        = IPC_OK;
            s->reply_len = len;
        }
        s->done = 1;
        rc      = IPC_OK;
        break;
    }
    if (rc == IPC_OK) {
        pthread_cond_broadcast(&p->cv);
    }
    pthread_mutex_unlock(&p->lock);
    return rc;
}

int ipc_pending_wait(ipc_pending_t *p, int idx, size_t *reply_len_out)
{
    int rc = IPC_OK;

    if (p == NULL || idx < 0 || idx >= p->cap) {
        return IPC_ERR_INVAL;
    }
    pthread_mutex_lock(&p->lock);
    for (;;) {
        ipc_pending_slot_t *s = &p->slots[idx];

        if (p->shutting_down) {
            rc = IPC_ERR_STOPPED;
            break;
        }
        if (s->done) {
            rc = s->rc;
            break;
        }
        if (s->deadline_ns != 0) {
            uint64_t now = ipc_mono_ns();
            if (now >= s->deadline_ns) {
                s->done     = 1;
                s->rc       = IPC_ERR_TIMEOUT;
                s->reply_len = 0;
                rc          = IPC_ERR_TIMEOUT;
                break;
            }
            {
                struct timespec ts;
                uint64_t        left = s->deadline_ns - now;
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_sec += (time_t)(left / 1000000000ull);
                ts.tv_nsec += (long)(left % 1000000000ull);
                if (ts.tv_nsec >= 1000000000L) {
                    ts.tv_sec += 1;
                    ts.tv_nsec -= 1000000000L;
                }
                pthread_cond_timedwait(&p->cv, &p->lock, &ts);
            }
        } else {
            pthread_cond_wait(&p->cv, &p->lock);
        }
    }
    if (reply_len_out != NULL) {
        *reply_len_out = p->slots[idx].reply_len;
    }
    pthread_mutex_unlock(&p->lock);
    return rc;
}

void ipc_pending_shutdown(ipc_pending_t *p)
{
    int i;

    if (p == NULL) {
        return;
    }
    pthread_mutex_lock(&p->lock);
    p->shutting_down = 1;
    for (i = 0; i < p->cap; i++) {
        if (p->slots[i].in_use && !p->slots[i].done) {
            p->slots[i].done      = 1;
            p->slots[i].rc        = IPC_ERR_STOPPED;
            p->slots[i].reply_len = 0;
        }
    }
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->lock);
}

int ipc_pending_used(ipc_pending_t *p)
{
    int n;

    if (p == NULL) {
        return 0;
    }
    pthread_mutex_lock(&p->lock);
    n = p->used;
    pthread_mutex_unlock(&p->lock);
    return n;
}

int ipc_pending_cap(const ipc_pending_t *p)
{
    return p ? p->cap : 0;
}
