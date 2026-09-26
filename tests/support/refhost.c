/*
 * Copyright (c) 2026 <版权方待定>
 * 许可协议：<待定>
 *
 * refhost.c -- 参考宿主的实现。**仅供测试，正式接入时整个文件都不要**。
 * 见 refhost.h 顶部：它是「用库的人怎么写宿主」的示例，不是交付物。
 *
 * 三样东西对应老系统的三样东西：
 *   独立 select 线程        → SelectMain
 *   回调线程池 + 队列        → WorkerMain + 环形队列
 *   回调注册表 + 事件分发    → handler 表 + IpcRefHostDispatch
 *
 * 特别说明本文件的线程都归**宿主**所有：库本身一个线程都不建（那是设计约束
 * 之一）。这里之所以能有线程，正是因为它扮演的是宿主。
 */
#include "refhost.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* 类型                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    int32_t           inUse;
    uint32_t          event;
    IpcRefHandlerFunc handler;
    void             *user;
} IpcRefHandlerEntry;

/*
 * 一个排队中的回调任务。
 *
 * 为什么要深拷：库交给 dispatch 的 IpcMessage 里，ns / src / dst / data 都是
 * **只在本次 dispatch 调用期间有效**的指针（见 ipc.h 的生命周期约定）。本宿主
 * 是「投线程池」那一类，dispatch 返回时 worker 还没跑，所以必须在这里把内容
 * 拷成自己的。唯独 reply 是按值携带的，整体结构体拷贝会把它一起带过来。
 */
typedef struct {
    IpcMessage message;
    char       ns[IPC_NS_MAX];
    char       src[IPC_NAME_MAX];
    char       dst[IPC_NAME_MAX];
    void      *payload;
    size_t     payloadLen;
} IpcRefJob;

struct IpcRefHost {
    IpcRefHostOptions options;

    IpcContext *contexts[IPC_REFHOST_MAX_CONTEXTS];
    int32_t     contextCount;

    IpcRefHandlerEntry handlers[IPC_REFHOST_MAX_HANDLERS];

    IpcRefJob *jobs;
    int32_t    jobCap;
    int32_t    jobHead;
    int32_t    jobCount;

    /* 一把锁护住 handler 表与队列。刻意只用一把：本文件里没有嵌套获取，
     * 而且所有跨线程调用的动作（handler、malloc）都在**不持锁**时做。 */
    pthread_mutex_t lock;
    pthread_cond_t  queueCv;
    int32_t         lockCreated;
    int32_t         cvCreated;

    pthread_t selectThread;
    pthread_t workers[IPC_REFHOST_MAX_WORKERS];
    int32_t   workerCount;
    int32_t   selectStarted;
    int32_t   workerStarted;

    int32_t wakeFd[2]; /* 自管道：Stop() 用它把 select 捅醒 */
    int32_t wakeCreated;

    _Atomic int32_t stopRequested;
    int32_t         running;
    int32_t         runResult;

    uint64_t queueFullDropped;
    uint64_t noHandlerDropped;
    uint64_t handlerErrors;
};

/* ------------------------------------------------------------------ */
/* 小工具                                                             */
/* ------------------------------------------------------------------ */

static int32_t ClampInt(int32_t value, int32_t low, int32_t high, int32_t fallback)
{
    if (value == 0) {
        return fallback;
    }
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

/* 关掉自管道里的字节。select 是水平触发的，不排空会一直立刻返回。 */
static void DrainWakePipe(IpcRefHost *host)
{
    char    scratch[64];
    ssize_t n;

    for (;;) {
        n = read(host->wakeFd[0], scratch, sizeof(scratch));
        if (n <= 0) {
            return;
        }
    }
}

static void WakeSelectThread(IpcRefHost *host)
{
    char    byte = 'w';
    ssize_t n;

    if (!host->wakeCreated) {
        return;
    }
    /*
     * 管道是 O_NONBLOCK：满了说明已经有人写过，本来就会醒，EAGAIN 属正常路径。
     * 但**不能**用 (void) 把返回值丢掉 —— glibc 给 write 标了 warn_unused_result，
     * 而 (void) 转换压不住它，在 -Werror 下会直接编译失败（集成测试因此整个
     * 没编译过）。把结果接到局部变量里才算消费掉。
     */
    n = write(host->wakeFd[1], &byte, 1);
    (void)n;
}

/* 在持锁状态下找 handler。找不到返回 NULL。 */
static IpcRefHandlerEntry *FindHandlerLocked(IpcRefHost *host, uint32_t event)
{
    int32_t i;

    for (i = 0; i < IPC_REFHOST_MAX_HANDLERS; i++) {
        if (host->handlers[i].inUse && host->handlers[i].event == event) {
            return &host->handlers[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* 生命周期                                                           */
/* ------------------------------------------------------------------ */

int32_t IpcRefHostCreate(const IpcRefHostOptions *options, IpcRefHost **outHost)
{
    IpcRefHost *host;

    if (outHost == NULL) {
        return IPC_ERR_INVAL;
    }
    *outHost = NULL;

    host = (IpcRefHost *)calloc(1, sizeof(*host));
    if (host == NULL) {
        return IPC_ERR_NOMEM;
    }
    host->options.workers =
        ClampInt((options != NULL) ? options->workers : 0, 1, IPC_REFHOST_MAX_WORKERS, 2);
    host->jobCap = ClampInt((options != NULL) ? options->queueMax : 0, 1,
                            IPC_REFHOST_QUEUE_MAX, 64);
    host->options.selectTimeoutMs =
        (options != NULL && options->selectTimeoutMs != 0) ? options->selectTimeoutMs
                                                          : 200;
    host->options.maxBatch =
        (options != NULL && options->maxBatch != 0) ? options->maxBatch : 64;

    host->jobs = (IpcRefJob *)calloc((size_t)host->jobCap, sizeof(*host->jobs));
    if (host->jobs == NULL) {
        free(host);
        return IPC_ERR_NOMEM;
    }
    if (pthread_mutex_init(&host->lock, NULL) != 0) {
        free(host->jobs);
        free(host);
        return IPC_ERR_IO;
    }
    host->lockCreated = 1;
    if (pthread_cond_init(&host->queueCv, NULL) != 0) {
        (void)pthread_mutex_destroy(&host->lock);
        free(host->jobs);
        free(host);
        return IPC_ERR_IO;
    }
    host->cvCreated = 1;

    /*
     * 自管道。用 pipe2 一步带上 CLOEXEC 与非阻塞：非阻塞是必须的（否则
     * 一个不读的 select 线程会让写端把 Stop 卡住），CLOEXEC 是防 fd 泄漏到
     * 子进程。
     */
    if (pipe(host->wakeFd) != 0) {
        (void)pthread_cond_destroy(&host->queueCv);
        (void)pthread_mutex_destroy(&host->lock);
        free(host->jobs);
        free(host);
        return IPC_ERR_IO;
    }
    host->wakeCreated = 1;
    {
        int32_t i;

        for (i = 0; i < 2; i++) {
            int32_t flags = fcntl(host->wakeFd[i], F_GETFL, 0);

            if (flags >= 0) {
                (void)fcntl(host->wakeFd[i], F_SETFL, flags | O_NONBLOCK);
            }
            (void)fcntl(host->wakeFd[i], F_SETFD, FD_CLOEXEC);
        }
    }

    *outHost = host;
    return IPC_OK;
}

void IpcRefHostDestroy(IpcRefHost *host)
{
    int32_t i;

    if (host == NULL) {
        return;
    }
    if (host->running) {
        fprintf(stderr, "!! IpcRefHostDestroy: 宿主还在跑（先 IpcRefHostStop）\n");
        return;
    }
    /* 队列里可能还有没被处理的 job —— 它们持有 malloc 出来的载荷。 */
    for (i = 0; i < host->jobCount; i++) {
        int32_t slot = (host->jobHead + i) % host->jobCap;

        free(host->jobs[slot].payload);
        host->jobs[slot].payload = NULL;
    }
    free(host->jobs);
    if (host->wakeCreated) {
        (void)close(host->wakeFd[0]);
        (void)close(host->wakeFd[1]);
    }
    if (host->cvCreated) {
        (void)pthread_cond_destroy(&host->queueCv);
    }
    if (host->lockCreated) {
        (void)pthread_mutex_destroy(&host->lock);
    }
    free(host);
}

int32_t IpcRefHostBind(IpcRefHost *host, IpcContext *ctx)
{
    int32_t rc = IPC_OK;

    if (host == NULL || ctx == NULL) {
        return IPC_ERR_INVAL;
    }
    (void)pthread_mutex_lock(&host->lock);
    if (host->running) {
        rc = IPC_ERR_STATE; /* Run 之后再绑：select 线程看不到，不如直接拒绝 */
    } else if (host->contextCount >= IPC_REFHOST_MAX_CONTEXTS) {
        rc = IPC_ERR_TOOMANY;
    } else {
        host->contexts[host->contextCount] = ctx;
        host->contextCount++;
    }
    (void)pthread_mutex_unlock(&host->lock);
    return rc;
}

/* ------------------------------------------------------------------ */
/* 注册表                                                             */
/* ------------------------------------------------------------------ */

int32_t IpcRefHostAddHandler(IpcRefHost *host, uint32_t event,
                             IpcRefHandlerFunc handler, void *user)
{
    int32_t i;
    int32_t freeSlot = -1;
    int32_t rc       = IPC_OK;

    if (host == NULL || handler == NULL) {
        return IPC_ERR_INVAL;
    }
    (void)pthread_mutex_lock(&host->lock);
    for (i = 0; i < IPC_REFHOST_MAX_HANDLERS; i++) {
        if (host->handlers[i].inUse && host->handlers[i].event == event) {
            rc = IPC_ERR_BUSY; /* 不覆盖：静默顶掉别人的回调是最难查的那类问题 */
            break;
        }
        if (!host->handlers[i].inUse && freeSlot < 0) {
            freeSlot = i;
        }
    }
    if (rc == IPC_OK) {
        if (freeSlot < 0) {
            rc = IPC_ERR_TOOMANY;
        } else {
            host->handlers[freeSlot].inUse   = 1;
            host->handlers[freeSlot].event   = event;
            host->handlers[freeSlot].handler = handler;
            host->handlers[freeSlot].user    = user;
        }
    }
    (void)pthread_mutex_unlock(&host->lock);
    return rc;
}

int32_t IpcRefHostRemoveHandler(IpcRefHost *host, uint32_t event)
{
    IpcRefHandlerEntry *entry;

    if (host == NULL) {
        return IPC_ERR_INVAL;
    }
    (void)pthread_mutex_lock(&host->lock);
    entry = FindHandlerLocked(host, event);
    if (entry != NULL) {
        memset(entry, 0, sizeof(*entry));
    }
    (void)pthread_mutex_unlock(&host->lock);
    return (entry != NULL) ? IPC_OK : IPC_ERR_NOENT;
}

/* ------------------------------------------------------------------ */
/* 分发                                                               */
/* ------------------------------------------------------------------ */

/*
 * 把一条报文深拷进一个 job 槽位。调用者已持有锁。
 * 载荷分配失败时返回 IPC_ERR_NOMEM，调用者负责计数并丢弃。
 */
static int32_t FillJob(IpcRefJob *job, const IpcMessage *message)
{
    memset(job, 0, sizeof(*job));

    /*
     * 整体结构体拷贝：reply 域是按值携带的，这一下就把它带过来了。
     * 但 ns / src / dst / data 四个指针仍然指向库内部缓冲，必须重指。
     */
    job->message      = *message;
    job->message.ns   = job->ns;
    job->message.src  = job->src;
    job->message.dst  = job->dst;
    job->message.data = NULL;

    (void)snprintf(job->ns, sizeof(job->ns), "%s",
                   (message->ns != NULL) ? message->ns : "");
    (void)snprintf(job->src, sizeof(job->src), "%s",
                   (message->src != NULL) ? message->src : "");
    (void)snprintf(job->dst, sizeof(job->dst), "%s",
                   (message->dst != NULL) ? message->dst : "");

    job->payloadLen = message->len;
    if (message->len > 0) {
        if (message->data == NULL) {
            return IPC_ERR_INVAL;
        }
        job->payload = malloc(message->len);
        if (job->payload == NULL) {
            return IPC_ERR_NOMEM;
        }
        memcpy(job->payload, message->data, message->len);
        job->message.data = job->payload;
    }
    return IPC_OK;
}

int32_t IpcRefHostDispatch(IpcContext *ctx, const IpcMessage *message, void *hostUser)
{
    IpcRefHost *host = (IpcRefHost *)hostUser;
    IpcRefJob  *job;
    int32_t     slot;
    int32_t     rc;

    if (host == NULL || message == NULL) {
        return IPC_ERR_INVAL;
    }
    /*
     * ctx 用不上：worker 靠 message.reply.ctx 回复，而库已经把那个填好了。
     * 真实宿主的 dispatch 同样不需要它（它自己有注册表对象）。
     */
    (void)ctx;
    (void)pthread_mutex_lock(&host->lock);
    if (host->jobCount >= host->jobCap) {
        host->queueFullDropped++;
        (void)pthread_mutex_unlock(&host->lock);
        return IPC_ERR_AGAIN; /* 库会把它计入 dispatchFailed */
    }
    slot = (host->jobHead + host->jobCount) % host->jobCap;
    job  = &host->jobs[slot];
    rc   = FillJob(job, message);
    if (rc != IPC_OK) {
        memset(job, 0, sizeof(*job));
        host->queueFullDropped++; /* 记在同一个「没排上队」的口径里 */
        (void)pthread_mutex_unlock(&host->lock);
        return rc;
    }
    /*
     * 顶层分类消息的「哪个上下文收到的」由 reply.ctx 携带（库已经填好），
     * 所以这里不需要再存 ctx 给 worker —— worker 用 message.reply.ctx 回。
     */
    host->jobCount++;
    (void)pthread_cond_signal(&host->queueCv);
    (void)pthread_mutex_unlock(&host->lock);
    return IPC_OK;
}

int32_t IpcRefHostDispatchInline(IpcContext *ctx, const IpcMessage *message,
                                 void *hostUser)
{
    IpcRefHost        *host = (IpcRefHost *)hostUser;
    IpcRefHandlerEntry entry;
    int32_t            found = 0;

    if (host == NULL || message == NULL) {
        return IPC_ERR_INVAL;
    }
    (void)ctx;
    (void)pthread_mutex_lock(&host->lock);
    {
        IpcRefHandlerEntry *live = FindHandlerLocked(host, message->event);

        if (live != NULL) {
            entry = *live;
            found = 1;
        }
    }
    (void)pthread_mutex_unlock(&host->lock);
    if (!found) {
        host->noHandlerDropped++;
        return IPC_ERR_NOENT;
    }

    /*
     * 就在这里、本线程上直接调业务回调。这正是 ipc.h 里死锁检测要识别的
     * 形态：回调若调 IpcSend，库会看到「dispatch 还没返回」而返回
     * IPC_ERR_DEADLOCK，而不是真的卡死。
     *
     * 注意 message 是 const，而回调要写 reply.replied，所以这里做一次显式
     * 去 const —— 与 IpcReply 内部的处理同源（本示例代码忠于真实宿主的写法）。
     */
    {
        IpcMessage       *writable = (IpcMessage *)(uintptr_t)message;
        IpcRefHandlerFunc handler  = entry.handler;
        int32_t           rc       = handler(writable, entry.user);

        if (rc != IPC_OK) {
            host->handlerErrors++;
        }
        return rc;
    }
}

/* ------------------------------------------------------------------ */
/* 线程                                                               */
/* ------------------------------------------------------------------ */

static void *WorkerMain(void *arg)
{
    IpcRefHost *host = (IpcRefHost *)arg;

    for (;;) {
        IpcRefJob          job;
        IpcRefHandlerEntry entry;
        int32_t            found = 0;
        int32_t            rc;

        (void)pthread_mutex_lock(&host->lock);
        while (host->jobCount == 0 &&
               !atomic_load_explicit(&host->stopRequested, memory_order_relaxed)) {
            (void)pthread_cond_wait(&host->queueCv, &host->lock);
        }
        if (host->jobCount == 0) {
            (void)pthread_mutex_unlock(&host->lock);
            break; /* 收到停止，且队列已空 */
        }
        /*
         * 把 job **搬到自己栈上**再放槽位：槽位一放回环形队列就随时可能被
         * 新的 dispatch 复用，留在那里读就是数据竞争。
         * 载荷的所有权一并转移（槽位里的指针置空）。
         */
        job = host->jobs[host->jobHead];
        host->jobs[host->jobHead].payload = NULL;
        /*
         * 结构体拷贝只复制指针的**值**：ns / src / dst 仍然指着队列槽里
         * 的数组，而槽马上回到环里，会被下一次 FillJob 先 memset 再写上
         * 新名字。不重指的话，本回调读 message->src 拿到的就是**别人的
         * 身份** —— 一次静默的串包，外加跨线程数据竞争（UB）。
         * data 不用重指：它指向 malloc 出来的载荷缓冲，所有权已经随这次
         * 拷贝转移到 worker（槽里的指针刚被置 NULL，worker 负责释放）。
         * reply 是按值携带的，结构体拷贝自然带上。
         */
        job.message.ns  = job.ns;
        job.message.src = job.src;
        job.message.dst = job.dst;
        host->jobHead = (host->jobHead + 1) % host->jobCap;
        host->jobCount--;

        {
            IpcRefHandlerEntry *live = FindHandlerLocked(host, job.message.event);

            if (live != NULL) {
                entry = *live;
                found = 1;
            }
        }
        (void)pthread_mutex_unlock(&host->lock);

        if (!found) {
            host->noHandlerDropped++;
        } else {
            rc = entry.handler(&job.message, entry.user);
            if (rc != IPC_OK) {
                host->handlerErrors++;
            }
        }
        free(job.payload);
    }
    return NULL;
}

static void *SelectMain(void *arg)
{
    IpcRefHost *host = (IpcRefHost *)arg;
    int32_t     fatal = IPC_OK;

    while (!atomic_load_explicit(&host->stopRequested, memory_order_relaxed)) {
        fd_set          readSet;
        struct timeval  timeout;
        struct timeval *timeoutPtr = NULL;
        int32_t         maxFd      = -1;
        int32_t         i;
        int32_t         rc;

        FD_ZERO(&readSet);
        FD_SET(host->wakeFd[0], &readSet);
        maxFd = host->wakeFd[0];

        for (i = 0; i < host->contextCount; i++) {
            IpcContext *ctx = host->contexts[i];
            int32_t     fd;

            if (ctx == NULL || IpcIsStopped(ctx) != 0) {
                /*
                 * 已经停止/注销的上下文不再加进 fd_set。必须跳过：它的 fd
                 * 已经关了或永远可读（IpcHandleReadable 直接返回 STOPPED
                 * 且不排空），留在集合里就是一个忙循环。
                 */
                continue;
            }
            fd = IpcGetSelectFd(ctx);
            if (fd < 0) {
                continue;
            }
            if (fd >= FD_SETSIZE) {
                /* 超出 select 能表达的 fd 范围。真实宿主会改用 poll/epoll，
                 * 参考宿主选择直接报错而不是悄悄漏掉。 */
                fprintf(stderr, "!! refhost: fd %d 超出 FD_SETSIZE，本示例不支持\n", fd);
                fatal = IPC_ERR_STATE;
                break;
            }
            FD_SET(fd, &readSet);
            if (fd > maxFd) {
                maxFd = fd;
            }
        }
        if (fatal != IPC_OK) {
            break;
        }

        if (host->options.selectTimeoutMs > 0) {
            timeout.tv_sec  = host->options.selectTimeoutMs / 1000;
            timeout.tv_usec = (host->options.selectTimeoutMs % 1000) * 1000;
            timeoutPtr      = &timeout;
        }

        rc = select(maxFd + 1, &readSet, NULL, NULL, timeoutPtr);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "!! refhost: select 失败: %s\n", strerror(errno));
            fatal = IPC_ERR_IO;
            break;
        }
        if (FD_ISSET(host->wakeFd[0], &readSet)) {
            DrainWakePipe(host);
        }

        for (i = 0; i < host->contextCount; i++) {
            IpcContext *ctx = host->contexts[i];
            int32_t     fd;
            int32_t     handled;

            if (ctx == NULL || IpcIsStopped(ctx) != 0) {
                continue;
            }
            fd = IpcGetSelectFd(ctx);
            if (fd < 0 || !FD_ISSET(fd, &readSet)) {
                continue;
            }
            handled = IpcHandleReadable(ctx, host->options.maxBatch);
            if (handled == IPC_ERR_STOPPED) {
                continue; /* 正常收摊，下一轮它就不在集合里了 */
            }
            if (handled < 0) {
                fprintf(stderr, "!! refhost: IpcHandleReadable 返回 %s（模块 %s）\n",
                        IpcResultToString(handled), IpcGetModuleId(ctx));
                fatal = handled;
                break;
            }
        }
        if (fatal != IPC_OK) {
            break;
        }
    }

    host->runResult = fatal;
    return NULL;
}

int32_t IpcRefHostRun(IpcRefHost *host)
{
    int32_t i;
    int32_t rc;

    if (host == NULL) {
        return IPC_ERR_INVAL;
    }
    (void)pthread_mutex_lock(&host->lock);
    if (host->running || host->contextCount == 0) {
        (void)pthread_mutex_unlock(&host->lock);
        return IPC_ERR_STATE; /* 没绑上下文就没有 fd 可 select，别假装能跑 */
    }
    host->running = 1;
    (void)pthread_mutex_unlock(&host->lock);

    /*
     * 这里**故意不**清 stopRequested。原来写的是无条件
     *     atomic_store(&host->stopRequested, 0);
     * 那是个能把进程永久挂死的 bug，机制如下。
     *
     * 调用方（宿主）通常这样用：主线程启动宿主线程跑 Run()，自己干活，
     * 收尾时 Stop() + pthread_join(宿主线程)。于是存在一个窗口：
     * **Stop() 已经执行完，而宿主线程还没被调度、还没进到 Run() 里**。
     * 此时 Stop 已经把 stopRequested 置成 1 并往自管道写了唤醒字节；
     * Run 一进来就把它清成 0 —— 停止请求被整个吃掉。后果是：
     *   - select 线程的退出条件（`while (!stopRequested)`）永远不成立，
     *     它就按 200ms 超时一直空转下去；
     *   - Run() 卡在 pthread_join(select) 上不返回；
     *   - 调用方卡在 pthread_join(宿主线程) 上不返回；
     *   - 进程再也退不出去。
     *
     * 用 gdb 抓到的现场（三者同时成立，正是上面这条链）：
     *   SelectMain      @ refhost.c 的 select() 里，反复超时
     *   IpcRefHostRun   @ pthread_join(selectThread)
     *   主线程          @ pthread_join(hostThread)
     *
     * 这个窗口不是理论上的：只要脚本/主循环短到「跑完并 Stop 的速度快过
     * 新线程被调度」，它就必然发生。实测把进程钉在单核（taskset -c 0）后
     * 40/40 稳定复现；集成测试里表现为**偶发**的「发送方进程 4s 内没结束」，
     * 而且只在脚本里没有 sleep 的进程上出现。
     *
     * 契约上也站不住：refhost.h 写明 Stop「任意线程可调，幂等」，Run
     * 「阻塞直到 IpcRefHostStop() 被调用」。一个先于 Run 到达的 Stop 必须
     * 被认账 —— Run 应当立刻收摊返回。
     *
     * 不清它有没有副作用？没有。下面照常起 worker 与 select 线程，而它们
     * 一进循环就看到 stopRequested 已经是 1，立刻各自收摊返回，Run 随即
     * 返回。用 IPC 发出去的报文不受影响（发送发生在调用方自己的线程上）。
     * 重复 Run 本来就不支持（见 refhost.h：Run 一次，之后 Destroy）。
     */
    host->runResult = IPC_OK;

    for (i = 0; i < host->options.workers; i++) {
        if (pthread_create(&host->workers[i], NULL, WorkerMain, host) != 0) {
            fprintf(stderr, "!! refhost: 起 worker 失败（第 %d 个）\n", i);
            break;
        }
        host->workerStarted++;
    }

    rc = pthread_create(&host->selectThread, NULL, SelectMain, host);
    if (rc != 0) {
        fprintf(stderr, "!! refhost: 起 select 线程失败: %s\n", strerror(rc));
        atomic_store_explicit(&host->stopRequested, 1, memory_order_relaxed);
    } else {
        host->selectStarted = 1;
        (void)pthread_join(host->selectThread, NULL);
    }

    /* select 线程已经收摊，再让 worker 池收摊。 */
    (void)pthread_mutex_lock(&host->lock);
    atomic_store_explicit(&host->stopRequested, 1, memory_order_relaxed);
    (void)pthread_cond_broadcast(&host->queueCv);
    (void)pthread_mutex_unlock(&host->lock);

    for (i = 0; i < host->workerStarted; i++) {
        (void)pthread_join(host->workers[i], NULL);
    }
    host->workerStarted = 0;

    (void)pthread_mutex_lock(&host->lock);
    host->running = 0;
    (void)pthread_mutex_unlock(&host->lock);

    if (!host->selectStarted) {
        return IPC_ERR_IO;
    }
    return host->runResult;
}

int32_t IpcRefHostStop(IpcRefHost *host)
{
    int32_t i;

    if (host == NULL) {
        return IPC_ERR_INVAL;
    }
    /*
     * 先翻停止位，再唤醒两边：
     *   - IpcRequestStop 让正在 IpcSend 里等回复的业务线程立刻返回；
     *   - 自管道让 select 线程从 select 里出来。
     * 顺序不能反：反了会出现「等待者先醒、但库还不知道要停」的窗口。
     */
    for (i = 0; i < host->contextCount; i++) {
        if (host->contexts[i] != NULL) {
            (void)IpcRequestStop(host->contexts[i]);
        }
    }
    atomic_store_explicit(&host->stopRequested, 1, memory_order_release);
    WakeSelectThread(host);
    (void)pthread_mutex_lock(&host->lock);
    (void)pthread_cond_broadcast(&host->queueCv);
    (void)pthread_mutex_unlock(&host->lock);
    return IPC_OK;
}

/* ------------------------------------------------------------------ */
/* 诊断                                                               */
/* ------------------------------------------------------------------ */

int32_t IpcRefHostQueueDepth(IpcRefHost *host, int32_t *outDepth)
{
    if (host == NULL || outDepth == NULL) {
        return IPC_ERR_INVAL;
    }
    (void)pthread_mutex_lock(&host->lock);
    *outDepth = host->jobCount;
    (void)pthread_mutex_unlock(&host->lock);
    return IPC_OK;
}

int32_t IpcRefHostDropped(IpcRefHost *host, uint64_t *outQueueFull,
                          uint64_t *outNoHandler)
{
    if (host == NULL) {
        return IPC_ERR_INVAL;
    }
    (void)pthread_mutex_lock(&host->lock);
    if (outQueueFull != NULL) {
        *outQueueFull = host->queueFullDropped;
    }
    if (outNoHandler != NULL) {
        *outNoHandler = host->noHandlerDropped;
    }
    (void)pthread_mutex_unlock(&host->lock);
    return IPC_OK;
}

int32_t IpcRefHostHandlerErrors(IpcRefHost *host, uint64_t *outErrors)
{
    if (host == NULL || outErrors == NULL) {
        return IPC_ERR_INVAL;
    }
    (void)pthread_mutex_lock(&host->lock);
    *outErrors = host->handlerErrors;
    (void)pthread_mutex_unlock(&host->lock);
    return IPC_OK;
}
