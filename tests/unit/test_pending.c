/*
 * Copyright (c) 2026 <版权方待定>
 * 许可协议：<待定>
 *
 * test_pending.c -- ipc_pending 的白盒单测。**仅供测试**。
 *
 * 等待表是「多并发同步请求不串包」的唯一保证，所以这里除了正常的收发配对，
 * 重点覆盖**认领条件的三件套**：reqId、来源模块、实例代际号。任何一条不匹配
 * 都必须变成「无人认领」，而不是「凑合给一个等待者」。
 *
 * 它不碰 socket，所以超时、放弃、排空这些时序行为可以在这里稳定地测出来
 * —— 在真 socket 上测这些要依赖睡眠，那正是本仓库要避免的东西。
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ipc_pending.h"
#include "ipc/ipc.h"
#include "utest.h"

#define TEST_INSTANCE 0x1234567890abcdefull

/* 睡眠一小段。**只在测等待/超时本身时用**，不用来同步业务结果。 */
static void SleepMs(int32_t ms)
{
    struct timespec ts;

    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    (void)nanosleep(&ts, NULL);
}

/* ------------------------------------------------------------------ */
/* 初始化                                                             */
/* ------------------------------------------------------------------ */

UTEST_CASE(pending, init_defaults_and_destroy)
{
    IpcPending pending;

    UTEST_ASSERT_EQ(IpcPendingInit(NULL, 0, NULL), IPC_ERR_INVAL);

    /* capacity <= 0 取默认值。 */
    UTEST_ASSERT_EQ(IpcPendingInit(&pending, 0, NULL), IPC_OK);
    UTEST_ASSERT_EQ(pending.capacity, IPC_PENDING_DEFAULT_CAPACITY);
    UTEST_ASSERT_NOTNULL(pending.slots);
    UTEST_ASSERT_EQ(pending.liveSlots, 0);
    IpcPendingDestroy(&pending);

    UTEST_ASSERT_EQ(IpcPendingInit(&pending, 2, NULL), IPC_OK);
    UTEST_ASSERT_EQ(pending.capacity, 2);
    IpcPendingDestroy(&pending);

    /* NULL 上销毁是安全的。 */
    IpcPendingDestroy(NULL);
}

/* ------------------------------------------------------------------ */
/* 登记                                                               */
/* ------------------------------------------------------------------ */

UTEST_CASE(pending, add_assigns_increasing_req_ids_and_slots)
{
    IpcPending pending;
    int32_t    index0 = -1;
    int32_t    index1 = -1;
    uint64_t   reqId0 = 0;
    uint64_t   reqId1 = 0;
    uint8_t    buf[16];

    UTEST_ASSERT_EQ(IpcPendingInit(&pending, 4, NULL), IPC_OK);

    UTEST_ASSERT_EQ(IpcPendingAdd(&pending, "peer", TEST_INSTANCE, 100, buf,
                                  sizeof(buf), &index0, &reqId0),
                    IPC_OK);
    UTEST_ASSERT_EQ(IpcPendingAdd(&pending, "peer", TEST_INSTANCE, 100, buf,
                                  sizeof(buf), &index1, &reqId1),
                    IPC_OK);
    UTEST_ASSERT_EQ(index0, 0);
    UTEST_ASSERT_EQ(index1, 1);
    /* reqId 必须互不相同：否则并发请求之间没法区分。 */
    UTEST_ASSERT(reqId1 != reqId0);
    UTEST_ASSERT_EQ(pending.liveSlots, 2);

    IpcPendingRelease(&pending, index0);
    IpcPendingRelease(&pending, index1);
    UTEST_ASSERT_EQ(pending.liveSlots, 0);
    IpcPendingDestroy(&pending);
}

UTEST_CASE(pending, add_rejects_bad_arguments)
{
    IpcPending pending;
    int32_t    index = -1;
    uint64_t   reqId = 0;

    UTEST_ASSERT_EQ(IpcPendingInit(&pending, 2, NULL), IPC_OK);

    UTEST_ASSERT_EQ(IpcPendingAdd(NULL, "p", 1, 0, NULL, 0, &index, &reqId),
                    IPC_ERR_INVAL);
    UTEST_ASSERT_EQ(IpcPendingAdd(&pending, NULL, 1, 0, NULL, 0, &index, &reqId),
                    IPC_ERR_INVAL);
    UTEST_ASSERT_EQ(IpcPendingAdd(&pending, "p", 1, 0, NULL, 0, NULL, &reqId),
                    IPC_ERR_INVAL);
    UTEST_ASSERT_EQ(IpcPendingAdd(&pending, "p", 1, 0, NULL, 0, &index, NULL),
                    IPC_ERR_INVAL);
    /* replyBuf 为空却给了容量：矛盾的参数。 */
    UTEST_ASSERT_EQ(IpcPendingAdd(&pending, "p", 1, 0, NULL, 8, &index, &reqId),
                    IPC_ERR_INVAL);

    UTEST_ASSERT_EQ(pending.liveSlots, 0);
    IpcPendingDestroy(&pending);
}

UTEST_CASE(pending, add_reports_toomany_without_creating_a_slot)
{
    IpcPending pending;
    int32_t    index = -1;
    uint64_t   reqId = 0;
    int32_t    first = -1;
    uint64_t   firstId = 0;

    UTEST_ASSERT_EQ(IpcPendingInit(&pending, 1, NULL), IPC_OK);
    UTEST_ASSERT_EQ(IpcPendingAdd(&pending, "p", TEST_INSTANCE, 100, NULL, 0, &first,
                                  &firstId),
                    IPC_OK);

    index = -1;
    reqId = 0;
    UTEST_ASSERT_EQ(IpcPendingAdd(&pending, "p", TEST_INSTANCE, 100, NULL, 0, &index,
                                  &reqId),
                    IPC_ERR_TOOMANY);
    /* 表满时不许改动出参，也不许偷偷建槽位 —— 调用方要据此**不发送**。 */
    UTEST_ASSERT_EQ(index, -1);
    UTEST_ASSERT_EQ_U64(reqId, 0);
    UTEST_ASSERT_EQ(pending.liveSlots, 1);

    IpcPendingRelease(&pending, first);
    IpcPendingDestroy(&pending);
}

/* ------------------------------------------------------------------ */
/* 认领：三件套                                                       */
/* ------------------------------------------------------------------ */

UTEST_CASE(pending, complete_matches_on_reqid_source_and_instance)
{
    IpcPending pending;
    int32_t    index = -1;
    uint64_t   reqId = 0;
    int32_t    matched = 0;
    size_t     len = 0;
    size_t     full = 0;
    char       reply[32];
    int32_t    rc;

    UTEST_ASSERT_EQ(IpcPendingInit(&pending, 4, NULL), IPC_OK);
    UTEST_ASSERT_EQ(IpcPendingAdd(&pending, "peer", TEST_INSTANCE, 500, reply,
                                  sizeof(reply), &index, &reqId),
                    IPC_OK);

    IpcPendingComplete(&pending, "peer", TEST_INSTANCE, reqId, "pong", 5, &matched);
    UTEST_ASSERT_EQ(matched, 1);

    rc = IpcPendingWait(&pending, index, &len, &full);
    UTEST_ASSERT_EQ(rc, IPC_OK);
    UTEST_ASSERT_EQ_U64(len, 5);
    UTEST_ASSERT_EQ_U64(full, 5);
    UTEST_ASSERT_EQ(memcmp(reply, "pong", 5), 0);

    IpcPendingRelease(&pending, index);
    IpcPendingDestroy(&pending);
}

UTEST_CASE(pending, complete_rejects_wrong_source_module)
{
    IpcPending pending;
    int32_t    index = -1;
    uint64_t   reqId = 0;
    int32_t    matched = 1;

    UTEST_ASSERT_EQ(IpcPendingInit(&pending, 4, NULL), IPC_OK);
    UTEST_ASSERT_EQ(IpcPendingAdd(&pending, "peer", TEST_INSTANCE, 0, NULL, 0, &index,
                                  &reqId),
                    IPC_OK);

    /* 别的模块冒充回复：reqId 对得上也不行。 */
    IpcPendingComplete(&pending, "impostor", TEST_INSTANCE, reqId, "x", 1, &matched);
    UTEST_ASSERT_EQ(matched, 0);

    /* 正确的来源仍然能认领，说明上面那次没有把槽位弄坏。 */
    IpcPendingComplete(&pending, "peer", TEST_INSTANCE, reqId, "x", 1, &matched);
    UTEST_ASSERT_EQ(matched, 1);

    IpcPendingRelease(&pending, index);
    IpcPendingDestroy(&pending);
}

UTEST_CASE(pending, complete_rejects_stale_instance_and_wrong_reqid)
{
    IpcPending pending;
    int32_t    index = -1;
    uint64_t   reqId = 0;
    int32_t    matched = 1;

    UTEST_ASSERT_EQ(IpcPendingInit(&pending, 4, NULL), IPC_OK);
    UTEST_ASSERT_EQ(IpcPendingAdd(&pending, "peer", TEST_INSTANCE, 0, NULL, 0, &index,
                                  &reqId),
                    IPC_OK);

    /* 上一代实例留下的陈旧回复：instanceId 回显得不一样。 */
    IpcPendingComplete(&pending, "peer", TEST_INSTANCE + 1, reqId, "x", 1, &matched);
    UTEST_ASSERT_EQ(matched, 0);

    /* reqId 不对。 */
    IpcPendingComplete(&pending, "peer", TEST_INSTANCE, reqId + 1, "x", 1, &matched);
    UTEST_ASSERT_EQ(matched, 0);

    IpcPendingRelease(&pending, index);
    IpcPendingDestroy(&pending);
}

UTEST_CASE(pending, complete_twice_only_matches_once)
{
    IpcPending pending;
    int32_t    index = -1;
    uint64_t   reqId = 0;
    int32_t    matched = 0;

    UTEST_ASSERT_EQ(IpcPendingInit(&pending, 4, NULL), IPC_OK);
    UTEST_ASSERT_EQ(IpcPendingAdd(&pending, "peer", TEST_INSTANCE, 0, NULL, 0, &index,
                                  &reqId),
                    IPC_OK);

    IpcPendingComplete(&pending, "peer", TEST_INSTANCE, reqId, "a", 1, &matched);
    UTEST_ASSERT_EQ(matched, 1);
    IpcPendingComplete(&pending, "peer", TEST_INSTANCE, reqId, "b", 1, &matched);
    UTEST_ASSERT_EQ(matched, 0); /* 已经结束的槽位不再被认领 */

    IpcPendingRelease(&pending, index);
    IpcPendingDestroy(&pending);
}

UTEST_CASE(pending, complete_truncates_reply_to_caller_capacity)
{
    IpcPending pending;
    int32_t    index = -1;
    uint64_t   reqId = 0;
    int32_t    matched = 0;
    size_t     len = 0;
    size_t     full = 0;
    char       reply[4];
    const char kBig[] = "0123456789";

    UTEST_ASSERT_EQ(IpcPendingInit(&pending, 4, NULL), IPC_OK);
    UTEST_ASSERT_EQ(IpcPendingAdd(&pending, "peer", TEST_INSTANCE, 500, reply,
                                  sizeof(reply), &index, &reqId),
                    IPC_OK);

    IpcPendingComplete(&pending, "peer", TEST_INSTANCE, reqId, kBig, sizeof(kBig) - 1,
                       &matched);
    UTEST_ASSERT_EQ(matched, 1);
    UTEST_ASSERT_EQ(IpcPendingWait(&pending, index, &len, &full), IPC_OK);

    /*
     * len（写进去的）与 full（回复真实长度）必须分开报。
     * 上层就是靠这两个数不相等来判断「被截断了」，否则会把半条回复当完整回复用。
     */
    UTEST_ASSERT_EQ_U64(len, sizeof(reply));
    UTEST_ASSERT_EQ_U64(full, sizeof(kBig) - 1);
    UTEST_ASSERT_EQ(memcmp(reply, "0123", 4), 0);

    IpcPendingRelease(&pending, index);
    IpcPendingDestroy(&pending);
}

UTEST_CASE(pending, complete_with_null_out_pointer_and_null_payload)
{
    IpcPending pending;
    int32_t    index = -1;
    uint64_t   reqId = 0;
    size_t     len = 99;
    size_t     full = 99;

    UTEST_ASSERT_EQ(IpcPendingInit(&pending, 4, NULL), IPC_OK);
    /* replyBuf 传 NULL 表示「丢弃内容」，但请求本身仍然是有效的。 */
    UTEST_ASSERT_EQ(IpcPendingAdd(&pending, "peer", TEST_INSTANCE, 500, NULL, 0, &index,
                                  &reqId),
                    IPC_OK);

    IpcPendingComplete(&pending, "peer", TEST_INSTANCE, reqId, "payload", 7, NULL);
    UTEST_ASSERT_EQ(IpcPendingWait(&pending, index, &len, &full), IPC_OK);
    UTEST_ASSERT_EQ_U64(len, 0);  /* 没缓冲，写进去 0 字节 */
    UTEST_ASSERT_EQ_U64(full, 7); /* 但真实长度照报 */

    IpcPendingRelease(&pending, index);
    IpcPendingRelease(&pending, index);

    /* NULL 参数不应该崩（NULL 的 pending、NULL 的 src 都提前返回，不去碰锁）。 */
    IpcPendingComplete(&pending, NULL, 1, 1, "x", 1, NULL);
    IpcPendingComplete(NULL, "peer", 1, 1, "x", 1, NULL);

    IpcPendingDestroy(&pending);
}

/* ------------------------------------------------------------------ */
/* 超时与放弃                                                         */
/* ------------------------------------------------------------------ */

UTEST_CASE(pending, wait_times_out_and_pins_the_slot_as_finished)
{
    IpcPending pending;
    int32_t    index = -1;
    uint64_t   reqId = 0;
    size_t     len = 99;
    size_t     full = 99;
    int32_t    matched = 0;

    UTEST_ASSERT_EQ(IpcPendingInit(&pending, 4, NULL), IPC_OK);
    /* timeoutMs == 0：只探一次、不等待。 */
    UTEST_ASSERT_EQ(IpcPendingAdd(&pending, "peer", TEST_INSTANCE, 0, NULL, 0, &index,
                                  &reqId),
                    IPC_OK);
    UTEST_ASSERT_EQ(IpcPendingWait(&pending, index, &len, &full), IPC_ERR_TIMEOUT);
    UTEST_ASSERT_EQ_U64(len, 0);
    UTEST_ASSERT_EQ_U64(full, 0);

    /*
     * 超时之后、Release 之前那条迟到的回复**必须**被拒。
     * 如果不钉住 done，它会被算成一次成功匹配 —— 而调用方早就拿到超时了。
     */
    IpcPendingComplete(&pending, "peer", TEST_INSTANCE, reqId, "late", 4, &matched);
    UTEST_ASSERT_EQ(matched, 0);

    IpcPendingRelease(&pending, index);
    UTEST_ASSERT_EQ(pending.liveSlots, 0);
    /* Release 幂等。 */
    IpcPendingRelease(&pending, index);
    UTEST_ASSERT_EQ(pending.liveSlots, 0);
    IpcPendingDestroy(&pending);
}

UTEST_CASE(pending, wait_and_release_argument_checks)
{
    IpcPending pending;

    UTEST_ASSERT_EQ(IpcPendingInit(&pending, 2, NULL), IPC_OK);

    UTEST_ASSERT_EQ(IpcPendingWait(NULL, 0, NULL, NULL), IPC_ERR_INVAL);
    UTEST_ASSERT_EQ(IpcPendingWait(&pending, -1, NULL, NULL), IPC_ERR_INVAL);
    UTEST_ASSERT_EQ(IpcPendingWait(&pending, 2, NULL, NULL), IPC_ERR_INVAL);

    /* 越界/负下标的 Release 必须无害。 */
    IpcPendingRelease(&pending, -1);
    IpcPendingRelease(&pending, 99);
    IpcPendingRelease(NULL, 0);

    /* 空表上排空立即返回。 */
    IpcPendingWaitDrained(&pending);
    IpcPendingWaitDrained(NULL);
    UTEST_ASSERT_EQ(pending.liveSlots, 0);

    IpcPendingDestroy(&pending);
}

UTEST_CASE(pending, wait_returns_stopped_when_abort_flag_is_set)
{
    IpcPending     pending;
    _Atomic int32_t abortFlag = 0;
    int32_t         index = -1;
    uint64_t        reqId = 0;

    UTEST_ASSERT_EQ(IpcPendingInit(&pending, 2, &abortFlag), IPC_OK);

    /* 已经置位：不等。 */
    atomic_store(&abortFlag, 1);
    UTEST_ASSERT_EQ(IpcPendingAdd(&pending, "peer", TEST_INSTANCE, -1, NULL, 0, &index,
                                  &reqId),
                    IPC_OK);
    UTEST_ASSERT_EQ(IpcPendingWait(&pending, index, NULL, NULL), IPC_ERR_STOPPED);
    IpcPendingRelease(&pending, index);

    /* 置位之后也必须是 STOPPED，而且这一次要能立刻返回（不能睡在条件变量上）。 */
    atomic_store(&abortFlag, 0);
    UTEST_ASSERT_EQ(IpcPendingAdd(&pending, "peer", TEST_INSTANCE, -1, NULL, 0, &index,
                                  &reqId),
                    IPC_OK);
    atomic_store(&abortFlag, 1);
    IpcPendingWakeAll(&pending);
    UTEST_ASSERT_EQ(IpcPendingWait(&pending, index, NULL, NULL), IPC_ERR_STOPPED);
    IpcPendingRelease(&pending, index);

    /* WakeAll 在 NULL 上安全。 */
    IpcPendingWakeAll(NULL);
    IpcPendingDestroy(&pending);
}

/* ------------------------------------------------------------------ */
/* 真实等待（多线程）                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    IpcPending *pending;
    int32_t     index;
    int32_t     result;
    size_t      len;
    size_t      full;
} WaitArgs;

static void *WaitThread(void *arg)
{
    WaitArgs *args = (WaitArgs *)arg;

    args->result = IpcPendingWait(args->pending, args->index, &args->len, &args->full);
    return NULL;
}

UTEST_CASE(pending, waiter_wakes_up_on_completion)
{
    IpcPending pending;
    WaitArgs   args;
    pthread_t  thread;
    int32_t    index = -1;
    uint64_t   reqId = 0;
    int32_t    matched = 0;
    char       reply[16];

    UTEST_ASSERT_EQ(IpcPendingInit(&pending, 4, NULL), IPC_OK);
    UTEST_ASSERT_EQ(IpcPendingAdd(&pending, "peer", TEST_INSTANCE, 2000, reply,
                                  sizeof(reply), &index, &reqId),
                    IPC_OK);

    args.pending = &pending;
    args.index   = index;
    args.result  = 12345;
    args.len     = 0;
    args.full    = 0;
    UTEST_ASSERT_EQ(pthread_create(&thread, NULL, WaitThread, &args), 0);

    /*
     * 这里必须给一点真实时间，否则回复可能在等待者真正睡下之前就到了 ——
     * 那条路径（done 已经置位的快路径）本身也是要覆盖的，这一条用例
     * 故意走「先睡下、后唤醒」的慢路径。
     */
    SleepMs(30);
    IpcPendingComplete(&pending, "peer", TEST_INSTANCE, reqId, "hi", 2, &matched);
    UTEST_ASSERT_EQ(matched, 1);
    (void)pthread_join(thread, NULL);

    UTEST_ASSERT_EQ(args.result, IPC_OK);
    UTEST_ASSERT_EQ_U64(args.len, 2);
    UTEST_ASSERT_EQ(memcmp(reply, "hi", 2), 0);

    IpcPendingRelease(&pending, index);
    IpcPendingDestroy(&pending);
}

UTEST_CASE(pending, waiter_times_out_on_its_own)
{
    IpcPending pending;
    WaitArgs   args;
    pthread_t  thread;
    int32_t    index = -1;
    uint64_t   reqId = 0;

    UTEST_ASSERT_EQ(IpcPendingInit(&pending, 4, NULL), IPC_OK);
    UTEST_ASSERT_EQ(IpcPendingAdd(&pending, "peer", TEST_INSTANCE, 40, NULL, 0, &index,
                                  &reqId),
                    IPC_OK);

    args.pending = &pending;
    args.index   = index;
    args.result  = 12345;
    UTEST_ASSERT_EQ(pthread_create(&thread, NULL, WaitThread, &args), 0);
    (void)pthread_join(thread, NULL);

    /* 40ms 的时限到了自己醒，没人来叫它。 */
    UTEST_ASSERT_EQ(args.result, IPC_ERR_TIMEOUT);

    IpcPendingRelease(&pending, index);
    IpcPendingDestroy(&pending);
}

UTEST_CASE(pending, waiter_is_woken_by_abort)
{
    IpcPending      pending;
    _Atomic int32_t abortFlag = 0;
    WaitArgs        args;
    pthread_t       thread;
    int32_t         index = -1;
    uint64_t        reqId = 0;

    UTEST_ASSERT_EQ(IpcPendingInit(&pending, 4, &abortFlag), IPC_OK);
    /* 无限等待：只有外部中断能把它叫醒。 */
    UTEST_ASSERT_EQ(IpcPendingAdd(&pending, "peer", TEST_INSTANCE, -1, NULL, 0, &index,
                                  &reqId),
                    IPC_OK);

    args.pending = &pending;
    args.index   = index;
    args.result  = 12345;
    UTEST_ASSERT_EQ(pthread_create(&thread, NULL, WaitThread, &args), 0);

    SleepMs(30);
    atomic_store(&abortFlag, 1); /* 相当于 IpcRequestStop 先翻停止位 */
    IpcPendingWakeAll(&pending); /* 再唤醒 */
    (void)pthread_join(thread, NULL);

    UTEST_ASSERT_EQ(args.result, IPC_ERR_STOPPED);

    IpcPendingRelease(&pending, index);
    IpcPendingDestroy(&pending);
}

UTEST_CASE(pending, wait_drained_returns_after_all_slots_released)
{
    IpcPending pending;
    int32_t    index = -1;
    uint64_t   reqId = 0;

    UTEST_ASSERT_EQ(IpcPendingInit(&pending, 4, NULL), IPC_OK);
    UTEST_ASSERT_EQ(IpcPendingAdd(&pending, "peer", TEST_INSTANCE, -1, NULL, 0, &index,
                                  &reqId),
                    IPC_OK);
    UTEST_ASSERT_EQ(pending.liveSlots, 1);

    /* 都不放了：排空应当立刻返回，不能被卡住。
     * （IpcUnregister 用的正是「先 WakeAll 再 WaitDrained」这个顺序，
     *   而这里连 WakeAll 都不需要 —— 没有人在等。） */
    IpcPendingWaitDrained(&pending);

    IpcPendingRelease(&pending, index);
    IpcPendingWaitDrained(&pending);
    UTEST_ASSERT_EQ(pending.liveSlots, 0);
    IpcPendingDestroy(&pending);
}
