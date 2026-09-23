/*
 * Copyright (c) 2026 <版权方待定>
 * 许可协议：<待定>
 *
 * test_lifecycle.c -- 注册 / 注销 / 销毁 / 诊断访问器的白盒单测。**仅供测试**。
 *
 * 这一组会**真的建 AF_UNIX socket**（在 mkdtemp 出来的目录里），所以它覆盖的是
 * ipc_ctx.c 与 ipc_io.c 里那些「只有摸到内核才会走」的分支。不需要 root：
 * 同一个 uid 下的模块互相投递本来就不需要特权。
 * 跨 uid 的场景（chown 到别的属组、别的用户的授权 uid）需要 root，
 * 那部分在集成测试里，非 root 时整条套件报 BLOCKED 而不是假装通过。
 *
 * 注册流程的每一步失败都要有一个用例 —— 因为「注册成功了但状态不对」这种
 * 问题在线上表现为「对端收不到」，极难从现象倒推。
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "ipc/ipc.h"
#include "lab.h"
#include "utest.h"

/* ------------------------------------------------------------------ */
/* 实验台                                                             */
/* ------------------------------------------------------------------ */

#define LAB_NS "testns"

typedef struct {
    char dir[256];
    char conf[512];
    char path[512];
    char pathOther[512];
} TestLab;

static void LabLayout(TestLab *lab, const char *module, const char *other)
{
    IpcLabConfPath(lab->dir, lab->conf, sizeof(lab->conf));
    IpcLabSockPath(lab->dir, module, lab->path, sizeof(lab->path));
    IpcLabSockPath(lab->dir, other, lab->pathOther, sizeof(lab->pathOther));
}

/* 造一份「两个模块、uid 是本进程」的配置。 */
static int32_t LabSetup(TestLab *lab, const char *module, const char *other)
{
    static const char *modules[2];
    char               text[1024];
    size_t             written;

    memset(lab, 0, sizeof(*lab));
    if (IpcLabCreate(lab->dir, sizeof(lab->dir)) != 0) {
        return -1;
    }
    modules[0] = module;
    modules[1] = other;
    written    = IpcLabBuildConf(lab->dir, LAB_NS, modules, 2, text, sizeof(text));
    if (written == 0) {
        IpcLabRemove(lab->dir);
        return -1;
    }
    LabLayout(lab, module, other);
    if (IpcLabWriteFile(lab->conf, text) != 0) {
        IpcLabRemove(lab->dir);
        return -1;
    }
    IpcLabSilenceLog();
    return 0;
}

static void LabTeardown(TestLab *lab)
{
    IpcLabCaptureEnd();
    IpcLabRemove(lab->dir);
}

/* 直接往配置表里写一份自定义文本（用于构造 uid 不匹配之类的场景）。 */
static int32_t LabWriteRawConf(TestLab *lab, const char *text)
{
    return IpcLabWriteFile(lab->conf, text);
}

/* 一个真实但不属于任何注册实例的 socket 文件 —— 模拟「上一实例崩溃的残留」。 */
static int32_t MakeStaleSocket(const char *path)
{
    struct sockaddr_un addr;
    int32_t            fd;
    size_t             len = strlen(path);

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (len >= sizeof(addr.sun_path)) {
        return -1;
    }
    memcpy(addr.sun_path, path, len + 1);
    fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }
    if (bind(fd, (struct sockaddr *)&addr,
             (socklen_t)(offsetof(struct sockaddr_un, sun_path) + len + 1)) != 0) {
        (void)close(fd);
        return -1;
    }
    (void)close(fd); /* 关掉 fd，但文件留在磁盘上 */
    return 0;
}

static int32_t FileExists(const char *path)
{
    struct stat st;

    return (lstat(path, &st) == 0) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* 参数与配置错误                                                     */
/* ------------------------------------------------------------------ */

UTEST_CASE(lifecycle, register_argument_checks)
{
    IpcModuleOptions options = IPC_MODULE_OPTIONS_INIT;
    IpcContext      *ctx     = NULL;

    IpcLabSilenceLog();

    UTEST_ASSERT_EQ(IpcRegister(NULL, &ctx), IPC_ERR_INVAL);
    UTEST_ASSERT_EQ(IpcRegister(&options, NULL), IPC_ERR_INVAL);

    /* moduleId 必填。 */
    options.moduleId = "";
    UTEST_ASSERT_EQ(IpcRegister(&options, &ctx), IPC_ERR_INVAL);
    UTEST_ASSERT_NULL(ctx);

    /* 超长（32 字符，字段只有 31 字符 + NUL）。 */
    options.moduleId = "module-name-that-is-exactly32ch";
    UTEST_ASSERT_EQ(IpcRegister(&options, &ctx), IPC_ERR_INVAL);
    UTEST_ASSERT_NULL(ctx);
    IpcLabCaptureEnd();
}

UTEST_CASE(lifecycle, register_reports_missing_config_file)
{
    IpcModuleOptions options = IPC_MODULE_OPTIONS_INIT;
    IpcContext      *ctx     = NULL;

    IpcLabSilenceLog();
    options.moduleId = "alpha";
    options.confPath = "/tmp/ipc-lab-conf-that-does-not-exist/nothing.conf";
    UTEST_ASSERT_EQ(IpcRegister(&options, &ctx), IPC_ERR_NOENT);
    UTEST_ASSERT_NULL(ctx);
    IpcLabCaptureEnd();
}

UTEST_CASE(lifecycle, register_rejects_unknown_module)
{
    TestLab          lab;
    IpcModuleOptions options = IPC_MODULE_OPTIONS_INIT;
    IpcContext      *ctx     = NULL;

    UTEST_ASSERT_EQ(LabSetup(&lab, "alpha", "beta"), 0);

    options.moduleId = "gamma"; /* 配置里没有 */
    options.confPath = lab.conf;
    options.ns       = LAB_NS;
    UTEST_ASSERT_EQ(IpcRegister(&options, &ctx), IPC_ERR_NOENT);
    UTEST_ASSERT_NULL(ctx);

    /* 模块在表里，但命名空间写错也一样查不到。 */
    options.ns = "otherns";
    UTEST_ASSERT_EQ(IpcRegister(&options, &ctx), IPC_ERR_NOENT);
    UTEST_ASSERT_NULL(ctx);

    LabTeardown(&lab);
}

UTEST_CASE(lifecycle, register_rejects_ambiguous_namespace)
{
    TestLab          lab;
    IpcModuleOptions options = IPC_MODULE_OPTIONS_INIT;
    IpcContext      *ctx     = NULL;
    char             text[1024];
    char             pathA[512];
    char             pathB[512];

    UTEST_ASSERT_EQ(LabSetup(&lab, "alpha", "beta"), 0);
    IpcLabSockPath(lab.dir, "dupA", pathA, sizeof(pathA));
    IpcLabSockPath(lab.dir, "dupB", pathB, sizeof(pathB));
    (void)snprintf(text, sizeof(text), "nsA dup %lu %s\nnsB dup %lu %s\n",
                   (unsigned long)getuid(), pathA, (unsigned long)getuid(), pathB);
    UTEST_ASSERT_EQ(LabWriteRawConf(&lab, text), 0);

    options.moduleId = "dup";
    options.confPath = lab.conf;
    /* 不给 ns：同名模块出现在两个命名空间里 → 推导不出来，报 CONFIG。 */
    UTEST_ASSERT_EQ(IpcRegister(&options, &ctx), IPC_ERR_CONFIG);
    UTEST_ASSERT_NULL(ctx);

    /* 显式给 ns 就能定下来。 */
    options.ns = "nsA";
    UTEST_ASSERT_EQ(IpcRegister(&options, &ctx), IPC_OK);
    UTEST_ASSERT_NOTNULL(ctx);
    UTEST_ASSERT_STREQ(IpcGetNamespace(ctx), "nsA");
    UTEST_ASSERT_STREQ(IpcGetSocketPath(ctx), pathA);
    UTEST_ASSERT_EQ(IpcUnregister(ctx), IPC_OK);
    UTEST_ASSERT_EQ(IpcDestroy(ctx), IPC_OK);

    LabTeardown(&lab);
}

UTEST_CASE(lifecycle, register_rejects_uid_mismatch)
{
    TestLab          lab;
    IpcModuleOptions options = IPC_MODULE_OPTIONS_INIT;
    IpcContext      *ctx     = NULL;
    char             text[512];
    char             path[512];
    uid_t            someoneElse = (uid_t)(getuid() + 1000);

    UTEST_ASSERT_EQ(LabSetup(&lab, "alpha", "beta"), 0);
    IpcLabSockPath(lab.dir, "stranger", path, sizeof(path));
    (void)snprintf(text, sizeof(text), "%s stranger %lu %s\n", LAB_NS,
                   (unsigned long)someoneElse, path);
    UTEST_ASSERT_EQ(LabWriteRawConf(&lab, text), 0);

    /*
     * 配置说这个模块属于别的 uid，而我们是本 uid。这是「以错误的身份启动」，
     * 必须在注册时就拦下 —— 否则要等到第一条报文被对端凭据校验拒掉才发现。
     * 注意这条检查**不是安全边界**（真正的边界是文件权限与接收侧校验），
     * 它只是让错误提早暴露。
     */
    options.moduleId = "stranger";
    options.confPath = lab.conf;
    options.ns       = LAB_NS;
    UTEST_ASSERT_EQ(IpcRegister(&options, &ctx), IPC_ERR_PERM);
    UTEST_ASSERT_NULL(ctx);

    /* allowUidSplit = 1 时只比 effective uid，仍然对不上。 */
    options.allowUidSplit = 1;
    UTEST_ASSERT_EQ(IpcRegister(&options, &ctx), IPC_ERR_PERM);
    UTEST_ASSERT_NULL(ctx);

    LabTeardown(&lab);
}

UTEST_CASE(lifecycle, register_rejects_unknown_group)
{
    TestLab          lab;
    IpcModuleOptions options = IPC_MODULE_OPTIONS_INIT;
    IpcContext      *ctx     = NULL;

    UTEST_ASSERT_EQ(LabSetup(&lab, "alpha", "beta"), 0);

    options.moduleId  = "alpha";
    options.confPath  = lab.conf;
    options.ns        = LAB_NS;
    options.groupName = "no-such-group-ipc-lab-xyz";
    UTEST_ASSERT_EQ(IpcRegister(&options, &ctx), IPC_ERR_NOENT);
    UTEST_ASSERT_NULL(ctx);

    LabTeardown(&lab);
}

/* ------------------------------------------------------------------ */
/* 成功路径                                                           */
/* ------------------------------------------------------------------ */

UTEST_CASE(lifecycle, register_creates_endpoint_with_expected_ownership)
{
    TestLab          lab;
    IpcModuleOptions options = IPC_MODULE_OPTIONS_INIT;
    IpcContext      *ctx     = NULL;
    struct stat      st;

    UTEST_ASSERT_EQ(LabSetup(&lab, "alpha", "beta"), 0);

    options.moduleId = "alpha";
    options.confPath = lab.conf;
    options.ns       = LAB_NS;
    UTEST_ASSERT_EQ(IpcRegister(&options, &ctx), IPC_OK);
    UTEST_ASSERT_NOTNULL(ctx);

    /* 端点文件确实建出来了，而且是 socket。 */
    UTEST_ASSERT_EQ(FileExists(lab.path), 1);
    UTEST_ASSERT_EQ(lstat(lab.path, &st), 0);
    UTEST_ASSERT_TRUE(S_ISSOCK(st.st_mode));
    /* 没指定属组时模式是 0600（同 uid 内互通，别人进不来）。 */
    UTEST_ASSERT_EQ(st.st_mode & 07777, 0600);
    UTEST_ASSERT_EQ_U64(st.st_uid, getuid());

    /* 锁文件在旁边，而且**是持有着的**（同一路径再来一个实例会 BUSY）。 */
    {
        char lockPath[600];

        (void)snprintf(lockPath, sizeof(lockPath), "%s.lock", lab.path);
        UTEST_ASSERT_EQ(FileExists(lockPath), 1);
    }

    /* 诊断访问器。 */
    UTEST_ASSERT_STREQ(IpcGetModuleId(ctx), "alpha");
    UTEST_ASSERT_STREQ(IpcGetNamespace(ctx), LAB_NS);
    UTEST_ASSERT_STREQ(IpcGetSocketPath(ctx), lab.path);
    UTEST_ASSERT(IpcGetInstanceId(ctx) != 0);
    UTEST_ASSERT(IpcGetSelectFd(ctx) >= 0);
    UTEST_ASSERT_EQ(IpcIsStopped(ctx), 0);

    /* NULL 参数上的诊断访问器要安全。 */
    UTEST_ASSERT_NULL(IpcGetModuleId(NULL));
    UTEST_ASSERT_NULL(IpcGetNamespace(NULL));
    UTEST_ASSERT_NULL(IpcGetSocketPath(NULL));
    UTEST_ASSERT_EQ_U64(IpcGetInstanceId(NULL), 0);
    UTEST_ASSERT(IpcGetSelectFd(NULL) < 0);
    UTEST_ASSERT_EQ(IpcIsStopped(NULL), 1);

    UTEST_ASSERT_EQ(IpcUnregister(ctx), IPC_OK);
    /* 注销把端点与锁都放掉了：路径上不该再有东西。 */
    UTEST_ASSERT_EQ(FileExists(lab.path), 0);
    UTEST_ASSERT_EQ(IpcDestroy(ctx), IPC_OK);

    LabTeardown(&lab);
}

UTEST_CASE(lifecycle, two_modules_in_one_process_are_independent)
{
    TestLab           lab;
    IpcModuleOptions  optionsA = IPC_MODULE_OPTIONS_INIT;
    IpcModuleOptions  optionsB = IPC_MODULE_OPTIONS_INIT;
    IpcContext       *ctxA = NULL;
    IpcContext       *ctxB = NULL;

    UTEST_ASSERT_EQ(LabSetup(&lab, "alpha", "beta"), 0);

    optionsA.moduleId = "alpha";
    optionsA.confPath = lab.conf;
    optionsA.ns       = LAB_NS;
    optionsB.moduleId = "beta";
    optionsB.confPath = lab.conf;
    optionsB.ns       = LAB_NS;

    UTEST_ASSERT_EQ(IpcRegister(&optionsA, &ctxA), IPC_OK);
    UTEST_ASSERT_EQ(IpcRegister(&optionsB, &ctxB), IPC_OK);
    UTEST_ASSERT_NOTNULL(ctxA);
    UTEST_ASSERT_NOTNULL(ctxB);
    UTEST_ASSERT(ctxA != ctxB);

    /* 两个上下文各自有独立的 fd、独立的代际号、独立的统计。 */
    UTEST_ASSERT(IpcGetSelectFd(ctxA) >= 0);
    UTEST_ASSERT(IpcGetSelectFd(ctxB) >= 0);
    UTEST_ASSERT(IpcGetSelectFd(ctxA) != IpcGetSelectFd(ctxB));

    UTEST_ASSERT_EQ(FileExists(lab.path), 1);
    UTEST_ASSERT_EQ(FileExists(lab.pathOther), 1);

    /* 注销其中一个不影响另一个 —— 尤其不能顺手把对方的端点删掉。 */
    UTEST_ASSERT_EQ(IpcUnregister(ctxA), IPC_OK);
    UTEST_ASSERT_EQ(FileExists(lab.path), 0);
    UTEST_ASSERT_EQ(FileExists(lab.pathOther), 1);
    UTEST_ASSERT_EQ(IpcDestroy(ctxA), IPC_OK);

    UTEST_ASSERT_EQ(IpcUnregister(ctxB), IPC_OK);
    UTEST_ASSERT_EQ(IpcDestroy(ctxB), IPC_OK);
    UTEST_ASSERT_EQ(FileExists(lab.pathOther), 0);

    LabTeardown(&lab);
}

UTEST_CASE(lifecycle, max_payload_is_clamped_to_the_hard_ceiling)
{
    TestLab          lab;
    IpcModuleOptions options = IPC_MODULE_OPTIONS_INIT;
    IpcContext      *ctx     = NULL;
    uint8_t         *big;

    UTEST_ASSERT_EQ(LabSetup(&lab, "alpha", "beta"), 0);

    /*
     * 填一个远大于硬上限的值：实际生效的应当是 clamp 到 IPC_PAYLOAD_HARD_MAX。
     * 这里没有 maxPayload 的访问器，所以从**行为**上验证：
     * 载荷长度恰为 HARD_MAX 时不该被 MSGSIZE 拒掉。
     * （IpcPost 的检查顺序是 MSGSIZE 先于查表，所以对不存在的模块
     *   也能拿到「没被 MSGSIZE 拒」这个信息：会走到 NOENT。）
     */
    options.moduleId   = "alpha";
    options.confPath   = lab.conf;
    options.ns         = LAB_NS;
    options.maxPayload = 1u << 20; /* 1 MiB，远超 64 KiB */
    UTEST_ASSERT_EQ(IpcRegister(&options, &ctx), IPC_OK);

    big = (uint8_t *)calloc(1, IPC_PAYLOAD_HARD_MAX);
    UTEST_ASSERT_NOTNULL(big);

    UTEST_ASSERT_EQ(IpcPost(ctx, "beta", 1, big, IPC_PAYLOAD_HARD_MAX), IPC_ERR_OFFLINE);
    /* 超一个字节就必须被拒，而且是在发送动作之前拒。 */
    UTEST_ASSERT_EQ(IpcPost(ctx, "gamma", 1, big, IPC_PAYLOAD_HARD_MAX + 1u),
                    IPC_ERR_MSGSIZE);

    free(big);
    UTEST_ASSERT_EQ(IpcUnregister(ctx), IPC_OK);
    UTEST_ASSERT_EQ(IpcDestroy(ctx), IPC_OK);
    LabTeardown(&lab);
}

UTEST_CASE(lifecycle, max_payload_default_when_zero)
{
    TestLab          lab;
    IpcModuleOptions options = IPC_MODULE_OPTIONS_INIT;
    IpcContext      *ctx     = NULL;
    uint8_t         *buf;
    size_t           defaultSize = (size_t)IPC_PAYLOAD_DEFAULT;

    UTEST_ASSERT_EQ(LabSetup(&lab, "alpha", "beta"), 0);

    options.moduleId = "alpha"; /* maxPayload 留 0 → 取缺省值 */
    options.confPath = lab.conf;
    options.ns       = LAB_NS;
    UTEST_ASSERT_EQ(IpcRegister(&options, &ctx), IPC_OK);

    buf = (uint8_t *)calloc(1, defaultSize);
    UTEST_ASSERT_NOTNULL(buf);

    UTEST_ASSERT_EQ(IpcPost(ctx, "beta", 1, buf, defaultSize), IPC_ERR_OFFLINE);
    UTEST_ASSERT_EQ(IpcPost(ctx, "beta", 1, buf, defaultSize + 1u), IPC_ERR_MSGSIZE);

    free(buf);
    UTEST_ASSERT_EQ(IpcUnregister(ctx), IPC_OK);
    UTEST_ASSERT_EQ(IpcDestroy(ctx), IPC_OK);
    LabTeardown(&lab);
}

/* ------------------------------------------------------------------ */
/* 独占与残留                                                         */
/* ------------------------------------------------------------------ */

UTEST_CASE(lifecycle, second_instance_on_the_same_path_is_busy)
{
    TestLab          lab;
    IpcModuleOptions options = IPC_MODULE_OPTIONS_INIT;
    IpcContext      *first   = NULL;
    IpcContext      *second  = NULL;

    UTEST_ASSERT_EQ(LabSetup(&lab, "alpha", "beta"), 0);

    options.moduleId = "alpha";
    options.confPath = lab.conf;
    options.ns       = LAB_NS;
    UTEST_ASSERT_EQ(IpcRegister(&options, &first), IPC_OK);

    /*
     * 同一个模块、同一个路径再来一次。flock 是进程级的 open file description
     * 锁，所以**同一个进程**的第二次注册也会撞上 —— 这正是我们要的：
     * 「同一路径两个实例」不管是不是同进程都挡得住。
     */
    UTEST_ASSERT_EQ(IpcRegister(&options, &second), IPC_ERR_BUSY);
    UTEST_ASSERT_NULL(second);

    /* 失败者绝不能把赢家的端点删掉或改坏。 */
    UTEST_ASSERT_EQ(FileExists(lab.path), 1);
    UTEST_ASSERT_STREQ(IpcGetModuleId(first), "alpha");

    UTEST_ASSERT_EQ(IpcUnregister(first), IPC_OK);
    UTEST_ASSERT_EQ(IpcDestroy(first), IPC_OK);

    /* 赢家退出之后，同一个路径必须能重新注册（锁是活的，不是永久占着）。 */
    second = NULL;
    UTEST_ASSERT_EQ(IpcRegister(&options, &second), IPC_OK);
    UTEST_ASSERT_NOTNULL(second);
    UTEST_ASSERT_EQ(IpcUnregister(second), IPC_OK);
    UTEST_ASSERT_EQ(IpcDestroy(second), IPC_OK);

    LabTeardown(&lab);
}

UTEST_CASE(lifecycle, register_clears_a_stale_socket_left_behind)
{
    TestLab          lab;
    IpcModuleOptions options = IPC_MODULE_OPTIONS_INIT;
    IpcContext      *ctx     = NULL;

    UTEST_ASSERT_EQ(LabSetup(&lab, "alpha", "beta"), 0);

    /* 模拟上一实例崩溃后留下的残留 socket 文件：路径在、没人 bind。 */
    UTEST_ASSERT_EQ(MakeStaleSocket(lab.path), 0);
    UTEST_ASSERT_EQ(FileExists(lab.path), 1);

    options.moduleId = "alpha";
    options.confPath = lab.conf;
    options.ns       = LAB_NS;
    UTEST_ASSERT_EQ(IpcRegister(&options, &ctx), IPC_OK);
    UTEST_ASSERT_NOTNULL(ctx);

    {
        struct stat st;

        UTEST_ASSERT_EQ(lstat(lab.path, &st), 0);
        UTEST_ASSERT_TRUE(S_ISSOCK(st.st_mode));
        UTEST_ASSERT_EQ(st.st_mode & 07777, 0600);
    }
    UTEST_ASSERT_EQ(IpcUnregister(ctx), IPC_OK);
    UTEST_ASSERT_EQ(IpcDestroy(ctx), IPC_OK);
    LabTeardown(&lab);
}

UTEST_CASE(lifecycle, register_refuses_to_delete_a_non_socket)
{
    TestLab          lab;
    IpcModuleOptions options = IPC_MODULE_OPTIONS_INIT;
    IpcContext      *ctx     = NULL;

    UTEST_ASSERT_EQ(LabSetup(&lab, "alpha", "beta"), 0);

    /*
     * 路径上是个**普通文件**。持锁清理残留时只删「确实是 socket」的东西：
     * 普通文件说明配置或部署有问题，删掉就是毁别人的数据。
     */
    UTEST_ASSERT_EQ(IpcLabWriteFile(lab.path, "important data\n"), 0);

    options.moduleId = "alpha";
    options.confPath = lab.conf;
    options.ns       = LAB_NS;
    UTEST_ASSERT_EQ(IpcRegister(&options, &ctx), IPC_ERR_PERM);
    UTEST_ASSERT_NULL(ctx);

    /* 文件必须原封不动。 */
    UTEST_ASSERT_EQ(FileExists(lab.path), 1);
    {
        struct stat st;

        UTEST_ASSERT_EQ(lstat(lab.path, &st), 0);
        UTEST_ASSERT_TRUE(S_ISREG(st.st_mode));
    }

    LabTeardown(&lab);
}

/* ------------------------------------------------------------------ */
/* 停止、注销、销毁                                                   */
/* ------------------------------------------------------------------ */

UTEST_CASE(lifecycle, unregister_is_idempotent)
{
    TestLab          lab;
    IpcModuleOptions options = IPC_MODULE_OPTIONS_INIT;
    IpcContext      *ctx     = NULL;

    UTEST_ASSERT_EQ(LabSetup(&lab, "alpha", "beta"), 0);

    options.moduleId = "alpha";
    options.confPath = lab.conf;
    options.ns       = LAB_NS;
    UTEST_ASSERT_EQ(IpcRegister(&options, &ctx), IPC_OK);

    /*
     * 连调三次必须都返回 IPC_OK 且不崩。
     * 【回归用例】早期版本在注销里 free(ctx)，于是第二次调用就是
     * use-after-free（ASan 复现过）。守卫必须建立在对象还活着的时候。
     * 本用例在 ASan 树下跑一遍才有完整意义。
     */
    UTEST_ASSERT_EQ(IpcUnregister(ctx), IPC_OK);
    UTEST_ASSERT_EQ(IpcUnregister(ctx), IPC_OK);
    UTEST_ASSERT_EQ(IpcUnregister(ctx), IPC_OK);

    /* 注销之后：端点没了、停止态、fd 无效。 */
    UTEST_ASSERT_EQ(FileExists(lab.path), 0);
    UTEST_ASSERT_EQ(IpcIsStopped(ctx), 1);
    UTEST_ASSERT(IpcGetSelectFd(ctx) < 0);

    /* 再操作就是 STOPPED / STATE，不是崩。 */
    UTEST_ASSERT_EQ(IpcPost(ctx, "beta", 1, "x", 1), IPC_ERR_STOPPED);
    UTEST_ASSERT_EQ(IpcHandleReadable(ctx, 0), IPC_ERR_STOPPED);

    /* 销毁：注销之前调用要被拒；之后调用成功。 */
    UTEST_ASSERT_EQ(IpcDestroy(ctx), IPC_OK);
    LabTeardown(&lab);
}

UTEST_CASE(lifecycle, destroy_requires_unregister_first)
{
    TestLab          lab;
    IpcModuleOptions options = IPC_MODULE_OPTIONS_INIT;
    IpcContext      *ctx     = NULL;

    UTEST_ASSERT_EQ(LabSetup(&lab, "alpha", "beta"), 0);

    options.moduleId = "alpha";
    options.confPath = lab.conf;
    options.ns       = LAB_NS;
    UTEST_ASSERT_EQ(IpcRegister(&options, &ctx), IPC_OK);

    /* 契约：没注销就销毁 → STATE，并且**什么都不释放**。 */
    UTEST_ASSERT_EQ(IpcDestroy(ctx), IPC_ERR_STATE);
    UTEST_ASSERT_EQ(FileExists(lab.path), 1); /* 端点还在，说明真没释放 */
    UTEST_ASSERT_EQ(IpcIsStopped(ctx), 0);
    UTEST_ASSERT_STREQ(IpcGetModuleId(ctx), "alpha");

    UTEST_ASSERT_EQ(IpcUnregister(ctx), IPC_OK);
    UTEST_ASSERT_EQ(IpcDestroy(ctx), IPC_OK);

    UTEST_ASSERT_EQ(IpcDestroy(NULL), IPC_ERR_INVAL);
    UTEST_ASSERT_EQ(IpcUnregister(NULL), IPC_ERR_INVAL);
    UTEST_ASSERT_EQ(IpcRequestStop(NULL), IPC_ERR_INVAL);
    UTEST_ASSERT_EQ(IpcGetStatistics(NULL, NULL), IPC_ERR_INVAL);

    LabTeardown(&lab);
}

UTEST_CASE(lifecycle, request_stop_is_idempotent_and_blocks_sends)
{
    TestLab          lab;
    IpcModuleOptions options = IPC_MODULE_OPTIONS_INIT;
    IpcContext      *ctx     = NULL;

    UTEST_ASSERT_EQ(LabSetup(&lab, "alpha", "beta"), 0);

    options.moduleId = "alpha";
    options.confPath = lab.conf;
    options.ns       = LAB_NS;
    UTEST_ASSERT_EQ(IpcRegister(&options, &ctx), IPC_OK);
    UTEST_ASSERT_EQ(IpcIsStopped(ctx), 0);

    UTEST_ASSERT_EQ(IpcRequestStop(ctx), IPC_OK);
    UTEST_ASSERT_EQ(IpcIsStopped(ctx), 1);
    UTEST_ASSERT_EQ(IpcRequestStop(ctx), IPC_OK); /* 幂等 */

    /* 停止之后所有发送路径都拒绝，且**都不碰内核**。 */
    UTEST_ASSERT_EQ(IpcPost(ctx, "beta", 1, "x", 1), IPC_ERR_STOPPED);
    UTEST_ASSERT_EQ(IpcBroadcast(ctx, 1, "x", 1), IPC_ERR_STOPPED);
    UTEST_ASSERT_EQ(IpcSend(ctx, "beta", 1, "x", 1, NULL, 0, NULL), IPC_ERR_STOPPED);
    UTEST_ASSERT_EQ(IpcHandleReadable(ctx, 0), IPC_ERR_STOPPED);

    /* 停止不等于注销：端点文件还在（宿主可能还要把已排队的报文读完）。 */
    UTEST_ASSERT_EQ(FileExists(lab.path), 1);

    UTEST_ASSERT_EQ(IpcUnregister(ctx), IPC_OK);
    UTEST_ASSERT_EQ(IpcDestroy(ctx), IPC_OK);
    LabTeardown(&lab);
}

/* ------------------------------------------------------------------ */
/* 统计                                                               */
/* ------------------------------------------------------------------ */

UTEST_CASE(lifecycle, statistics_start_at_zero_and_count_attempts)
{
    TestLab          lab;
    IpcModuleOptions options = IPC_MODULE_OPTIONS_INIT;
    IpcContext      *ctx     = NULL;
    IpcStatistics    stats;

    UTEST_ASSERT_EQ(LabSetup(&lab, "alpha", "beta"), 0);

    options.moduleId = "alpha";
    options.confPath = lab.conf;
    options.ns       = LAB_NS;
    UTEST_ASSERT_EQ(IpcRegister(&options, &ctx), IPC_OK);

    memset(&stats, 0xFF, sizeof(stats));
    UTEST_ASSERT_EQ(IpcGetStatistics(ctx, &stats), IPC_OK);
    UTEST_ASSERT_EQ_U64(stats.sendAttempts, 0);
    UTEST_ASSERT_EQ_U64(stats.recvRead, 0);

    /* 目标模块在配置里但没起：算一次尝试、一次失败，不计入「入队」。 */
    UTEST_ASSERT_EQ(IpcPost(ctx, "beta", 1, "x", 1), IPC_ERR_OFFLINE);
    UTEST_ASSERT_EQ(IpcGetStatistics(ctx, &stats), IPC_OK);
    UTEST_ASSERT_EQ_U64(stats.sendAttempts, 1);
    UTEST_ASSERT_EQ_U64(stats.sendEnqueued, 0);
    UTEST_ASSERT_EQ_U64(stats.sendFailed, 1);

    /* 目标不在配置里：同样计一次尝试与失败（口径一致）。 */
    UTEST_ASSERT_EQ(IpcPost(ctx, "gamma", 1, "x", 1), IPC_ERR_NOENT);
    UTEST_ASSERT_EQ(IpcGetStatistics(ctx, &stats), IPC_OK);
    UTEST_ASSERT_EQ_U64(stats.sendAttempts, 2);
    UTEST_ASSERT_EQ_U64(stats.sendFailed, 2);

    /* MSGSIZE 在发送之前就拒了，但仍计入 sendFailed（调用者视角是失败的发送）。 */
    {
        uint8_t one = 0;

        UTEST_ASSERT_EQ(IpcPost(ctx, "beta", 1, &one, (size_t)IPC_PAYLOAD_DEFAULT + 1u),
                        IPC_ERR_MSGSIZE);
        UTEST_ASSERT_EQ(IpcGetStatistics(ctx, &stats), IPC_OK);
        UTEST_ASSERT_EQ_U64(stats.sendFailed, 3);
        UTEST_ASSERT_EQ_U64(stats.sendAttempts, 2); /* 没走到「尝试发送」那一步 */
    }

    /* outStatistics 为 NULL 或 ctx 为 NULL → INVAL。 */
    UTEST_ASSERT_EQ(IpcGetStatistics(ctx, NULL), IPC_ERR_INVAL);
    UTEST_ASSERT_EQ(IpcGetStatistics(NULL, &stats), IPC_ERR_INVAL);

    UTEST_ASSERT_EQ(IpcUnregister(ctx), IPC_OK);
    UTEST_ASSERT_EQ(IpcDestroy(ctx), IPC_OK);
    LabTeardown(&lab);
}

/* ------------------------------------------------------------------ */
/* Q12：配置里的模块不存在时要留下痕迹                                */
/* ------------------------------------------------------------------ */

UTEST_CASE(lifecycle, startup_health_check_warns_about_missing_peers)
{
    TestLab          lab;
    IpcModuleOptions options = IPC_MODULE_OPTIONS_INIT;
    IpcContext      *ctx     = NULL;

    UTEST_ASSERT_EQ(LabSetup(&lab, "alpha", "beta"), 0);
    IpcLabCaptureBegin(); /* 这一次要真的看日志，所以不静音 */

    options.moduleId = "alpha";
    options.confPath = lab.conf;
    options.ns       = LAB_NS;
    UTEST_ASSERT_EQ(IpcRegister(&options, &ctx), IPC_OK);

    /*
     * Q12 的答复要求「配置中的模块不存在时打印 warning 日志」。
     * 断言到**具体那一条**上：级别是 WARN，内容是聚合后的缺失模块名。
     * 聚合成一条是刻意的 —— 9 个模块只起了 1 个的时候不该刷 8 行。
     */
    UTEST_ASSERT(IpcLabCaptureContains("have no endpoint yet", "beta"));
    {
        int32_t i;
        int32_t missingWarnings = 0;

        for (i = 0; i < IpcLabCaptureCount(); i++) {
            if (IpcLabCaptureLevel(i) == IPC_LOG_WARN &&
                strstr(IpcLabCaptureText(i), "have no endpoint yet") != NULL) {
                missingWarnings++;
            }
        }
        UTEST_ASSERT_EQ(missingWarnings, 1); /* 只有一条，不是每个模块一条 */
    }
    /* moduleId 要带在日志上，宿主才分得清是哪个模块报的。 */
    UTEST_ASSERT_STREQ(IpcLabCaptureModule(0), "alpha");

    UTEST_ASSERT_EQ(IpcUnregister(ctx), IPC_OK);
    UTEST_ASSERT_EQ(IpcDestroy(ctx), IPC_OK);
    LabTeardown(&lab);
}

UTEST_CASE(lifecycle, startup_health_check_stays_quiet_when_peer_exists)
{
    TestLab          lab;
    IpcModuleOptions options = IPC_MODULE_OPTIONS_INIT;
    IpcContext      *ctx     = NULL;

    UTEST_ASSERT_EQ(LabSetup(&lab, "alpha", "beta"), 0);
    /* 让 beta 的端点先存在（随便建个 socket 文件即可，检查只看路径在不在）。 */
    UTEST_ASSERT_EQ(MakeStaleSocket(lab.pathOther), 0);

    IpcLabCaptureBegin();
    options.moduleId = "alpha";
    options.confPath = lab.conf;
    options.ns       = LAB_NS;
    UTEST_ASSERT_EQ(IpcRegister(&options, &ctx), IPC_OK);
    UTEST_ASSERT_FALSE(IpcLabCaptureContains("have no endpoint yet", NULL));

    UTEST_ASSERT_EQ(IpcUnregister(ctx), IPC_OK);
    UTEST_ASSERT_EQ(IpcDestroy(ctx), IPC_OK);
    LabTeardown(&lab);
}

UTEST_CASE(lifecycle, offline_send_logs_a_warning)
{
    TestLab          lab;
    IpcModuleOptions options = IPC_MODULE_OPTIONS_INIT;
    IpcContext      *ctx     = NULL;

    UTEST_ASSERT_EQ(LabSetup(&lab, "alpha", "beta"), 0);
    IpcLabCaptureBegin();

    options.moduleId = "alpha";
    options.confPath = lab.conf;
    options.ns       = LAB_NS;
    UTEST_ASSERT_EQ(IpcRegister(&options, &ctx), IPC_OK);

    /* Q12 的 (b)：发送时发现目标端点不存在 → 一条 WARN + IPC_ERR_OFFLINE。 */
    UTEST_ASSERT_EQ(IpcPost(ctx, "beta", 42, "x", 1), IPC_ERR_OFFLINE);
    UTEST_ASSERT(IpcLabCaptureContains("is offline", "beta"));

    UTEST_ASSERT_EQ(IpcUnregister(ctx), IPC_OK);
    UTEST_ASSERT_EQ(IpcDestroy(ctx), IPC_OK);
    LabTeardown(&lab);
}
