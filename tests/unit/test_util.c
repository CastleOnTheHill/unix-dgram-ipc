/*
 * Copyright (c) 2026 <版权方待定>
 * 许可协议：<待定>
 *
 * test_util.c -- ipc_util 的白盒单测。**仅供测试**。
 *
 * 这一组覆盖的是纯函数：不建 socket、不碰内核、不需要 root。
 * 它们同时也是覆盖率的主力 —— 每条分支都能在一台普通机器上跑出来。
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ipc_util.h"
#include "ipc/ipc.h"
#include "lab.h"
#include "utest.h"

/* ------------------------------------------------------------------ */
/* IpcStrlcpy                                                         */
/* ------------------------------------------------------------------ */

UTEST_CASE(util, strlcpy_copies_and_returns_source_length)
{
    char   buf[16];
    size_t n;

    memset(buf, 'x', sizeof(buf));
    n = IpcStrlcpy(buf, "hello", sizeof(buf));
    UTEST_ASSERT_EQ_U64(n, 5);
    UTEST_ASSERT_STREQ(buf, "hello");
}

UTEST_CASE(util, strlcpy_truncates_and_still_terminates)
{
    char   buf[4];
    size_t n;

    n = IpcStrlcpy(buf, "abcdef", sizeof(buf));
    /* 返回值是**源串完整长度**，调用者据此判断截断 —— 不是拷贝进去的长度。 */
    UTEST_ASSERT_EQ_U64(n, 6);
    UTEST_ASSERT_STREQ(buf, "abc");
    UTEST_ASSERT_EQ(buf[3], '\0');
}

UTEST_CASE(util, strlcpy_edge_capacities)
{
    char   one[1];
    char   two[2];
    size_t n;

    /* cap == 1：只能放 NUL。 */
    n = IpcStrlcpy(one, "abc", 1);
    UTEST_ASSERT_EQ_U64(n, 3);
    UTEST_ASSERT_EQ(one[0], '\0');

    /* 恰好放得下（含 NUL）：不算截断。 */
    n = IpcStrlcpy(two, "a", 2);
    UTEST_ASSERT_EQ_U64(n, 1);
    UTEST_ASSERT_STREQ(two, "a");

    /* cap == 0 与 dst == NULL：只返回长度，不写任何地方。 */
    n = IpcStrlcpy(NULL, "abc", 8);
    UTEST_ASSERT_EQ_U64(n, 3);
    n = IpcStrlcpy(one, "abc", 0);
    UTEST_ASSERT_EQ_U64(n, 3);
}

UTEST_CASE(util, strlcpy_null_source_clears_destination)
{
    char   buf[8];
    size_t n;

    memcpy(buf, "xxxxxxx", 8);
    n = IpcStrlcpy(buf, NULL, sizeof(buf));
    UTEST_ASSERT_EQ_U64(n, 0);
    UTEST_ASSERT_EQ(buf[0], '\0');

    /* src == NULL 且 dst == NULL 也不能崩。 */
    UTEST_ASSERT_EQ_U64(IpcStrlcpy(NULL, NULL, 4), 0);
}

/* ------------------------------------------------------------------ */
/* IpcPayloadToCStr                                                   */
/* ------------------------------------------------------------------ */

UTEST_CASE(util, payload_to_cstr_passes_through_printable)
{
    char out[16];

    IpcPayloadToCStr("ok", 2, out, sizeof(out));
    UTEST_ASSERT_STREQ(out, "ok");
}

UTEST_CASE(util, payload_to_cstr_replaces_non_printable)
{
    /* 0x00 0x1f 是 < 0x20；0x7f 是 DEL，也必须被替换 —— 边界要覆盖到。 */
    const unsigned char raw[] = { 'a', 0x00, 0x1f, 0x7f, 0x80, 'z' };
    char                out[16];

    IpcPayloadToCStr(raw, sizeof(raw), out, sizeof(out));
    UTEST_ASSERT_STREQ(out, "a....z");
    /* 再逐字节确认一遍：'a' 保留、四个不可打印字节变 '.'、'z' 保留。 */
    UTEST_ASSERT_EQ(out[0], 'a');
    UTEST_ASSERT_EQ(out[1], '.');
    UTEST_ASSERT_EQ(out[2], '.');
    UTEST_ASSERT_EQ(out[3], '.');
    UTEST_ASSERT_EQ(out[4], '.');
    UTEST_ASSERT_EQ(out[5], 'z');
    UTEST_ASSERT_EQ(out[6], '\0');
}

UTEST_CASE(util, payload_to_cstr_truncates_to_capacity)
{
    char out[4];

    IpcPayloadToCStr("abcdef", 6, out, sizeof(out));
    UTEST_ASSERT_STREQ(out, "abc");
    UTEST_ASSERT_EQ(out[3], '\0');
}

UTEST_CASE(util, payload_to_cstr_degenerate_inputs)
{
    char out[8];

    out[0] = 'x';
    IpcPayloadToCStr(NULL, 5, out, sizeof(out));
    UTEST_ASSERT_EQ(out[0], '\0');

    out[0] = 'x';
    IpcPayloadToCStr("abc", 0, out, sizeof(out));
    UTEST_ASSERT_EQ(out[0], '\0');

    /* 容量为 0 / 输出为空：什么都不写，也不崩。 */
    IpcPayloadToCStr("abc", 3, NULL, 8);
    IpcPayloadToCStr("abc", 3, out, 0);
}

/* ------------------------------------------------------------------ */
/* 错误码映射                                                         */
/* ------------------------------------------------------------------ */

UTEST_CASE(util, errno_to_result_known_mappings)
{
    UTEST_ASSERT_EQ(IpcErrnoToResult(0), IPC_OK);
    UTEST_ASSERT_EQ(IpcErrnoToResult(EAGAIN), IPC_ERR_AGAIN);
    UTEST_ASSERT_EQ(IpcErrnoToResult(ENOENT), IPC_ERR_NOENT);
    UTEST_ASSERT_EQ(IpcErrnoToResult(ECONNREFUSED), IPC_ERR_OFFLINE);
    UTEST_ASSERT_EQ(IpcErrnoToResult(ECONNRESET), IPC_ERR_OFFLINE);
    UTEST_ASSERT_EQ(IpcErrnoToResult(ENOTCONN), IPC_ERR_OFFLINE);
    UTEST_ASSERT_EQ(IpcErrnoToResult(EACCES), IPC_ERR_PERM);
    UTEST_ASSERT_EQ(IpcErrnoToResult(EPERM), IPC_ERR_PERM);
    UTEST_ASSERT_EQ(IpcErrnoToResult(EMSGSIZE), IPC_ERR_MSGSIZE);
    UTEST_ASSERT_EQ(IpcErrnoToResult(ENOMEM), IPC_ERR_NOMEM);
    UTEST_ASSERT_EQ(IpcErrnoToResult(EINVAL), IPC_ERR_INVAL);
    UTEST_ASSERT_EQ(IpcErrnoToResult(EBUSY), IPC_ERR_BUSY);
    /* 没列进表的错误码落到默认分支，映射成 IO 而不是「成功」。
     * 这一条很重要：默认值绝不能是 IPC_OK。 */
    UTEST_ASSERT_EQ(IpcErrnoToResult(ERANGE), IPC_ERR_IO);
}

UTEST_CASE(util, errno_string_never_null)
{
    UTEST_ASSERT_NOTNULL(IpcErrnoString(0));
    UTEST_ASSERT_NOTNULL(IpcErrnoString(ENOENT));
    UTEST_ASSERT_LT(strlen(IpcErrnoString(0)), 200u);
}

UTEST_CASE(util, result_to_string_covers_every_code)
{
    static const int32_t codes[] = {
        IPC_OK,        IPC_ERR_INVAL,     IPC_ERR_NOMEM,   IPC_ERR_IO,
        IPC_ERR_AGAIN, IPC_ERR_NOENT,     IPC_ERR_OFFLINE, IPC_ERR_PERM,
        IPC_ERR_BUSY,  IPC_ERR_CRED,      IPC_ERR_PROTO,   IPC_ERR_TIMEOUT,
        IPC_ERR_STOPPED, IPC_ERR_DEADLOCK, IPC_ERR_MSGSIZE, IPC_ERR_CONFIG,
        IPC_ERR_TOOMANY, IPC_ERR_STATE
    };
    size_t i;

    for (i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
        const char *text = IpcResultToString(codes[i]);

        UTEST_ASSERT_MSG(text != NULL, "返回码 %d 的字符串不该是 NULL", codes[i]);
        UTEST_ASSERT_MSG(text[0] != '\0', "返回码 %d 的字符串不该是空的", codes[i]);
        /* 除了 IPC_OK，其余都要与 not-found 区分得开。 */
        if (codes[i] != (int32_t)IPC_OK) {
            UTEST_ASSERT_MSG(strcmp(text, "ok") != 0, "返回码 %d 的字符串不该是 ok",
                             codes[i]);
        }
    }
    /* 未知值：接口承诺返回 "unknown"，不返回 NULL。 */
    UTEST_ASSERT_STREQ(IpcResultToString(99), "unknown");
    UTEST_ASSERT_STREQ(IpcResultToString(-999), "unknown");
}

/* ------------------------------------------------------------------ */
/* 时间与代际号                                                       */
/* ------------------------------------------------------------------ */

UTEST_CASE(util, mono_ns_is_monotonic_and_nonzero)
{
    uint64_t a = IpcMonoNs();
    uint64_t b = IpcMonoNs();

    UTEST_ASSERT(a > 0);      /* 拿到真实时钟 */
    UTEST_ASSERT_GE(b, a);    /* 单调不减 */
}

UTEST_CASE(util, mix64_matches_reference_vectors)
{
    /*
     * 这几个期望值是用独立的 Python 实现算出来的（splitmix64 常数 + 64 位回绕），
     * 不是从本实现反抄的。所以它们能抓住「移位量写错」「常数抄错」这类问题
     * —— 那种错误下函数仍然「看起来像个哈希」，只有对参考值才验得出来。
     */
    UTEST_ASSERT_EQ_U64(IpcMix64(0), 0xe220a8397b1dcdafull);
    UTEST_ASSERT_EQ_U64(IpcMix64(1), 0x910a2dec89025cc1ull);
    UTEST_ASSERT_EQ_U64(IpcMix64(2), 0x975835de1c9756ceull);
    UTEST_ASSERT_EQ_U64(IpcMix64(0xffffffffffffffffull), 0xe4d971771b652c20ull);
    UTEST_ASSERT_EQ_U64(IpcMix64(0x0123456789abcdefull), 0x157a3807a48faa9dull);
}

UTEST_CASE(util, gen_instance_id_is_nonzero_and_varies)
{
    uint64_t first = IpcGenInstanceId();
    uint64_t seen[64];
    int32_t  distinct = 0;
    int32_t  i;
    int32_t  j;

    UTEST_ASSERT(first != 0); /* 0 保留给「未初始化」 */
    for (i = 0; i < 64; i++) {
        seen[i] = IpcGenInstanceId();
        UTEST_ASSERT(seen[i] != 0);
    }
    for (i = 0; i < 64; i++) {
        int32_t isNew = 1;

        for (j = 0; j < i; j++) {
            if (seen[j] == seen[i]) {
                isNew = 0;
                break;
            }
        }
        distinct += isNew;
    }
    /* 不要求 64 个全不同（时钟分辨率可能让相邻两次相同），但绝不该退化成
     * 「每次都一样」—— 那样陈旧回复的识别就完全失效了。 */
    UTEST_ASSERT_GE(distinct, 8);
}

/* ------------------------------------------------------------------ */
/* IpcReadFile                                                        */
/* ------------------------------------------------------------------ */

UTEST_CASE(util, read_file_roundtrip)
{
    char        dir[256];
    char        path[512];
    char       *text;
    int32_t     err = 0;
    static const char kBody[] = "alpha\nbeta\n";

    UTEST_ASSERT_EQ(IpcLabCreate(dir, sizeof(dir)), 0);
    (void)snprintf(path, sizeof(path), "%s/file.txt", dir);
    UTEST_ASSERT_EQ(IpcLabWriteFile(path, kBody), 0);

    text = IpcReadFile(path, 4096, &err);
    UTEST_ASSERT_NOTNULL(text);
    UTEST_ASSERT_EQ(err, IPC_OK);
    UTEST_ASSERT_STREQ(text, kBody);
    free(text);

    IpcLabRemove(dir);
}

UTEST_CASE(util, read_file_rejects_oversized_content)
{
    char     dir[256];
    char     path[512];
    char     big[128];
    char    *text;
    int32_t  err = 0;

    UTEST_ASSERT_EQ(IpcLabCreate(dir, sizeof(dir)), 0);
    (void)snprintf(path, sizeof(path), "%s/big.txt", dir);
    memset(big, 'A', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    UTEST_ASSERT_EQ(IpcLabWriteFile(path, big), 0);

    /* maxBytes 比文件小：必须报 MSGSIZE，而不是截断后返回半截内容。 */
    text = IpcReadFile(path, 16, &err);
    UTEST_ASSERT_NULL(text);
    UTEST_ASSERT_EQ(err, IPC_ERR_MSGSIZE);

    /* 正好等于文件大小时应当成功（边界两侧都要覆盖）。 */
    text = IpcReadFile(path, sizeof(big) - 1, &err);
    UTEST_ASSERT_NOTNULL(text);
    UTEST_ASSERT_EQ(strlen(text), sizeof(big) - 1);
    free(text);

    IpcLabRemove(dir);
}

UTEST_CASE(util, read_file_error_paths)
{
    char     dir[256];
    char     path[512];
    char    *text;
    int32_t  err = 0;

    UTEST_ASSERT_EQ(IpcLabCreate(dir, sizeof(dir)), 0);
    (void)snprintf(path, sizeof(path), "%s/absent.txt", dir);

    text = IpcReadFile(path, 4096, &err);
    UTEST_ASSERT_NULL(text);
    UTEST_ASSERT_EQ(err, IPC_ERR_NOENT);

    err = IPC_OK;
    UTEST_ASSERT_NULL(IpcReadFile(NULL, 4096, &err));
    UTEST_ASSERT_EQ(err, IPC_ERR_INVAL);

    err = IPC_OK;
    UTEST_ASSERT_NULL(IpcReadFile(path, 0, &err));
    UTEST_ASSERT_EQ(err, IPC_ERR_INVAL);

    /* outErr 允许传 NULL —— 内部每处都要判它。 */
    UTEST_ASSERT_NULL(IpcReadFile(path, 4096, NULL));

    IpcLabRemove(dir);
}
