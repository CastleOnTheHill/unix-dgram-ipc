/*
 * Copyright (c) 2026 <版权方待定>
 * 许可协议：<待定>
 *
 * test_forward.c -- 白盒转发测试：真的建两个上下文、真的投递、真的回复。
 * **仅供测试**。
 *
 * =====================================================================
 * 这一组和 test_lifecycle 的分工
 * =====================================================================
 *   test_lifecycle —— 只走「注册 / 注销 / 销毁」这条线，报文一条都不发。
 *   本文件         —— 在**已经注册好的两个上下文之间**真的收发，覆盖
 *                     ipc_send.c 与 ipc_recv.c 的全部主路径：
 *                     登记槽位 → sendto → recvmsg → 逐条校验 → dispatch
 *                     → IpcReply → 回复读回 → 等待表匹配。
 *
 * 为什么它是白盒：它直接包含 src/ipc_internal.h，用内部结构体读统计、
 * 用 IpcProtoEncode 手工构造线格式报文（用来喂**畸形**输入）。
 * 对外接口测不到那些分支 —— 畸形的报文只能由内部编码器造出来。
 *
 * 需要的权限：不需要 root。同 uid 下两个模块互相投递是本设计的正常用法。
 *
 * 同步手段：**不用固定 sleep 做判定**。需要并发的地方用一个驱动线程
 * 反复调 IpcHandleReadable（这正是宿主 select 线程要干的事），主线程负责
 * 发与等；判定点是「库自己的统计/返回值」，不是「睡够时间了大概到了吧」。
 * 唯一的例外是超时用例 —— 那个用例测的就是「时间到了就返回超时」。
 */
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "ipc/ipc.h"
#include "ipc_internal.h" /* 白盒：统计、IpcProtoHeader、IpcProtoEncode */
#include "lab.h"
#include "utest.h"

/* ------------------------------------------------------------------ */
/* 实验台                                                             */
/* ------------------------------------------------------------------ */

#define FWD_NS "fwdns"

typedef struct {
    char dir[256];
    char conf[512];
    char pathA[512];
    char pathB[512];
    char pathC[512];
} FwdLab;

typedef int32_t (*FwdDispatch)(IpcContext *ctx, const IpcMessage *message,
                               void *user);

/* 造一个实验目录 + 一份配置（模块名与 uid 都给定）。 */
static int32_t FwdSetup(FwdLab *lab, const char *const *modules, int32_t count)
{
    char   text[1024];
    size_t written;

    memset(lab, 0, sizeof(*lab));
    if (IpcLabCreate(lab->dir, sizeof(lab->dir)) != 0) {
        return -1;
    }
    written = IpcLabBuildConf(lab->dir, FWD_NS, modules, count, text, sizeof(text));
    if (written == 0) {
        IpcLabRemove(lab->dir);
        return -1;
    }
    IpcLabConfPath(lab->dir, lab->conf, sizeof(lab->conf));
    IpcLabSockPath(lab->dir, "alpha", lab->pathA, sizeof(lab->pathA));
    IpcLabSockPath(lab->dir, "beta", lab->pathB, sizeof(lab->pathB));
    IpcLabSockPath(lab->dir, "gamma", lab->pathC, sizeof(lab->pathC));
    IpcLabSilenceLog();
    return (IpcLabWriteFile(lab->conf, text) == 0) ? 0 : -1;
}

static void FwdTeardown(FwdLab *lab)
{
    IpcLabCaptureEnd();
    IpcLabRemove(lab->dir);
}

static int32_t FwdRegister(const FwdLab *lab, const char *module, uint32_t maxPayload,
                           FwdDispatch dispatch, void *user, IpcContext **outCtx)
{
    IpcModuleOptions options = IPC_MODULE_OPTIONS_INIT;

    options.moduleId   = module;
    options.ns         = FWD_NS;
    options.confPath   = lab->conf;
    options.maxPayload = maxPayload;
    options.dispatch   = dispatch;
    options.dispatchUser = user;
    return IpcRegister(&options, outCtx);
}

/*
 * 把已经注册的上下文拆干净。顺序不能反：先 IpcUnregister 再 IpcDestroy，
 * 而且 IpcDestroy 必须在 Unregister 返回之后（返回即代表拆除完成）。
 */
static void FwdRetire(IpcContext *ctx)
{
    if (ctx != NULL) {
        (void)IpcUnregister(ctx);
        (void)IpcDestroy(ctx);
    }
}

static void FwdTakeStats(IpcContext *ctx, IpcStatistics *out)
{
    (void)IpcGetStatistics(ctx, out);
}

/* ------------------------------------------------------------------ */
/* 业务侧记录桶                                                       */
/* ------------------------------------------------------------------ */

/*
 * 一个「什么都记下来」的 dispatch 实现。测试要断言的东西都落在这里，
 * 不去读 stderr、不去猜。replyMode 让它可选地在 dispatch 内直接回复
 * （这正是 ipc.h 说的两种合法用法里的第一种）。
 */
typedef struct {
    int32_t    calls;
    int32_t    lastRc;
    char       ns[64];
    char       src[64];
    char       dst[64];
    uint32_t   event;
    IpcMsgType type;
    uint64_t   reqId;
    uint64_t   instanceId;
    uid_t      peerUid;
    size_t     len;
    uint8_t    data[1024];

    /* replyMode: 0 = 不回；1 = 在 dispatch 里直接 IpcReply；
     *            2 = 连续回两次（验证「同一份 token 最多回一次」）。 */
    int32_t    replyMode;
    const char *replyText;
    int32_t    replyRc;
    int32_t    replyCalls;
    int32_t    secondReplyRc;
    int32_t    secondReplyTried;

    /* 想在 dispatch 里调一次 IpcSend（用来验证死锁检测） */
    int32_t    trySend;
    IpcContext *sendCtx;
    const char *sendDst; /* NULL = "alpha"；跨上下文用例选不在配置表里的名字 */
    int32_t    sendRc;

    /* 想在 dispatch 里调一次 IpcPost */
    int32_t    tryPost;
} Sink;

static int32_t SinkDispatch(IpcContext *ctx, const IpcMessage *message, void *user)
{
    Sink *sink = (Sink *)user;

    sink->calls++;
    sink->event      = message->event;
    sink->type       = message->type;
    sink->reqId      = message->reqId;
    sink->instanceId = message->instanceId;
    sink->peerUid    = message->peerUid;
    sink->len        = message->len;
    (void)IpcStrlcpy(sink->ns, message->ns, sizeof(sink->ns));
    (void)IpcStrlcpy(sink->src, message->src, sizeof(sink->src));
    (void)IpcStrlcpy(sink->dst, message->dst, sizeof(sink->dst));
    sink->data[0] = 0;
    if (message->len > 0 && message->data != NULL) {
        size_t copy = (message->len < sizeof(sink->data) - 1)
                          ? message->len
                          : sizeof(sink->data) - 1;
        memcpy(sink->data, message->data, copy);
        sink->data[copy] = 0;
    }

    if (sink->trySend != 0 && sink->sendCtx != NULL) {
        /* 本线程此刻正在跑 dispatch，所以这条必须被死锁检测拦下
         * （无论 sendCtx 是不是正在分发的那个上下文）。目标默认 alpha；
         * 跨上下文用例会把 sendDst 换成配置表里不存在的名字 —— 这样若
         * 检测失手，结果是可断言的 NOENT，而不是真的挂进去等回复。 */
        sink->sendRc = IpcSend(sink->sendCtx,
                               (sink->sendDst != NULL) ? sink->sendDst : "alpha",
                               0x99u, "x", 1, NULL, 0, NULL);
    }
    if (sink->tryPost != 0) {
        /* 反过来，IpcPost 是非阻塞的，必须在 dispatch 里也能用。 */
        sink->replyRc = IpcPost(ctx, "alpha", 0x98u, "p", 1);
    }
    if (sink->replyMode == 1) {
        sink->replyCalls++;
        sink->replyRc = IpcReply(message, sink->replyText,
                                 strlen(sink->replyText));
    } else if (sink->replyMode == 2) {
        /*
         * 同一份回复路由连回两次。第二次必须被拒（IPC_ERR_STATE）——
         * 记账字段 replied 就写在 token 里面，所以这个拒绝是**随 token**
         * 走的，不需要库另存一张表。
         */
        sink->replyCalls++;
        sink->replyRc = IpcReply(message, sink->replyText,
                                 strlen(sink->replyText));
        sink->secondReplyTried++;
        sink->secondReplyRc = IpcReply(message, sink->replyText,
                                       strlen(sink->replyText));
    }
    sink->lastRc = IPC_OK;
    return IPC_OK;
}

/* ------------------------------------------------------------------ */
/* 驱动线程 —— 扮演宿主那个独立的 select 线程                          */
/* ------------------------------------------------------------------ */

typedef struct {
    IpcContext     *ctxs[2];
    _Atomic int32_t stop;
    _Atomic int64_t rounds;
    _Atomic int64_t delivered;
} Driver;

static void *DriverMain(void *arg)
{
    Driver *d = (Driver *)arg;
    struct timespec nap;

    nap.tv_sec  = 0;
    nap.tv_nsec = 200000; /* 0.2ms：够密，又不至于空转烧 CPU */

    while (atomic_load_explicit(&d->stop, memory_order_relaxed) == 0) {
        int32_t handled = 0;
        int32_t i;

        for (i = 0; i < 2; i++) {
            if (d->ctxs[i] == NULL) {
                continue;
            }
            int32_t n = IpcHandleReadable(d->ctxs[i], 8);
            if (n > 0) {
                handled += n;
            } else if (n < 0) {
                /* 上下文已停止（宿主的常规退出路径）。 */
                atomic_store_explicit(&d->stop, 1, memory_order_relaxed);
            }
        }
        if (handled > 0) {
            atomic_fetch_add_explicit(&d->delivered, handled, memory_order_relaxed);
        }
        atomic_fetch_add_explicit(&d->rounds, 1, memory_order_relaxed);
        (void)nanosleep(&nap, NULL);
    }
    return NULL;
}

static void DriverStart(Driver *d, IpcContext *first, IpcContext *second,
                        pthread_t *thread)
{
    memset(d, 0, sizeof(*d));
    d->ctxs[0] = first;
    d->ctxs[1] = second;
    UTEST_ASSERT_EQ(pthread_create(thread, NULL, DriverMain, d), 0);
}

static void DriverStop(Driver *d, pthread_t thread)
{
    atomic_store_explicit(&d->stop, 1, memory_order_relaxed);
    (void)pthread_join(thread, NULL);
}

/* ------------------------------------------------------------------ */
/* 「卡在 IpcSend 里等回复」的业务线程                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    IpcContext     *ctx;
    _Atomic int32_t rc;
    _Atomic int32_t done;
} WaitArgs;

static void *WaitForeverMain(void *arg)
{
    WaitArgs *w = (WaitArgs *)arg;
    int32_t   rc;

    /* 无限等待（IpcSend 的默认契约）。没有人会回复它，
     * 所以它只能靠 IpcRequestStop 被唤醒。 */
    rc = IpcSend(w->ctx, "beta", 0x71u, "q", 1, NULL, 0, NULL);
    atomic_store(&w->rc, rc);
    atomic_store(&w->done, 1);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* 手工构造的发送 socket（用来喂畸形报文）                             */
/* ------------------------------------------------------------------ */

/*
 * 直接往目标路径投一段字节，不经过库的发送路径。
 * 存在的理由：**畸形输入只能这么造**。库自己的发送接口只产出合法报文，
 * 所以 ipc_recv.c 里那一堆拒绝分支靠对外接口永远走不到。
 *
 * 内核会照常给这条报文附上 SCM_CREDENTIALS（接收端开了 SO_PASSCRED），
 * 所以它能一路走到凭据校验那一步 —— 这正是我们要的。
 */
static int32_t RawSendBytes(const char *dstPath, const void *buf, size_t len)
{
    struct sockaddr_un addr;
    size_t             pathLen = strlen(dstPath);
    int32_t            fd;
    ssize_t            sent;

    if (pathLen >= sizeof(addr.sun_path)) {
        return -1;
    }
    fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, dstPath, pathLen + 1);

    sent = sendto(fd, buf, len, MSG_DONTWAIT, (struct sockaddr *)&addr,
                  (socklen_t)(offsetof(struct sockaddr_un, sun_path) + pathLen + 1));
    (void)close(fd);
    return (sent == (ssize_t)len) ? 0 : -1;
}

/* 用库自己的编码器造一条**头部合法、可以任意改字段**的报文。 */
static int32_t RawSendHeader(const char *dstPath, IpcProtoHeader *hdr,
                             const void *payload, size_t len)
{
    uint8_t buf[IPC_HDR_SIZE + 256];

    if (len > 256) {
        return -1;
    }
    hdr->payloadLen = (uint32_t)len;
    if (IpcProtoEncode(hdr, buf, sizeof(buf)) != IPC_HDR_SIZE) {
        return -1;
    }
    if (len > 0) {
        memcpy(buf + IPC_HDR_SIZE, payload, len);
    }
    return RawSendBytes(dstPath, buf, IPC_HDR_SIZE + len);
}

static void RawHeaderInit(IpcProtoHeader *hdr, const char *src, const char *dst)
{
    memset(hdr, 0, sizeof(*hdr));
    hdr->version = (uint8_t)IPC_PROTOCOL_VERSION;
    hdr->type    = (uint8_t)IPC_MSG_TYPE_POST;
    hdr->flags   = 0;
    hdr->event   = 7;
    (void)IpcStrlcpy(hdr->ns, FWD_NS, sizeof(hdr->ns));
    (void)IpcStrlcpy(hdr->src, src, sizeof(hdr->src));
    (void)IpcStrlcpy(hdr->dst, dst, sizeof(hdr->dst));
}

/* ------------------------------------------------------------------ */
/* 1. 最基本的投递                                                     */
/* ------------------------------------------------------------------ */

UTEST_CASE(forward, post_is_delivered_to_the_other_context)
{
    FwdLab    lab;
    IpcContext *alpha = NULL;
    IpcContext *beta  = NULL;
    Sink        sink;
    IpcStatistics st;

    static const char *const modules[2] = { "alpha", "beta" };

    UTEST_ASSERT_EQ(FwdSetup(&lab, modules, 2), 0);
    memset(&sink, 0, sizeof(sink));
    UTEST_ASSERT_EQ(FwdRegister(&lab, "alpha", 0, NULL, NULL, &alpha), IPC_OK);
    UTEST_ASSERT_EQ(FwdRegister(&lab, "beta", 0, SinkDispatch, &sink, &beta),
                    IPC_OK);
    UTEST_ASSERT_NOTNULL(alpha);
    UTEST_ASSERT_NOTNULL(beta);

    UTEST_ASSERT_EQ(IpcPost(alpha, "beta", 0x21u, "hello", 5), IPC_OK);

    /* 还没人收 —— 报文在内核队列里。 */
    UTEST_ASSERT_EQ(sink.calls, 0);

    UTEST_ASSERT_EQ(IpcHandleReadable(beta, 0), 1);
    UTEST_ASSERT_EQ(sink.calls, 1);
    UTEST_ASSERT_STREQ(sink.src, "alpha");
    UTEST_ASSERT_STREQ(sink.dst, "beta");
    UTEST_ASSERT_STREQ(sink.ns, FWD_NS);
    UTEST_ASSERT_EQ(sink.event, 0x21u);
    UTEST_ASSERT_EQ(sink.type, IPC_MSG_TYPE_POST);
    UTEST_ASSERT_EQ(sink.len, (size_t)5);
    UTEST_ASSERT_STREQ((const char *)sink.data, "hello");

    /* 凭据必须是内核填的那个 uid，不是报文自称的。 */
    UTEST_ASSERT_EQ(sink.peerUid, getuid());

    /* POST 没有回复路由：reply.ctx 必须是 NULL。 */
    FwdTakeStats(beta, &st);
    UTEST_ASSERT_EQ(st.recvDelivered, (uint64_t)1);
    UTEST_ASSERT_EQ(st.dispatchInvoked, (uint64_t)1);
    UTEST_ASSERT_EQ(st.recvRejected, (uint64_t)0);

    FwdTakeStats(alpha, &st);
    UTEST_ASSERT_EQ(st.sendAttempts, (uint64_t)1);
    UTEST_ASSERT_EQ(st.sendEnqueued, (uint64_t)1);
    UTEST_ASSERT_EQ(st.sendFailed, (uint64_t)0);

    FwdRetire(beta);
    FwdRetire(alpha);
    FwdTeardown(&lab);
}

UTEST_CASE(forward, post_without_dispatch_is_counted_as_delivered)
{
    /*
     * 自测模式的语义要能区分开：「通过了全部校验，但没人处理它」和
     * 「被校验拒绝」是两件不同的事。recvDelivered 计前者，recvRejected
     * 计后者 —— 这条用例钉住的就是这个区分。
     */
    FwdLab      lab;
    IpcContext *alpha = NULL;
    IpcContext *beta  = NULL;
    IpcStatistics st;

    static const char *const modules[2] = { "alpha", "beta" };

    UTEST_ASSERT_EQ(FwdSetup(&lab, modules, 2), 0);
    UTEST_ASSERT_EQ(FwdRegister(&lab, "alpha", 0, NULL, NULL, &alpha), IPC_OK);
    UTEST_ASSERT_EQ(FwdRegister(&lab, "beta", 0, NULL, NULL, &beta), IPC_OK);

    UTEST_ASSERT_EQ(IpcPost(alpha, "beta", 1u, "x", 1), IPC_OK);
    UTEST_ASSERT_EQ(IpcHandleReadable(beta, 0), 1);

    FwdTakeStats(beta, &st);
    UTEST_ASSERT_EQ(st.recvDelivered, (uint64_t)1);
    UTEST_ASSERT_EQ(st.dispatchInvoked, (uint64_t)0);
    UTEST_ASSERT_EQ(st.dispatchFailed, (uint64_t)0);
    UTEST_ASSERT_EQ(st.recvRejected, (uint64_t)0);

    FwdRetire(beta);
    FwdRetire(alpha);
    FwdTeardown(&lab);
}

UTEST_CASE(forward, receive_drains_everything_that_is_queued)
{
    FwdLab      lab;
    IpcContext *alpha = NULL;
    IpcContext *beta  = NULL;
    Sink        sink;
    int32_t     i;

    static const char *const modules[2] = { "alpha", "beta" };

    UTEST_ASSERT_EQ(FwdSetup(&lab, modules, 2), 0);
    memset(&sink, 0, sizeof(sink));
    UTEST_ASSERT_EQ(FwdRegister(&lab, "alpha", 0, NULL, NULL, &alpha), IPC_OK);
    UTEST_ASSERT_EQ(FwdRegister(&lab, "beta", 0, SinkDispatch, &sink, &beta),
                    IPC_OK);

    for (i = 0; i < 8; i++) {
        UTEST_ASSERT_EQ(IpcPost(alpha, "beta", (uint32_t)(0x30 + i), "z", 1),
                        IPC_OK);
    }
    /* maxCount<=0 = 一路读到 EAGAIN。 */
    UTEST_ASSERT_EQ(IpcHandleReadable(beta, 0), 8);
    UTEST_ASSERT_EQ(sink.calls, 8);

    /* 再读一次：没有任何残留。 */
    UTEST_ASSERT_EQ(IpcHandleReadable(beta, 0), 0);

    FwdRetire(beta);
    FwdRetire(alpha);
    FwdTeardown(&lab);
}

UTEST_CASE(forward, maxcount_caps_how_many_are_handled)
{
    FwdLab      lab;
    IpcContext *alpha = NULL;
    IpcContext *beta  = NULL;
    Sink        sink;

    static const char *const modules[2] = { "alpha", "beta" };

    UTEST_ASSERT_EQ(FwdSetup(&lab, modules, 2), 0);
    memset(&sink, 0, sizeof(sink));
    UTEST_ASSERT_EQ(FwdRegister(&lab, "alpha", 0, NULL, NULL, &alpha), IPC_OK);
    UTEST_ASSERT_EQ(FwdRegister(&lab, "beta", 0, SinkDispatch, &sink, &beta),
                    IPC_OK);

    UTEST_ASSERT_EQ(IpcPost(alpha, "beta", 1u, "a", 1), IPC_OK);
    UTEST_ASSERT_EQ(IpcPost(alpha, "beta", 2u, "b", 1), IPC_OK);
    UTEST_ASSERT_EQ(IpcPost(alpha, "beta", 3u, "c", 1), IPC_OK);

    /*
     * maxCount 约束的是**读取次数**（含被丢弃的报文）。这三条都是合法
     * 报文，所以读 2 条 = 交付 2 条；「垃圾洪泛下上限仍然生效」由下面
     * maxcount_bounds_reads_not_just_deliveries 那条专门钉住。
     */
    UTEST_ASSERT_EQ(IpcHandleReadable(beta, 2), 2);
    UTEST_ASSERT_EQ(sink.calls, 2);
    UTEST_ASSERT_EQ(IpcHandleReadable(beta, 2), 1); /* 只剩一条 */
    UTEST_ASSERT_EQ(sink.calls, 3);
    UTEST_ASSERT_EQ(IpcHandleReadable(beta, 2), 0);

    FwdRetire(beta);
    FwdRetire(alpha);
    FwdTeardown(&lab);
}

UTEST_CASE(forward, maxcount_bounds_reads_not_just_deliveries)
{
    /*
     * maxCount 的洪泛回归：上限必须约束**读取次数**，而不是「成功交给
     * 宿主的条数」。对端灌 10 条全垃圾的报文（比报头还短，逐条被丢弃），
     * maxCount=4 时一次调用只能读 4 条 —— 否则 processed 恒为 0、上限
     * 永不命中，select 线程可以被一段垃圾流钉死在 IpcHandleReadable 里。
     *
     * 判据是 recvRead 的**累进**（4 → 8 → 10），不是返回值（全垃圾时
     * 返回值恒为 0，恰好证明「交付条数」不再是上限的判据）。
     */
    FwdLab      lab;
    IpcContext *alpha = NULL;
    IpcContext *beta  = NULL;
    IpcStatistics st;
    uint8_t     junk[10];
    int32_t     i;

    static const char *const modules[2] = { "alpha", "beta" };

    UTEST_ASSERT_EQ(FwdSetup(&lab, modules, 2), 0);
    UTEST_ASSERT_EQ(FwdRegister(&lab, "alpha", 0, NULL, NULL, &alpha), IPC_OK);
    UTEST_ASSERT_EQ(FwdRegister(&lab, "beta", 0, NULL, NULL, &beta), IPC_OK);

    memset(junk, 0xAA, sizeof(junk));
    for (i = 0; i < 10; i++) {
        UTEST_ASSERT_EQ(RawSendBytes(lab.pathB, junk, sizeof(junk)), 0);
    }

    /* 三轮 maxCount=4：读取数 4 → 8 → 10，返回值全是 0（全被丢弃）。 */
    UTEST_ASSERT_EQ(IpcHandleReadable(beta, 4), 0);
    FwdTakeStats(beta, &st);
    UTEST_ASSERT_EQ(st.recvRead, (uint64_t)4);
    UTEST_ASSERT_EQ(st.recvRejected, (uint64_t)4);

    UTEST_ASSERT_EQ(IpcHandleReadable(beta, 4), 0);
    FwdTakeStats(beta, &st);
    UTEST_ASSERT_EQ(st.recvRead, (uint64_t)8);

    UTEST_ASSERT_EQ(IpcHandleReadable(beta, 4), 0);
    FwdTakeStats(beta, &st);
    UTEST_ASSERT_EQ(st.recvRead, (uint64_t)10);

    /* 干净了：再读是 0，计数不再前进。 */
    UTEST_ASSERT_EQ(IpcHandleReadable(beta, 4), 0);
    FwdTakeStats(beta, &st);
    UTEST_ASSERT_EQ(st.recvRead, (uint64_t)10);
    UTEST_ASSERT_EQ(st.recvDelivered, (uint64_t)0);

    FwdRetire(beta);
    FwdRetire(alpha);
    FwdTeardown(&lab);
}

/* ------------------------------------------------------------------ */
/* 2. 发送前的参数与目标校验                                           */
/* ------------------------------------------------------------------ */

UTEST_CASE(forward, oversize_is_rejected_before_any_send_attempt)
{
    FwdLab      lab;
    IpcContext *alpha = NULL;
    IpcContext *beta  = NULL;
    IpcStatistics st;
    char        big[64];

    static const char *const modules[2] = { "alpha", "beta" };

    UTEST_ASSERT_EQ(FwdSetup(&lab, modules, 2), 0);
    memset(big, 'B', sizeof(big));
    /* maxPayload = 16，载荷 64 —— 必须在发送动作之前就拒绝。 */
    UTEST_ASSERT_EQ(FwdRegister(&lab, "alpha", 16, NULL, NULL, &alpha), IPC_OK);
    UTEST_ASSERT_EQ(FwdRegister(&lab, "beta", 0, NULL, NULL, &beta), IPC_OK);

    UTEST_ASSERT_EQ(IpcPost(alpha, "beta", 1u, big, 64), IPC_ERR_MSGSIZE);
    UTEST_ASSERT_EQ(IpcPost(alpha, "beta", 1u, big, 16), IPC_OK); /* 恰好等于上限 */

    FwdTakeStats(alpha, &st);
    /* 只有那条合法的算了一次尝试。 */
    UTEST_ASSERT_EQ(st.sendAttempts, (uint64_t)1);
    UTEST_ASSERT_EQ(st.sendFailed, (uint64_t)1);

    /* 目标那边只该收到那一条。 */
    UTEST_ASSERT_EQ(IpcHandleReadable(beta, 0), 1);

    FwdRetire(beta);
    FwdRetire(alpha);
    FwdTeardown(&lab);
}

UTEST_CASE(forward, unknown_target_and_offline_target_are_different_errors)
{
    FwdLab      lab;
    IpcContext *alpha = NULL;
    IpcStatistics st;

    static const char *const modules[2] = { "alpha", "beta" };

    UTEST_ASSERT_EQ(FwdSetup(&lab, modules, 2), 0);
    UTEST_ASSERT_EQ(FwdRegister(&lab, "alpha", 0, NULL, NULL, &alpha), IPC_OK);

    /* 配置表里没有 gamma —— NOENT。 */
    UTEST_ASSERT_EQ(IpcPost(alpha, "gamma", 1u, "x", 1), IPC_ERR_NOENT);
    /* beta 在配置表里，但没注册 —— OFFLINE。这两个错必须分得开：
     * 前者是配置写错了，后者只是启动顺序。 */
    UTEST_ASSERT_EQ(IpcPost(alpha, "beta", 1u, "x", 1), IPC_ERR_OFFLINE);

    UTEST_ASSERT_EQ(IpcPost(alpha, NULL, 1u, "x", 1), IPC_ERR_INVAL);
    UTEST_ASSERT_EQ(IpcPost(alpha, "", 1u, "x", 1), IPC_ERR_INVAL);
    UTEST_ASSERT_EQ(IpcPost(alpha, "beta", 1u, NULL, 4), IPC_ERR_INVAL);
    UTEST_ASSERT_EQ(IpcPost(NULL, "beta", 1u, "x", 1), IPC_ERR_INVAL);
    /* len == 0 时 data 可以传 NULL —— 这是一条合法发送（目标离线）。 */
    UTEST_ASSERT_EQ(IpcPost(alpha, "beta", 1u, NULL, 0), IPC_ERR_OFFLINE);

    /*
     * sendFailed 的口径：**只有真的走到发送那一步并且失败了**才计入。
     * 所以数下来是 3 条：gamma 的 NOENT、两条 beta 的 OFFLINE。
     * 那 4 条参数非法的 INVAL 一条都不计 —— 参数错误不是发送失败。
     * （第一版这里写的 2，漏了最后那条 len==0 的 OFFLINE。）
     */
    FwdTakeStats(alpha, &st);
    UTEST_ASSERT_EQ(st.sendFailed, (uint64_t)3);
    UTEST_ASSERT_EQ(st.sendAttempts, (uint64_t)3);
    UTEST_ASSERT_EQ(st.sendEnqueued, (uint64_t)0);

    FwdRetire(alpha);
    FwdTeardown(&lab);
}

/* ------------------------------------------------------------------ */
/* 3. 同步请求 / 回复                                                  */
/* ------------------------------------------------------------------ */

UTEST_CASE(forward, send_gets_the_reply_back_through_the_driver)
{
    FwdLab      lab;
    IpcContext *alpha = NULL;
    IpcContext *beta  = NULL;
    Sink        sink;
    Driver      driver;
    pthread_t   thread;
    IpcStatistics st;
    char        reply[64];
    size_t      outLen = 0;

    static const char *const modules[2] = { "alpha", "beta" };

    UTEST_ASSERT_EQ(FwdSetup(&lab, modules, 2), 0);
    memset(&sink, 0, sizeof(sink));
    sink.replyMode = 1;
    sink.replyText = "pong";
    UTEST_ASSERT_EQ(FwdRegister(&lab, "alpha", 0, NULL, NULL, &alpha), IPC_OK);
    UTEST_ASSERT_EQ(FwdRegister(&lab, "beta", 0, SinkDispatch, &sink, &beta),
                    IPC_OK);

    /* 驱动线程同时看两边的 fd：请求要送到 beta、回复要读回 alpha。 */
    DriverStart(&driver, alpha, beta, &thread);

    memset(reply, 0, sizeof(reply));
    UTEST_ASSERT_EQ(IpcSend(alpha, "beta", 0x41u, "ping", 4, reply,
                            sizeof(reply), &outLen),
                    IPC_OK);
    UTEST_ASSERT_EQ(outLen, (size_t)4);
    UTEST_ASSERT_STREQ(reply, "pong");

    DriverStop(&driver, thread);

    /* 对端：收到一条 REQ，回了 1 条。 */
    UTEST_ASSERT_EQ(sink.calls, 1);
    UTEST_ASSERT_EQ(sink.type, IPC_MSG_TYPE_REQ);
    UTEST_ASSERT_EQ(sink.replyCalls, 1);
    UTEST_ASSERT_EQ(sink.replyRc, IPC_OK);
    FwdTakeStats(beta, &st);
    UTEST_ASSERT_EQ(st.replySent, (uint64_t)1);

    /* 本端：登记了 1 个槽位、发出 1 条、回复匹配上 1 条。 */
    FwdTakeStats(alpha, &st);
    UTEST_ASSERT_EQ(st.sendEnqueued, (uint64_t)1);
    UTEST_ASSERT_EQ(st.replyMatched, (uint64_t)1);
    UTEST_ASSERT_EQ(st.replyUnmatched, (uint64_t)0);
    UTEST_ASSERT_EQ(st.pendingTimeout, (uint64_t)0);

    FwdRetire(beta);
    FwdRetire(alpha);
    FwdTeardown(&lab);
}

UTEST_CASE(forward, send_echoes_reqid_and_sender_instance)
{
    FwdLab      lab;
    IpcContext *alpha = NULL;
    IpcContext *beta  = NULL;
    Sink        sink;
    Driver      driver;
    pthread_t   thread;
    char        reply[16];
    size_t      outLen = 0;

    static const char *const modules[2] = { "alpha", "beta" };

    UTEST_ASSERT_EQ(FwdSetup(&lab, modules, 2), 0);
    memset(&sink, 0, sizeof(sink));
    sink.replyMode = 1;
    sink.replyText = "ok";
    UTEST_ASSERT_EQ(FwdRegister(&lab, "alpha", 0, NULL, NULL, &alpha), IPC_OK);
    UTEST_ASSERT_EQ(FwdRegister(&lab, "beta", 0, SinkDispatch, &sink, &beta),
                    IPC_OK);

    DriverStart(&driver, alpha, beta, &thread);
    UTEST_ASSERT_EQ(IpcSend(alpha, "beta", 0x42u, "q", 1, reply, sizeof(reply),
                            &outLen),
                    IPC_OK);
    DriverStop(&driver, thread);

    /*
     * reqId 由库自己分配（Q5 的答复：不额外暴露分配钩子），所以这里断言
     * 的是**关系**而不是具体值：请求方的 reqId 必须非零，对端看到的
     * instanceId 必须是请求方的代际号。
     */
    UTEST_ASSERT(sink.reqId != 0);
    UTEST_ASSERT_EQ_U64(sink.instanceId, IpcGetInstanceId(alpha));
    UTEST_ASSERT_EQ_U64(sink.instanceId == IpcGetInstanceId(beta), (uint64_t)0);

    FwdRetire(beta);
    FwdRetire(alpha);
    FwdTeardown(&lab);
}

UTEST_CASE(forward, send_times_out_when_nobody_replies)
{
    FwdLab      lab;
    IpcContext *alpha = NULL;
    IpcContext *beta  = NULL;
    IpcStatistics st;

    static const char *const modules[2] = { "alpha", "beta" };

    UTEST_ASSERT_EQ(FwdSetup(&lab, modules, 2), 0);
    /* beta 有 dispatch，但它不回复。而且没有驱动线程 —— 没人读回复。 */
    UTEST_ASSERT_EQ(FwdRegister(&lab, "alpha", 0, NULL, NULL, &alpha), IPC_OK);
    UTEST_ASSERT_EQ(FwdRegister(&lab, "beta", 0, NULL, NULL, &beta), IPC_OK);

    /*
     * 这个用例故意不回避时间：它测的**就是**「时间到了要返回超时」。
     * 30ms 足够走完一次 sendto + 超时唤醒；断言点仍然只是返回值与统计，
     * 不是「睡了 100ms 大概好了」。
     */
    UTEST_ASSERT_EQ(IpcSendTimeout(alpha, "beta", 1u, "q", 1, NULL, 0, NULL, 30),
                    IPC_ERR_TIMEOUT);

    FwdTakeStats(alpha, &st);
    UTEST_ASSERT_EQ(st.pendingTimeout, (uint64_t)1);
    UTEST_ASSERT_EQ(st.replyMatched, (uint64_t)0);

    /* 槽位必须还回去了：再来一次不能报 TOOMANY。 */
    UTEST_ASSERT(IpcSendTimeout(alpha, "beta", 1u, "q", 1, NULL, 0, NULL, 20) ==
                 IPC_ERR_TIMEOUT);

    FwdRetire(beta);
    FwdRetire(alpha);
    FwdTeardown(&lab);
}

UTEST_CASE(forward, late_reply_after_timeout_is_counted_as_unmatched)
{
    /*
     * 这是「请求方先超时、对端后回复」的时序。那条回复必须被识别为
     * 无人认领（replyUnmatched），而**不能**被误配给下一次请求。
     * 这条性质是 Q5 答复里 reqId 存在的全部意义。
     */
    FwdLab      lab;
    IpcContext *alpha = NULL;
    IpcContext *beta  = NULL;
    Sink        sink;
    IpcStatistics st;

    static const char *const modules[2] = { "alpha", "beta" };

    UTEST_ASSERT_EQ(FwdSetup(&lab, modules, 2), 0);
    memset(&sink, 0, sizeof(sink));
    sink.replyMode = 1;
    sink.replyText = "late";
    UTEST_ASSERT_EQ(FwdRegister(&lab, "alpha", 0, NULL, NULL, &alpha), IPC_OK);
    UTEST_ASSERT_EQ(FwdRegister(&lab, "beta", 0, SinkDispatch, &sink, &beta),
                    IPC_OK);

    UTEST_ASSERT_EQ(IpcSendTimeout(alpha, "beta", 1u, "q", 1, NULL, 0, NULL, 20),
                    IPC_ERR_TIMEOUT);

    /* 现在才让对端处理那条请求 —— 它会回一条「迟到」的回复。 */
    UTEST_ASSERT_EQ(IpcHandleReadable(beta, 0), 1);
    UTEST_ASSERT_EQ(sink.replyCalls, 1);

    /* 把回复读回请求方：必须落进 replyUnmatched。 */
    UTEST_ASSERT_EQ(IpcHandleReadable(alpha, 0), 0);
    FwdTakeStats(alpha, &st);
    UTEST_ASSERT_EQ(st.replyUnmatched, (uint64_t)1);
    UTEST_ASSERT_EQ(st.replyMatched, (uint64_t)0);

    FwdRetire(beta);
    FwdRetire(alpha);
    FwdTeardown(&lab);
}

UTEST_CASE(forward, reply_more_than_once_is_rejected)
{
    FwdLab      lab;
    IpcContext *alpha = NULL;
    IpcContext *beta  = NULL;
    Sink        sink;
    IpcStatistics st;
    char        reply[16];
    size_t      outLen = 0;

    static const char *const modules[2] = { "alpha", "beta" };

    UTEST_ASSERT_EQ(FwdSetup(&lab, modules, 2), 0);
    memset(&sink, 0, sizeof(sink));
    sink.replyMode = 2; /* 在 dispatch 里连回两次 */
    sink.replyText = "once";
    UTEST_ASSERT_EQ(FwdRegister(&lab, "alpha", 0, NULL, NULL, &alpha), IPC_OK);
    UTEST_ASSERT_EQ(FwdRegister(&lab, "beta", 0, SinkDispatch, &sink, &beta),
                    IPC_OK);

    /* 这里**不用驱动线程**：请求方自己超时，但 beta 照样会处理并回复两次。
     * 我们关心的是 IpcReply 的第二次返回值，不是回复有没有被收走。 */
    UTEST_ASSERT_EQ(IpcSendTimeout(alpha, "beta", 1u, "q", 1, reply,
                                   sizeof(reply), &outLen, 20),
                    IPC_ERR_TIMEOUT);

    /* 单线程驱动 beta，让它处理那条请求（dispatch 内会连回两次）。 */
    UTEST_ASSERT_EQ(IpcHandleReadable(beta, 0), 1);
    UTEST_ASSERT_EQ(sink.replyCalls, 1);
    UTEST_ASSERT_EQ(sink.replyRc, IPC_OK);          /* 第一次成功 */
    UTEST_ASSERT_EQ(sink.secondReplyTried, 1);
    UTEST_ASSERT_EQ(sink.secondReplyRc, IPC_ERR_STATE); /* 第二次被拒 */

    /* 只有一条真的发出去了。 */
    FwdTakeStats(beta, &st);
    UTEST_ASSERT_EQ(st.replySent, (uint64_t)1);

    FwdRetire(beta);
    FwdRetire(alpha);
    FwdTeardown(&lab);
}

UTEST_CASE(forward, reply_rejects_messages_without_a_reply_route)
{
    /*
     * 手搓 message：POST 上没有回复路由（reply.ctx == NULL）。
     * IpcReply 必须拒绝，而不是拿一个空 ctx 去发。
     */
    IpcMessage message;

    memset(&message, 0, sizeof(message));
    message.type = IPC_MSG_TYPE_POST;
    message.data = "x";
    message.len  = 1;

    UTEST_ASSERT_EQ(IpcReply(NULL, "y", 1), IPC_ERR_INVAL);
    /* reply.ctx == NULL -> 不可回复。 */
    UTEST_ASSERT_EQ(IpcReply(&message, "y", 1), IPC_ERR_INVAL);

    /* type 不是 REQ：即使 ctx 非空也不能回。 */
    message.reply.ctx = (IpcContext *)(uintptr_t)1; /* 只走到 type 判断就返回 */
    message.type      = IPC_MSG_TYPE_REP;
    UTEST_ASSERT_EQ(IpcReply(&message, "y", 1), IPC_ERR_INVAL);
    message.type = IPC_MSG_TYPE_POST;
    UTEST_ASSERT_EQ(IpcReply(&message, "y", 1), IPC_ERR_INVAL);
}

/* ------------------------------------------------------------------ */
/* 4. 死锁检测                                                         */
/* ------------------------------------------------------------------ */

UTEST_CASE(forward, send_inside_an_inline_dispatch_is_refused_not_deadlocked)
{
    FwdLab      lab;
    IpcContext *alpha = NULL;
    IpcContext *beta  = NULL;
    Sink        sink;
    IpcStatistics st;

    static const char *const modules[2] = { "alpha", "beta" };

    UTEST_ASSERT_EQ(FwdSetup(&lab, modules, 2), 0);
    memset(&sink, 0, sizeof(sink));
    /*
     * 让 beta 的 dispatch 里尝试对自己的上下文做一次同步请求。
     * 这是「宿主把 dispatch 写成内联执行」时最容易犯的错：
     * select 线程会在回调里卡住，回复永远读不回来。
     */
    sink.trySend = 1;
    sink.sendRc  = 0;
    UTEST_ASSERT_EQ(FwdRegister(&lab, "alpha", 0, NULL, NULL, &alpha), IPC_OK);
    UTEST_ASSERT_EQ(FwdRegister(&lab, "beta", 0, SinkDispatch, &sink, &beta),
                    IPC_OK);
    sink.sendCtx = beta;

    UTEST_ASSERT_EQ(IpcPost(alpha, "beta", 1u, "x", 1), IPC_OK);
    /* 这一句就是在 select 线程上跑的（本用例的主线程扮演那个角色）。 */
    UTEST_ASSERT_EQ(IpcHandleReadable(beta, 0), 1);

    UTEST_ASSERT_EQ(sink.sendRc, IPC_ERR_DEADLOCK);
    FwdTakeStats(beta, &st);
    UTEST_ASSERT_EQ(st.deadlockProbes, (uint64_t)1);

    FwdRetire(beta);
    FwdRetire(alpha);
    FwdTeardown(&lab);
}

UTEST_CASE(forward, send_on_another_context_inside_dispatch_is_also_refused)
{
    /*
     * 死锁检测的跨上下文回归。老系统的形态是**一条** select 线程服务
     * 所有上下文：在 ctx beta 的内联 dispatch 里对 ctx alpha 做同步发送，
     * 回复同样要靠这条（此刻正卡在回调里的）线程读回来 —— 与同上下文
     * 的情形是同一种死锁，必须同样返回 DEADLOCK。
     *
     * 目标刻意选配置表里不存在的模块：若检测失手（早期版本判 `== ctx`
     * 就是这种），IpcSend 会在 peer 查表那一步返回 NOENT —— 用例以一个
     * 干净的断言失败收场，而不是真的挂进去等回复。
     */
    FwdLab      lab;
    IpcContext *alpha = NULL;
    IpcContext *beta  = NULL;
    Sink        sink;
    IpcStatistics st;

    static const char *const modules[2] = { "alpha", "beta" };

    UTEST_ASSERT_EQ(FwdSetup(&lab, modules, 2), 0);
    memset(&sink, 0, sizeof(sink));
    sink.trySend = 1;
    sink.sendDst = "nosuchmodule"; /* 不在配置表里 */
    UTEST_ASSERT_EQ(FwdRegister(&lab, "alpha", 0, NULL, NULL, &alpha), IPC_OK);
    UTEST_ASSERT_EQ(FwdRegister(&lab, "beta", 0, SinkDispatch, &sink, &beta),
                    IPC_OK);
    /* dispatch 跑在 beta 上，发送走 alpha 的上下文（注册成功后指针才有效）。 */
    sink.sendCtx = alpha;

    UTEST_ASSERT_EQ(IpcPost(alpha, "beta", 1u, "x", 1), IPC_OK);
    /* 主线程此刻扮演 select 线程：dispatch 在它的调用栈里内联执行。 */
    UTEST_ASSERT_EQ(IpcHandleReadable(beta, 0), 1);

    UTEST_ASSERT_EQ(sink.sendRc, IPC_ERR_DEADLOCK);
    /* 统计记在 IpcSend 的那个 ctx（alpha）上，不在被分发的 beta 上。 */
    FwdTakeStats(alpha, &st);
    UTEST_ASSERT_EQ(st.deadlockProbes, (uint64_t)1);
    FwdTakeStats(beta, &st);
    UTEST_ASSERT_EQ(st.deadlockProbes, (uint64_t)0);

    FwdRetire(beta);
    FwdRetire(alpha);
    FwdTeardown(&lab);
}

UTEST_CASE(forward, deadlock_probe_does_not_fire_outside_dispatch)
{
    /*
     * 反向对照：不在 dispatch 里调 IpcSend 就不该被拦（否则库就不能用了）。
     * 用一个必然超时的请求来证明它走到了「真的发出去再等」那一步 ——
     * 返回 TIMEOUT 而不是 DEADLOCK，就说明死锁检测没误伤。
     */
    FwdLab      lab;
    IpcContext *alpha = NULL;
    IpcContext *beta  = NULL;
    IpcStatistics st;

    static const char *const modules[2] = { "alpha", "beta" };

    UTEST_ASSERT_EQ(FwdSetup(&lab, modules, 2), 0);
    UTEST_ASSERT_EQ(FwdRegister(&lab, "alpha", 0, SinkDispatch, NULL, &alpha),
                    IPC_OK);
    UTEST_ASSERT_EQ(FwdRegister(&lab, "beta", 0, NULL, NULL, &beta), IPC_OK);

    /*
     * 注意这里 alpha 自己也有 dispatch：如果死锁标记是「全局脏了」而不是
     * 「本线程正在跑本上下文的 dispatch」，下面这句就会被误判。它返回
     * TIMEOUT 才说明标记是准的。
     */
    UTEST_ASSERT_EQ(IpcSendTimeout(alpha, "beta", 1u, "x", 1, NULL, 0, NULL, 20),
                    IPC_ERR_TIMEOUT);

    FwdTakeStats(alpha, &st);
    UTEST_ASSERT_EQ(st.deadlockProbes, (uint64_t)0);

    FwdRetire(beta);
    FwdRetire(alpha);
    FwdTeardown(&lab);
}

UTEST_CASE(forward, post_is_allowed_inside_dispatch)
{
    /* IpcPost 是非阻塞的，所以在 dispatch 里必须能用 —— 否则「收到通知
     * 转发给别人」这种最常见的业务写法就做不了。 */
    FwdLab      lab;
    IpcContext *alpha = NULL;
    IpcContext *beta  = NULL;
    Sink        sinkA;
    Sink        sinkB;

    static const char *const modules[2] = { "alpha", "beta" };

    UTEST_ASSERT_EQ(FwdSetup(&lab, modules, 2), 0);
    memset(&sinkA, 0, sizeof(sinkA));
    memset(&sinkB, 0, sizeof(sinkB));
    sinkB.tryPost = 1;
    UTEST_ASSERT_EQ(FwdRegister(&lab, "alpha", 0, SinkDispatch, &sinkA, &alpha),
                    IPC_OK);
    UTEST_ASSERT_EQ(FwdRegister(&lab, "beta", 0, SinkDispatch, &sinkB, &beta),
                    IPC_OK);

    UTEST_ASSERT_EQ(IpcPost(alpha, "beta", 1u, "x", 1), IPC_OK);
    UTEST_ASSERT_EQ(IpcHandleReadable(beta, 0), 1);
    /* beta 在 dispatch 里把一条 POST 转发给了 alpha。 */
    UTEST_ASSERT_EQ(sinkB.replyRc, IPC_OK);

    UTEST_ASSERT_EQ(IpcHandleReadable(alpha, 0), 1);
    UTEST_ASSERT_EQ(sinkA.calls, 1);
    UTEST_ASSERT_EQ(sinkA.event, 0x98u);
    UTEST_ASSERT_STREQ(sinkA.src, "beta");

    FwdRetire(beta);
    FwdRetire(alpha);
    FwdTeardown(&lab);
}

/* ------------------------------------------------------------------ */
/* 5. 广播                                                             */
/* ------------------------------------------------------------------ */

UTEST_CASE(forward, broadcast_skips_itself_and_counts_offline_peers)
{
    FwdLab      lab;
    IpcContext *alpha = NULL;
    IpcContext *beta  = NULL;
    Sink        sinkB;
    IpcStatistics st;
    int32_t     sent;

    static const char *const modules[3] = { "alpha", "beta", "gamma" };

    UTEST_ASSERT_EQ(FwdSetup(&lab, modules, 3), 0);
    memset(&sinkB, 0, sizeof(sinkB));
    UTEST_ASSERT_EQ(FwdRegister(&lab, "alpha", 0, NULL, NULL, &alpha), IPC_OK);
    UTEST_ASSERT_EQ(FwdRegister(&lab, "beta", 0, SinkDispatch, &sinkB, &beta),
                    IPC_OK);
    /* gamma 只在配置表里，没有注册 —— 期望被判为离线并跳过。 */

    sent = IpcBroadcast(alpha, 0x51u, "bc", 2);
    /* 默认不包含自己：3 个模块里 alpha 跳过自己、gamma 离线 -> 只有 beta。 */
    UTEST_ASSERT_EQ(sent, 1);

    FwdTakeStats(alpha, &st);
    UTEST_ASSERT_EQ(st.broadcastTargets, (uint64_t)2);
    UTEST_ASSERT_EQ(st.broadcastSkipped, (uint64_t)1);
    UTEST_ASSERT_EQ(st.sendEnqueued, (uint64_t)1);

    UTEST_ASSERT_EQ(IpcHandleReadable(beta, 0), 1);
    UTEST_ASSERT_EQ(sinkB.event, 0x51u);
    UTEST_ASSERT_STREQ(sinkB.src, "alpha");

    FwdRetire(beta);
    FwdRetire(alpha);
    FwdTeardown(&lab);
}

UTEST_CASE(forward, broadcast_can_include_self)
{
    FwdLab      lab;
    IpcContext *alpha = NULL;
    Sink        sinkA;
    IpcModuleOptions options = IPC_MODULE_OPTIONS_INIT;
    int32_t     sent;

    static const char *const modules[3] = { "alpha", "beta", "gamma" };

    UTEST_ASSERT_EQ(FwdSetup(&lab, modules, 3), 0);
    memset(&sinkA, 0, sizeof(sinkA));
    options.moduleId             = "alpha";
    options.ns                   = FWD_NS;
    options.confPath             = lab.conf;
    options.dispatch             = SinkDispatch;
    options.dispatchUser         = &sinkA;
    options.broadcastIncludeSelf = 1;
    UTEST_ASSERT_EQ(IpcRegister(&options, &alpha), IPC_OK);

    sent = IpcBroadcast(alpha, 0x52u, "bc", 2);
    /*
     * 含自己：配置里有 3 个模块，但**只有 alpha 注册了**。beta / gamma 都
     * 没有端点 -> 都被判离线跳过。所以成功数是 1，不是 2。
     * （第一版这里写的 2，是照着「含自己 = alpha + beta」想当然写的，
     *   而 beta 在这个用例里压根没注册 —— 编译器抓不到这种错。）
     */
    UTEST_ASSERT_EQ(sent, 1);

    /* 自己发的那条也要能被自己读到（端点收自己的报文是合法的）。 */
    UTEST_ASSERT_EQ(IpcHandleReadable(alpha, 0), 1);
    UTEST_ASSERT_EQ(sinkA.event, 0x52u);

    FwdRetire(alpha);
    FwdTeardown(&lab);
}

UTEST_CASE(forward, broadcast_argument_checks)
{
    FwdLab      lab;
    IpcContext *alpha = NULL;
    IpcModuleOptions options = IPC_MODULE_OPTIONS_INIT;

    static const char *const modules[2] = { "alpha", "beta" };

    UTEST_ASSERT_EQ(FwdSetup(&lab, modules, 2), 0);
    /* maxPayload 必须调小才有意义：默认 8192 下 32 字节根本触不到上限。 */
    options.moduleId   = "alpha";
    options.ns         = FWD_NS;
    options.confPath   = lab.conf;
    options.maxPayload = 16;
    UTEST_ASSERT_EQ(IpcRegister(&options, &alpha), IPC_OK);

    UTEST_ASSERT_EQ(IpcBroadcast(NULL, 1u, "x", 1), IPC_ERR_INVAL);
    UTEST_ASSERT_EQ(IpcBroadcast(alpha, 1u, NULL, 1), IPC_ERR_INVAL);
    /* 载荷超上限 —— 必须在展开目标之前就拒绝。 */
    {
        char big[32];
        memset(big, 'z', sizeof(big));
        UTEST_ASSERT_EQ(IpcBroadcast(alpha, 1u, big, sizeof(big)),
                        IPC_ERR_MSGSIZE);
    }
    /* 恰好等于上限要走得通（beta 离线，所以成功数是 0，但绝不是错误码）。 */
    {
        char exact[16];
        memset(exact, 'e', sizeof(exact));
        UTEST_ASSERT_EQ(IpcBroadcast(alpha, 1u, exact, sizeof(exact)), 0);
    }

    FwdRetire(alpha);
    FwdTeardown(&lab);
}

/* ------------------------------------------------------------------ */
/* 6. 畸形输入：这一节是 ipc_recv.c 拒绝分支的唯一覆盖来源             */
/* ------------------------------------------------------------------ */

UTEST_CASE(forward, malformed_datagrams_are_rejected_by_category)
{
    FwdLab      lab;
    IpcContext *alpha = NULL;
    IpcContext *beta  = NULL;
    IpcProtoHeader hdr;
    IpcStatistics st;
    uint8_t     junk[IPC_HDR_SIZE + 8];
    uint64_t    rejProto = 0;
    uint64_t    rejCred  = 0;

    static const char *const modules[2] = { "alpha", "beta" };

    UTEST_ASSERT_EQ(FwdSetup(&lab, modules, 2), 0);
    UTEST_ASSERT_EQ(FwdRegister(&lab, "alpha", 0, SinkDispatch, NULL, &alpha),
                    IPC_OK);
    UTEST_ASSERT_EQ(FwdRegister(&lab, "beta", 0, NULL, NULL, &beta), IPC_OK);

    /* (a) 比报头还短。 */
    memset(junk, 0xAA, sizeof(junk));
    UTEST_ASSERT_EQ(RawSendBytes(lab.pathB, junk, 10), 0);
    UTEST_ASSERT_EQ(IpcHandleReadable(beta, 0), 0);

    /* (b) magic 不对（长度够，但整个头是垃圾）。 */
    UTEST_ASSERT_EQ(RawSendBytes(lab.pathB, junk, IPC_HDR_SIZE), 0);
    UTEST_ASSERT_EQ(IpcHandleReadable(beta, 0), 0);

    FwdTakeStats(beta, &st);
    UTEST_ASSERT_EQ(st.recvRejProto, (uint64_t)2);
    UTEST_ASSERT_EQ(st.recvRejected, (uint64_t)2);
    UTEST_ASSERT_EQ(st.recvDelivered, (uint64_t)0);
    UTEST_ASSERT_EQ(st.dispatchInvoked, (uint64_t)0);

    /* (c) 头部合法但 ns 不对。 */
    RawHeaderInit(&hdr, "alpha", "beta");
    (void)IpcStrlcpy(hdr.ns, "otherns", sizeof(hdr.ns));
    UTEST_ASSERT_EQ(RawSendHeader(lab.pathB, &hdr, "x", 1), 0);
    UTEST_ASSERT_EQ(IpcHandleReadable(beta, 0), 0);

    /* (d) 头部合法但 dst 不是我。 */
    RawHeaderInit(&hdr, "alpha", "gamma");
    UTEST_ASSERT_EQ(RawSendHeader(lab.pathB, &hdr, "x", 1), 0);
    UTEST_ASSERT_EQ(IpcHandleReadable(beta, 0), 0);

    /* (e) 头部说 payloadLen=100，实际只来了 1 字节。 */
    RawHeaderInit(&hdr, "alpha", "beta");
    hdr.payloadLen = 100;
    {
        uint8_t buf[IPC_HDR_SIZE + 1];
        UTEST_ASSERT_EQ((int32_t)IpcProtoEncode(&hdr, buf, sizeof(buf)),
                        IPC_HDR_SIZE);
        buf[IPC_HDR_SIZE] = 'x';
        UTEST_ASSERT_EQ(RawSendBytes(lab.pathB, buf, IPC_HDR_SIZE + 1), 0);
    }
    UTEST_ASSERT_EQ(IpcHandleReadable(beta, 0), 0);

    FwdTakeStats(beta, &st);
    rejProto = st.recvRejProto;
    UTEST_ASSERT_EQ(rejProto, (uint64_t)5);
    UTEST_ASSERT_EQ(st.recvDelivered, (uint64_t)0);

    /*
     * (f) 头部完全合法、但自称的 src 不在配置表里。
     * 这一条会走到凭据校验 —— 内核凭据是好的（确实是我们这个 uid），
     * 但「src 自称的身份」在配置里查不到，所以必须拒绝。
     * 它和上面几条的区别在于拒绝**分类**不同（recvRejCred 而不是
     * recvRejProto），这正说明「凭据缺失/不符」与「报文格式错」是分开计的。
     */
    RawHeaderInit(&hdr, "imposter", "beta");
    UTEST_ASSERT_EQ(RawSendHeader(lab.pathB, &hdr, "x", 1), 0);
    UTEST_ASSERT_EQ(IpcHandleReadable(beta, 0), 0);

    FwdTakeStats(beta, &st);
    rejCred = st.recvRejCred;
    UTEST_ASSERT_EQ(rejCred, (uint64_t)1);
    UTEST_ASSERT_EQ(st.recvRejProto, rejProto); /* proto 计数没被这一条污染 */
    UTEST_ASSERT_EQ(st.recvDelivered, (uint64_t)0);

    FwdRetire(beta);
    FwdRetire(alpha);
    FwdTeardown(&lab);
}

UTEST_CASE(forward, oversized_datagram_is_dropped_but_sender_still_sees_ok)
{
    /*
     * 这条是设计里明写的陷阱，值得有一个专门的回归用例：
     * 接收端的缓冲是 `IPC_HDR_SIZE + 自己的 maxPayload`。接收方配小之后，
     * 大报文会被 MSG_TRUNC 检测到并丢弃，而**发送方看到的仍然是 IPC_OK**。
     *
     * 所以「maxPayload 必须同命名空间一致」是部署级约束。凡是引用这条
     * 行为的地方都不许把它说成 bug。
     */
    FwdLab      lab;
    IpcContext *alpha = NULL;
    IpcContext *betaSmall = NULL;
    Sink        sink;
    IpcStatistics st;
    char        payload[300];

    static const char *const modules[2] = { "alpha", "beta" };

    UTEST_ASSERT_EQ(FwdSetup(&lab, modules, 2), 0);
    memset(&sink, 0, sizeof(sink));
    memset(payload, 'Q', sizeof(payload));

    /* alpha 配 1024（放得下 300），beta 只配 64（放不下）。 */
    UTEST_ASSERT_EQ(FwdRegister(&lab, "alpha", 1024, NULL, NULL, &alpha), IPC_OK);
    UTEST_ASSERT_EQ(FwdRegister(&lab, "beta", 64, SinkDispatch, &sink, &betaSmall),
                    IPC_OK);

    UTEST_ASSERT_EQ(IpcPost(alpha, "beta", 1u, payload, 300), IPC_OK);

    /* beta 这边：被 MSG_TRUNC 拦下，不算「交给宿主」。 */
    UTEST_ASSERT_EQ(IpcHandleReadable(betaSmall, 0), 0);
    UTEST_ASSERT_EQ(sink.calls, 0);

    FwdTakeStats(betaSmall, &st);
    UTEST_ASSERT_EQ(st.recvRejTrunc, (uint64_t)1);
    UTEST_ASSERT_EQ(st.recvRejected, (uint64_t)1);
    UTEST_ASSERT_EQ(st.recvDelivered, (uint64_t)0);

    /* alpha 这边：完全不知道对方丢了它。 */
    FwdTakeStats(alpha, &st);
    UTEST_ASSERT_EQ(st.sendEnqueued, (uint64_t)1);
    UTEST_ASSERT_EQ(st.sendFailed, (uint64_t)0);

    FwdRetire(betaSmall);
    FwdRetire(alpha);
    FwdTeardown(&lab);
}

UTEST_CASE(forward, reply_datagram_is_matched_only_by_reqid_and_instance)
{
    /*
     * 直接手工投一条 REP 给 alpha：reqId 与 instanceId 都是编的，
     * 所以必须落进 replyUnmatched。这条比「迟到回复」更严格 ——
     * 它证明匹配条件里 reqId 与 instanceId 都在起作用。
     */
    FwdLab      lab;
    IpcContext *alpha = NULL;
    IpcProtoHeader hdr;
    IpcStatistics st;

    static const char *const modules[2] = { "alpha", "beta" };

    UTEST_ASSERT_EQ(FwdSetup(&lab, modules, 2), 0);
    UTEST_ASSERT_EQ(FwdRegister(&lab, "alpha", 0, NULL, NULL, &alpha), IPC_OK);
    /*
     * beta **不需要真的注册**：发送路径要的是配置表里的一条记录（用来
     * 解析出端点路径），而 src 的凭据校验查的也是配置表，不是「谁注册过」。
     * 这一点本身就是契约的一部分，所以这里刻意不注册它。
     */
    RawHeaderInit(&hdr, "beta", "alpha");
    hdr.type       = (uint8_t)IPC_MSG_TYPE_REP;
    hdr.reqId      = 0xDEADBEEFu;
    hdr.instanceId = 0x1234u;
    UTEST_ASSERT_EQ(RawSendHeader(lab.pathA, &hdr, "r", 1), 0);
    UTEST_ASSERT_EQ(IpcHandleReadable(alpha, 0), 0);

    FwdTakeStats(alpha, &st);
    UTEST_ASSERT_EQ(st.replyUnmatched, (uint64_t)1);
    UTEST_ASSERT_EQ(st.replyMatched, (uint64_t)0);
    /* REP 不进业务层。 */
    UTEST_ASSERT_EQ(st.dispatchInvoked, (uint64_t)0);
    UTEST_ASSERT_EQ(st.recvDelivered, (uint64_t)0);

    FwdRetire(alpha);
    FwdTeardown(&lab);
}

/* ------------------------------------------------------------------ */
/* 7. 停止之后的行为                                                   */
/* ------------------------------------------------------------------ */

UTEST_CASE(forward, stop_blocks_every_send_path)
{
    FwdLab      lab;
    IpcContext *alpha = NULL;
    IpcContext *beta  = NULL;
    IpcStatistics st;

    static const char *const modules[2] = { "alpha", "beta" };

    UTEST_ASSERT_EQ(FwdSetup(&lab, modules, 2), 0);
    UTEST_ASSERT_EQ(FwdRegister(&lab, "alpha", 0, NULL, NULL, &alpha), IPC_OK);
    UTEST_ASSERT_EQ(FwdRegister(&lab, "beta", 0, NULL, NULL, &beta), IPC_OK);

    UTEST_ASSERT_EQ(IpcRequestStop(alpha), IPC_OK);
    UTEST_ASSERT_EQ(IpcIsStopped(alpha), 1);

    UTEST_ASSERT_EQ(IpcPost(alpha, "beta", 1u, "x", 1), IPC_ERR_STOPPED);
    UTEST_ASSERT_EQ(IpcBroadcast(alpha, 1u, "x", 1), IPC_ERR_STOPPED);
    UTEST_ASSERT_EQ(IpcSendTimeout(alpha, "beta", 1u, "x", 1, NULL, 0, NULL, 10),
                    IPC_ERR_STOPPED);
    /* 接收侧也立刻告诉宿主该收摊了 —— 否则 level-triggered 的 select
     * 会一直报可读，宿主就空转了。 */
    UTEST_ASSERT_EQ(IpcHandleReadable(alpha, 0), IPC_ERR_STOPPED);

    FwdTakeStats(alpha, &st);
    UTEST_ASSERT_EQ(st.sendAttempts, (uint64_t)0);
    UTEST_ASSERT_EQ(st.sendEnqueued, (uint64_t)0);

    /* 停止是幂等的，且不影响别人。 */
    UTEST_ASSERT_EQ(IpcRequestStop(alpha), IPC_OK);
    UTEST_ASSERT_EQ(IpcIsStopped(alpha), 1);
    UTEST_ASSERT_EQ(IpcIsStopped(beta), 0);
    UTEST_ASSERT_EQ(IpcPost(alpha, "beta", 1u, "x", 1), IPC_ERR_STOPPED);
    UTEST_ASSERT_EQ(IpcPost(beta, "alpha", 1u, "x", 1), IPC_OK);

    FwdRetire(beta);
    FwdRetire(alpha);
    FwdTeardown(&lab);
}

UTEST_CASE(forward, request_stop_wakes_a_thread_waiting_for_a_reply)
{
    /*
     * 「慢路径」：业务线程正卡在 IpcSend 里等回复（无限等待），此时另一条
     * 线程调 IpcRequestStop。那条业务线程必须被唤醒并拿到 IPC_ERR_STOPPED，
     * 而不是永远等下去。
     *
     * 这条路径只有真的起线程才测得到 —— 顺序调用永远走的是「发送前就被
     * 挡住」的快路径。
     *
     * 时序保证：beta 没有 dispatch、也没人读 alpha，所以那条 IpcSend 一定
     * 会一直等；主线程的职责只是「确认它确实进入了等待」，然后停止它。
     * 「确认进入等待」用的是等待表自己的容量占用：maxPending=1 时，只要
     * 槽位还被占着，再来一次同步发送就会拿到 TOOMANY —— 这比 sleep 可靠。
     */
    FwdLab      lab;
    IpcContext *alpha = NULL;
    IpcContext *beta  = NULL;
    IpcModuleOptions options = IPC_MODULE_OPTIONS_INIT;
    pthread_t   thread;
    int32_t     rc = 0;

    static const char *const modules[2] = { "alpha", "beta" };

    UTEST_ASSERT_EQ(FwdSetup(&lab, modules, 2), 0);
    options.moduleId   = "alpha";
    options.ns         = FWD_NS;
    options.confPath   = lab.conf;
    options.maxPending = 1;
    UTEST_ASSERT_EQ(IpcRegister(&options, &alpha), IPC_OK);
    UTEST_ASSERT_EQ(FwdRegister(&lab, "beta", 0, NULL, NULL, &beta), IPC_OK);

    WaitArgs waiter;
    memset(&waiter, 0, sizeof(waiter));
    waiter.ctx = alpha;
    UTEST_ASSERT_EQ(pthread_create(&thread, NULL, WaitForeverMain, &waiter), 0);

    /* 有界轮询：等那条线程把唯一的槽位占上。判据是库自己的统计，
     * 不是「睡了一会儿应该够了」。 */
    {
        IpcStatistics st;
        int32_t       tries = 0;

        do {
            struct timespec nap = { 0, 2000000 }; /* 2ms */
            (void)nanosleep(&nap, NULL);
            FwdTakeStats(alpha, &st);
            tries++;
        } while (st.sendEnqueued == 0 && tries < 500);
        UTEST_ASSERT(st.sendEnqueued >= (uint64_t)1);
    }
    /* 槽位被占着，所以这次同步发送连发都发不出去。 */
    UTEST_ASSERT_EQ(IpcSendTimeout(alpha, "beta", 2u, "y", 1, NULL, 0, NULL, 1),
                    IPC_ERR_TOOMANY);

    /* 现在停止：等待中的那条线程必须醒过来。 */
    UTEST_ASSERT_EQ(IpcRequestStop(alpha), IPC_OK);
    UTEST_ASSERT_EQ(pthread_join(thread, NULL), 0);
    rc = atomic_load(&waiter.rc);
    UTEST_ASSERT_EQ(rc, IPC_ERR_STOPPED);
    UTEST_ASSERT_EQ(waiter.done, 1);

    FwdRetire(beta);
    FwdRetire(alpha);
    FwdTeardown(&lab);
}

/* ------------------------------------------------------------------ */
/* 8. 请求槽位耗尽                                                     */
/* ------------------------------------------------------------------ */

/*
 * 这一节被并进了 `request_stop_wakes_a_thread_waiting_for_a_reply`：
 * 要制造「等待表满」，必须有一条线程真的占着槽位，而那个用例已经建好了
 * 这个局面（它用 TOOMANY 当作「业务线程确实进入了等待」的判据）。
 * 早先这里另有一个用例，名字写的是「表满被拒且不发送」，断言里却在确认
 * `pendingRejected == 0` —— 名字和断言对不上，那种用例比没有更坏：
 * 它给人的印象是这条路径被测过了。删掉它，把覆盖让给真正测到的那条。
 *
 * 等待表本身的白盒（Add 表满不改动出参、Repeat Complete、超时钉住 done……）
 * 在 test_pending.c，不需要在这里重复。
 */
