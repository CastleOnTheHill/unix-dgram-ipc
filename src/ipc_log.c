/*
 * Copyright (c) 2026 <版权方待定>
 * 许可协议：<待定>
 *
 * ipc_log.c -- 日志出口的实现。
 *
 * 设计要点：**不持任何锁**。日志可能从「已经持有等待表锁」或「正在拆除」
 * 的路径上打出来，此时再去拿一把全局日志锁就有死锁风险。所以这里只用到
 * 原子读写，而且读侧全部是 relaxed —— 最坏情况是刚设置完出口时有一两条
 * 日志走了旧出口，这对诊断信息来说完全可以接受。
 */
#include "ipc_log.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "ipc/ipc.h"

/*
 * 全局出口的三件状态。g_ 前缀是《OpenHarmony C 语言编程规范》对全局变量的
 * 要求。三个都单独原子化，互不干扰。
 */
static _Atomic(IpcLogFunc) g_logFunc;
static _Atomic(void *)      g_logUser;
static _Atomic(int32_t)     g_minLevel; /* 0 = 未设置，走默认 */

static const char *const g_levelTag[] = { "DEBUG", "INFO ", "WARN ", "ERROR" };

/*
 * 把可能为 0 / 越界的级别解析成合法级别。
 *
 * 两个方向**故意不对称**，不是笔误：
 *   - 0 与负数 =「没填」（全零初始化的默认）→ WARN；
 *   - 大于 ERROR = 有人把「最高严重度」写大了 → **向上夹到 ERROR**。
 *     若一并降成 WARN，一条本该最严重的日志会因为在 WARN 以下被静默丢掉；
 *     反过来向上夹永远不会让消息「变轻」，失败方向安全。
 */
static int32_t ResolveLevel(int32_t level)
{
    if (level >= (int32_t)IPC_LOG_DEBUG && level <= (int32_t)IPC_LOG_ERROR) {
        return level;
    }
    if (level > (int32_t)IPC_LOG_ERROR) {
        return (int32_t)IPC_LOG_ERROR;
    }
    return (int32_t)IPC_LOG_WARN;
}

int32_t IpcLogGlobalLevel(void)
{
    return ResolveLevel(atomic_load_explicit(&g_minLevel, memory_order_relaxed));
}

void IpcSetLogLevel(int32_t minLevel)
{
    int32_t resolved = minLevel;

    /* 0 或非法值都表示「恢复默认」，与头文件的承诺一致。 */
    if (resolved != 0 &&
        (resolved < (int32_t)IPC_LOG_DEBUG || resolved > (int32_t)IPC_LOG_ERROR)) {
        resolved = 0;
    }
    atomic_store_explicit(&g_minLevel, resolved, memory_order_relaxed);
}

void IpcSetLogFunc(IpcLogFunc func, void *user)
{
    /*
     * 先写 user 再写 func，且 func 用 release 发布：
     * 读侧一旦看到新的 func，就一定也看到了配套的 user，
     * 不会出现「新函数配旧 user」这种错配。
     */
    atomic_store_explicit(&g_logUser, user, memory_order_relaxed);
    atomic_store_explicit(&g_logFunc, func, memory_order_release);
}

/*
 * 出口分发：**不做级别过滤**，只负责「用哪个出口、按什么级别报」。级别过滤
 * 由调用方负责 —— 因为过滤用的「有效级别」随调用方而变（模块级 vs 全局级）。
 *
 * 为什么必须拆出来：IpcLogEmit 先按**模块自己的**级别滤过一遍，再走
 * IpcLogGlobalEmitVa 的话会被**全局**级别再滤一次。于是「模块把级别开到
 * DEBUG、全局压到 ERROR」时，本模块的 DEBUG 永远出不来 —— 模块级覆盖形同
 * 虚设，而且表现是「日志莫名其妙不见了」，极难查。
 */
void IpcLogDispatchVa(int32_t level, const char *moduleId, const char *format,
                      va_list args)
{
    IpcLogFunc func;
    void      *user;
    int32_t    resolved;

    if (format == NULL) {
        return;
    }
    resolved = ResolveLevel(level);
    func     = atomic_load_explicit(&g_logFunc, memory_order_acquire);
    user     = atomic_load_explicit(&g_logUser, memory_order_relaxed);

    if (func != NULL) {
        func((IpcLogLevel)resolved, moduleId, format, args, user);
        return;
    }

    /*
     * 没人接管日志时的默认行为：WARN 及以上写到 stderr。
     *
     * 这条分支存在的理由不是「方便调试」，而是 Q12 的答复要求
     * 「配置中的模块不存在时打印 warning 日志」—— 如果没人接管就静默丢弃，
     * 那条需求就落不了地。一旦宿主调了 IpcSetLogFunc 或给了 per-module 出口，
     * 本模块**完全不碰标准流**。
     */
    {
        int idx = resolved - (int32_t)IPC_LOG_DEBUG;

        fprintf(stderr, "[ipc %s] module=%s ", g_levelTag[idx],
                (moduleId != NULL) ? moduleId : "-");
        vfprintf(stderr, format, args);
        fputc('\n', stderr);
        fflush(stderr);
    }
}

void IpcLogGlobalEmitVa(int32_t level, const char *moduleId, const char *format,
                        va_list args)
{
    if (format == NULL) {
        return;
    }
    if (ResolveLevel(level) < IpcLogGlobalLevel()) {
        return; /* 过滤在前：不做任何格式化工作 */
    }
    IpcLogDispatchVa(level, moduleId, format, args);
}

void IpcLogGlobalEmit(int32_t level, const char *moduleId, const char *format, ...)
{
    va_list args;

    va_start(args, format);
    IpcLogGlobalEmitVa(level, moduleId, format, args);
    va_end(args);
}
