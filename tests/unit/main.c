/*
 * Copyright (c) 2026 <版权方待定>
 * 许可协议：<待定>
 *
 * main.c -- 白盒单元测试的入口。**仅供测试，不随交付物发布**。
 *
 * 退出码约定（沿用本仓库对「检查器」的一贯要求）：
 *   0 —— 全部用例通过
 *   1 —— 有用例失败
 *   2 —— **没跑成**（框架自检没过、或一个用例都没登记到）
 *
 * 把 2 与 1 分开是有意义的：调用方（CI、脚本、人）必须能区分「代码错了」
 * 和「这次根本没测到东西」。只用一个非零码会把两者混成一件事故。
 */
#include <stdio.h>
#include <string.h>

#include "utest.h"

int main(int argc, char *argv[])
{
    const char *filter = NULL;
    int32_t     failed;

    if (argc > 1 && argv[1][0] != '\0') {
        filter = argv[1];
    }

    printf("== IPC 传输层白盒单元测试\n");
    if (filter != NULL) {
        printf("   过滤条件: %s\n", filter);
    }

    /* 先自检框架：一个「永远报通过」的框架比没有框架更危险。 */
    if (UtestSelfCheck() != 0) {
        fprintf(stderr, "!! 测试框架自检失败，本次结果不可信，退出码 2\n");
        return 2;
    }
    if (UtestCount() == 0) {
        fprintf(stderr, "!! 没有登记到任何用例，退出码 2（这不是「通过」）\n");
        return 2;
    }
    printf("   登记用例: %d 个\n\n", UtestCount());

    failed = UtestRunAll(filter);
    if (failed < 0) {
        fprintf(stderr, "!! 用例运行过程本身出了问题，退出码 2\n");
        return 2;
    }
    if (failed != 0) {
        printf("\n!! 白盒测试失败：%d 个用例\n", failed);
        return 1;
    }
    printf("\n== 白盒测试全部通过\n");
    return 0;
}
