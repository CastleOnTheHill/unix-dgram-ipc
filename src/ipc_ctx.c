/*
 * Copyright (c) 2026 <版权方待定>
 * 许可协议：<待定>
 *
 * ipc_ctx.c -- 上下文的生命周期：注册、注销、销毁，以及诊断访问器。
 *
 * 本文件不碰 socket 系统调用（那些在 ipc_io.c），只负责「身份 + 状态 + 资源
 * 归属」。拆开的理由是：注册流程要做的判断很多（配置、命名空间歧义、UID、
 * 属组），而真正跟内核打交道的只有最后一步。
 */
#include <errno.h>
#include <grp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ipc_internal.h"

/* ------------------------------------------------------------------ */
/* 日志                                                               */
/* ------------------------------------------------------------------ */

void IpcLogEmit(const IpcContext *ctx, int32_t level, const char *format, ...)
{
    const char *moduleId = NULL;
    int32_t     effective;
    va_list     args;

    if (format == NULL) {
        return;
    }
    if (ctx != NULL) {
        moduleId = (ctx->moduleId[0] != '\0') ? ctx->moduleId : NULL;
    }

    /*
     * 级别解析：先看本模块有没有单独设过，没有就用全局的。
     * 过滤放在最前面 —— 连 va_list 都不构造，所以「关掉 DEBUG」是零成本，
     * 而不是「格式化完再丢掉」。
     */
    if (ctx != NULL && ctx->logLevel != 0) {
        effective = ctx->logLevel;
    } else {
        effective = IpcLogGlobalLevel();
    }
    if (level < effective) {
        return;
    }
    if (level < (int32_t)IPC_LOG_DEBUG) {
        level = (int32_t)IPC_LOG_DEBUG;
    }
    if (level > (int32_t)IPC_LOG_ERROR) {
        level = (int32_t)IPC_LOG_ERROR;
    }

    /* 本模块有自己的出口就用它，否则退回全局出口。是覆盖关系，不是叠加。 */
    if (ctx != NULL && ctx->logFunc != NULL) {
        va_start(args, format);
        ctx->logFunc((IpcLogLevel)level, moduleId, format, args, ctx->logUser);
        va_end(args);
        return;
    }
    va_start(args, format);
    IpcLogGlobalEmitVa(level, moduleId, format, args);
    va_end(args);
}

/* ------------------------------------------------------------------ */
/* 状态查询辅助                                                       */
/* ------------------------------------------------------------------ */

int32_t IpcCheckAlive(const IpcContext *ctx)
{
    if (ctx == NULL || ctx->fd < 0) {
        return IPC_ERR_STATE;
    }
    if (atomic_load_explicit(&ctx->fatalError, memory_order_relaxed) != 0) {
        return IPC_ERR_IO; /* 端点进了不可恢复的故障态 */
    }
    if (atomic_load_explicit(&ctx->stopped, memory_order_relaxed) != 0) {
        return IPC_ERR_STOPPED;
    }
    return IPC_OK;
}

const IpcConfigEntry *IpcPeerEntry(IpcContext *ctx, const char *dstModuleId)
{
    if (ctx == NULL || dstModuleId == NULL || dstModuleId[0] == '\0') {
        return NULL;
    }
    return IpcConfigFindModule(ctx->config, ctx->ns, dstModuleId);
}

void IpcLogIdentity(const IpcContext *ctx, const char *what)
{
    if (ctx == NULL) {
        return;
    }
    IPC_LOGI(ctx, "%s: module=%s ns=%s path=%s pid=%ld instance=%016llx", what,
             ctx->moduleId, ctx->ns, ctx->path, (long)ctx->ownerPid,
             (unsigned long long)ctx->instanceId);
}

/*
 * 启动期健康检查（ipc.h 文末 Q12 的答复）。
 *
 * 把同一个命名空间里端点路径不存在的模块**汇总成一条** WARN。
 * 为什么不逐条打：9 个模块只起了 1 个的时候，逐条会刷 8 行，而现场需要的是
 * 「一共有哪几个没起来」这一句话。
 *
 * 为什么是 WARN 不是 ERROR：启动顺序天然会经过「别人还没起来」的阶段。
 */
int32_t IpcWarnMissingPeers(IpcContext *ctx)
{
    char    missing[512];
    size_t  used  = 0;
    int32_t count = 0;
    int32_t total;
    int32_t i;

    if (ctx == NULL || ctx->config == NULL) {
        return 0;
    }
    missing[0] = '\0';
    total      = IpcConfigGetCount(ctx->config);

    for (i = 0; i < total; i++) {
        const IpcConfigEntry *entry = IpcConfigGetEntry(ctx->config, i);
        struct stat           st;

        if (entry == NULL) {
            continue;
        }
        if (strcmp(entry->ns, ctx->ns) != 0) {
            continue; /* 只关心自己这个命名空间 */
        }
        if (strcmp(entry->moduleId, ctx->moduleId) == 0) {
            continue; /* 自己不算 */
        }
        if (stat(entry->path, &st) == 0) {
            continue; /* 存在 */
        }
        count++;
        if (used < sizeof(missing) - 1) {
            size_t room = sizeof(missing) - 1 - used;
            size_t len  = IpcStrlcpy(missing + used, entry->moduleId, room + 1);

            if (len > room) {
                len = room; /* 缓冲满了：名字被截断，但计数仍然准确 */
            }
            used += len;
            if (count < total && used + 2 < sizeof(missing) - 1) {
                missing[used++]     = ',';
                missing[used]       = '\0';
            }
        }
    }

    if (count > 0) {
        IPC_LOGW(ctx, "%d configured module(s) in ns '%s' have no endpoint yet: %s",
                 count, ctx->ns, missing);
    }
    return count;
}

/* ------------------------------------------------------------------ */
/* 接收缓冲                                                           */
/* ------------------------------------------------------------------ */

/*
 * 分配接收缓冲。**尺寸只由我们自己的 maxPayload 决定**，与任何外来输入无关。
 *
 * 这条不变式是整库最重要的一条：如果按报头里声明的 payloadLen 去分配，
 * 发送方只要谎报一个巨大的数字，就能让接收方按那个数字去 malloc。
 * 超长报文交给 recvmsg 的 MSG_TRUNC 检测，不需要先把它们收下来。
 */
static int32_t AllocRecvBuffers(IpcContext *ctx)
{
    ctx->recvBufSize = (size_t)IPC_HDR_SIZE + (size_t)ctx->maxPayload;
    ctx->recvBuf     = (uint8_t *)calloc(1, ctx->recvBufSize);
    ctx->ctrlBuf     = (uint8_t *)calloc(1, IPC_CTRL_SIZE);
    if (ctx->recvBuf == NULL || ctx->ctrlBuf == NULL) {
        free(ctx->recvBuf);
        free(ctx->ctrlBuf);
        ctx->recvBuf = NULL;
        ctx->ctrlBuf = NULL;
        return IPC_ERR_NOMEM;
    }
    ctx->recvPayload = ctx->recvBuf + IPC_HDR_SIZE;

    /*
     * msg 骨架在这里建一次、之后复用：只有 msg_control / msg_controllen /
     * msg_flags 需要每条报文复位（recvmsg 会就地改写它们）。
     * 刻意不填 msg_name/msg_namelen —— 我们不关心对端的**地址**，
     * 身份一律以内核凭据为准，地址在这里没有任何授权含义。
     */
    ctx->recvIov.iov_base = ctx->recvBuf;
    ctx->recvIov.iov_len  = ctx->recvBufSize;
    memset(&ctx->recvMsg, 0, sizeof(ctx->recvMsg));
    ctx->recvMsg.msg_iov    = &ctx->recvIov;
    ctx->recvMsg.msg_iovlen = 1;
    return IPC_OK;
}

static void FreeRecvBuffers(IpcContext *ctx)
{
    free(ctx->recvBuf);
    free(ctx->ctrlBuf);
    ctx->recvBuf     = NULL;
    ctx->ctrlBuf     = NULL;
    ctx->recvPayload = NULL;
}

/* ------------------------------------------------------------------ */
/* 注册                                                               */
/* ------------------------------------------------------------------ */

/* 从调用者给的选项里解析出名字字段，失败时统一走清理路径。 */
static int32_t CopyOptionStrings(IpcContext *ctx, const IpcModuleOptions *options)
{
    if (options->moduleId == NULL || options->moduleId[0] == '\0') {
        return IPC_ERR_INVAL;
    }
    if (IpcStrlcpy(ctx->moduleId, options->moduleId, sizeof(ctx->moduleId)) >=
        sizeof(ctx->moduleId)) {
        return IPC_ERR_INVAL; /* 名字超长 */
    }
    if (options->groupName != NULL && options->groupName[0] != '\0') {
        if (IpcStrlcpy(ctx->groupName, options->groupName, sizeof(ctx->groupName)) >=
            sizeof(ctx->groupName)) {
            return IPC_ERR_INVAL;
        }
    }
    return IPC_OK;
}

/*
 * 按配置表推导命名空间：只找到唯一一处同名模块时取它的 ns，找到多处报歧义。
 * 这正是 examples/README.md 里 `conf/02-two-namespaces.conf` 要测的场景。
 */
static int32_t DeriveNamespace(IpcContext *ctx, const char *moduleId)
{
    const IpcConfigEntry *found = NULL;
    int32_t               total;
    int32_t               i;

    total = IpcConfigGetCount(ctx->config);
    for (i = 0; i < total; i++) {
        const IpcConfigEntry *entry = IpcConfigGetEntry(ctx->config, i);

        if (entry == NULL || strcmp(entry->moduleId, moduleId) != 0) {
            continue;
        }
        if (found != NULL) {
            IpcLogGlobalEmit(IPC_LOG_WARN, moduleId,
                             "module id appears in more than one namespace; "
                             "set IpcModuleOptions.ns explicitly");
            return IPC_ERR_CONFIG;
        }
        found = entry;
    }
    if (found == NULL) {
        return IPC_ERR_NOENT;
    }
    (void)IpcStrlcpy(ctx->ns, found->ns, sizeof(ctx->ns));
    return IPC_OK;
}

/*
 * 注册时的身份自检。
 *
 * **这不是安全边界**，只是防误用：真正的强制保护来自目录/文件权限，以及
 * 接收端的凭据校验。这里的意义是让「以错误的身份启动」在启动时就暴露，
 * 而不是等到第一条报文被对端拒绝。
 *
 * allowUidSplit == 0：real / effective / 配置 UID 三者必须一致。
 * allowUidSplit == 1：只比 effective（为 setuid 场景放宽）。
 *
 * 注意接收侧**没有**这个开关：内核 SCM_CREDENTIALS 只给对端的 real UID。
 */
static int32_t CheckOwnIdentity(const IpcContext *ctx, uid_t authorizedUid,
                               int32_t allowUidSplit)
{
    uid_t real = ctx->realUid;
    uid_t eff  = geteuid();

    if (allowUidSplit != 0) {
        if (eff != authorizedUid) {
            IPC_LOGE(ctx,
                     "uid self-check failed: effective uid %u != authorized uid %u",
                     (unsigned)eff, (unsigned)authorizedUid);
            return IPC_ERR_PERM;
        }
        return IPC_OK;
    }
    if (real != authorizedUid || eff != authorizedUid) {
        IPC_LOGE(ctx,
                 "uid self-check failed: real=%u effective=%u authorized=%u "
                 "(allowUidSplit=0 requires all three to match)",
                 (unsigned)real, (unsigned)eff, (unsigned)authorizedUid);
        return IPC_ERR_PERM;
    }
    return IPC_OK;
}

int32_t IpcRegister(const IpcModuleOptions *options, IpcContext **outContext)
{
    IpcContext           *ctx;
    const IpcConfigEntry *entry;
    const char           *confPath;
    int32_t               rc;

    if (options == NULL || outContext == NULL) {
        return IPC_ERR_INVAL;
    }
    *outContext = NULL;

    ctx = (IpcContext *)calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return IPC_ERR_NOMEM;
    }
    ctx->fd       = -1;
    ctx->lockFd   = -1;

    rc = CopyOptionStrings(ctx, options);
    if (rc != IPC_OK) {
        IPC_LOGE(ctx, "invalid IpcModuleOptions (moduleId missing or too long)");
        goto Fail;
    }

    /*
     * 载荷上限的两步串联：先补缺省，再压天花板。
     * 顺序不能反，否则会出现「默认值本身超上限」这种自相矛盾的结果。
     */
    {
        uint32_t want = (options->maxPayload != 0) ? options->maxPayload
                                                   : (uint32_t)IPC_PAYLOAD_DEFAULT;

        if (want > (uint32_t)IPC_PAYLOAD_HARD_MAX) {
            want = (uint32_t)IPC_PAYLOAD_HARD_MAX;
        }
        ctx->maxPayload = want;
    }
    ctx->allowUidSplit         = options->allowUidSplit;
    ctx->broadcastIncludeSelf  = options->broadcastIncludeSelf;
    ctx->sndBufSize            = options->sendBufSize;
    ctx->rcvBufSize            = options->recvBufSize;
    ctx->dispatch              = options->dispatch;
    ctx->dispatchUser          = options->dispatchUser;
    ctx->logFunc               = options->log;
    ctx->logUser               = options->logUser;
    ctx->logLevel              = options->logLevel;
    ctx->realUid               = getuid();
    ctx->ownerPid              = getpid();

    /* 1) 配置表。 */
    confPath = (options->confPath != NULL) ? options->confPath : IPC_CONF_DEFAULT;
    rc       = IpcConfigLoad(confPath, &ctx->config);
    if (rc != IPC_OK) {
        IPC_LOGE(ctx, "cannot load module table '%s': %s", confPath,
                 IpcResultToString(rc));
        goto Fail;
    }

    /* 2) 命名空间：显式给了就用，没给就按模块名唯一推导。 */
    if (options->ns != NULL && options->ns[0] != '\0') {
        if (IpcStrlcpy(ctx->ns, options->ns, sizeof(ctx->ns)) >= sizeof(ctx->ns)) {
            rc = IPC_ERR_INVAL;
            goto Fail;
        }
    } else {
        rc = DeriveNamespace(ctx, ctx->moduleId);
        if (rc != IPC_OK) {
            IPC_LOGE(ctx, "cannot derive namespace for '%s': %s", ctx->moduleId,
                     IpcResultToString(rc));
            goto Fail;
        }
    }

    /* 3) 查表。查不到就是「配置里没有这个模块」，不是「对端离线」。 */
    entry = IpcConfigFindModule(ctx->config, ctx->ns, ctx->moduleId);
    if (entry == NULL) {
        IPC_LOGE(ctx, "module '%s' is not declared in ns '%s'", ctx->moduleId, ctx->ns);
        rc = IPC_ERR_NOENT;
        goto Fail;
    }
    (void)IpcStrlcpy(ctx->path, entry->path, sizeof(ctx->path));
    ctx->authorizedUid = entry->uid;

    /* 4) 身份自检。 */
    rc = CheckOwnIdentity(ctx, ctx->authorizedUid, ctx->allowUidSplit);
    if (rc != IPC_OK) {
        goto Fail;
    }

    /* 5) 属组（可选）。 */
    if (ctx->groupName[0] != '\0') {
        struct group *gr = getgrnam(ctx->groupName);

        if (gr == NULL) {
            IPC_LOGE(ctx, "group '%s' does not exist", ctx->groupName);
            rc = IPC_ERR_NOENT;
            goto Fail;
        }
        ctx->groupGid  = gr->gr_gid;
        ctx->haveGroup = 1;
    }

    ctx->instanceId = IpcGenInstanceId();

    /* 6) 接收缓冲。必须在返回成功之前就绪，否则 IpcHandleReadable 会拒绝工作。 */
    rc = AllocRecvBuffers(ctx);
    if (rc != IPC_OK) {
        goto Fail;
    }

    /* 7) 等待表。abortFlag 指向 stopped，IpcRequestStop 一翻它，
     *    所有等待者立刻从 IpcPendingWait 里出来。 */
    rc = IpcPendingInit(&ctx->pending, options->maxPending, &ctx->stopped);
    if (rc != IPC_OK) {
        goto Fail;
    }
    ctx->pendingCreated = 1;

    if (pthread_mutex_init(&ctx->lifeLock, NULL) != 0) {
        rc = IPC_ERR_IO;
        goto Fail;
    }
    ctx->lifeLockCreated = 1;
    if (pthread_cond_init(&ctx->lifeCv, NULL) != 0) {
        rc = IPC_ERR_IO;
        goto Fail;
    }
    ctx->lifeCvCreated = 1;

    /* 8) 真正跟内核打交道的那一步。 */
    rc = IpcIoCreateEndpoint(ctx);
    if (rc != IPC_OK) {
        goto Fail;
    }

    /* 9) 启动期健康检查：同命名空间里还没起来的模块，一条 WARN 说清楚。 */
    (void)IpcWarnMissingPeers(ctx);

    *outContext = ctx;
    return IPC_OK;

Fail:
    /*
     * 完整回滚：任何一步失败都不留半成品。
     * IpcIoCreateEndpoint 失败时内部已经把自己建过的东西收干净了，
     * 所以这里只需要处理本函数自己申请的。
     */
    IpcIoDestroyEndpoint(ctx);
    if (ctx->lifeCvCreated) {
        (void)pthread_cond_destroy(&ctx->lifeCv);
    }
    if (ctx->lifeLockCreated) {
        (void)pthread_mutex_destroy(&ctx->lifeLock);
    }
    if (ctx->pendingCreated) {
        IpcPendingDestroy(&ctx->pending);
    }
    FreeRecvBuffers(ctx);
    IpcConfigDestroy(ctx->config);
    free(ctx);
    return rc;
}

/* ------------------------------------------------------------------ */
/* 注销与销毁                                                         */
/* ------------------------------------------------------------------ */

int32_t IpcUnregister(IpcContext *ctx)
{
    if (ctx == NULL) {
        return IPC_ERR_INVAL;
    }
    if (atomic_load_explicit(&ctx->teardownDone, memory_order_acquire) != 0) {
        return IPC_OK; /* 已经拆完了，幂等 */
    }

    (void)pthread_mutex_lock(&ctx->lifeLock);
    if (atomic_load_explicit(&ctx->teardownStarted, memory_order_relaxed) == 0) {
        /*
         * 第一个调用者干活。守卫必须建立在**对象还活着的时候**：
         * 早期版本用「标记 + 之后 free」，导致连调两次就是 use-after-free
         * （ASan 复现过）。本函数绝不 free，free 只在 IpcDestroy 里。
         */
        atomic_store_explicit(&ctx->teardownStarted, 1, memory_order_relaxed);
        (void)pthread_mutex_unlock(&ctx->lifeLock);

        /* 1) 停止接活。 */
        atomic_store_explicit(&ctx->stopped, 1, memory_order_release);

        /* 2) 了结挂起的同步请求：先广播唤醒，再等它们真正退出等待。 */
        IpcPendingWakeAll(&ctx->pending);
        IpcPendingWaitDrained(&ctx->pending);

        /* 3) 关 fd → 持锁 unlink → 放锁（都在 IpcIoDestroyEndpoint 里）。 */
        IpcIoDestroyEndpoint(ctx);

        /* 4) 配置表与接收缓冲都是本实例私有的，可以释放。 */
        FreeRecvBuffers(ctx);
        IpcConfigDestroy(ctx->config);
        ctx->config = NULL;

        (void)pthread_mutex_lock(&ctx->lifeLock);
        atomic_store_explicit(&ctx->teardownDone, 1, memory_order_release);
        (void)pthread_cond_broadcast(&ctx->lifeCv);
        (void)pthread_mutex_unlock(&ctx->lifeLock);
        IpcLogGlobalEmit(IPC_LOG_INFO, ctx->moduleId, "unregistered");
        return IPC_OK;
    }
    /* 别的线程正在拆：等到它拆完再返回，这样「我返回了」就等于「资源已释放」。 */
    while (atomic_load_explicit(&ctx->teardownDone, memory_order_acquire) == 0) {
        (void)pthread_cond_wait(&ctx->lifeCv, &ctx->lifeLock);
    }
    (void)pthread_mutex_unlock(&ctx->lifeLock);
    return IPC_OK;
}

int32_t IpcDestroy(IpcContext *ctx)
{
    if (ctx == NULL) {
        return IPC_ERR_INVAL;
    }
    if (atomic_load_explicit(&ctx->teardownDone, memory_order_acquire) == 0) {
        /* 契约：只能在 IpcUnregister 完成之后调用。这里什么都不释放。 */
        return IPC_ERR_STATE;
    }
    IpcPendingDestroy(&ctx->pending);
    (void)pthread_cond_destroy(&ctx->lifeCv);
    (void)pthread_mutex_destroy(&ctx->lifeLock);
    free(ctx);
    return IPC_OK;
}

int32_t IpcRequestStop(IpcContext *ctx)
{
    if (ctx == NULL) {
        return IPC_ERR_INVAL;
    }
    atomic_store_explicit(&ctx->stopped, 1, memory_order_release);
    IpcPendingWakeAll(&ctx->pending); /* 唤醒所有在 IpcSend 里等回复的线程 */
    return IPC_OK;
}

int32_t IpcIsStopped(const IpcContext *ctx)
{
    if (ctx == NULL) {
        return 1;
    }
    if (ctx->fd < 0) {
        return 1;
    }
    return (atomic_load_explicit(&ctx->stopped, memory_order_relaxed) != 0) ? 1 : 0;
}

/*
 * 本端点的 select fd。宿主的 select 线程在每轮循环里取一次。
 *
 * 【2026-09-23】这个函数之前在 src/ 下**根本没有定义** —— 头文件里声明了、
 * 参考宿主也在调，但实现漏了，于是所有引用它的测试都链接不上。补在这里，
 * 并且放在 IpcIsStopped 旁边：这两个函数是配套的，宿主的标准用法是
 * 「先问 IpcIsStopped，再拿 fd」，见 tests/support/refhost.c。
 *
 * 返回值语义严格按 ipc.h 的约定来，不多不少：
 *   - ctx 为 NULL            → IPC_ERR_INVAL（上下文非法）
 *   - fd < 0                 → IPC_ERR_STATE（已注销 / 正在拆除）
 *   - 已 IpcRequestStop 但尚未拆除 → **照常返回 fd**
 *
 * 最后那条是有意这样定的，别「顺手」改成返回错误：宿主把 fd 加进 fd_set 之后
 * 才收到停止请求是正常时序，这时如果 IpcGetSelectFd 突然开始报错，宿主就得去
 * 猜「到底该关掉哪个 fd」。判断该不该继续 select 是 IpcIsStopped 的职责，
 * 这个函数只负责报 fd。
 */
int32_t IpcGetSelectFd(const IpcContext *ctx)
{
    if (ctx == NULL) {
        return IPC_ERR_INVAL;
    }
    if (ctx->fd < 0) {
        return IPC_ERR_STATE;
    }
    return ctx->fd;
}

/* ------------------------------------------------------------------ */
/* 诊断访问器                                                         */
/* ------------------------------------------------------------------ */

const char *IpcGetModuleId(const IpcContext *ctx)
{
    return (ctx != NULL) ? ctx->moduleId : NULL;
}

const char *IpcGetNamespace(const IpcContext *ctx)
{
    return (ctx != NULL) ? ctx->ns : NULL;
}

const char *IpcGetSocketPath(const IpcContext *ctx)
{
    return (ctx != NULL) ? ctx->path : NULL;
}

uint64_t IpcGetInstanceId(const IpcContext *ctx)
{
    return (ctx != NULL) ? ctx->instanceId : 0;
}

int32_t IpcGetStatistics(const IpcContext *ctx, IpcStatistics *outStatistics)
{
    if (ctx == NULL || outStatistics == NULL) {
        return IPC_ERR_INVAL;
    }
    memset(outStatistics, 0, sizeof(*outStatistics));
/*
 * 用同一份字段表生成快照。relaxed 就够了：统计是**只增计数器**，
 * 单个字段本身的读是原子的；但整体不是一个一致快照，所以 ipc.h 里
 * 明确写了「只用于日志和测试断言」。
 * C11 的 atomic_load_explicit 接受 const volatile 指针，所以 ctx 的
 * const 属性在这里不需要被强转掉。
 */
#define IPC_STAT_SNAPSHOT(field)                                               \
    outStatistics->field =                                                      \
        atomic_load_explicit(&ctx->st.field, memory_order_relaxed);
    IPC_STAT_FIELDS(IPC_STAT_SNAPSHOT)
#undef IPC_STAT_SNAPSHOT
    return IPC_OK;
}
