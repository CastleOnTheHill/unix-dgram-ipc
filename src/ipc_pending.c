/*
 * Copyright (c) 2026 <版权方待定>
 * 许可协议：<待定>
 *
 * ipc_pending.c -- 同步请求等待表的实现。
 *
 * 只依赖 pthread 互斥/条件变量，不感知 socket，因此可以脱离内核完整单测。
 */
#include "ipc_pending.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ipc_util.h"

/* ------------------------------------------------------------------ */
/* 内部小工具                                                         */
/* ------------------------------------------------------------------ */

/*
 * 取单调时钟的绝对时间点，供 pthread_cond_timedwait 使用。
 * 条件变量初始化时已经绑到 CLOCK_MONOTONIC，两边必须一致 —— 混用
 * CLOCK_REALTIME 的等待会立刻返回 ETIMEDOUT，是个很难看的 bug。
 */
static int32_t MonoDeadlineAfter(int32_t timeoutMs, uint64_t *outNs)
{
    uint64_t now = IpcMonoNs();
    uint64_t delta;

    if (timeoutMs < 0) {
        *outNs = 0; /* 无限等待 */
        return IPC_OK;
    }
    delta = (uint64_t)timeoutMs * 1000000ull;
    *outNs = now + delta;
    if (*outNs <= now) {
        *outNs = now + 1; /* 保证单调前进，0 有特殊含义不能用 */
    }
    return IPC_OK;
}

static void MonoNsToTimespec(uint64_t ns, struct timespec *ts)
{
    ts->tv_sec  = (time_t)(ns / 1000000000ull);
    ts->tv_nsec = (long)(ns % 1000000000ull);
}

/* ------------------------------------------------------------------ */
/* 生命周期                                                           */
/* ------------------------------------------------------------------ */

int32_t IpcPendingInit(IpcPending *pending, int32_t capacity,
                       _Atomic int32_t *abortFlag)
{
    pthread_condattr_t attr;
    int32_t            rc;

    if (pending == NULL) {
        return IPC_ERR_INVAL;
    }
    memset(pending, 0, sizeof(*pending));

    if (capacity <= 0) {
        capacity = IPC_PENDING_DEFAULT_CAPACITY;
    }
    pending->slots =
        (IpcPendingSlot *)calloc((size_t)capacity, sizeof(*pending->slots));
    if (pending->slots == NULL) {
        return IPC_ERR_NOMEM;
    }
    pending->capacity   = capacity;
    pending->nextReqId  = 1;
    pending->abortFlag  = abortFlag;

    rc = pthread_mutex_init(&pending->lock, NULL);
    if (rc != 0) {
        free(pending->slots);
        pending->slots = NULL;
        return IPC_ERR_IO;
    }
    rc = pthread_condattr_init(&attr);
    if (rc != 0) {
        (void)pthread_mutex_destroy(&pending->lock);
        free(pending->slots);
        pending->slots = NULL;
        return IPC_ERR_IO;
    }
    /*
     * 把条件变量绑到单调时钟。用墙钟的话，一次 NTP 校时或手工改时间就能让
     * 「等 100 ms」变成「立刻超时」或「等一年」。
     */
    rc = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if (rc == 0) {
        rc = pthread_cond_init(&pending->cv, &attr);
    }
    (void)pthread_condattr_destroy(&attr);
    if (rc != 0) {
        (void)pthread_mutex_destroy(&pending->lock);
        free(pending->slots);
        pending->slots = NULL;
        return IPC_ERR_IO;
    }
    return IPC_OK;
}

void IpcPendingDestroy(IpcPending *pending)
{
    if (pending == NULL) {
        return;
    }
    if (pending->slots != NULL) {
        free(pending->slots);
        pending->slots = NULL;
    }
    (void)pthread_cond_destroy(&pending->cv);
    (void)pthread_mutex_destroy(&pending->lock);
}

/* ------------------------------------------------------------------ */
/* 登记 / 等待 / 释放                                                 */
/* ------------------------------------------------------------------ */

int32_t IpcPendingAdd(IpcPending *pending, const char *dstModuleId,
                      uint64_t expectedInstanceId, int32_t timeoutMs, void *replyBuf,
                      size_t replyCap, int32_t *outIndex, uint64_t *outReqId)
{
    int32_t  index = -1;
    int32_t  i;
    int32_t  rc;
    uint64_t deadline = 0;

    if (pending == NULL || dstModuleId == NULL || outIndex == NULL || outReqId == NULL) {
        return IPC_ERR_INVAL;
    }
    if (replyBuf == NULL && replyCap != 0) {
        return IPC_ERR_INVAL;
    }

    (void)pthread_mutex_lock(&pending->lock);
    for (i = 0; i < pending->capacity; i++) {
        if (!pending->slots[i].inUse) {
            index = i;
            break;
        }
    }
    if (index < 0) {
        (void)pthread_mutex_unlock(&pending->lock);
        return IPC_ERR_TOOMANY;
    }

    rc = MonoDeadlineAfter(timeoutMs, &deadline);
    if (rc != IPC_OK) {
        (void)pthread_mutex_unlock(&pending->lock);
        return rc;
    }

    {
        IpcPendingSlot *slot = &pending->slots[index];

        memset(slot, 0, sizeof(*slot));
        slot->inUse       = 1;
        slot->reqId       = pending->nextReqId;
        slot->expectedInstanceId = expectedInstanceId;
        slot->replyBuf    = replyBuf;
        slot->replyCap    = (replyBuf != NULL) ? replyCap : 0;
        slot->replyLen    = 0;
        slot->fullLen     = 0;
        slot->done        = 0;
        slot->result      = IPC_OK;
        slot->deadlineNs  = deadline;
        (void)IpcStrlcpy(slot->dstModuleId, dstModuleId, sizeof(slot->dstModuleId));

        pending->nextReqId++;
        pending->liveSlots++;
        *outReqId = slot->reqId;
    }
    (void)pthread_mutex_unlock(&pending->lock);

    *outIndex = index;
    return IPC_OK;
}

int32_t IpcPendingWait(IpcPending *pending, int32_t index, size_t *outLen,
                       size_t *outFullLen)
{
    IpcPendingSlot *slot;
    int32_t         result;

    if (pending == NULL || index < 0 || index >= pending->capacity) {
        return IPC_ERR_INVAL;
    }
    slot = &pending->slots[index];

    (void)pthread_mutex_lock(&pending->lock);
    for (;;) {
        if (slot->done != 0) {
            result = slot->result;
            break;
        }
        if (pending->abortFlag != NULL &&
            atomic_load_explicit(pending->abortFlag, memory_order_relaxed) != 0) {
            result = IPC_ERR_STOPPED;
            break;
        }
        if (slot->deadlineNs == 0) {
            (void)pthread_cond_wait(&pending->cv, &pending->lock);
            continue;
        }
        {
            uint64_t        now = IpcMonoNs();
            struct timespec ts;

            if (slot->deadlineNs != 0 && now >= slot->deadlineNs) {
                result = IPC_ERR_TIMEOUT;
                break;
            }
            MonoNsToTimespec(slot->deadlineNs, &ts);
            /* ETIMEDOUT 不在这里判定，交给下一轮循环统一处理，
             * 这样「超时的同时回复刚好到达」的竞态只有一处裁决点。 */
            (void)pthread_cond_timedwait(&pending->cv, &pending->lock, &ts);
        }
    }

    if (result != IPC_OK) {
        /*
         * 把槽位钉成「已结束」。否则在调用者拿到超时之后、IpcPendingRelease
         * 之前的这个窗口里，一条迟到的回复仍然会认领这个槽位，被统计成
         * replyMatched —— 而调用者明明已经收到 TIMEOUT 了。
         */
        slot->done   = 1;
        slot->result = result;
    }
    (void)pthread_mutex_unlock(&pending->lock);

    if (outLen != NULL) {
        *outLen = slot->replyLen;
    }
    if (outFullLen != NULL) {
        *outFullLen = slot->fullLen;
    }
    return result;
}

void IpcPendingRelease(IpcPending *pending, int32_t index)
{
    if (pending == NULL || index < 0 || index >= pending->capacity) {
        return;
    }
    (void)pthread_mutex_lock(&pending->lock);
    if (pending->slots[index].inUse != 0) {
        memset(&pending->slots[index], 0, sizeof(pending->slots[index]));
        pending->liveSlots--;
        if (pending->liveSlots < 0) {
            pending->liveSlots = 0; /* 理论不可达，防呆 */
        }
        (void)pthread_cond_broadcast(&pending->cv); /* 让排空等待者有机会醒来 */
    }
    (void)pthread_mutex_unlock(&pending->lock);
}

void IpcPendingWaitDrained(IpcPending *pending)
{
    if (pending == NULL) {
        return;
    }
    (void)pthread_mutex_lock(&pending->lock);
    while (pending->liveSlots > 0) {
        (void)pthread_cond_wait(&pending->cv, &pending->lock);
    }
    (void)pthread_mutex_unlock(&pending->lock);
}

/* ------------------------------------------------------------------ */
/* 回复认领                                                           */
/* ------------------------------------------------------------------ */

void IpcPendingComplete(IpcPending *pending, const char *srcModuleId,
                        uint64_t instanceId, uint64_t reqId, const void *payload,
                        size_t len, int32_t *outMatched)
{
    int32_t i;

    if (outMatched != NULL) {
        *outMatched = 0;
    }
    if (pending == NULL || srcModuleId == NULL) {
        return;
    }

    (void)pthread_mutex_lock(&pending->lock);
    for (i = 0; i < pending->capacity; i++) {
        IpcPendingSlot *slot = &pending->slots[i];
        size_t          copyLen;

        if (slot->inUse == 0 || slot->done != 0) {
            continue;
        }
        if (slot->reqId != reqId) {
            continue;
        }
        if (strcmp(slot->dstModuleId, srcModuleId) != 0) {
            continue; /* 别的模块冒充回复 */
        }
        if (slot->expectedInstanceId != instanceId) {
            continue; /* 上一代实例残留的陈旧回复 */
        }

        slot->fullLen = len;
        copyLen       = 0;
        if (slot->replyBuf != NULL && slot->replyCap > 0 && len > 0 && payload != NULL) {
            copyLen = (len < slot->replyCap) ? len : slot->replyCap; /* 截断 */
            memcpy(slot->replyBuf, payload, copyLen);
        }
        slot->replyLen = copyLen;
        slot->result   = IPC_OK;
        slot->done     = 1;
        (void)pthread_cond_broadcast(&pending->cv);
        if (outMatched != NULL) {
            *outMatched = 1;
        }
        break;
    }
    (void)pthread_mutex_unlock(&pending->lock);
}

void IpcPendingWakeAll(IpcPending *pending)
{
    if (pending == NULL) {
        return;
    }
    (void)pthread_mutex_lock(&pending->lock);
    (void)pthread_cond_broadcast(&pending->cv);
    (void)pthread_mutex_unlock(&pending->lock);
}
