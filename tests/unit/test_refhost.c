/*
 * Copyright (c) 2026 <版权方待定>
 * 许可协议：<待定>
 *
 * test_refhost.c -- 参考宿主自身的白盒单测。**仅供测试**。
 *
 * 目前只有一条，但值得单开一个文件：它钉的是 WorkerMain 出队拷贝里
 * 最容易写错的一步 —— 结构体拷贝只复制指针的**值**，ns / src / dst
 * 若不重指，回调读到的就是**已被下一个 job 复用覆写**的队列槽。
 *
 * 为什么这条必须是「确定性」的：它本质是个竞态（槽被复用发生在回调
 * 执行期间），靠碰运气的多轮压力测试只能让它「偶尔红」。这里的做法是
 * 把时序钉死：
 *   1. 队列深度 1、worker 1 条 —— 槽的下标在两次入队之间必然重合；
 *   2. handler A 先阻塞，等主线程**确实把 B 填进同一个槽**之后再读
 *      message->src —— 读的那一刻，覆写已经发生完毕。
 * 于是「修了必绿、坏了必红」，不需要任何 sleep。
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ipc/ipc.h"
#include "refhost.h"
#include "lab.h"
#include "utest.h"

/* ------------------------------------------------------------------ */
/* 实验台（与 test_forward 同款的最小配置：一个模块足够）              */
/* ------------------------------------------------------------------ */

#define RH_NS "rhns"

typedef struct {
    char dir[256];
    char conf[512];
} RhLab;

static int32_t RhSetup(RhLab *lab)
{
    char   text[512];
    size_t written;
    static const char *const modules[1] = { "alpha" };

    memset(lab, 0, sizeof(*lab));
    if (IpcLabCreate(lab->dir, sizeof(lab->dir)) != 0) {
        return -1;
    }
    written = IpcLabBuildConf(lab->dir, RH_NS, modules, 1, text, sizeof(text));
    if (written == 0) {
        IpcLabRemove(lab->dir);
        return -1;
    }
    IpcLabConfPath(lab->dir, lab->conf, sizeof(lab->conf));
    IpcLabSilenceLog();
    return (IpcLabWriteFile(lab->conf, text) == 0) ? 0 : -1;
}

static void RhTeardown(RhLab *lab)
{
    IpcLabCaptureEnd();
    IpcLabRemove(lab->dir);
}

/* ------------------------------------------------------------------ */
/* 两个 handler 与它们和主线程的同步                                   */
/* ------------------------------------------------------------------ */

static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv  = PTHREAD_COND_INITIALIZER;

static int32_t g_aEntered;    /* handler A 已经进入（= A 已被 worker 出队） */
static int32_t g_bDispatched; /* B 已被主线程填进队列槽 */
static char    g_aSrc[IPC_NAME_MAX + 8]; /* A 在放行那一刻读到的自称来源 */
static char    g_bSrc[IPC_NAME_MAX + 8]; /* B 的 handler 读到的来源 */

/* event 1：进入后阻塞，等 B 复用完本 job 原来所在的槽，再读 src。 */
static int32_t HandlerBlockThenRead(IpcMessage *message, void *user)
{
    (void)user;
    (void)pthread_mutex_lock(&g_mtx);
    g_aEntered = 1;
    (void)pthread_cond_broadcast(&g_cv);
    while (g_bDispatched == 0) {
        (void)pthread_cond_wait(&g_cv, &g_mtx);
    }
    (void)pthread_mutex_unlock(&g_mtx);

    /*
     * 此刻队列槽已被 FillJob(B) 先 memset 再写上 "beta"。
     * 修复后的实现里 message->src 指向本 job 的栈拷贝自己的数组，
     * 读到的必须是 "alpha"；修复前它指着槽里的数组，读到 "beta"。
     */
    (void)snprintf(g_aSrc, sizeof(g_aSrc), "%s", message->src);
    return IPC_OK;
}

/* event 2：记录来源即返回。 */
static int32_t HandlerNote(IpcMessage *message, void *user)
{
    (void)user;
    (void)snprintf(g_bSrc, sizeof(g_bSrc), "%s", message->src);
    return IPC_OK;
}

/* 造一条直接喂给 IpcRefHostDispatch 的报文视图（不经过内核）。 */
static void MakeMessage(IpcMessage *message, const char *src, uint32_t event)
{
    memset(message, 0, sizeof(*message));
    message->ns    = RH_NS;
    message->src   = src;
    message->dst   = "alpha";
    message->event = event;
    message->type  = IPC_MSG_TYPE_POST;
    message->data  = "x";
    message->len   = 1;
}

typedef struct {
    IpcRefHost *host;
    int32_t     rc;
} RhRunArgs;

static void *RhRunMain(void *arg)
{
    RhRunArgs *ra = (RhRunArgs *)arg;

    ra->rc = IpcRefHostRun(ra->host);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* 用例                                                               */
/* ------------------------------------------------------------------ */

UTEST_CASE(refhost, worker_sees_its_own_names_after_the_slot_is_reused)
{
    RhLab             lab;
    IpcRefHost       *host = NULL;
    IpcRefHostOptions hostOptions = IPC_REFHOST_OPTIONS_INIT;
    IpcContext       *ctx  = NULL;
    IpcModuleOptions  options = IPC_MODULE_OPTIONS_INIT;
    RhRunArgs         runArgs;
    pthread_t         runThread;
    IpcMessage        msgA;
    IpcMessage        msgB;

    UTEST_ASSERT_EQ(RhSetup(&lab), 0);

    /* 队列深度 1 + 单 worker：A 出队后槽 0 立即可复用，B 必然落进同一个槽。 */
    hostOptions.workers  = 1;
    hostOptions.queueMax = 1;
    UTEST_ASSERT_EQ(IpcRefHostCreate(&hostOptions, &host), IPC_OK);

    options.moduleId     = "alpha";
    options.ns           = RH_NS;
    options.confPath     = lab.conf;
    options.dispatch     = IpcRefHostDispatch;
    options.dispatchUser = host;
    UTEST_ASSERT_EQ(IpcRegister(&options, &ctx), IPC_OK);
    UTEST_ASSERT_EQ(IpcRefHostBind(host, ctx), IPC_OK);
    UTEST_ASSERT_EQ(IpcRefHostAddHandler(host, 1u, HandlerBlockThenRead, NULL),
                    IPC_OK);
    UTEST_ASSERT_EQ(IpcRefHostAddHandler(host, 2u, HandlerNote, NULL), IPC_OK);

    g_aEntered    = 0;
    g_bDispatched = 0;
    g_aSrc[0]     = '\0';
    g_bSrc[0]     = '\0';

    runArgs.host = host;
    runArgs.rc   = 0;
    UTEST_ASSERT_EQ(pthread_create(&runThread, NULL, RhRunMain, &runArgs), 0);

    /* 1) 投 A（自称来源 alpha）。worker 出队后进 handler 并阻塞。 */
    MakeMessage(&msgA, "alpha", 1u);
    UTEST_ASSERT_EQ(IpcRefHostDispatch(ctx, &msgA, host), IPC_OK);
    {
        struct timespec nap;
        int32_t         tries = 0;

        (void)pthread_mutex_lock(&g_mtx);
        while (g_aEntered == 0) {
            /* 有界等待，判据是 handler 自己置的标志，不是睡够时间。 */
            nap.tv_sec  = 0;
            nap.tv_nsec = 2000000; /* 2ms */
            (void)pthread_mutex_unlock(&g_mtx);
            (void)nanosleep(&nap, NULL);
            (void)pthread_mutex_lock(&g_mtx);
            tries++;
            if (tries > 5000) {
                break; /* 10s 都没进 handler：宿主没在跑，下面断言会红 */
            }
        }
        (void)pthread_mutex_unlock(&g_mtx);
    }
    UTEST_ASSERT_EQ(g_aEntered, 1);

    /*
     * 2) A 已被出队（槽 0 空闲），现在投 B —— 它必然填进同一个槽，
     *    FillJob 会先 memset 再写 "beta"。dispatch 返回即覆写完成。
     */
    MakeMessage(&msgB, "beta", 2u);
    UTEST_ASSERT_EQ(IpcRefHostDispatch(ctx, &msgB, host), IPC_OK);

    /* 3) 放行 handler A：它在「覆写已发生」之后才读 message->src。 */
    (void)pthread_mutex_lock(&g_mtx);
    g_bDispatched = 1;
    (void)pthread_cond_broadcast(&g_cv);
    (void)pthread_mutex_unlock(&g_mtx);

    /* 4) 收摊：Stop 会先停宿主再收线程；B 在收摊前会被 worker 处理掉。 */
    UTEST_ASSERT_EQ(IpcRefHostStop(host), IPC_OK);
    UTEST_ASSERT_EQ(pthread_join(runThread, NULL), 0);
    UTEST_ASSERT_EQ(runArgs.rc, IPC_OK);

    /* 断言：A 读到的是自己的名字，B 读到的是它自己的名字。 */
    UTEST_ASSERT_STREQ(g_aSrc, "alpha");
    UTEST_ASSERT_STREQ(g_bSrc, "beta");

    IpcRefHostDestroy(host);
    (void)IpcUnregister(ctx);
    (void)IpcDestroy(ctx);
    RhTeardown(&lab);
}
