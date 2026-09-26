/*
 * Copyright (c) 2026 <版权方待定>
 * 许可协议：<待定>
 *
 * ipc_testmod.c -- 可脚本化的模块进程，供**黑盒**集成测试使用。
 *
 * =====================================================================
 * 它为什么是黑盒
 * =====================================================================
 * 本文件**只**包含两个头文件：
 *     include/ipc/ipc.h   （对外唯一头文件）
 *     tests/support/refhost.h（参考宿主）
 * 它**不**包含 src/ 下的任何内部头。所以用它做的测试只能看到「公开契约」，
 * 看不到内部结构体、内部函数、统计之外的实现细节。
 *
 * 这一条是机械检查的（不是自我声明）：.workbuddy/checks/zcc.sh 会 grep
 * 工具源码里有没有 ipc_internal.h / ipc_*.h 这类内部头，出现就报错。
 * 白盒测试（tests/unit/）相反，它们就是要 include ipc_internal.h。
 * 两类测试的界线因此落在**目录 + 包含关系**上，不靠人记。
 *
 * =====================================================================
 * 用法
 * =====================================================================
 *   ipc_testmod --conf <path> --module <id> [选项]
 *
 *   --ns <ns>            命名空间，默认 testns
 *   --max-payload <n>    传给 IpcModuleOptions.maxPayload
 *   --pending <n>        传给 IpcModuleOptions.maxPending
 *   --journal <path>     日志行追加写（同时镜像到 stdout）
 *   --script <path>      顺序执行脚本；不给则从 stdin 读
 *   --serve-ms <n>       脚本跑完后继续服务这么久（给对端留出收尾时间）
 *
 * =====================================================================
 * 线程模型（这正是老系统的形态，工具的职责就是复现它）
 * =====================================================================
 *   主线程      —— 执行脚本（发报文、等回复）。等价于「业务线程」。
 *   宿主线程    —— IpcRefHostRun：跑独立 select 线程 + 回调线程池。
 *
 * 所以同步 send 在主线程阻塞时，宿主线程仍然能把回复读回来 ——
 * 这正是库要求的前提。工具**不自己写 select 循环**，一律走参考宿主，
 * 避免「测试代码里实现了半个库」。
 *
 * =====================================================================
 * journal 行格式（集成测试就靠它做同步，不用固定 sleep）
 * =====================================================================
 *   READY <module> <socket-path>        注册完成，端点已建立
 *   RECV <src> <event> <len> <text>     业务回调收到一条报文
 *   REPLY <dst> <rc>                    在回调里回复过
 *   SEND <dst> <rc-name>                一次发送的结果
 *   STEP <n> <cmd>                      脚本第 n 条命令即将执行
 *   STATS <name>=<value> ...            统计快照
 *   DONE <exit-code>                    收尾完成
 *
 * 每行都立即 fflush，「行已出现」就等于「那件事已经发生」——
 * 这就是集成测试唯一允许的同步手段。
 */
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ipc/ipc.h"
#include "refhost.h"

/* ------------------------------------------------------------------ */
/* journal                                                            */
/* ------------------------------------------------------------------ */

static FILE *g_journal;      /* NULL = 只输出到 stdout */
static char  g_module[64];

static void Journal(const char *format, ...)
{
    va_list args;
    va_list copy;

    va_start(args, format);
    va_copy(copy, args);
    (void)vfprintf(stdout, format, args);
    (void)fputc('\n', stdout);
    (void)fflush(stdout);
    if (g_journal != NULL) {
        (void)vfprintf(g_journal, format, copy);
        (void)fputc('\n', g_journal);
        /* 每次都 flush：集成测试把「行出现」当作事件已经发生的证据，
         * 缓存在缓冲区里的一行等于没写。 */
        (void)fflush(g_journal);
    }
    va_end(copy);
    va_end(args);
}

/* 把 IpcResult 转成一个不含空格的名字，方便 shell 侧 grep。 */
static const char *ResultName(int32_t rc)
{
    switch (rc) {
    case IPC_OK:            return "ok";
    case IPC_ERR_INVAL:     return "inval";
    case IPC_ERR_NOMEM:     return "nomem";
    case IPC_ERR_IO:        return "io";
    case IPC_ERR_AGAIN:     return "again";
    case IPC_ERR_NOENT:     return "noent";
    case IPC_ERR_OFFLINE:   return "offline";
    case IPC_ERR_PERM:      return "perm";
    case IPC_ERR_BUSY:      return "busy";
    case IPC_ERR_CRED:      return "cred";
    case IPC_ERR_PROTO:     return "proto";
    case IPC_ERR_TIMEOUT:   return "timeout";
    case IPC_ERR_STOPPED:   return "stopped";
    case IPC_ERR_DEADLOCK:  return "deadlock";
    case IPC_ERR_MSGSIZE:   return "msgsize";
    case IPC_ERR_CONFIG:    return "config";
    case IPC_ERR_TOOMANY:   return "toomany";
    case IPC_ERR_STATE:     return "state";
    default:                return "unknown";
    }
}

/* ------------------------------------------------------------------ */
/* 业务回调                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t event;
    int32_t  mode; /* 0 = 只记录；1 = 回显 */
} Handler;

#define MAX_HANDLERS 16

static Handler g_handlers[MAX_HANDLERS];
static int32_t g_handlerCount;

static const Handler *FindHandler(uint32_t event)
{
    int32_t i;

    for (i = 0; i < g_handlerCount; i++) {
        if (g_handlers[i].event == event) {
            return &g_handlers[i];
        }
    }
    return NULL;
}

/*
 * 业务回调。跑在宿主的 worker 线程上（不是 select 线程），所以这里可以
 * 安全地调 IpcReply —— 这正是老系统的形态。
 */
static int32_t OnMessage(IpcMessage *message, void *user)
{
    const Handler *handler;
    char           text[256];
    size_t         copy;

    (void)user;

    /*
     * 载荷当文本处理：测试只发可打印 ASCII，遇到不可打印字节换成 '.'。
     * 不这么做的话 journal 行会被二进制字节切断，shell 侧的 grep 就失效。
     */
    copy = message->len;
    if (copy > sizeof(text) - 1) {
        copy = sizeof(text) - 1;
    }
    {
        size_t i;
        const unsigned char *raw = (const unsigned char *)message->data;

        for (i = 0; i < copy; i++) {
            text[i] = (raw[i] >= 0x20 && raw[i] < 0x7F) ? (char)raw[i] : '.';
        }
        text[copy] = '\0';
    }

    Journal("RECV %s %u %zu %s", message->src, message->event, message->len, text);

    handler = FindHandler(message->event);
    if (handler == NULL || handler->mode != 1) {
        return IPC_OK; /* 只记录，不回复 */
    }

    /*
     * 回复。注意 message 是宿主的、**非 const**（IpcReply 要写
     * reply.replied 那个记账字段），这里原样传即可。
     *
     * 回复内容是 "echo:" + 收到的文本，这样请求方能断言「回的是我这一条」，
     * 而不是「有个回复来了」。
     */
    {
        char   reply[300];
        size_t used = 0;
        size_t i;

        used = (size_t)snprintf(reply, sizeof(reply), "echo:");
        for (i = 0; i < copy && used + 2 < sizeof(reply); i++) {
            reply[used++] = text[i];
        }
        reply[used] = '\0';
        {
            int32_t rc = IpcReply(message, reply, used);
            Journal("REPLY %s %s", message->src, ResultName(rc));
        }
    }
    return IPC_OK;
}

/* ------------------------------------------------------------------ */
/* 宿主线程                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    IpcRefHost *host;
    int32_t     rc;
} HostArgs;

static void *HostMain(void *arg)
{
    HostArgs *ha = (HostArgs *)arg;

    ha->rc = IpcRefHostRun(ha->host);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* 脚本命令                                                           */
/* ------------------------------------------------------------------ */

static void SleepMs(int32_t ms)
{
    struct timespec ts;

    if (ms <= 0) {
        return;
    }
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    (void)nanosleep(&ts, NULL);
}

static void DumpStats(IpcContext *ctx)
{
    IpcStatistics st;

    if (IpcGetStatistics(ctx, &st) != IPC_OK) {
        Journal("STATS error");
        return;
    }
    /*
     * 只打集成测试真正要断言的字段。全打出来的话，shell 侧的
     * grep 会被噪音淹没，而「断言某个值」需要的是一个稳定的键名。
     */
    Journal("STATS sendAttempts=%llu sendEnqueued=%llu sendFailed=%llu "
            "broadcastTargets=%llu broadcastSkipped=%llu "
            "recvRead=%llu recvRejected=%llu recvRejCred=%llu "
            "recvRejProto=%llu recvRejTrunc=%llu recvDelivered=%llu "
            "dispatchInvoked=%llu replySent=%llu replyMatched=%llu "
            "replyUnmatched=%llu pendingRejected=%llu pendingTimeout=%llu",
            (unsigned long long)st.sendAttempts,
            (unsigned long long)st.sendEnqueued,
            (unsigned long long)st.sendFailed,
            (unsigned long long)st.broadcastTargets,
            (unsigned long long)st.broadcastSkipped,
            (unsigned long long)st.recvRead,
            (unsigned long long)st.recvRejected,
            (unsigned long long)st.recvRejCred,
            (unsigned long long)st.recvRejProto,
            (unsigned long long)st.recvRejTrunc,
            (unsigned long long)st.recvDelivered,
            (unsigned long long)st.dispatchInvoked,
            (unsigned long long)st.replySent,
            (unsigned long long)st.replyMatched,
            (unsigned long long)st.replyUnmatched,
            (unsigned long long)st.pendingRejected,
            (unsigned long long)st.pendingTimeout);
}

/* 一条命令。返回 1 = 继续，0 = 脚本要求停止。 */
/*
 * 命令的字段约定，**统一是「定长字段 + 末尾整段负载」**：
 *
 *     handler   <event> <mode>
 *     post      <dst> <event> <text>
 *     send      <dst> <event> <text>
 *     sendto    <dst> <event> <timeoutMs> <text>
 *     broadcast <event> <text>
 *     sleep     <ms>
 *     stats
 *     stop
 *
 * 负载永远是**最后一段**（可以含空格，不需要引号）。早先的版本用
 * 「strtok 三次 + 剩余整段」去凑，结果 `send <dst> <text>` 里的负载被
 * a2 吃掉、a3 拿到空串 —— 命令能解析但语义是错的，而且看起来一切正常。
 * 所以改成先声明字段个数，再按个数取，剩下的一律当负载。
 */
#define CMD_MAX_ARGS 3

/* 从 *cursor 处取下一个空白分隔的词；取完把 *cursor 推到词尾之后。
 * 返回 NULL 表示这一行没有更多字段了。 */
static char *NextToken(char **cursor)
{
    char *p = *cursor;
    char *start;

    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p == '\0' || *p == '\n' || *p == '\r') {
        *cursor = p;
        return NULL;
    }
    start = p;
    while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
        p++;
    }
    if (*p != '\0') {
        *p = '\0';
        p++;
    }
    *cursor = p;
    return start;
}

/* 把游标之后剩下的整段当作负载（去掉首尾空白，去掉行尾 CR/LF）。
 * 负载里可以有空格 —— 它永远是每条命令的最后一段。 */
static char *RestPayload(char *cursor)
{
    char  *start = cursor;
    size_t len;

    while (*start == ' ' || *start == '\t') {
        start++;
    }
    len = strlen(start);
    while (len > 0 && (start[len - 1] == '\n' || start[len - 1] == '\r' ||
                       start[len - 1] == ' ' || start[len - 1] == '\t')) {
        start[--len] = '\0';
    }
    return start;
}

/*
 * 每条命令各自吃掉**自己声明的**字段个数。
 *
 * 早先的实现是「先无脑吃掉 4 个词，再把剩下的当负载」，于是
 * `sendto <dst> <event> <timeout> <text>` 里 text 的第一个词被当成第 4 个
 * 字段吃掉，负载凭空少了一截 —— 命令能跑，断言却对不上，而且看不出来。
 * 字段个数必须由命令自己说，不能由解析器猜。
 */
static int32_t TakeArgs(char **cursor, char **args, int32_t want)
{
    int32_t got = 0;

    while (got < want && got < CMD_MAX_ARGS) {
        char *tok = NextToken(cursor);

        if (tok == NULL) {
            break;
        }
        args[got++] = tok;
    }
    return got;
}

static int32_t RunCommand(IpcContext *ctx, IpcRefHost *host, int32_t index,
                          char *line)
{
    char   *cursor = line;
    char   *cmd;
    char   *args[CMD_MAX_ARGS];
    char   *payload;
    int32_t got = 0;

    cmd = NextToken(&cursor);
    if (cmd == NULL || cmd[0] == '#') {
        return 1; /* 空行与注释行 */
    }

    Journal("STEP %d %s", index, cmd);

    if (strcmp(cmd, "handler") == 0) {
        /* handler <event> <mode>; mode: note | echo */
        int32_t  mode;
        uint32_t event;

        got = TakeArgs(&cursor, args, 2);
        if (got < 2) {
            Journal("ERR handler 参数不足");
            return 1;
        }
        event = (uint32_t)strtoul(args[0], NULL, 0);
        mode  = (strcmp(args[1], "echo") == 0) ? 1 : 0;
        if (g_handlerCount >= MAX_HANDLERS) {
            Journal("ERR handler 太多");
            return 1;
        }
        /*
         * 把 OnMessage 真的挂进宿主的注册表 —— 不挂的话报文会被参考宿主
         * 当成「没有 handler」丢掉，业务回调永远不跑，而 journal 里只会
         * 少一行，看起来像「对端没发」。
         *
         * 同一个 event 重复登记会被参考宿主拒绝（BUSY），所以这里先查
         * 自己那张表，允许脚本重复写 `handler 7 echo` 而不报错。
         */
        if (FindHandler(event) == NULL &&
            IpcRefHostAddHandler(host, event, OnMessage, NULL) != IPC_OK) {
            Journal("ERR 注册 handler 失败 event=%u", event);
            return 1;
        }
        g_handlers[g_handlerCount].event = event;
        g_handlers[g_handlerCount].mode  = mode;
        g_handlerCount++;
        return 1;
    }

    if (strcmp(cmd, "post") == 0) {
        int32_t rc;

        got = TakeArgs(&cursor, args, 2);
        if (got < 2) {
            Journal("ERR post 参数不足");
            return 1;
        }
        payload = RestPayload(cursor);
        rc = IpcPost(ctx, args[0], (uint32_t)strtoul(args[1], NULL, 0), payload,
                     strlen(payload));
        Journal("SEND %s %s", args[0], ResultName(rc));
        return 1;
    }

    if (strcmp(cmd, "send") == 0 || strcmp(cmd, "sendto") == 0) {
        int32_t isSendto = (strcmp(cmd, "sendto") == 0);
        int32_t timeoutMs = -1;
        char    reply[256];
        size_t  outLen = 0;
        int32_t rc;

        got = TakeArgs(&cursor, args, isSendto ? 3 : 2);
        if (got < (isSendto ? 3 : 2)) {
            Journal("ERR %s 参数不足", cmd);
            return 1;
        }
        if (isSendto) {
            timeoutMs = (int32_t)strtol(args[2], NULL, 0);
        }
        payload = RestPayload(cursor);
        memset(reply, 0, sizeof(reply));
        rc = IpcSendTimeout(ctx, args[0], (uint32_t)strtoul(args[1], NULL, 0),
                            payload, strlen(payload), reply, sizeof(reply) - 1,
                            &outLen, timeoutMs);
        Journal("SEND %s %s", args[0], ResultName(rc));
        if (rc == IPC_OK) {
            /* 把回复原文也打出来 —— 请求方要能断言「回的是我这一条」，
             * 而不是「有个回复来了」。 */
            Journal("GOT %zu %s", outLen, reply);
        }
        return 1;
    }

    if (strcmp(cmd, "broadcast") == 0) {
        int32_t sent;

        got = TakeArgs(&cursor, args, 1);
        if (got < 1) {
            Journal("ERR broadcast 参数不足");
            return 1;
        }
        payload = RestPayload(cursor);
        sent = IpcBroadcast(ctx, (uint32_t)strtoul(args[0], NULL, 0), payload,
                            strlen(payload));
        Journal("BCAST %d", sent);
        return 1;
    }

    if (strcmp(cmd, "sleep") == 0) {
        got = TakeArgs(&cursor, args, 1);
        if (got < 1) {
            Journal("ERR sleep 参数不足");
            return 1;
        }
        SleepMs((int32_t)strtol(args[0], NULL, 0));
        Journal("SLEPT %ld", strtol(args[0], NULL, 0));
        return 1;
    }

    if (strcmp(cmd, "stats") == 0) {
        DumpStats(ctx);
        return 1;
    }

    if (strcmp(cmd, "stop") == 0) {
        return 0;
    }

    Journal("ERR 未知命令 %s", cmd);
    return 1;
}

/* ------------------------------------------------------------------ */

static void Usage(void)
{
    (void)fprintf(stderr,
                  "用法: ipc_testmod --conf <path> --module <id> [选项]\n"
                  "  --ns <ns>           默认 testns\n"
                  "  --max-payload <n>   0 表示用库的缺省值\n"
                  "  --pending <n>       0 表示用库的缺省值\n"
                  "  --journal <path>    追加写 journal\n"
                  "  --group <name>      端点属组；给了就变 0660（供跨 uid 用例用）\n"
                  "  --script <path>     脚本文件；不给则读 stdin\n"
                  "  --serve-ms <n>      脚本之后继续服务 N 毫秒\n");
}

int main(int argc, char **argv)
{
    const char *conf   = NULL;
    const char *module = NULL;
    const char *ns     = "testns";
    const char *script = NULL;
    const char *journalPath = NULL;
    const char *group      = NULL;
    uint32_t    maxPayload = 0;
    int32_t     pending    = 0;
    int32_t     serveMs    = 200;
    int32_t     i;
    int32_t     exitCode   = 0;

    IpcRefHostOptions hostOptions = IPC_REFHOST_OPTIONS_INIT;
    IpcRefHost       *host        = NULL;
    IpcContext       *ctx         = NULL;
    IpcModuleOptions  options     = IPC_MODULE_OPTIONS_INIT;
    pthread_t         hostThread;
    HostArgs          hostArgs;
    FILE             *scriptFile  = stdin;
    char              line[1024];
    int32_t           index       = 0;
    int32_t           keepGoing   = 1;
    int32_t           rc;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--conf") == 0 && i + 1 < argc) {
            conf = argv[++i];
        } else if (strcmp(argv[i], "--module") == 0 && i + 1 < argc) {
            module = argv[++i];
        } else if (strcmp(argv[i], "--ns") == 0 && i + 1 < argc) {
            ns = argv[++i];
        } else if (strcmp(argv[i], "--max-payload") == 0 && i + 1 < argc) {
            maxPayload = (uint32_t)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--pending") == 0 && i + 1 < argc) {
            pending = (int32_t)strtol(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--journal") == 0 && i + 1 < argc) {
            journalPath = argv[++i];
        } else if (strcmp(argv[i], "--group") == 0 && i + 1 < argc) {
            group = argv[++i];
        } else if (strcmp(argv[i], "--script") == 0 && i + 1 < argc) {
            script = argv[++i];
        } else if (strcmp(argv[i], "--serve-ms") == 0 && i + 1 < argc) {
            serveMs = (int32_t)strtol(argv[++i], NULL, 0);
        } else {
            Usage();
            return 2;
        }
    }
    if (conf == NULL || module == NULL) {
        Usage();
        return 2;
    }
    if (strlen(module) >= sizeof(g_module)) {
        (void)fprintf(stderr, "!! module 名太长\n");
        return 2;
    }
    (void)snprintf(g_module, sizeof(g_module), "%s", module);

    if (journalPath != NULL) {
        g_journal = fopen(journalPath, "a");
        if (g_journal == NULL) {
            (void)fprintf(stderr, "!! 打不开 journal %s: %s\n", journalPath,
                          strerror(errno));
            return 2;
        }
    }
    if (script != NULL) {
        scriptFile = fopen(script, "r");
        if (scriptFile == NULL) {
            (void)fprintf(stderr, "!! 打不开脚本 %s: %s\n", script,
                          strerror(errno));
            return 2;
        }
    }

    /*
     * SIGPIPE：报文通道断了的时候不要直接把进程打死 —— 集成测试要的是
     * 可判定的退出码，不是「被信号杀掉」。库自己用的是 sendto 返回码，
     * 所以屏蔽它是安全的。
     */
    (void)signal(SIGPIPE, SIG_IGN);

    /* 1) 宿主对象 */
    rc = IpcRefHostCreate(&hostOptions, &host);
    if (rc != IPC_OK) {
        Journal("DONE %d", 2);
        return 2;
    }

    /* 2) 注册。调度入口与 user 都指向参考宿主。 */
    options.moduleId     = module;
    options.ns           = ns;
    options.confPath     = conf;
    /*
     * 属组。不给就是 NULL = 不指定属组，库会把端点设成 0600（只属主可写）。
     *
     * 之所以要暴露这个选项：跨 uid 用例（t07）必须靠**属组**让两个 uid
     * 互相写得进对方的端点。用「把目录放开到 0777」是没用的 —— 端点文件
     * 的模式由库自己设，与目录模式无关；实测踩过：目录 0777 下 nobody 向
     * root 的端点发送仍然是 EACCES(perm)。
     */
    options.groupName    = group;
    options.maxPayload   = maxPayload;
    options.maxPending   = pending;
    options.dispatch     = IpcRefHostDispatch;
    options.dispatchUser = host;
    rc = IpcRegister(&options, &ctx);
    if (rc != IPC_OK) {
        /*
         * 注册失败是集成测试里一个**正常的可断言结果**（配置非法 / uid 不匹配 /
         * 已被占用……），所以这里不是崩溃，而是一条 journal 行 + 明确的退出码。
         */
        Journal("REGFAIL %s", ResultName(rc));
        Journal("DONE 3");
        IpcRefHostDestroy(host);
        return 3;
    }
    /*
     * 报「已就绪」并把**端点路径**写出来。刻意不写 fd 号：fd 是本进程内
     * 部的数字，集成测试拿它做任何断言都会变成对实现细节的依赖；路径才是
     * 黑盒能观察、也真正有意义的东西。
     */
    Journal("READY %s %s", module, IpcGetSocketPath(ctx));

    if (IpcRefHostBind(host, ctx) != IPC_OK) {
        Journal("DONE 2");
        (void)IpcUnregister(ctx);
        (void)IpcDestroy(ctx);
        IpcRefHostDestroy(host);
        return 2;
    }

    /* 3) 宿主线程（独立 select 线程 + 回调线程池）。 */
    hostArgs.host = host;
    hostArgs.rc   = 0;
    if (pthread_create(&hostThread, NULL, HostMain, &hostArgs) != 0) {
        Journal("DONE 2");
        (void)IpcUnregister(ctx);
        (void)IpcDestroy(ctx);
        IpcRefHostDestroy(host);
        return 2;
    }

    /* 4) 主线程跑脚本。 */
    while (keepGoing != 0 && fgets(line, (int)sizeof(line), scriptFile) != NULL) {
        index++;
        keepGoing = RunCommand(ctx, host, index, line);
    }
    if (scriptFile != stdin) {
        (void)fclose(scriptFile);
    }

    /* 5) 脚本跑完再服务一会儿：对端可能还在发同步请求，需要有人应答。 */
    if (keepGoing != 0 && serveMs > 0) {
        SleepMs(serveMs);
    }

    DumpStats(ctx);

    /* 6) 收尾。顺序：先让宿主停（唤醒等待者、收线程），再注销。 */
    (void)IpcRefHostStop(host);
    (void)pthread_join(hostThread, NULL);
    if (hostArgs.rc != IPC_OK) {
        Journal("HOSTRC %s", ResultName(hostArgs.rc));
        exitCode = 4;
    }
    (void)IpcUnregister(ctx);
    (void)IpcDestroy(ctx);
    IpcRefHostDestroy(host);

    Journal("DONE %d", exitCode);
    if (g_journal != NULL) {
        (void)fclose(g_journal);
    }
    return exitCode;
}
