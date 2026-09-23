/*
 * Copyright (c) 2026 <版权方待定>
 * 许可协议：<待定>
 *
 * test_config.c -- ipc_config 的白盒单测。**仅供测试**。
 *
 * 配置解析的每一条规则都以 examples/README.md 为准，本文件是那份说明的
 * 可执行版本。**任何一条「拒绝」都要有对应用例** —— 配置错了会静默地少一个
 * 模块，那种故障比崩溃难查得多。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ipc_config.h"
#include "ipc/ipc.h"
#include "lab.h"
#include "utest.h"

/* 解析单行：返回结果码，成功时把 entry 填好。line 会被就地改写。 */
static int32_t ParseOne(char *line, IpcConfigEntry *entry)
{
    char scratch[512];

    (void)snprintf(scratch, sizeof(scratch), "%s", line);
    (void)IpcConfigStripEol(scratch);
    return IpcConfigParseLine(scratch, entry, 1);
}

/* ------------------------------------------------------------------ */
/* 行尾处理                                                           */
/* ------------------------------------------------------------------ */

UTEST_CASE(config, strip_eol_removes_all_trailing_line_endings)
{
    char buf[32];

    (void)snprintf(buf, sizeof(buf), "abc\n");
    UTEST_ASSERT_EQ_U64(IpcConfigStripEol(buf), 3);
    UTEST_ASSERT_STREQ(buf, "abc");

    (void)snprintf(buf, sizeof(buf), "abc\r\n");
    UTEST_ASSERT_EQ_U64(IpcConfigStripEol(buf), 3);
    UTEST_ASSERT_STREQ(buf, "abc");

    /* 连续多个都要剥掉。 */
    (void)snprintf(buf, sizeof(buf), "abc\r\n\r\n");
    UTEST_ASSERT_EQ_U64(IpcConfigStripEol(buf), 3);
    UTEST_ASSERT_STREQ(buf, "abc");

    /* 中间的不动。 */
    (void)snprintf(buf, sizeof(buf), "a\rb\n");
    UTEST_ASSERT_EQ_U64(IpcConfigStripEol(buf), 3);
    UTEST_ASSERT_STREQ(buf, "a\rb");

    /* 空串、纯换行、NULL。 */
    buf[0] = '\0';
    UTEST_ASSERT_EQ_U64(IpcConfigStripEol(buf), 0);
    (void)snprintf(buf, sizeof(buf), "\n");
    UTEST_ASSERT_EQ_U64(IpcConfigStripEol(buf), 0);
    UTEST_ASSERT_EQ_U64(IpcConfigStripEol(NULL), 0);
}

/* ------------------------------------------------------------------ */
/* 单行解析：合法形状                                                 */
/* ------------------------------------------------------------------ */

UTEST_CASE(config, parse_line_accepts_wellformed_input)
{
    IpcConfigEntry entry;

    UTEST_ASSERT_EQ(ParseOne("ns1 mod_a 0 /tmp/a.sock", &entry), IPC_OK);
    UTEST_ASSERT_STREQ(entry.ns, "ns1");
    UTEST_ASSERT_STREQ(entry.moduleId, "mod_a");
    UTEST_ASSERT_EQ_U64(entry.uid, 0);
    UTEST_ASSERT_STREQ(entry.path, "/tmp/a.sock");

    /* 多个空格、TAB、\r 混用都算分隔符。 */
    UTEST_ASSERT_EQ(ParseOne("ns1\t\tmod_a \t 1000 \t /tmp/b.sock", &entry), IPC_OK);
    UTEST_ASSERT_STREQ(entry.moduleId, "mod_a");
    UTEST_ASSERT_EQ_U64(entry.uid, 1000);

    /* moduleId 允许 '_' '-' '.'；ns 允许 '_' '-'。 */
    UTEST_ASSERT_EQ(ParseOne("a-b_c mod.a-b_c 7 /tmp/c.sock", &entry), IPC_OK);
    UTEST_ASSERT_STREQ(entry.ns, "a-b_c");
    UTEST_ASSERT_STREQ(entry.moduleId, "mod.a-b_c");

    /* uid 的合法边界：0 与 4294967294。 */
    UTEST_ASSERT_EQ(ParseOne("ns m 0 /tmp/x", &entry), IPC_OK);
    UTEST_ASSERT_EQ(ParseOne("ns m 4294967294 /tmp/x", &entry), IPC_OK);

    /* 长度边界：ns 15 字符合法、module 31 字符合法、path 107 字符合法。 */
    {
        char line[512];
        char ns[16];
        char mod[32];
        char path[128];

        memset(ns, 'n', 15);
        ns[15] = '\0';
        memset(mod, 'm', 31);
        mod[31] = '\0';
        memset(path, 'p', sizeof(path) - 1);
        path[0] = '/';
        path[107] = '\0';

        (void)snprintf(line, sizeof(line), "%s %s 0 %s", ns, mod, path);
        UTEST_ASSERT_EQ(ParseOne(line, &entry), IPC_OK);
        UTEST_ASSERT_EQ_U64(strlen(entry.ns), 15);
        UTEST_ASSERT_EQ_U64(strlen(entry.moduleId), 31);
        UTEST_ASSERT_EQ_U64(strlen(entry.path), 107);
    }
}

/* ------------------------------------------------------------------ */
/* 单行解析：拒绝                                                     */
/* ------------------------------------------------------------------ */

UTEST_CASE(config, parse_line_rejects_wrong_field_count)
{
    IpcConfigEntry entry;

    UTEST_ASSERT_EQ(ParseOne("ns mod 0", &entry), IPC_ERR_CONFIG);            /* 3 个 */
    UTEST_ASSERT_EQ(ParseOne("ns mod 0 /tmp/a extra", &entry), IPC_ERR_CONFIG); /* 5 个 */
    UTEST_ASSERT_EQ(ParseOne("", &entry), IPC_ERR_CONFIG);
    UTEST_ASSERT_EQ(ParseOne("   \t ", &entry), IPC_ERR_CONFIG);
}

UTEST_CASE(config, parse_line_rejects_bad_namespace)
{
    IpcConfigEntry entry;
    char            line[512];
    char            ns[24];

    /* 含非法字符：'/' ':' 与 '.'（ns 不允许 '.'）。每个都保持 4 个字段，
     * 这样失败原因只可能是字符集，不会与「字段数不对」混在一起。 */
    UTEST_ASSERT_EQ(ParseOne("ns/x mod 0 /tmp/a", &entry), IPC_ERR_CONFIG);
    UTEST_ASSERT_EQ(ParseOne("n:sx mod 0 /tmp/a", &entry), IPC_ERR_CONFIG);
    UTEST_ASSERT_EQ(ParseOne("ns.x mod 0 /tmp/a", &entry), IPC_ERR_CONFIG);

    /* 16 字符：超出一个字节。 */
    memset(ns, 'n', 16);
    ns[16] = '\0';
    (void)snprintf(line, sizeof(line), "%s mod 0 /tmp/a", ns);
    UTEST_ASSERT_EQ(ParseOne(line, &entry), IPC_ERR_CONFIG);
}

UTEST_CASE(config, parse_line_rejects_bad_module_id)
{
    IpcConfigEntry entry;
    char            line[512];
    char            mod[40];

    UTEST_ASSERT_EQ(ParseOne("ns mo/d 0 /tmp/a", &entry), IPC_ERR_CONFIG);
    UTEST_ASSERT_EQ(ParseOne("ns mo:d 0 /tmp/a", &entry), IPC_ERR_CONFIG);

    memset(mod, 'm', 32);
    mod[32] = '\0';
    (void)snprintf(line, sizeof(line), "ns %s 0 /tmp/a", mod);
    UTEST_ASSERT_EQ(ParseOne(line, &entry), IPC_ERR_CONFIG);
}

UTEST_CASE(config, parse_line_rejects_bad_uid)
{
    IpcConfigEntry entry;

    /* 只收纯十进制。负数、正号、十六进制、用户名、尾随垃圾一律拒绝。 */
    UTEST_ASSERT_EQ(ParseOne("ns mod -1 /tmp/a", &entry), IPC_ERR_CONFIG);
    UTEST_ASSERT_EQ(ParseOne("ns mod +1 /tmp/a", &entry), IPC_ERR_CONFIG);
    UTEST_ASSERT_EQ(ParseOne("ns mod 0x10 /tmp/a", &entry), IPC_ERR_CONFIG);
    UTEST_ASSERT_EQ(ParseOne("ns mod root /tmp/a", &entry), IPC_ERR_CONFIG);
    UTEST_ASSERT_EQ(ParseOne("ns mod 12abc /tmp/a", &entry), IPC_ERR_CONFIG);
    UTEST_ASSERT_EQ(ParseOne("ns mod abc /tmp/a", &entry), IPC_ERR_CONFIG);
    /* 越界一个数。 */
    UTEST_ASSERT_EQ(ParseOne("ns mod 4294967295 /tmp/a", &entry), IPC_ERR_CONFIG);
    UTEST_ASSERT_EQ(ParseOne("ns mod 99999999999999999999 /tmp/a", &entry),
                    IPC_ERR_CONFIG);
}

UTEST_CASE(config, parse_line_rejects_bad_path)
{
    IpcConfigEntry entry;

    UTEST_ASSERT_EQ(ParseOne("ns mod 0 relative/path", &entry), IPC_ERR_CONFIG);
    UTEST_ASSERT_EQ(ParseOne("ns mod 0 ./x", &entry), IPC_ERR_CONFIG);
    UTEST_ASSERT_EQ(ParseOne("ns mod 0 /", &entry), IPC_ERR_CONFIG); /* 目录不是端点 */

    /* 108 字符：sun_path 只有 108 字节，含 NUL 就放不下了。 */
    {
        char line[512];
        char path[128];

        memset(path, 'p', sizeof(path));
        path[0] = '/';
        path[108] = '\0';
        (void)snprintf(line, sizeof(line), "ns mod 0 %s", path);
        UTEST_ASSERT_EQ(ParseOne(line, &entry), IPC_ERR_CONFIG);
    }
}

UTEST_CASE(config, parse_line_rejects_null_arguments)
{
    IpcConfigEntry entry;
    char           line[64];

    (void)snprintf(line, sizeof(line), "ns mod 0 /tmp/a");
    UTEST_ASSERT_EQ(IpcConfigParseLine(NULL, &entry, 1), IPC_ERR_INVAL);
    UTEST_ASSERT_EQ(IpcConfigParseLine(line, NULL, 1), IPC_ERR_INVAL);
}

/* ------------------------------------------------------------------ */
/* 整表解析                                                           */
/* ------------------------------------------------------------------ */

UTEST_CASE(config, parse_table_handles_comments_and_blank_lines)
{
    IpcConfig     *config = NULL;
    const char    *text   = "# 整行注释\n"
                            "\n"
                            "   \n"
                            "ns1 a 0 /tmp/a.sock\n"
                            "ns1 b 1000 /tmp/b.sock   # 行内注释\n"
                            "#再来一行注释\n"
                            "ns1 c 1000 /tmp/c.sock";

    UTEST_ASSERT_EQ(IpcConfigParse(text, &config), IPC_OK);
    UTEST_ASSERT_NOTNULL(config);
    UTEST_ASSERT_EQ(IpcConfigGetCount(config), 3);
    UTEST_ASSERT_STREQ(IpcConfigGetEntry(config, 1)->moduleId, "b");
    /* 行内注释被切掉，路径不含 '#' 后面的内容。 */
    UTEST_ASSERT_STREQ(IpcConfigGetEntry(config, 1)->path, "/tmp/b.sock");
    IpcConfigDestroy(config);
}

UTEST_CASE(config, parse_table_handles_crlf)
{
    IpcConfig  *config = NULL;
    const char *text   = "ns1 a 0 /tmp/a.sock\r\nns1 b 0 /tmp/b.sock\r\n";

    /*
     * CRLF 必须能过。这里测的是**显式承诺**那条：老实现能过是因为按空白切分
     * 时 isspace('\r') 恰好为真，本实现则是先显式剥掉 \r。
     * 两种实现都能过，但只有后者在将来改成定宽解析时仍然成立。
     */
    UTEST_ASSERT_EQ(IpcConfigParse(text, &config), IPC_OK);
    UTEST_ASSERT_EQ(IpcConfigGetCount(config), 2);
    UTEST_ASSERT_STREQ(IpcConfigGetEntry(config, 0)->path, "/tmp/a.sock");
    IpcConfigDestroy(config);
}

UTEST_CASE(config, parse_table_rejects_duplicates_as_a_whole)
{
    IpcConfig *config = NULL;

    /* 同一个 (ns, moduleId) 出现两次。 */
    config = NULL;
    UTEST_ASSERT_EQ(IpcConfigParse("ns1 a 0 /tmp/a.sock\nns1 a 0 /tmp/other.sock\n",
                                   &config),
                    IPC_ERR_CONFIG);
    UTEST_ASSERT_NULL(config);

    /* 同一个 path 出现两次 —— 注意是**跨命名空间**也不允许：
     * 路径是内核地址空间里的名字，与库的 ns 无关。 */
    config = NULL;
    UTEST_ASSERT_EQ(IpcConfigParse("ns1 a 0 /tmp/a.sock\nns2 b 0 /tmp/a.sock\n", &config),
                    IPC_ERR_CONFIG);
    UTEST_ASSERT_NULL(config);

    /* 反过来：同名模块在不同 ns 下是合法的（配置层允许，注册时会判歧义）。 */
    config = NULL;
    UTEST_ASSERT_EQ(IpcConfigParse("ns1 a 0 /tmp/a.sock\nns2 a 0 /tmp/b.sock\n", &config),
                    IPC_OK);
    UTEST_ASSERT_EQ(IpcConfigGetCount(config), 2);
    IpcConfigDestroy(config);
}

UTEST_CASE(config, parse_table_rejects_on_first_bad_line)
{
    IpcConfig *config = NULL;

    /* 前面的行合法、后面的行非法 → 整表拒绝，且 outConfig 必须是 NULL。 */
    UTEST_ASSERT_EQ(IpcConfigParse("ns1 a 0 /tmp/a.sock\nns1 b 0 relative\n", &config),
                    IPC_ERR_CONFIG);
    UTEST_ASSERT_NULL(config);

    /* 非法行在前面也一样。 */
    UTEST_ASSERT_EQ(IpcConfigParse("ns1 b 0 relative\nns1 a 0 /tmp/a.sock\n", &config),
                    IPC_ERR_CONFIG);
    UTEST_ASSERT_NULL(config);
}

UTEST_CASE(config, parse_table_degenerate_inputs)
{
    IpcConfig *config = NULL;

    UTEST_ASSERT_EQ(IpcConfigParse(NULL, &config), IPC_ERR_INVAL);
    UTEST_ASSERT_EQ(IpcConfigParse("ns1 a 0 /tmp/a", NULL), IPC_ERR_INVAL);

    /* 空表是合法的（一个模块都没有）。 */
    config = NULL;
    UTEST_ASSERT_EQ(IpcConfigParse("", &config), IPC_OK);
    UTEST_ASSERT_EQ(IpcConfigGetCount(config), 0);
    IpcConfigDestroy(config);

    /* 只有注释与空白也一样。 */
    config = NULL;
    UTEST_ASSERT_EQ(IpcConfigParse("# 什么都没有\n\n", &config), IPC_OK);
    UTEST_ASSERT_EQ(IpcConfigGetCount(config), 0);
    IpcConfigDestroy(config);
}

UTEST_CASE(config, parse_table_grows_past_initial_capacity)
{
    /* 初始容量是 16，这里给 40 条：验证扩容路径（realloc）没写坏。 */
    char       text[8192];
    size_t     used = 0;
    int32_t    i;
    IpcConfig *config = NULL;

    text[0] = '\0';
    for (i = 0; i < 40; i++) {
        used += (size_t)snprintf(text + used, sizeof(text) - used,
                                 "ns1 mod%02d %lu /tmp/m%02d.sock\n", i,
                                 (unsigned long)getuid(), i);
        if (used >= sizeof(text)) {
            break;
        }
    }
    UTEST_ASSERT_EQ(IpcConfigParse(text, &config), IPC_OK);
    UTEST_ASSERT_EQ(IpcConfigGetCount(config), 40);
    UTEST_ASSERT_STREQ(IpcConfigGetEntry(config, 39)->moduleId, "mod39");
    UTEST_ASSERT_STREQ(IpcConfigGetEntry(config, 39)->path, "/tmp/m39.sock");
    IpcConfigDestroy(config);
}

/* ------------------------------------------------------------------ */
/* 查找与访问                                                         */
/* ------------------------------------------------------------------ */

UTEST_CASE(config, lookup_helpers)
{
    IpcConfig *config = NULL;

    UTEST_ASSERT_EQ(IpcConfigParse("ns1 a 0 /tmp/a.sock\nns2 b 0 /tmp/b.sock\n",
                                   &config),
                    IPC_OK);

    UTEST_ASSERT_NOTNULL(IpcConfigFindModule(config, "ns1", "a"));
    UTEST_ASSERT_NOTNULL(IpcConfigFindModule(config, "ns2", "b"));
    UTEST_ASSERT_NULL(IpcConfigFindModule(config, "ns1", "b")); /* ns 也对不上 */
    UTEST_ASSERT_NULL(IpcConfigFindModule(config, "ns3", "a"));
    UTEST_ASSERT_NULL(IpcConfigFindModule(config, NULL, "a"));
    UTEST_ASSERT_NULL(IpcConfigFindModule(config, "ns1", NULL));

    UTEST_ASSERT_NOTNULL(IpcConfigFindByPath(config, "/tmp/a.sock"));
    UTEST_ASSERT_NULL(IpcConfigFindByPath(config, "/tmp/absent"));
    UTEST_ASSERT_NULL(IpcConfigFindByPath(config, NULL));
    UTEST_ASSERT_STREQ(IpcConfigFindByPath(config, "/tmp/b.sock")->moduleId, "b");

    UTEST_ASSERT_NOTNULL(IpcConfigGetEntry(config, 0));
    UTEST_ASSERT_NOTNULL(IpcConfigGetEntry(config, 1));
    UTEST_ASSERT_NULL(IpcConfigGetEntry(config, 2));  /* 越界 */
    UTEST_ASSERT_NULL(IpcConfigGetEntry(config, -1)); /* 负下标 */

    IpcConfigDestroy(config);

    /* NULL 配置对象上的每一个查询都要安全。 */
    UTEST_ASSERT_EQ(IpcConfigGetCount(NULL), 0);
    UTEST_ASSERT_NULL(IpcConfigGetEntry(NULL, 0));
    UTEST_ASSERT_NULL(IpcConfigFindModule(NULL, "ns1", "a"));
    UTEST_ASSERT_NULL(IpcConfigFindByPath(NULL, "/tmp/a.sock"));
    IpcConfigDestroy(NULL); /* 幂等 */
}

/* ------------------------------------------------------------------ */
/* 从文件加载                                                         */
/* ------------------------------------------------------------------ */

UTEST_CASE(config, load_from_file_and_error_paths)
{
    char        dir[256];
    char        path[512];
    char        text[512];
    IpcConfig  *config = NULL;
    static const char *modules[2] = { "alpha", "beta" };
    size_t      written;

    UTEST_ASSERT_EQ(IpcLabCreate(dir, sizeof(dir)), 0);
    IpcLabConfPath(dir, path, sizeof(path));

    written = IpcLabBuildConf(dir, "ns1", modules, 2, text, sizeof(text));
    UTEST_ASSERT(written > 0);
    UTEST_ASSERT_EQ(IpcLabWriteFile(path, text), 0);

    UTEST_ASSERT_EQ(IpcConfigLoad(path, &config), IPC_OK);
    UTEST_ASSERT_EQ(IpcConfigGetCount(config), 2);
    UTEST_ASSERT_STREQ(IpcConfigGetEntry(config, 0)->ns, "ns1");
    UTEST_ASSERT_EQ_U64(IpcConfigGetEntry(config, 0)->uid, getuid());
    IpcConfigDestroy(config);

    /* 不存在 → NOENT（不是 GENERIC 的 IO）。 */
    config = NULL;
    UTEST_ASSERT_EQ(IpcConfigLoad("/tmp/definitely-not-here.conf", &config),
                    IPC_ERR_NOENT);
    UTEST_ASSERT_NULL(config);

    /* 参数检查。 */
    UTEST_ASSERT_EQ(IpcConfigLoad(NULL, &config), IPC_ERR_INVAL);
    UTEST_ASSERT_EQ(IpcConfigLoad(path, NULL), IPC_ERR_INVAL);

    /* 文件内容非法 → CONFIG。 */
    UTEST_ASSERT_EQ(IpcLabWriteFile(path, "ns1 a 0 relative-path\n"), 0);
    config = NULL;
    UTEST_ASSERT_EQ(IpcConfigLoad(path, &config), IPC_ERR_CONFIG);
    UTEST_ASSERT_NULL(config);

    IpcLabRemove(dir);
}
