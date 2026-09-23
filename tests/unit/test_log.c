/*
 * Copyright (c) 2026 <版权方待定>
 * 许可协议：<待定>
 *
 * test_log.c -- 日志出口的白盒单测。**仅供测试**。
 *
 * 这一组测的是「宿主能把库的诊断输出接进自己的日志系统」这条承诺能不能兑现，
 * 所以刻意用**假日志系统**（一个把内容抓下来的回调）来验证，而不是去看 stderr。
 *
 * 另外要覆盖两层出口的关系：per-module 出口**覆盖**全局出口，不是叠加；
 * per-module 级别 0 表示沿用全局，不是 DEBUG。
 */
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ipc_internal.h" /* 白盒：需要 IpcLogEmit 与 ctx 的字段布局 */
#include "ipc_log.h"
#include "ipc/ipc.h"
#include "utest.h"

/* ------------------------------------------------------------------ */
/* 假日志系统                                                         */
/* ------------------------------------------------------------------ */

static int32_t  g_calls;
static int32_t  g_lastLevel;
static char     g_lastModule[64];
static char     g_lastText[256];
static void    *g_lastUser;

static void CaptureLog(IpcLogLevel level, const char *moduleId, const char *format,
                       va_list args, void *user)
{
    g_calls++;
    g_lastLevel = (int32_t)level;
    g_lastUser  = user;
    (void)snprintf(g_lastModule, sizeof(g_lastModule), "%s",
                   (moduleId != NULL) ? moduleId : "(null)");
    (void)vsnprintf(g_lastText, sizeof(g_lastText), format, args);
}

static void OtherLog(IpcLogLevel level, const char *moduleId, const char *format,
                     va_list args, void *user)
{
    (void)level;
    (void)moduleId;
    (void)format;
    (void)args;
    (void)user;
    g_calls += 1000; /* 用一个显眼的偏移量区分「走的是哪个出口」 */
}

static void ResetCapture(void)
{
    g_calls     = 0;
    g_lastLevel = 0;
    g_lastUser  = NULL;
    g_lastModule[0] = '\0';
    g_lastText[0]   = '\0';
}

/* 把库的全局状态恢复到「没人接管」，免得污染后面的用例。 */
static void RestoreDefaults(void)
{
    IpcSetLogFunc(NULL, NULL);
    IpcSetLogLevel(0);
}

/* 造一个只用于测日志的假上下文。**不注册**，所以里面没有有效的 fd ——
 * 日志路径不碰 fd，这正是这里能这么干的原因。 */
static void InitFakeContext(IpcContext *ctx, const char *moduleId)
{
    memset(ctx, 0, sizeof(*ctx));
    (void)snprintf(ctx->moduleId, sizeof(ctx->moduleId), "%s", moduleId);
    ctx->fd = -1;
}

/* ------------------------------------------------------------------ */
/* 全局级别                                                           */
/* ------------------------------------------------------------------ */

UTEST_CASE(log, global_level_defaults_and_clamping)
{
    int32_t defaultLevel;

    RestoreDefaults();
    defaultLevel = IpcLogGlobalLevel();
    /* 默认是 WARN：不设任何东西时，DEBUG/INFO 不该刷屏。 */
    UTEST_ASSERT_EQ(defaultLevel, IPC_LOG_WARN);

    IpcSetLogLevel(IPC_LOG_DEBUG);
    UTEST_ASSERT_EQ(IpcLogGlobalLevel(), IPC_LOG_DEBUG);

    IpcSetLogLevel(IPC_LOG_ERROR);
    UTEST_ASSERT_EQ(IpcLogGlobalLevel(), IPC_LOG_ERROR);

    /* 0 与非法值都表示「恢复默认」，不是「比 DEBUG 还低」。 */
    IpcSetLogLevel(0);
    UTEST_ASSERT_EQ(IpcLogGlobalLevel(), defaultLevel);
    IpcSetLogLevel(99);
    UTEST_ASSERT_EQ(IpcLogGlobalLevel(), defaultLevel);
    IpcSetLogLevel(-5);
    UTEST_ASSERT_EQ(IpcLogGlobalLevel(), defaultLevel);
}

/* ------------------------------------------------------------------ */
/* 全局出口                                                           */
/* ------------------------------------------------------------------ */

UTEST_CASE(log, global_emit_reaches_the_installed_func)
{
    RestoreDefaults();
    ResetCapture();
    IpcSetLogLevel(IPC_LOG_DEBUG);
    IpcSetLogFunc(CaptureLog, (void *)0x1234);

    IpcLogGlobalEmit(IPC_LOG_INFO, "modA", "value=%d name=%s", 42, "x");

    UTEST_ASSERT_EQ(g_calls, 1);
    UTEST_ASSERT_EQ(g_lastLevel, IPC_LOG_INFO);
    UTEST_ASSERT_STREQ(g_lastModule, "modA");
    UTEST_ASSERT_STREQ(g_lastText, "value=42 name=x");
    /* user 要原样传回去，宿主靠它找到自己的日志对象。 */
    UTEST_ASSERT(g_lastUser == (void *)0x1234);

    RestoreDefaults();
}

UTEST_CASE(log, global_emit_filters_below_the_level)
{
    RestoreDefaults();
    ResetCapture();
    IpcSetLogFunc(CaptureLog, NULL);

    IpcSetLogLevel(IPC_LOG_WARN);
    IpcLogGlobalEmit(IPC_LOG_DEBUG, "m", "should be filtered");
    IpcLogGlobalEmit(IPC_LOG_INFO, "m", "should be filtered too");
    UTEST_ASSERT_EQ(g_calls, 0);

    IpcLogGlobalEmit(IPC_LOG_WARN, "m", "warn passes");
    UTEST_ASSERT_EQ(g_calls, 1);
    UTEST_ASSERT_EQ(g_lastLevel, IPC_LOG_WARN);

    IpcLogGlobalEmit(IPC_LOG_ERROR, "m", "error passes");
    UTEST_ASSERT_EQ(g_calls, 2);
    UTEST_ASSERT_EQ(g_lastLevel, IPC_LOG_ERROR);

    /* 全开之后 DEBUG 也能到。 */
    IpcSetLogLevel(IPC_LOG_DEBUG);
    IpcLogGlobalEmit(IPC_LOG_DEBUG, "m", "debug passes");
    UTEST_ASSERT_EQ(g_calls, 3);

    RestoreDefaults();
}

UTEST_CASE(log, global_emit_normalizes_illegal_levels)
{
    RestoreDefaults();
    ResetCapture();
    IpcSetLogFunc(CaptureLog, NULL);

    /* level = 0 不是一个合法级别（全零初始化要能表示「没填」），按 WARN 处理。 */
    IpcSetLogLevel(IPC_LOG_WARN);
    IpcLogGlobalEmit(0, "m", "zero level");
    UTEST_ASSERT_EQ(g_calls, 1);
    UTEST_ASSERT_EQ(g_lastLevel, IPC_LOG_WARN);

    /* 越界的级别夹到 ERROR。 */
    IpcLogGlobalEmit(999, "m", "too big");
    UTEST_ASSERT_EQ(g_calls, 2);
    UTEST_ASSERT_EQ(g_lastLevel, IPC_LOG_ERROR);

    IpcLogGlobalEmit(-3, "m", "too small");
    UTEST_ASSERT_EQ(g_calls, 3);
    UTEST_ASSERT_EQ(g_lastLevel, IPC_LOG_WARN);

    /* moduleId 允许为 NULL（配置加载阶段还没有模块身份），宿主必须自己接住。 */
    IpcLogGlobalEmit(IPC_LOG_WARN, NULL, "no module");
    UTEST_ASSERT_EQ(g_calls, 4);
    UTEST_ASSERT_STREQ(g_lastModule, "(null)");

    /* format 为 NULL：直接返回，不做任何事 —— 不能崩在 va_start 上。 */
    IpcLogGlobalEmit(IPC_LOG_WARN, "m", NULL);
    UTEST_ASSERT_EQ(g_calls, 4);

    RestoreDefaults();
}

UTEST_CASE(log, setting_the_func_again_replaces_it)
{
    RestoreDefaults();
    ResetCapture();
    IpcSetLogLevel(IPC_LOG_DEBUG);

    IpcSetLogFunc(OtherLog, NULL);
    IpcLogGlobalEmit(IPC_LOG_WARN, "m", "goes to OtherLog");
    UTEST_ASSERT_EQ(g_calls, 1000);

    ResetCapture();
    IpcSetLogFunc(CaptureLog, NULL);
    IpcLogGlobalEmit(IPC_LOG_WARN, "m", "goes to CaptureLog");
    UTEST_ASSERT_EQ(g_calls, 1);

    RestoreDefaults();
    /*
     * 恢复默认（func == NULL）之后回调不再被调用。这里刻意用 INFO 而不是
     * WARN+：默认出口会把 WARN 及以上写到 stderr，那会在测试输出里留下一行
     * 看着像报错的东西。用被过滤掉的级别来断言「出口确实换回来了」。
     */
    ResetCapture();
    IpcLogGlobalEmit(IPC_LOG_INFO, "m", "default path, level=%d", 1);
    UTEST_ASSERT_EQ(g_calls, 0);
}

/* ------------------------------------------------------------------ */
/* 带上下文的出口                                                     */
/* ------------------------------------------------------------------ */

UTEST_CASE(log, ctx_without_own_func_falls_back_to_global)
{
    IpcContext ctx;

    RestoreDefaults();
    ResetCapture();
    IpcSetLogLevel(IPC_LOG_DEBUG);
    IpcSetLogFunc(CaptureLog, NULL);
    InitFakeContext(&ctx, "modB");

    IpcLogEmit(&ctx, IPC_LOG_INFO, "hello %d", 7);
    UTEST_ASSERT_EQ(g_calls, 1);
    /* moduleId 取自上下文，不需要调用方自己拼前缀。 */
    UTEST_ASSERT_STREQ(g_lastModule, "modB");
    UTEST_ASSERT_STREQ(g_lastText, "hello 7");

    RestoreDefaults();
}

UTEST_CASE(log, ctx_own_func_overrides_global)
{
    IpcContext ctx;

    RestoreDefaults();
    ResetCapture();
    IpcSetLogLevel(IPC_LOG_DEBUG);
    IpcSetLogFunc(OtherLog, NULL); /* 全局挂了别的出口 */
    InitFakeContext(&ctx, "modC");
    ctx.logFunc = CaptureLog; /* 本模块自己挂一个 */
    ctx.logUser = (void *)0xBEEF;

    IpcLogEmit(&ctx, IPC_LOG_WARN, "per-module wins");
    /* 覆盖关系：全局那个一次都不该被调用（否则 g_calls 会是 1000）。 */
    UTEST_ASSERT_EQ(g_calls, 1);
    UTEST_ASSERT(g_lastUser == (void *)0xBEEF);
    UTEST_ASSERT_STREQ(g_lastModule, "modC");

    RestoreDefaults();
}

UTEST_CASE(log, ctx_own_level_overrides_global)
{
    IpcContext ctx;

    RestoreDefaults();
    ResetCapture();
    IpcSetLogFunc(CaptureLog, NULL);
    InitFakeContext(&ctx, "modD");

    /* 全局压到 ERROR，本模块开到 DEBUG：本模块的 DEBUG 必须能出来。 */
    IpcSetLogLevel(IPC_LOG_ERROR);
    ctx.logLevel = IPC_LOG_DEBUG;
    IpcLogEmit(&ctx, IPC_LOG_DEBUG, "debug from this module");
    UTEST_ASSERT_EQ(g_calls, 1);

    /* 反过来：全局开到 DEBUG，本模块只要 ERROR。 */
    ResetCapture();
    IpcSetLogLevel(IPC_LOG_DEBUG);
    ctx.logLevel = IPC_LOG_ERROR;
    IpcLogEmit(&ctx, IPC_LOG_INFO, "filtered for this module");
    UTEST_ASSERT_EQ(g_calls, 0);
    IpcLogEmit(&ctx, IPC_LOG_ERROR, "error passes");
    UTEST_ASSERT_EQ(g_calls, 1);

    /* logLevel == 0：沿用全局级别，**不是** DEBUG。 */
    ResetCapture();
    ctx.logLevel = 0;
    IpcSetLogLevel(IPC_LOG_ERROR);
    IpcLogEmit(&ctx, IPC_LOG_WARN, "should be filtered (global is ERROR)");
    UTEST_ASSERT_EQ(g_calls, 0);
    IpcLogEmit(&ctx, IPC_LOG_ERROR, "should pass");
    UTEST_ASSERT_EQ(g_calls, 1);

    RestoreDefaults();
}

UTEST_CASE(log, ctx_variants_are_safe)
{
    IpcContext ctx;

    RestoreDefaults();
    ResetCapture();
    IpcSetLogLevel(IPC_LOG_DEBUG);
    IpcSetLogFunc(CaptureLog, NULL);
    InitFakeContext(&ctx, "modE");

    /* ctx 为 NULL：退化成全局出口，moduleId 按 NULL 处理。 */
    IpcLogEmit(NULL, IPC_LOG_WARN, "no context");
    UTEST_ASSERT_EQ(g_calls, 1);
    UTEST_ASSERT_STREQ(g_lastModule, "(null)");

    /* ctx->moduleId 为空串时也按 NULL 处理，不要打出空的前缀。 */
    (void)snprintf(ctx.moduleId, sizeof(ctx.moduleId), "%s", "");
    IpcLogEmit(&ctx, IPC_LOG_WARN, "empty module id");
    UTEST_ASSERT_EQ(g_calls, 2);
    UTEST_ASSERT_STREQ(g_lastModule, "(null)");

    /* format 为 NULL。 */
    IpcLogEmit(&ctx, IPC_LOG_WARN, NULL);
    UTEST_ASSERT_EQ(g_calls, 2);

    /* 四个宏都要能展开并走到出口。 */
    (void)snprintf(ctx.moduleId, sizeof(ctx.moduleId), "%s", "modE");
    IPC_LOGD(&ctx, "d");
    IPC_LOGI(&ctx, "i");
    IPC_LOGW(&ctx, "w");
    IPC_LOGE(&ctx, "e");
    UTEST_ASSERT_EQ(g_calls, 6);

    RestoreDefaults();
}

UTEST_CASE(log, level_filter_drops_everything_below_it)
{
    IpcContext ctx;

    RestoreDefaults();
    ResetCapture();
    IpcSetLogFunc(CaptureLog, NULL);
    InitFakeContext(&ctx, "modF");
    IpcSetLogLevel(IPC_LOG_ERROR);

    /*
     * 被过滤掉的日志必须一条都到不了出口。
     *
     * 说明一下这条用例的边界：ipc.h 里承诺的「DEBUG 关掉时开销为零」是指
     * **连参数都不展开**，那件事在外面是观察不到的（观察点只有在代价里）。
     * 这里能钉住的是「一条都不发」；「不过早格式化」靠读 ipc_log.c 里
     * 过滤放在 va_start 之前这一条来保证，属于人工复核项，不是本用例的结论。
     */
    IpcLogEmit(&ctx, IPC_LOG_DEBUG, "filtered %d %s", 1, "x");
    IpcLogEmit(&ctx, IPC_LOG_INFO, "filtered %d %s", 2, "y");
    IpcLogEmit(&ctx, IPC_LOG_WARN, "filtered %d %s", 3, "z");
    UTEST_ASSERT_EQ(g_calls, 0);

    IpcLogEmit(&ctx, IPC_LOG_ERROR, "passes %d", 4);
    UTEST_ASSERT_EQ(g_calls, 1);
    UTEST_ASSERT_STREQ(g_lastText, "passes 4");

    RestoreDefaults();
}
