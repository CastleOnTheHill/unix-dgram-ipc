/*
 * Copyright (c) 2026 <版权方待定>
 * 许可协议：<待定>
 *
 * ipc_config.h -- 静态模块表的内部表示。**不对外安装**。
 *
 * 对外只有 ipc/ipc.h 里那一组 IpcConfig* 函数；本头文件暴露的是内部结构和
 * 两三个可单独测试的纯函数，供白盒单测直接引用（规范允许用 extern 声明
 * 引用内部函数做单测）。
 */
#ifndef IPC_CONFIG_H
#define IPC_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#include "ipc/ipc.h"

/*
 * 表的内部表示。
 *
 * 用**连续数组**而不是链表：两个主要用法是「按下标访问」和「顺序遍历
 * （广播要遍历同命名空间的所有条目）」，连续数组对两者都最友好；
 * 而一条 entry 只有约 160 字节，几千条也不过几百 KiB。
 */
struct IpcConfig {
    IpcConfigEntry *entries;
    int32_t         count;
    int32_t         cap;
};

/*
 * 文件大小上限 1 MiB。它同时充当「条目数上限」的实际约束：一条 entry 最少
 * 也要 8 个字节以上的文本，所以 1 MiB 最多也就十来万条，内存占用有界。
 * （examples/README.md §7 里列的「要不要再加一个 IPC_CONFIG_MAX_ENTRIES」
 * 仍是待定项；在有文件大小上限的前提下，它不是一个安全缺口。）
 */
#define IPC_CONFIG_FILE_MAX ((size_t)1u << 20)

/*
 * 剥掉行尾的 `\r`（以及 `\n`）。返回剥离后的长度。
 *
 * 这是**有意的契约**，不是碰巧成立：旧实现之所以能处理 CRLF，是因为它按
 * 空白字符切分字段，而 isspace('\r') 恰好为真 —— 一旦以后改成按固定列宽
 * 切分就会立刻踩坑。本实现显式剥掉，把这条行为钉死。
 */
size_t IpcConfigStripEol(char *line);

/*
 * 解析一行（恰好 4 个字段）。line 会被**就地修改**（切分出的 NUL）。
 * 调用者负责先剥注释、去空白、跳过空行。
 * 成功返回 IPC_OK，失败返回 IPC_ERR_CONFIG，并把原因写进日志。
 */
int32_t IpcConfigParseLine(char *line, IpcConfigEntry *out, int32_t lineno);

#endif /* IPC_CONFIG_H */
