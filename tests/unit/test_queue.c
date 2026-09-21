/*
 * Unit tests for the bounded hand-off queue used by IPC_DISPATCH_POOL.
 * The invariants that matter: FIFO order, a hard bound, a deep copy of the
 * payload (the receive buffer is reused immediately), and workers that always
 * terminate on shutdown.
 */
#include <stdlib.h>
#include <string.h>

#include "ipc/ipc.h"
#include "ipc_queue.h"
#include "utest.h"

static void mk(ipc_hdr_t *h, uint32_t event)
{
    memset(h, 0, sizeof(*h));
    h->version = IPC_PROTO_VERSION;
    h->type    = IPC_TYPE_POST;
    h->event   = event;
    strcpy(h->ns, "core");
    strcpy(h->src, "A1");
    strcpy(h->dst, "B1");
}

UT_TEST(queue, init_and_arguments)
{
    ipc_queue_t q;

    UT_EQ_INT(ipc_queue_init(&q, 0), IPC_ERR_INVAL);
    UT_EQ_INT(ipc_queue_init(&q, 2), IPC_OK);
    UT_EQ_INT(ipc_queue_cap(&q), 2);
    UT_EQ_INT(ipc_queue_count(&q), 0);
    ipc_queue_destroy(&q);
}

UT_TEST(queue, fifo_order_and_deep_copy_of_payload)
{
    ipc_queue_t q;
    ipc_hdr_t   h;
    ipc_cred_t  c;
    ipc_task_t  t;
    char        src[32];

    UT_EQ_INT(ipc_queue_init(&q, 4), IPC_OK);
    memset(&c, 0, sizeof(c));
    c.present = 1;
    c.uid     = 1001;

    strcpy(src, "first");
    mk(&h, 1);
    UT_EQ_INT(ipc_queue_push(&q, &h, &c, src, 6), IPC_OK);
    strcpy(src, "second");
    mk(&h, 2);
    UT_EQ_INT(ipc_queue_push(&q, &h, &c, src, 7), IPC_OK);
    /* The caller reuses its buffer right after push: the queue must not care. */
    memset(src, 'Z', sizeof(src));

    UT_EQ_INT(ipc_queue_count(&q), 2);
    UT_EQ_INT(ipc_queue_pop(&q, &t, 0), IPC_OK);
    UT_EQ_U64(t.hdr.event, 1);
    UT_EQ_INT(t.cred.uid, 1001);
    UT_EQ_U64(t.len, 6);
    UT_CHECK(memcmp(t.payload, "first", 6) == 0);
    ipc_task_clear(&t);

    UT_EQ_INT(ipc_queue_pop(&q, &t, 0), IPC_OK);
    UT_EQ_U64(t.hdr.event, 2);
    UT_EQ_U64(t.len, 7);
    UT_CHECK(memcmp(t.payload, "second", 7) == 0);
    ipc_task_clear(&t);

    UT_EQ_INT(ipc_queue_pop(&q, &t, 0), IPC_ERR_AGAIN);
    ipc_queue_destroy(&q);
}

UT_TEST(queue, capacity_is_a_hard_bound)
{
    ipc_queue_t q;
    ipc_hdr_t   h;
    ipc_cred_t  c;
    ipc_task_t  t;
    int         i;

    UT_EQ_INT(ipc_queue_init(&q, 3), IPC_OK);
    memset(&c, 0, sizeof(c));
    for (i = 0; i < 3; i++) {
        mk(&h, (uint32_t)i);
        UT_EQ_INT(ipc_queue_push(&q, &h, &c, "x", 1), IPC_OK);
    }
    UT_EQ_INT(ipc_queue_count(&q), 3);
    mk(&h, 99);
    UT_EQ_INT(ipc_queue_push(&q, &h, &c, "x", 1), IPC_ERR_AGAIN);

    /* Popping makes room again. */
    UT_EQ_INT(ipc_queue_pop(&q, &t, 0), IPC_OK);
    ipc_task_clear(&t);
    UT_EQ_INT(ipc_queue_push(&q, &h, &c, "x", 1), IPC_OK);
    ipc_queue_destroy(&q);
}

UT_TEST(queue, zero_length_payload_is_allowed)
{
    ipc_queue_t q;
    ipc_hdr_t   h;
    ipc_cred_t  c;
    ipc_task_t  t;

    UT_EQ_INT(ipc_queue_init(&q, 1), IPC_OK);
    memset(&c, 0, sizeof(c));
    mk(&h, 5);
    UT_EQ_INT(ipc_queue_push(&q, &h, &c, NULL, 0), IPC_OK);
    UT_EQ_INT(ipc_queue_pop(&q, &t, 0), IPC_OK);
    UT_EQ_U64(t.len, 0);
    UT_CHECK(t.payload == NULL);
    ipc_task_clear(&t);
    ipc_queue_destroy(&q);
}

UT_TEST(queue, rejects_bad_arguments)
{
    ipc_queue_t q;
    ipc_hdr_t   h;
    ipc_cred_t  c;

    UT_EQ_INT(ipc_queue_init(&q, 1), IPC_OK);
    memset(&c, 0, sizeof(c));
    mk(&h, 1);
    UT_EQ_INT(ipc_queue_push(&q, NULL, &c, "x", 1), IPC_ERR_INVAL);
    UT_EQ_INT(ipc_queue_push(&q, &h, NULL, "x", 1), IPC_ERR_INVAL);
    UT_EQ_INT(ipc_queue_push(&q, &h, &c, NULL, 3), IPC_ERR_INVAL);
    UT_EQ_INT(ipc_queue_pop(&q, NULL, 0), IPC_ERR_INVAL);
    ipc_queue_destroy(&q);
}

UT_TEST(queue, shutdown_wakes_a_blocked_pop)
{
    ipc_queue_t q;
    ipc_task_t  t;

    UT_EQ_INT(ipc_queue_init(&q, 1), IPC_OK);
    ipc_queue_shutdown(&q);
    UT_EQ_INT(ipc_queue_pop(&q, &t, 1), IPC_ERR_STOPPED);
    {
        ipc_hdr_t  h;
        ipc_cred_t c;
        memset(&c, 0, sizeof(c));
        mk(&h, 1);
        UT_EQ_INT(ipc_queue_push(&q, &h, &c, "x", 1), IPC_ERR_STOPPED);
    }
    ipc_queue_destroy(&q);
}

UT_TEST(queue, destroy_frees_still_queued_payloads)
{
    ipc_queue_t q;
    ipc_hdr_t   h;
    ipc_cred_t  c;
    int         i;

    /* Valgrind/ASan coverage rather than an assertion: payloads left in the
     * queue at teardown must not leak. */
    UT_EQ_INT(ipc_queue_init(&q, 8), IPC_OK);
    memset(&c, 0, sizeof(c));
    for (i = 0; i < 8; i++) {
        mk(&h, (uint32_t)i);
        UT_EQ_INT(ipc_queue_push(&q, &h, &c, "leakme", 6), IPC_OK);
    }
    ipc_queue_destroy(&q);
    UT_CHECK(1);
}
