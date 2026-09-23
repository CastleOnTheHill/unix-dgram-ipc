/*
 * Copyright (c) 2026 <版权方待定>
 * 许可协议：<待定>
 *
 * ipc_send.c -- 发送侧：post / send / sendTimeout / broadcast / reply。
 *
 * 四条接口共用同一个出口（IpcIoSendTo），差别只在「报头怎么填、要不要等回复」。
 * 集中在一处的理由：发送路径上的错误映射、统计口径、以及 MSG_DONTWAIT 的契约
 * 只应该有一个版本。
 */
#include <string.h>

#include "ipc_internal.h"

/* ------------------------------------------------------------------ */
/* 异步发送                                                           */
/* ------------------------------------------------------------------ */

int32_t IpcPost(IpcContext *ctx, const char *dstModuleId, uint32_t event,
                const void *data, size_t len)
{
    const IpcConfigEntry *peer;
    IpcProtoHeader        header;
    int32_t               rc;

    if (dstModuleId == NULL || dstModuleId[0] == '\0') {
        return IPC_ERR_INVAL;
    }
    if (len > 0 && data == NULL) {
        return IPC_ERR_INVAL;
    }
    rc = IpcCheckAlive(ctx);
    if (rc != IPC_OK) {
        return rc;
    }
    if (len > (size_t)ctx->maxPayload) {
        /* 在发送动作之前就拒绝：不构造报文、不碰内核。 */
        IPC_STAT_INC(ctx, sendFailed);
        return IPC_ERR_MSGSIZE;
    }

    peer = IpcPeerEntry(ctx, dstModuleId);
    if (peer == NULL) {
        IPC_STAT_INC(ctx, sendFailed);
        return IPC_ERR_NOENT; /* 配置里没有这个模块，与「离线」不是一回事 */
    }

    IpcIoFillHeader(ctx, &header, (uint8_t)IPC_MSG_TYPE_POST, dstModuleId, event,
                    (uint32_t)len, 0);
    IPC_STAT_INC(ctx, sendAttempts);
    rc = IpcIoSendTo(ctx, peer->path, &header, data, len);
    if (rc == IPC_OK) {
        IPC_STAT_INC(ctx, sendEnqueued);
    } else {
        IPC_STAT_INC(ctx, sendFailed);
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* 同步发送                                                           */
/* ------------------------------------------------------------------ */

int32_t IpcSendInternal(IpcContext *ctx, const char *dstModuleId, uint32_t event,
                        const void *data, size_t len, void *replyBuf, size_t replyCap,
                        size_t *outLen, int32_t timeoutMs)
{
    const IpcConfigEntry *peer;
    IpcProtoHeader        header;
    int32_t               slotIndex = -1;
    uint64_t              reqId     = 0;
    size_t                gotLen    = 0;
    size_t                fullLen   = 0;
    int32_t               rc;

    if (outLen != NULL) {
        *outLen = 0; /* 无论走哪条路径，都要有一个确定的值 */
    }
    if (dstModuleId == NULL || dstModuleId[0] == '\0') {
        return IPC_ERR_INVAL;
    }
    if (len > 0 && data == NULL) {
        return IPC_ERR_INVAL;
    }
    if (replyBuf == NULL && replyCap != 0) {
        return IPC_ERR_INVAL;
    }
    rc = IpcCheckAlive(ctx);
    if (rc != IPC_OK) {
        return rc;
    }
    if (len > (size_t)ctx->maxPayload) {
        IPC_STAT_INC(ctx, sendFailed);
        return IPC_ERR_MSGSIZE;
    }

    /*
     * 死锁检测。必须放在最前面（早于任何真实动作），因为它的意义就是
     * 「提前拒绝」而不是「撞上去再报错」。
     *
     * 判定依据是线程局部标记：本线程正在跑宿主的 dispatch 调用。只有
     * 「dispatch 内联执行回调」的宿主会命中；投线程池的宿主不会（dispatch
     * 在回调执行前就已经返回了）。见 ipc_internal.h 里 IpcTlsDispatchContext
     * 的说明。
     */
    if (IpcTlsDispatchContext == ctx) {
        IPC_STAT_INC(ctx, deadlockProbes);
        IPC_LOGW(ctx,
                 "IpcSend() called from inside a dispatch callback on the same "
                 "thread; the reply could never be read back, refusing instead of "
                 "deadlocking. Use IpcReply() here, or move the callback to a worker "
                 "thread.");
        return IPC_ERR_DEADLOCK;
    }

    /*
     * 提醒（不是错误）：从来没有宿主线程来收过报文，那这条同步请求几乎必然
     * 等不到回复。宿主完全可以在我们开始等之后才启动它的 select 线程，所以
     * 不能升级成错误 —— 但让人对着一句 warning 去排查，总好过对着一个卡死的
     * 进程。
     */
    if (atomic_load_explicit(&ctx->readableServiced, memory_order_relaxed) == 0) {
        int32_t expected = 0;

        if (atomic_compare_exchange_strong(&ctx->warnedNoDriver, &expected, 1)) {
            IPC_LOGW(ctx,
                     "no IpcHandleReadable() has ever serviced this endpoint yet; "
                     "a synchronous send may block until the host's select loop runs");
        }
    }

    peer = IpcPeerEntry(ctx, dstModuleId);
    if (peer == NULL) {
        IPC_STAT_INC(ctx, sendFailed);
        return IPC_ERR_NOENT;
    }

    /*
     * 槽位必须在 sendto **之前**登记。
     * 反过来的话，对端再快也能抢在「请求方开始等」之前把回复发出来，
     * 那条回复就会撞上一个还没建立、或者属于上一次请求的槽位。
     * 期望的 instanceId 用**本进程自己**的代际号：回复要求原样回显它，
     * 于是上一个实例（或上一个进程）残留的陈旧回复会被自然淘汰。
     */
    rc = IpcPendingAdd(&ctx->pending, dstModuleId, ctx->instanceId, timeoutMs, replyBuf,
                       replyCap, &slotIndex, &reqId);
    if (rc != IPC_OK) {
        IPC_STAT_INC(ctx, pendingRejected);
        IPC_LOGW(ctx, "no free pending slot for '%s' (maxPending=%d)", dstModuleId,
                 ctx->pending.capacity);
        return rc; /* 表满，且**不发送** */
    }

    IpcIoFillHeader(ctx, &header, (uint8_t)IPC_MSG_TYPE_REQ, dstModuleId, event,
                    (uint32_t)len, reqId);
    IPC_STAT_INC(ctx, sendAttempts);
    rc = IpcIoSendTo(ctx, peer->path, &header, data, len);
    if (rc != IPC_OK) {
        IPC_STAT_INC(ctx, sendFailed);
        IpcPendingRelease(&ctx->pending, slotIndex); /* 没进等待，直接还回去 */
        return rc;
    }
    IPC_STAT_INC(ctx, sendEnqueued);

    rc = IpcPendingWait(&ctx->pending, slotIndex, &gotLen, &fullLen);
    IpcPendingRelease(&ctx->pending, slotIndex);

    if (rc == IPC_ERR_TIMEOUT) {
        IPC_STAT_INC(ctx, pendingTimeout);
    }
    if (rc == IPC_OK && fullLen > gotLen) {
        /* 回复比调用者的缓冲长：内容被截断了。这件事必须说出来 ——
         * 否则上层会把半条回复当成完整回复用下去。 */
        IPC_LOGW(ctx, "reply from '%s' truncated: %zu of %zu bytes kept", dstModuleId,
                 gotLen, fullLen);
    }
    if (outLen != NULL) {
        *outLen = gotLen;
    }
    return rc;
}

int32_t IpcSend(IpcContext *ctx, const char *dstModuleId, uint32_t event,
                const void *data, size_t len, void *replyBuf, size_t replyCap,
                size_t *outLen)
{
    /* 老接口的契约：默认无限等待。要时限请用 IpcSendTimeout。 */
    return IpcSendInternal(ctx, dstModuleId, event, data, len, replyBuf, replyCap,
                           outLen, -1);
}

int32_t IpcSendTimeout(IpcContext *ctx, const char *dstModuleId, uint32_t event,
                       const void *data, size_t len, void *replyBuf, size_t replyCap,
                       size_t *outLen, int32_t timeoutMs)
{
    return IpcSendInternal(ctx, dstModuleId, event, data, len, replyBuf, replyCap,
                           outLen, timeoutMs);
}

/* ------------------------------------------------------------------ */
/* 广播                                                               */
/* ------------------------------------------------------------------ */

int32_t IpcBroadcast(IpcContext *ctx, uint32_t event, const void *data, size_t len)
{
    IpcProtoHeader header;
    int32_t        total;
    int32_t        sent = 0;
    int32_t        i;
    int32_t        rc;

    if (len > 0 && data == NULL) {
        return IPC_ERR_INVAL;
    }
    rc = IpcCheckAlive(ctx);
    if (rc != IPC_OK) {
        return rc;
    }
    if (len > (size_t)ctx->maxPayload) {
        return IPC_ERR_MSGSIZE;
    }
    if (ctx->config == NULL) {
        return IPC_ERR_STATE;
    }

    /*
     * 目标集合是**配置表里同命名空间的固定集合**，不是「当前在线的模块」。
     * 因此这个函数既不原子、也不承诺所有目标同时成功：
     *   - 离线的目标直接跳过并计数，**不会排队补发**（补发会造成重复业务副作用）；
     *   - 部分成功不算错误，返回值就是成功收下报文的目的小数。
     */
    total = IpcConfigGetCount(ctx->config);
    for (i = 0; i < total; i++) {
        const IpcConfigEntry *entry = IpcConfigGetEntry(ctx->config, i);

        if (entry == NULL || strcmp(entry->ns, ctx->ns) != 0) {
            continue;
        }
        if (!ctx->broadcastIncludeSelf &&
            strcmp(entry->moduleId, ctx->moduleId) == 0) {
            continue;
        }

        IpcIoFillHeader(ctx, &header, (uint8_t)IPC_MSG_TYPE_POST, entry->moduleId, event,
                        (uint32_t)len, 0);
        IPC_STAT_INC(ctx, broadcastTargets);
        IPC_STAT_INC(ctx, sendAttempts);
        rc = IpcIoSendTo(ctx, entry->path, &header, data, len);
        if (rc == IPC_OK) {
            IPC_STAT_INC(ctx, sendEnqueued);
            sent++;
        } else if (rc == IPC_ERR_OFFLINE) {
            IPC_STAT_INC(ctx, broadcastSkipped);
        } else {
            IPC_STAT_INC(ctx, sendFailed);
        }
    }
    return sent;
}

/* ------------------------------------------------------------------ */
/* 回复                                                               */
/* ------------------------------------------------------------------ */

int32_t IpcReply(const IpcMessage *message, const void *data, size_t len)
{
    /*
     * 显式去 const：本函数只写 reply.replied 这一个记账字段，其余字段一概不动。
     * 这么写是为了让「宿主把 message 深拷一份丢给 worker，worker 拿着那份拷贝
     * 调 IpcReply」这条路径成立 —— 拷贝出来的对象本来就是可写的。
     */
    IpcReplyToken        *reply;
    IpcContext           *ctx;
    const IpcConfigEntry *peer;
    IpcProtoHeader        header;
    int32_t               rc;

    if (message == NULL) {
        return IPC_ERR_INVAL;
    }
    reply = (IpcReplyToken *)(uintptr_t)&message->reply;
    if (reply->ctx == NULL) {
        /* POST / REP 上没有回复路由，或者调用者拿了一个没有 reply 的假报文。 */
        return IPC_ERR_INVAL;
    }
    if (message->type != IPC_MSG_TYPE_REQ) {
        return IPC_ERR_INVAL; /* 只有请求才有「回复」可言 */
    }
    if (len > 0 && data == NULL) {
        return IPC_ERR_INVAL;
    }
    ctx = reply->ctx;
    if (reply->replied != 0) {
        return IPC_ERR_STATE; /* 同一份回复路由最多回一次 */
    }
    rc = IpcCheckAlive(ctx);
    if (rc != IPC_OK) {
        return rc;
    }
    if (len > (size_t)ctx->maxPayload) {
        IPC_STAT_INC(ctx, sendFailed);
        return IPC_ERR_MSGSIZE;
    }

    /*
     * 目标地址取自报文**自称**的来源模块。这一步是安全的，因为这条报文能走到
     * dispatch 就说明它的来源已经过了内核凭据校验（peerUid 与配置里该 src 的
     * 授权 UID 一致）。换句话说是「内核把 src 背书过了」，不是「我们信任 src」。
     */
    peer = IpcPeerEntry(ctx, reply->dstModuleId);
    if (peer == NULL) {
        return IPC_ERR_NOENT;
    }

    IpcIoFillHeader(ctx, &header, (uint8_t)IPC_MSG_TYPE_REP, reply->dstModuleId,
                    reply->event, (uint32_t)len, reply->reqId);
    /*
     * 关键：回显**请求方**的 instanceId，而不是我们自己的。
     * 请求方靠这个字段识别「这条回复是不是给我这一代实例的」—— 一个重启过的
     * 请求方必须拒绝上一代实例留下的陈旧回复。
     */
    header.instanceId = reply->dstInstanceId;

    rc = IpcIoSendTo(ctx, peer->path, &header, data, len);
    if (rc == IPC_OK) {
        reply->replied = 1;
        IPC_STAT_INC(ctx, replySent);
    }
    return rc;
}
