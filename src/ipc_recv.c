/*
 * Copyright (c) 2026 <版权方待定>
 * 许可协议：<待定>
 *
 * ipc_recv.c -- 接收侧：把端点上的报文读干净、逐条校验、交给宿主。
 *
 * 本文件是「不可信输入」的唯一入口。写下这里的每一条校验时都按同一个前提：
 * **报头和载荷全部由发送方控制**，只有内核填的 SCM_CREDENTIALS 例外。
 *
 * 因此最重要的两条不变式是：
 *   1. 接收缓冲只按 `IPC_HDR_SIZE + 自己的 maxPayload` 这个**固定尺寸**准备，
 *      绝不按报头里声明的 payloadLen 去分配内存；
 *   2. 超长报文靠 recvmsg 的 MSG_TRUNC 检测出来丢弃，**不是**先收下来再量。
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "ipc_internal.h"

/* 线程局部死锁标记的定义所在（声明在 ipc_internal.h）。 */
__thread IpcContext *IpcTlsDispatchContext;

/* ------------------------------------------------------------------ */
/* 来源身份校验                                                       */
/* ------------------------------------------------------------------ */

/*
 * 用**内核给的 real UID** 去比对配置表里该来源模块声明的授权 UID。
 *
 * 这里为什么不能用 effective UID：内核的 SCM_CREDENTIALS 只填 real UID，
 * 对端的 effective UID 根本拿不到。所以接收侧永远按 real UID 走，这一点
 * 与 IpcModuleOptions.allowUidSplit 无关（那个开关只管注册时的自检）。
 *
 * 顺带说明「src 可以信任」这句话的确切含义：src 是发送方**自称**的，它之所
 * 以可信，是因为我们能拿它查到配置里声明的 UID，再用内核给的真实 UID 对上。
 * 换句话说，是内核给 src 背了书，而不是我们信任发送方。
 */
int32_t IpcVerifySourceUid(IpcContext *ctx, const IpcProtoHeader *header,
                           const IpcCred *cred)
{
    const IpcConfigEntry *src;

    if (ctx == NULL || header == NULL || cred == NULL || cred->present == 0) {
        return IPC_ERR_CRED;
    }
    src = IpcConfigFindModule(ctx->config, header->ns, header->src);
    if (src == NULL) {
        /* 自称了一个配置里根本不存在的模块 —— 可能是别的命名空间的模块
         * 走错了路，也可能是在试探。两种都不该放行。 */
        IPC_LOGW(ctx, "rejecting '%s': sender claims module '%s' in ns '%s' which is "
                      "not in the module table",
                 header->src, header->src, header->ns);
        return IPC_ERR_CRED;
    }
    if (src->uid != cred->uid) {
        IPC_LOGW(ctx, "rejecting '%s' (ns=%s): peer real uid %u != authorized uid %u",
                 header->src, header->ns, (unsigned)cred->uid, (unsigned)src->uid);
        return IPC_ERR_CRED;
    }
    return IPC_OK;
}

/* ------------------------------------------------------------------ */
/* 交给宿主                                                           */
/* ------------------------------------------------------------------ */

int32_t IpcDeliverToHost(IpcContext *ctx, const IpcProtoHeader *header,
                         const IpcCred *cred, const void *payload, size_t payloadLen)
{
    IpcMessage message;
    int32_t    rc;

    memset(&message, 0, sizeof(message));

    message.ns         = header->ns;   /* 以下四个指针只在本次调用期间有效 */
    message.src        = header->src;
    message.dst        = header->dst;
    message.event      = header->event;
    message.type       = (IpcMsgType)header->type;
    message.reqId      = header->reqId;
    message.instanceId = header->instanceId;
    message.data       = payload;
    message.len        = payloadLen;
    message.peerUid    = cred->uid;
    message.peerPid    = cred->pid;

    /*
     * 回复路由按值填进 message。这条 REQ 的回复目标、要回显的代际号与 reqId
     * 都在这里确定，宿主把 message 深拷一份留给 worker 之后仍然能回。
     * 非 REQ 报文让 reply.ctx 保持 NULL —— IpcReply 据此拒绝。
     */
    if (header->type == (uint8_t)IPC_MSG_TYPE_REQ) {
        message.reply.ctx = ctx;
        message.reply.reqId         = header->reqId;
        message.reply.event         = header->event;
        message.reply.dstInstanceId = header->instanceId; /* 要回显对方的代际号 */
        message.reply.replied       = 0;
        (void)IpcStrlcpy(message.reply.dstModuleId, header->src,
                         sizeof(message.reply.dstModuleId));
    }

    IPC_STAT_INC(ctx, recvDelivered);

    if (ctx->dispatch == NULL) {
        /*
         * 自测模式：注册时没给分发入口。
         * 报文到这里就为止了 —— 它通过了全部校验（这一点很重要，测试要能
         * 区分「被校验拒绝」和「没有业务层」），但没有人处理它。
         * 不计 dispatchInvoked：那个计数器的定义是「dispatch 被调用的次数」。
         */
        IPC_LOGD(ctx, "no dispatch installed; dropping %s from %s", header->dst,
                 header->src);
        return IPC_OK;
    }

    /*
     * 置线程局部标记，让回调里误用 IpcSend 时能立刻拿到 IPC_ERR_DEADLOCK。
     * 保存/恢复旧值是为了支持嵌套：一个 dispatch 里间接又驱动了另一个上下文
     * 的接收，那时得各自有各自的标记。
     */
    {
        IpcContext *saved = IpcTlsDispatchContext;

        IpcTlsDispatchContext = ctx;
        rc = ctx->dispatch(ctx, &message, ctx->dispatchUser);
        IpcTlsDispatchContext = saved;
    }

    IPC_STAT_INC(ctx, dispatchInvoked);
    if (rc != IPC_OK) {
        /*
         * 只计数，不影响后续报文的处理：丢不丢、重不重试，是宿主自己的决定。
         * 库在这里越权替宿主决定「这条报文算失败」会破坏宿主自己的语义。
         */
        IPC_STAT_INC(ctx, dispatchFailed);
        IPC_LOGW(ctx, "dispatch returned %s for event %u from '%s'",
                 IpcResultToString(rc), header->event, header->src);
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* 可读处理                                                           */
/* ------------------------------------------------------------------ */

/*
 * 单条报文的处理。返回 1 表示「算作本次交给宿主的条数」，0 表示被丢弃。
 */
static int32_t HandleOneDatagram(IpcContext *ctx, ssize_t received)
{
    IpcProtoHeader header;
    IpcCred        credential;
    size_t         payloadLen;
    int32_t        rc;

    IPC_STAT_INC(ctx, recvRead);

    /* 1) 截断。放在最前面判：一旦被截断，后面所有基于长度的判断都不可信。 */
    if ((ctx->recvMsgFlags & MSG_TRUNC) != 0) {
        IPC_STAT_INC(ctx, recvRejTrunc);
        IPC_STAT_INC(ctx, recvRejected);
        IPC_LOGW(ctx, "received a datagram larger than this module's maxPayload "
                      "(%u); dropped. Raise maxPayload for ALL modules in ns '%s'.",
                 ctx->maxPayload, ctx->ns);
        return 0;
    }

    /* 2) 连报头都不够。 */
    if (received < (ssize_t)IPC_HDR_SIZE) {
        IPC_STAT_INC(ctx, recvRejProto);
        IPC_STAT_INC(ctx, recvRejected);
        IPC_LOGW(ctx, "datagram shorter than the %d-byte header (%zd); dropped",
                 IPC_HDR_SIZE, received);
        return 0;
    }

    /* 3) 报头解析（magic / 版本 / 类型 / flags / hdrSize / 名字合法性）。 */
    rc = IpcProtoDecode(ctx->recvBuf, (size_t)received, &header);
    if (rc != IPC_OK) {
        IPC_STAT_INC(ctx, recvRejProto);
        IPC_STAT_INC(ctx, recvRejected);
        IPC_LOGW(ctx, "malformed header (%s); dropped", IpcResultToString(rc));
        return 0;
    }

    /* 4) 声明的载荷长度必须与实际收到的一致。 */
    payloadLen = (size_t)received - (size_t)IPC_HDR_SIZE;
    if ((size_t)header.payloadLen != payloadLen) {
        IPC_STAT_INC(ctx, recvRejProto);
        IPC_STAT_INC(ctx, recvRejected);
        IPC_LOGW(ctx, "header says payloadLen=%u but %zu bytes arrived; dropped",
                 header.payloadLen, payloadLen);
        return 0;
    }

    /* 5) 地址校验：命名空间与我一致，且 dst 确实是我。 */
    if (strcmp(header.ns, ctx->ns) != 0) {
        IPC_STAT_INC(ctx, recvRejProto);
        IPC_STAT_INC(ctx, recvRejected);
        IPC_LOGW(ctx, "ns mismatch: got '%s', I am '%s'; dropped", header.ns, ctx->ns);
        return 0;
    }
    if (strcmp(header.dst, ctx->moduleId) != 0) {
        IPC_STAT_INC(ctx, recvRejProto);
        IPC_STAT_INC(ctx, recvRejected);
        IPC_LOGW(ctx, "misrouted: addressed to '%s', I am '%s'; dropped", header.dst,
                 ctx->moduleId);
        return 0;
    }

    /* 6) 凭据。这是唯一的身份来源，取不到就不放行。 */
    rc = IpcProtoCredFromMsg(&ctx->recvMsg, ctx->recvMsgFlags, &credential);
    if (rc != IPC_OK) {
        IPC_STAT_INC(ctx, recvRejCred);
        IPC_STAT_INC(ctx, recvRejected);
        IPC_LOGW(ctx, "no usable sender credentials (%s); dropped",
                 IpcResultToString(rc));
        return 0;
    }

    /* 7) 来源身份：内核给的 real UID 必须对上配置里该模块的授权 UID。 */
    rc = IpcVerifySourceUid(ctx, &header, &credential);
    if (rc != IPC_OK) {
        IPC_STAT_INC(ctx, recvRejCred);
        IPC_STAT_INC(ctx, recvRejected);
        return 0;
    }

    /* 8) REP 不进业务层，它属于同步请求的匹配表。 */
    if (header.type == (uint8_t)IPC_MSG_TYPE_REP) {
        int32_t matched = 0;

        IpcPendingComplete(&ctx->pending, header.src, header.instanceId, header.reqId,
                           ctx->recvPayload, payloadLen, &matched);
        if (matched != 0) {
            IPC_STAT_INC(ctx, replyMatched);
        } else {
            /* 迟到、来源不符、或者实例代际不符。三者对统计是同一件事。 */
            IPC_STAT_INC(ctx, replyUnmatched);
            IPC_LOGD(ctx, "unclaimed reply from '%s' (reqId=%llu instance=%016llx)",
                     header.src, (unsigned long long)header.reqId,
                     (unsigned long long)header.instanceId);
        }
        return 0; /* 不算「交给宿主」的条数 */
    }

    (void)IpcDeliverToHost(ctx, &header, &credential, ctx->recvPayload, payloadLen);
    return 1;
}

int32_t IpcHandleReadable(IpcContext *ctx, int32_t maxCount)
{
    int32_t processed = 0;
    int32_t reads     = 0;

    if (ctx == NULL) {
        return IPC_ERR_INVAL;
    }
    if (atomic_load_explicit(&ctx->stopped, memory_order_relaxed) != 0) {
        return IPC_ERR_STOPPED; /* 宿主应当据此收摊 */
    }
    if (ctx->fd < 0 || ctx->recvBuf == NULL) {
        return IPC_ERR_STATE;
    }

    atomic_store_explicit(&ctx->readableServiced, 1, memory_order_relaxed);

    for (;;) {
        ssize_t received;

        /* 每条报文都要把控制缓冲的长度复位：recvmsg 成功后会把它改成实际
         * 长度，不复位的话第二条报文就只能收到一小截辅助数据。 */
        ctx->recvMsg.msg_control    = ctx->ctrlBuf;
        ctx->recvMsg.msg_controllen = IPC_CTRL_SIZE;
        ctx->recvMsg.msg_flags      = 0;
        ctx->recvMsgFlags           = 0;

        received = recvmsg(ctx->fd, &ctx->recvMsg, MSG_DONTWAIT);
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break; /* 读干净了 */
            }
            if (errno == EINTR) {
                continue; /* 被信号打断，接着读 */
            }
            /*
             * 其余错误说明端点本身出问题了（EBADF / ENOTCONN / ...）。
             * 记成故障态并返回，让宿主知道该收摊 —— 继续转圈只会一直报同一个错。
             */
            IPC_LOGE(ctx, "recvmsg failed: %s", IpcErrnoString(errno));
            atomic_store_explicit(&ctx->fatalError, 1, memory_order_relaxed);
            return IpcErrnoToResult(errno);
        }

        /* 把 msg_flags 抄出来：HandleOneDatagram 里要用，而下一次循环就会清掉。 */
        ctx->recvMsgFlags = ctx->recvMsg.msg_flags;

        reads++;
        processed += HandleOneDatagram(ctx, received);

        /*
         * 上限按**读取次数**算，不按交付条数算：被校验丢弃的报文同样
         * 消耗 select 线程的时间。若只数交付，对端灌一段全垃圾的报文
         * 就能让 processed 恒为 0、上限永不命中，select 线程被钉死在
         * 本函数里，宿主其余的 fd 全部饿死。
         */
        if (maxCount > 0 && reads >= maxCount) {
            break; /* 上限到了：返回值是交付数，可能小于 maxCount */
        }
    }
    return processed;
}
