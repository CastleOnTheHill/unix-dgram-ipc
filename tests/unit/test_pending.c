/*
 * Unit tests for the bounded outstanding-request table.
 *
 * The rules under test are the ones that decide whether a synchronous caller
 * can be wedged or woken with somebody else's data:
 *   - a request is matched on (req_id, source module), never on req_id alone
 *   - a completed slot is never completed twice
 *   - a too-small reply buffer is reported as IPC_ERR_MSGSIZE with the size
 *     that would have been needed
 *   - shutdown wakes every waiter instead of leaving it hanging forever
 */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ipc/ipc.h"
#include "ipc_pending.h"
#include "utest.h"

UT_TEST(pending, init_and_arguments)
{
    ipc_pending_t p;

    UT_EQ_INT(ipc_pending_init(&p, 0), IPC_ERR_INVAL);
    UT_EQ_INT(ipc_pending_init(&p, -1), IPC_ERR_INVAL);
    UT_EQ_INT(ipc_pending_init(&p, 4), IPC_OK);
    UT_EQ_INT(ipc_pending_cap(&p), 4);
    UT_EQ_INT(ipc_pending_used(&p), 0);
    ipc_pending_destroy(&p);
}

UT_TEST(pending, full_table_is_refused_with_toomany)
{
    ipc_pending_t p;
    int           idx, i;
    uint64_t      id;

    UT_EQ_INT(ipc_pending_init(&p, 3), IPC_OK);
    for (i = 0; i < 3; i++) {
        UT_EQ_INT(ipc_pending_add(&p, "B1", -1, NULL, 0, &idx, &id), IPC_OK);
    }
    UT_EQ_INT(ipc_pending_used(&p), 3);
    UT_EQ_INT(ipc_pending_add(&p, "B1", -1, NULL, 0, &idx, &id),
              IPC_ERR_TOOMANY);

    ipc_pending_release(&p, 0);
    UT_EQ_INT(ipc_pending_used(&p), 2);
    UT_EQ_INT(ipc_pending_add(&p, "B1", -1, NULL, 0, &idx, &id), IPC_OK);
    ipc_pending_destroy(&p);
}

UT_TEST(pending, request_ids_are_unique_and_increasing)
{
    ipc_pending_t p;
    int           idx, i;
    uint64_t      id, prev = 0;

    UT_EQ_INT(ipc_pending_init(&p, 8), IPC_OK);
    for (i = 0; i < 8; i++) {
        UT_EQ_INT(ipc_pending_add(&p, "B1", -1, NULL, 0, &idx, &id), IPC_OK);
        UT_CHECK_MSG(id > prev, "req_id %llu not greater than %llu",
                     (unsigned long long)id, (unsigned long long)prev);
        prev = id;
    }
    ipc_pending_destroy(&p);
}

UT_TEST(pending, matching_requires_the_right_source_module)
{
    ipc_pending_t p;
    int           idx;
    uint64_t      id;
    size_t        rlen = 0;

    UT_EQ_INT(ipc_pending_init(&p, 2), IPC_OK);
    UT_EQ_INT(ipc_pending_add(&p, "B1", 200, NULL, 0, &idx, &id), IPC_OK);

    /* A reply from C1 carrying a stolen req_id must not complete the call. */
    UT_EQ_INT(ipc_pending_complete(&p, id, "C1", "x", 1), IPC_ERR_NOENT);
    UT_EQ_INT(ipc_pending_wait(&p, idx, &rlen), IPC_ERR_TIMEOUT);
    ipc_pending_release(&p, idx);

    /* Nor may an unrelated req_id from the right module. */
    UT_EQ_INT(ipc_pending_add(&p, "B1", 200, NULL, 0, &idx, &id), IPC_OK);
    UT_EQ_INT(ipc_pending_complete(&p, id + 1000, "B1", "x", 1),
              IPC_ERR_NOENT);
    UT_EQ_INT(ipc_pending_wait(&p, idx, &rlen), IPC_ERR_TIMEOUT);
    ipc_pending_destroy(&p);
}

UT_TEST(pending, completion_is_at_most_once)
{
    ipc_pending_t p;
    int           idx;
    uint64_t      id;
    char          buf[16];
    size_t        rlen = 0;

    UT_EQ_INT(ipc_pending_init(&p, 2), IPC_OK);
    UT_EQ_INT(ipc_pending_add(&p, "B1", 200, buf, sizeof(buf), &idx, &id),
              IPC_OK);
    UT_EQ_INT(ipc_pending_complete(&p, id, "B1", "first", 5), IPC_OK);
    /* A duplicate/late reply must be reported as unmatched, not overwrite. */
    UT_EQ_INT(ipc_pending_complete(&p, id, "B1", "second", 6), IPC_ERR_NOENT);

    UT_EQ_INT(ipc_pending_wait(&p, idx, &rlen), IPC_OK);
    UT_EQ_U64(rlen, 5);
    UT_CHECK(memcmp(buf, "first", 5) == 0);
    ipc_pending_destroy(&p);
}

UT_TEST(pending, undersized_reply_buffer_reports_needed_size)
{
    ipc_pending_t p;
    int           idx;
    uint64_t      id;
    char          buf[4];
    size_t        rlen = 0;

    UT_EQ_INT(ipc_pending_init(&p, 2), IPC_OK);
    memset(buf, 0x5a, sizeof(buf)); /* sentinel */
    UT_EQ_INT(ipc_pending_add(&p, "B1", 200, buf, sizeof(buf), &idx, &id),
              IPC_OK);
    UT_EQ_INT(ipc_pending_complete(&p, id, "B1", "0123456789", 10), IPC_OK);
    UT_EQ_INT(ipc_pending_wait(&p, idx, &rlen), IPC_ERR_MSGSIZE);
    UT_EQ_U64(rlen, 10); /* the caller can learn how big the reply was */
    /* Nothing was copied at all: a truncated reply must never be handed back
     * as if it were complete. */
    UT_CHECK(memcmp(buf, "\x5a\x5a\x5a\x5a", 4) == 0);
    ipc_pending_release(&p, idx);
    ipc_pending_destroy(&p);
}

UT_TEST(pending, null_reply_buffer_still_succeeds)
{
    ipc_pending_t p;
    int           idx;
    uint64_t      id;
    size_t        rlen = 0;

    UT_EQ_INT(ipc_pending_init(&p, 2), IPC_OK);
    UT_EQ_INT(ipc_pending_add(&p, "B1", 200, NULL, 0, &idx, &id), IPC_OK);
    UT_EQ_INT(ipc_pending_complete(&p, id, "B1", "anything", 8), IPC_OK);
    UT_EQ_INT(ipc_pending_wait(&p, idx, &rlen), IPC_OK);
    UT_EQ_U64(rlen, 8);
    ipc_pending_release(&p, idx);
    ipc_pending_destroy(&p);
}

UT_TEST(pending, shutdown_wakes_every_waiter)
{
    ipc_pending_t p;
    int           idx1, idx2;
    uint64_t      id1, id2;

    UT_EQ_INT(ipc_pending_init(&p, 2), IPC_OK);
    UT_EQ_INT(ipc_pending_add(&p, "B1", -1, NULL, 0, &idx1, &id1), IPC_OK);
    UT_EQ_INT(ipc_pending_add(&p, "B1", -1, NULL, 0, &idx2, &id2), IPC_OK);

    ipc_pending_shutdown(&p);
    UT_EQ_INT(ipc_pending_wait(&p, idx1, NULL), IPC_ERR_STOPPED);
    UT_EQ_INT(ipc_pending_wait(&p, idx2, NULL), IPC_ERR_STOPPED);
    /* No new request may be admitted once shutdown started. */
    UT_EQ_INT(ipc_pending_add(&p, "B1", -1, NULL, 0, &idx1, &id1),
              IPC_ERR_STOPPED);
    ipc_pending_destroy(&p);
}

/* ------------------------------------------------------------------ */
/* concurrency: many waiters, replies arriving in a scrambled order     */
/* ------------------------------------------------------------------ */

#define CONC 4

typedef struct {
    ipc_pending_t  *p;
    int             n;
    pthread_mutex_t lock;
    pthread_cond_t  cv;
    int             published;
    int             idx[CONC];
    uint64_t        id[CONC];
    int             rc[CONC];
    char            got[CONC][32];
} conc_t;

static void *conc_waiter(void *arg)
{
    conc_t *c = arg;
    int     me;
    size_t  rlen = 0;

    pthread_mutex_lock(&c->lock);
    me = c->n;
    c->n++;
    pthread_mutex_unlock(&c->lock);

    /* The request id must be obtained before waiting, exactly like ipc_send()
     * does it: register first, send second. */
    if (ipc_pending_add(c->p, "B1", 3000, c->got[me], sizeof(c->got[me]),
                        &c->idx[me], &c->id[me]) != IPC_OK) {
        c->rc[me] = -1000;
        return NULL;
    }
    pthread_mutex_lock(&c->lock);
    c->published++;
    pthread_cond_broadcast(&c->cv);
    pthread_mutex_unlock(&c->lock);

    c->rc[me] = ipc_pending_wait(c->p, c->idx[me], &rlen);
    ipc_pending_release(c->p, c->idx[me]);
    return NULL;
}

UT_TEST(pending, concurrent_waiters_are_matched_correctly)
{
    ipc_pending_t   p;
    conc_t          c;
    pthread_t       th[CONC];
    int             i;
    struct timespec deadline;

    UT_EQ_INT(ipc_pending_init(&p, CONC), IPC_OK);
    memset(&c, 0, sizeof(c));
    c.p = &p;
    pthread_mutex_init(&c.lock, NULL);
    pthread_cond_init(&c.cv, NULL);

    for (i = 0; i < CONC; i++) {
        UT_EQ_INT(pthread_create(&th[i], NULL, conc_waiter, &c), 0);
    }

    /* Wait for all four to be registered.  This is a real condition wait with
     * a 5 s ceiling -- not a "sleep and hope" . */
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 5;
    pthread_mutex_lock(&c.lock);
    while (c.published < CONC) {
        if (pthread_cond_timedwait(&c.cv, &c.lock, &deadline) != 0) {
            break;
        }
    }
    pthread_mutex_unlock(&c.lock);
    UT_CHECK_MSG(c.published == CONC, "only %d of %d requests registered",
                 c.published, CONC);

    /* Answer them out of order to prove the matching is by id, not position. */
    for (i = CONC - 1; i >= 0; i--) {
        char body[32];
        snprintf(body, sizeof(body), "reply-%d", i);
        UT_EQ_INT(ipc_pending_complete(&p, c.id[i], "B1", body, strlen(body)),
                  IPC_OK);
    }
    for (i = 0; i < CONC; i++) {
        pthread_join(th[i], NULL);
        UT_EQ_INT(c.rc[i], IPC_OK);
    }
    for (i = 0; i < CONC; i++) {
        char want[32];
        snprintf(want, sizeof(want), "reply-%d", i);
        UT_EQ_STR(c.got[i], want);
    }
    pthread_cond_destroy(&c.cv);
    pthread_mutex_destroy(&c.lock);
    ipc_pending_destroy(&p);
}
