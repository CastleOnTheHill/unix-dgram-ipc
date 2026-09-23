/*
 * Copyright (c) 2026 <版权方待定>
 * 许可协议：<待定>
 *
 * ipc_internal.h -- 库实现内部共享声明。**不对外安装**。
 *
 * 单元测试为了做白盒测试可以直接包含本文件（《OpenHarmony C 语言编程规范》
 * 明确允许用 extern 声明引用内部函数做单测）。
 */
#ifndef IPC_INTERNAL_H
#define IPC_INTERNAL_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>

#include "ipc_config.h"
#include "ipc_log.h"
#include "ipc_pending.h"
#include "ipc_proto.h"
#include "ipc_util.h"
#include "ipc/ipc.h"

/* ------------------------------------------------------------------ */
/* 统计                                                               */
/* ------------------------------------------------------------------ */

/*
 * 统计字段的**唯一一处**定义。用它同时生成「内部原子存储」和「对外快照」，
 * 两边永远不会漂移 —— 加一个计数器只需要在这里加一行。
 * 顺序与 ipc.h 里 IpcStatistics 的声明顺序一致，便于人工核对。
 */
#define IPC_STAT_FIELDS(X)                                                     \
    X(sendAttempts)                                                            \
    X(sendEnqueued)                                                            \
    X(sendFailed)                                                              \
    X(broadcastTargets)                                                        \
    X(broadcastSkipped)                                                        \
    X(recvRead)                                                                \
    X(recvRejected)                                                            \
    X(recvRejCred)                                                             \
    X(recvRejProto)                                                            \
    X(recvRejTrunc)                                                            \
    X(recvDelivered)                                                           \
    X(dispatchInvoked)                                                         \
    X(dispatchFailed)                                                          \
    X(replySent)                                                               \
    X(replyMatched)                                                            \
    X(replyUnmatched)                                                          \
    X(pendingRejected)                                                         \
    X(pendingTimeout)                                                          \
    X(eagainCount)                                                             \
    X(deadlockProbes)

typedef struct {
#define IPC_STAT_DECLARE(field) _Atomic uint64_t field;
    IPC_STAT_FIELDS(IPC_STAT_DECLARE)
#undef IPC_STAT_DECLARE
} IpcStatsAtomic;

#define IPC_STAT_INC(ctx, field)                                               \
    atomic_fetch_add_explicit(&(ctx)->st.field, 1u, memory_order_relaxed)
#define IPC_STAT_ADD(ctx, field, value)                                        \
    atomic_fetch_add_explicit(&(ctx)->st.field, (uint64_t)(value),             \
                              memory_order_relaxed)

/* ------------------------------------------------------------------ */
/* 上下文                                                             */
/* ------------------------------------------------------------------ */

/*
 * 上下文布局。对外只有 IpcContext 这个不透明名字。
 *
 * 与旧原型的结构性差别（这是本次改造的核心）：
 *   - **没有** select 线程、没有工作线程、没有事件循环 → 字段里一个 pthread_t
 *     都没有；
 *   - **没有**回调注册表、没有回调队列 → 只有一个 dispatch 函数指针加一个
 *     透传的 user 指针；
 *   - 多了日志出口，因为「配置中的模块不存在」这类可观测性要求要落到
 *     真实系统的日志上（见 ipc.h 文末 Q12 的答复）。
 *
 * 保留的锁只有两把，各自职责单一，**没有嵌套获取**，因此不会死锁：
 *   - pending 内部的互斥 + 条件变量：等回复用；
 *   - lifeLock/lifeCv：IpcUnregister 的并发幂等用。
 */
struct IpcContext {
    /* ---- 身份 ---- */
    char     moduleId[IPC_NAME_MAX];
    char     ns[IPC_NS_MAX];
    char     path[IPC_PATH_MAX];
    char     lockPath[IPC_PATH_MAX + 8];
    char     groupName[IPC_NAME_MAX]; /* 空串 = 未指定属组 */
    uid_t    authorizedUid;           /* 配置表里声明的授权 UID */
    uid_t    realUid;                 /* 注册时本进程的 real UID */
    gid_t    groupGid;                /* 解析出的属组 GID */
    int32_t  haveGroup;               /* 1 = 要显式设置属组 */
    uint64_t instanceId;              /* 进程实例代际号 */
    pid_t    ownerPid;

    /* ---- 传输 ---- */
    int32_t fd;            /* 端点 fd；已拆除时为 -1 */
    int32_t lockFd;        /* 独占锁 fd；-1 表示未持有 */
    int32_t createdSocket; /* 1 = 端点路径是本实例创建的，拆除时 unlink */

    /* ---- 配置与选项（已从调用者内存拷进本结构） ---- */
    IpcConfig *config;
    uint32_t   maxPayload;
    int32_t    allowUidSplit;
    int32_t    broadcastIncludeSelf;
    int32_t    sndBufSize; /* 0 = 用内核默认 */
    int32_t    rcvBufSize; /* 0 = 用内核默认 */

    /* ---- 宿主注入 ---- */
    IpcDispatchFunc dispatch;
    void           *dispatchUser;
    IpcLogFunc      logFunc;
    void           *logUser;
    int32_t         logLevel; /* 0 = 沿用全局级别 */

    /* ---- 生命周期 ---- */
    _Atomic int32_t stopped;         /* IpcRequestStop 已调用 */
    _Atomic int32_t teardownStarted; /* IpcUnregister 已进入 */
    _Atomic int32_t teardownDone;    /* 拆除完成，只剩 IpcDestroy 能动 */
    _Atomic int32_t fatalError;      /* 端点进入不可恢复的故障态 */

    /*
     * 宿主的 select 线程有没有真的来收过报文。
     * 用途只有一个：IpcSend 在「从来没人收过」的上下文上等待时，几乎必然
     * 等不到回复（回复得先被读回来）。这时给一条 warning，比让人对着一个
     * 卡死的进程干等要好。**只提醒一次**，不重复刷。
     *
     * 注意它是提醒而不是错误：宿主的 select 线程完全可以在 IpcSend 已经在等
     * 之后才开始跑，那种情况下这个等待是正常的。
     */
    _Atomic int32_t readableServiced;
    _Atomic int32_t warnedNoDriver;

    pthread_mutex_t lifeLock;
    pthread_cond_t  lifeCv;

    /*
     * 三个「已经初始化过」的标记。它们存在的唯一理由是让注册失败时的回滚
     * 知道「哪些东西可以销毁」—— pthread_mutex_destroy 一个没 init 过的
     * 互斥量是未定义行为，不能靠猜。
     */
    int32_t pendingCreated;
    int32_t lifeLockCreated;
    int32_t lifeCvCreated;

    IpcPending pending;
    IpcStatsAtomic st;

    /* ---- 接收缓冲（只由宿主的 select 线程使用，不需要额外加锁） ---- */
    /*
     * 这三样是整库最要紧的内存安全约束的落点：缓冲大小固定为
     * `IPC_HDR_SIZE + ctx->maxPayload`，**永不**按报头里声明的 payloadLen
     * 去调整。报头是发送方说了算的，拿它当分配依据就是把对方的一个整数
     * 直接变成我们的一次 malloc。
     */
    uint8_t        *recvBuf;      /* 定长：IPC_HDR_SIZE + maxPayload */
    size_t          recvBufSize;
    uint8_t        *ctrlBuf;      /* 定长：IPC_CTRL_SIZE，恰好放一条 ucred */
    const void     *recvPayload;  /* 指向 recvBuf + IPC_HDR_SIZE，省得每处都算 */
    struct iovec    recvIov;
    struct msghdr   recvMsg;      /* 骨架在这里复用；msg_control 每条报文复位 */
    int32_t         recvMsgFlags; /* 本条报文的 msg_flags 副本 */
};

/* ------------------------------------------------------------------ */
/* 日志（带上下文）                                                   */
/* ------------------------------------------------------------------ */

/*
 * 带上下文的日志出口。ctx 可以为 NULL —— 那时退化成全局出口，
 * moduleId 按 NULL 处理。
 *
 * 出口选择规则：ctx 上有 per-module 出口就用它，否则用全局出口。
 * 级别也是同样规则：ctx->logLevel 为 0 时用全局级别。
 */
void IpcLogEmit(const IpcContext *ctx, int32_t level, const char *format, ...)
    IPC_PRINTF_LIKE(3, 4);

#define IPC_LOGD(ctx, ...) IpcLogEmit((ctx), (int32_t)IPC_LOG_DEBUG, __VA_ARGS__)
#define IPC_LOGI(ctx, ...) IpcLogEmit((ctx), (int32_t)IPC_LOG_INFO, __VA_ARGS__)
#define IPC_LOGW(ctx, ...) IpcLogEmit((ctx), (int32_t)IPC_LOG_WARN, __VA_ARGS__)
#define IPC_LOGE(ctx, ...) IpcLogEmit((ctx), (int32_t)IPC_LOG_ERROR, __VA_ARGS__)

/* ------------------------------------------------------------------ */
/* 回复路由                                                           */
/* ------------------------------------------------------------------ */

/*
 * 回复路由就是对外头文件里的 IpcReplyToken，**不再另有一个内部结构**。
 *
 * 早先的版本在这里定义了一个库内对象、把指针放进 message->opaque，而对象本身
 * 是 IpcDeliverToHost 的栈局部变量。那个设计只对「dispatch 内联跑回调」的宿主
 * 成立；对老系统那种「dispatch 投线程池」的宿主，worker 拿到的是一个已经失效
 * 的栈地址 —— 一次必然发生的 use-after-free。
 *
 * 现在的做法是让回复路由**按值**待在 IpcMessage 里（见 ipc.h 的 IpcReplyToken
 * 与 IpcMessage.reply）。代价是「每条最多回一次」这条规则跟着 token 副本走，
 * 收益是它对两种宿主形态都成立，而且库不需要去管理回调的生命周期。
 */

/* ------------------------------------------------------------------ */
/* 死锁检测                                                           */
/* ------------------------------------------------------------------ */

/*
 * 线程局部标记：非 NULL 表示「本线程正在 execute 宿主的 dispatch 调用」。
 *
 * 它精确地识别出「dispatch 是内联执行回调」这种宿主写法：那时回调就在
 * dispatch 的调用栈里跑，标记是命中的。而「dispatch 只是把报文投进线程池」
 * 的宿主，在回调真正执行时 dispatch 早就返回了，标记早已清掉 —— 于是
 * IpcSend 正常工作，这正是老系统的形态。
 *
 * 用一个精心放置的标记来区分两种线程模型，比加一个「宿主模式」配置项要好：
 * 配置项写错了不会报错，只会静默死锁。
 */
extern __thread IpcContext *IpcTlsDispatchContext;

/* ------------------------------------------------------------------ */
/* 内部接口                                                           */
/* ------------------------------------------------------------------ */

/* ipc_ctx.c */

/* 上下文是否还能收发：未停止、未拆除、端点未进入故障态。 */
int32_t IpcCheckAlive(const IpcContext *ctx);

/*
 * 由目标模块标识查出它的端点路径。找到返回配置项，否则返回 NULL。
 * uidOut（可传 NULL）带回配置里该模块的授权 UID。
 */
const IpcConfigEntry *IpcPeerEntry(IpcContext *ctx, const char *dstModuleId);

/*
 * 注册成功后的启动期健康检查：同命名空间里端点路径不存在的模块，
 * 聚合成一条 WARN。返回缺失模块的个数（供测试断言）。
 */
int32_t IpcWarnMissingPeers(IpcContext *ctx);

/* 把本实例的身份信息写进日志（INFO 级）。 */
void IpcLogIdentity(const IpcContext *ctx, const char *what);

/* ipc_io.c */

/* 完成「建端点」整条流程；任何一步失败都完整回滚。 */
int32_t IpcIoCreateEndpoint(IpcContext *ctx);
void    IpcIoDestroyEndpoint(IpcContext *ctx);

/* 统一的发送出口：填好报头的公共字段后发给 dstHeader 指定的目标路径。 */
int32_t IpcIoSendTo(IpcContext *ctx, const char *dstPath,
                    const IpcProtoHeader *header, const void *payload, size_t len);

/* 用本上下文的身份填一个报头的公共字段。 */
void IpcIoFillHeader(const IpcContext *ctx, IpcProtoHeader *header, uint8_t type,
                     const char *dstModuleId, uint32_t event, uint32_t payloadLen,
                     uint64_t reqId);

/* ipc_send.c */

/* IpcSend 与 IpcSendTimeout 的共同实现；timeoutMs < 0 表示无限等待。 */
int32_t IpcSendInternal(IpcContext *ctx, const char *dstModuleId, uint32_t event,
                        const void *data, size_t len, void *replyBuf, size_t replyCap,
                        size_t *outLen, int32_t timeoutMs);

/* ipc_recv.c */

/* 把一条已通过全部校验的报文交给宿主 dispatch。 */
int32_t IpcDeliverToHost(IpcContext *ctx, const IpcProtoHeader *header,
                         const IpcCred *cred, const void *payload, size_t payloadLen);

/* 校验来源模块的身份：凭据里的 real UID 必须与配置表中该 src 的授权 UID 一致。 */
int32_t IpcVerifySourceUid(IpcContext *ctx, const IpcProtoHeader *header,
                           const IpcCred *cred);

#endif /* IPC_INTERNAL_H */
