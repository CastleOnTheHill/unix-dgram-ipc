/*
 * Copyright (c) 2026 <版权方待定>
 * 许可协议：<待定>
 *
 * utest.c -- 极简单元测试框架的实现。**仅供测试，不随交付物发布**。
 *
 * 刻意不引入任何第三方测试框架：本项目的验证纪律是「每个检查器都要先
 * 证明自己能报脏」，而一个只有 200 行的框架我能逐行说清它的失效方式；
 * 换成一个大框架就多了一层「它到底在没在跑」的不确定性。
 */
#include "utest.h"

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ipc/ipc.h" /* 只为 IpcResultToString，打印返回码时用 */

/* 用例表上限。够用即可；超了会报出来，不会静默丢。 */
#define UTEST_MAX_CASES 512

static UtestCase g_cases[UTEST_MAX_CASES];
static int32_t   g_caseCount;
static int32_t   g_currentFailed;
static const char *g_currentName  = "(none)";
static const char *g_currentSuite = "(none)";

/* ------------------------------------------------------------------ */
/* 用例看门狗：让「挂起」变成「失败」                                   */
/* ------------------------------------------------------------------ */

/*
 * 一个卡住的用例（实测：在库的 condvar 上无限等）会让整个套件看起来
 * 「还在跑」。这比失败更糟：失败有名字、有行号、有非零退出码；挂起什么都
 * 没有 —— 本地表现是「跑了十几分钟没动静」，CI 上就是一直挂着，只能靠人
 * 发现。所以运行器必须给每个用例加时限。
 *
 * 超时之后**不能**从信号处理函数里返回、也不能 longjmp 出去：现场停在库
 * 内部的 pthread_mutex/pthread_cond 中间态，跳出来会把锁留在锁定状态，
 * 后面每个用例都会以「莫名其妙失败」的形式连坐，比原问题更难查。所以把
 * 卡住的用例名报出来，然后 _exit(1)（失败，而不是「没跑成」的 2）。
 *
 * 时限用环境变量 IPC_UTEST_TIMEOUT 覆盖（秒，0 = 关闭看门狗）。
 */
static int         g_watchdogSec = 30;
static int         g_hangControl = 0;
static const char *g_wdSuite     = "(none)";
static const char *g_wdName      = "(none)";

static void UtestWatchdogAlarm(int sig)
{
    static const char head[] = "\n!! 用例挂起（超过看门狗时限）：";
    static const char dot[]  = ".";
    static const char tail[] = "\n   判为**失败**（退出码 1）：挂起既不是「通过」，也不是\n"
                               "   「还在跑」。剩余用例未运行 —— 先查这个用例，别只看总数。\n";
    (void)sig;
    /* 信号处理函数里只用 async-signal-safe 的 write()。 */
    (void)!write(STDERR_FILENO, head, sizeof(head) - 1);
    (void)!write(STDERR_FILENO, g_wdSuite, strlen(g_wdSuite));
    (void)!write(STDERR_FILENO, dot, sizeof(dot) - 1);
    (void)!write(STDERR_FILENO, g_wdName, strlen(g_wdName));
    (void)!write(STDERR_FILENO, tail, sizeof(tail) - 1);
    _exit(1);
}

int32_t UtestRegister(const char *suite, const char *name, UtestFunc func)
{
    if (suite == NULL || name == NULL || func == NULL) {
        return IPC_ERR_INVAL;
    }
    if (g_caseCount >= UTEST_MAX_CASES) {
        fprintf(stderr, "!! utest: 用例表已满（%d），'%s.%s' 没有被登记\n",
                UTEST_MAX_CASES, suite, name);
        return IPC_ERR_NOMEM;
    }
    g_cases[g_caseCount].suite  = suite;
    g_cases[g_caseCount].name   = name;
    g_cases[g_caseCount].func   = func;
    g_cases[g_caseCount].ran    = 0;
    g_cases[g_caseCount].failed = 0;
    g_caseCount++;
    return IPC_OK;
}

int32_t UtestCount(void)
{
    return g_caseCount;
}

static void UtestReportFailure(const char *file, int line, const char *expr,
                              const char *detail)
{
    fprintf(stderr, "   FAIL %s.%s\n        %s:%d: %s\n", g_currentSuite,
            g_currentName, file, line, expr);
    if (detail != NULL) {
        fprintf(stderr, "        %s\n", detail);
    }
}

void UtestFail(const char *file, int line, const char *expr, const char *fmt, ...)
{
    char detail[512];

    detail[0] = '\0';
    if (fmt != NULL) {
        va_list args;

        va_start(args, fmt);
        (void)vsnprintf(detail, sizeof(detail), fmt, args);
        va_end(args);
    }
    g_currentFailed++;
    UtestReportFailure(file, line, (expr != NULL) ? expr : "(no expression)",
                       (detail[0] != '\0') ? detail : NULL);
}

int32_t UtestRunAll(const char *filter)
{
    int32_t     index;
    int32_t     ran     = 0;
    int32_t     failed  = 0;
    int32_t     skipped = 0;
    const char *env;
    long        seconds;

    /* 看门狗时限：默认 30 秒/用例，可用 IPC_UTEST_TIMEOUT 覆盖。 */
    env = getenv("IPC_UTEST_TIMEOUT");
    if (env != NULL && env[0] != '\0') {
        seconds = strtol(env, NULL, 10);
        if (seconds < 0) {
            seconds = 0;
        }
        if (seconds > 3600) {
            seconds = 3600;
        }
        g_watchdogSec = (int)seconds;
    }
    g_hangControl = (getenv("IPC_UTEST_HANG_CONTROL") != NULL);
    if (g_watchdogSec > 0) {
        (void)signal(SIGALRM, UtestWatchdogAlarm);
        printf("   用例看门狗: 每个用例 %d 秒（IPC_UTEST_TIMEOUT 可改，0=关闭）\n",
               g_watchdogSec);
    } else {
        printf("   用例看门狗: **已关闭**（IPC_UTEST_TIMEOUT=0）——"
               " 挂起的用例不会被发现\n");
    }

    if (g_caseCount == 0) {
        /*
         * 一个用例都没有 = constructor 机制没生效（或测试文件没编进来）。
         * 这**不是**「没有测试要跑」，必须判失败：如果这里返回 0，
         * 一个全部测试都掉了的构建会显示成「通过」。
         */
        fprintf(stderr, "!! utest: 一个用例都没有登记到。constructor 没生效或\n"
                        "   测试文件没被编进来 —— 不能当成通过。\n");
        return -1;
    }

    if (g_hangControl) {
        /*
         * 看门狗的自检入口：故意挂住不返回，它**必须**被看门狗判为失败。
         * 没有这条，看门狗自己失灵时和「全部通过」一样安静 —— 本仓库对
         * 每个检查器的要求都是「先证明它能报脏」。
         * 由 make unit-selftest 驱动。
         */
        g_wdSuite = "control";
        g_wdName  = "deliberate_hang";
        printf("  [控制] control.deliberate_hang ... ");
        (void)fflush(stdout);
        if (g_watchdogSec > 0) {
            (void)alarm((unsigned)g_watchdogSec);
        }
        for (;;) {
            (void)pause(); /* 等看门狗的信号；正常情况下永远等不到别的 */
        }
    }

    for (index = 0; index < g_caseCount; index++) {
        UtestCase *testCase = &g_cases[index];

        if (filter != NULL && strstr(testCase->name, filter) == NULL &&
            strstr(testCase->suite, filter) == NULL) {
            skipped++;
            continue;
        }
        g_currentFailed = 0;
        g_currentName   = testCase->name;
        g_currentSuite  = testCase->suite;
        g_wdSuite       = testCase->suite;
        g_wdName        = testCase->name;
        printf("  [%02d/%02d] %s.%s ... ", index + 1, g_caseCount, testCase->suite,
               testCase->name);
        (void)fflush(stdout);
        if (g_watchdogSec > 0) {
            (void)alarm((unsigned)g_watchdogSec);
        }
        testCase->func();
        if (g_watchdogSec > 0) {
            (void)alarm(0);
        }
        testCase->ran    = 1;
        testCase->failed = g_currentFailed;
        if (g_currentFailed == 0) {
            printf("ok\n");
        } else {
            printf("FAILED (%d 处断言不成立)\n", g_currentFailed);
        }
        (void)fflush(stdout);
        ran++;
        if (g_currentFailed != 0) {
            failed++;
        }
    }

    printf("\n  合计: %d 个用例，%d 失败", ran, failed);
    if (skipped > 0) {
        printf("，%d 个被过滤掉", skipped);
    }
    printf("\n");
    if (failed != 0) {
        fprintf(stderr, "!! 单元测试有 %d 个用例失败\n", failed);
    }
    return failed;
}

int32_t UtestSelfCheck(void)
{
    int32_t before = g_currentFailed;

    g_currentFailed = 0;
    /* 故意让一条恒假断言失败，确认它确实被记下来。 */
    UtestFail(__FILE__, __LINE__, "1 == 2", "（这条失败是自检故意制造的）");
    if (g_currentFailed != 1) {
        fprintf(stderr, "!! utest 自检失败：故意制造的失败没有被记下来"
                        "（计数=%d）\n",
                g_currentFailed);
        g_currentFailed = before;
        return -1;
    }
    /* 再确认一次「恒真断言不会误报」。 */
    g_currentFailed = 0;
    if (1 == 2) {
        UtestFail(__FILE__, __LINE__, "(should never run)", NULL);
    }
    if (g_currentFailed != 0) {
        fprintf(stderr, "!! utest 自检失败：没有失败却记了 %d 次\n", g_currentFailed);
        g_currentFailed = before;
        return -1;
    }
    g_currentFailed = before;
    fprintf(stderr, "[utest 自检通过] 能记录失败，也不会无故记失败\n");
    return 0;
}
