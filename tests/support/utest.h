/*
 * Copyright (c) 2026 <版权方待定>
 * 许可协议：<待定>
 *
 * utest.h -- 极简单元测试框架。**仅供测试，不随交付物发布**。
 *
 * =====================================================================
 * 这个文件属于哪一类代码
 * =====================================================================
 * tests/ 下的一切都是「为了测试的实现」，src/ 下的一切才是交付物。
 * 这条界线不是靠自觉维持的：
 *   - 构建规则上，libipc.a 只由 src/ 下的 .c 生成（见 Makefile）；
 *   - `make check-separation` 会把 libipc.a 的符号表翻一遍，出现测试用
 *     实现的名字（utest / refhost / testhost）或 main 就报错。
 *
 * 所以本框架**不需要**为了进交付物而克制依赖：它可以用编译器扩展、
 * 可以 printf、可以假设有文件系统。而 src/ 不行。
 *
 * =====================================================================
 * 用例是怎么被发现的
 * =====================================================================
 * 用 GCC/Clang 的 constructor 属性在加载期把用例登记进一张表，于是
 * run_unit 的 main 不需要维护一张「有哪些测试文件」的清单。
 * 这么做的理由很实际：手工清单的失效方式是**静默**的 —— 新写了一个
 * 测试文件、忘了加进清单，覆盖率掉下去而没有任何人知道。
 * constructor 是编译器扩展，但它只用在这里，不污染交付物。
 *
 * 兜底措施仍然留着：run_unit 在「一个用例都没登记到」时直接判失败，
 * 因为那说明 constructor 机制在这套工具链上没生效，而不是「没有测试」。
 */
#ifndef UTEST_H
#define UTEST_H

#include <stddef.h>
#include <stdint.h>

typedef void (*UtestFunc)(void);

typedef struct {
    const char *suite; /* 一般是文件名去掉 test_ 前缀与 .c 后缀 */
    const char *name;
    UtestFunc   func;
    /* 运行期状态 */
    int32_t     ran;
    int32_t     failed;
} UtestCase;

/*
 * 登记一个用例。constructor 会自动调用，测试代码一般不直接调它。
 * 表满返回非 0；运行器会把这个错误当失败报出来（不许静默丢用例）。
 */
int32_t UtestRegister(const char *suite, const char *name, UtestFunc func);

/* 已登记用例数。 */
int32_t UtestCount(void);

/*
 * 跑全部用例（filter 非 NULL 时只跑名字里含该子串的）。
 * 返回失败的用例数；0 表示全过。
 */
int32_t UtestRunAll(const char *filter);

/*
 * 记录一次断言失败。fmt 为 NULL 时不打印额外信息。
 * 由下面的宏调用，测试代码不直接调。
 */
void UtestFail(const char *file, int line, const char *expr, const char *fmt, ...);

/*
 * 框架自检：往失败计数里塞一次**故意的**失败，确认它确实被记下来、
 * 又被正确清掉。返回 0 表示框架可信。
 *
 * 存在的理由和本仓库其它检查器一样：一个「永远不会报错」的测试框架
 * 比没有框架更危险 —— 它会让人以为 0 失败意味着代码没问题。
 */
int32_t UtestSelfCheck(void);

/* ------------------------------------------------------------------ */
/* 用例定义与断言                                                     */
/* ------------------------------------------------------------------ */

#define UTEST_CASE(suite, name)                                                \
    static void UtestIsolate_##suite##_##name(void);                           \
    static void UtestRegister_##suite##_##name(void) __attribute__((constructor)); \
    static void UtestRegister_##suite##_##name(void)                           \
    {                                                                          \
        (void)UtestRegister(#suite, #name, UtestIsolate_##suite##_##name);      \
    }                                                                          \
    static void UtestIsolate_##suite##_##name(void)

#define UTEST_ASSERT(expr)                                                     \
    do {                                                                       \
        if (!(expr)) {                                                         \
            UtestFail(__FILE__, __LINE__, #expr, NULL);                        \
        }                                                                      \
    } while (0)

#define UTEST_ASSERT_MSG(expr, ...)                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            UtestFail(__FILE__, __LINE__, #expr, __VA_ARGS__);                 \
        }                                                                      \
    } while (0)

#define UTEST_ASSERT_TRUE(expr)  UTEST_ASSERT((expr) != 0)
#define UTEST_ASSERT_FALSE(expr) UTEST_ASSERT((expr) == 0)

#define UTEST_ASSERT_NULL(ptr)      UTEST_ASSERT((ptr) == NULL)
#define UTEST_ASSERT_NOTNULL(ptr)   UTEST_ASSERT((ptr) != NULL)

/* 整数比较：失败时把两边的值打出来，省掉一轮「到底差在哪」的猜测。 */
#define UTEST_ASSERT_EQ(actual, expected)                                      \
    do {                                                                       \
        long long utestA = (long long)(actual);                                \
        long long utestE = (long long)(expected);                              \
        if (utestA != utestE) {                                                \
            UtestFail(__FILE__, __LINE__, #actual " == " #expected,            \
                      "实际 %lld，期望 %lld", utestA, utestE);                  \
        }                                                                      \
    } while (0)

#define UTEST_ASSERT_LT(a, b)                                                  \
    do {                                                                       \
        long long utestA = (long long)(a);                                     \
        long long utestB = (long long)(b);                                     \
        if (!(utestA < utestB)) {                                              \
            UtestFail(__FILE__, __LINE__, #a " < " #b, "实际 %lld 与 %lld",     \
                      utestA, utestB);                                         \
        }                                                                      \
    } while (0)

#define UTEST_ASSERT_GE(a, b)                                                  \
    do {                                                                       \
        long long utestA = (long long)(a);                                     \
        long long utestB = (long long)(b);                                     \
        if (!(utestA >= utestB)) {                                             \
            UtestFail(__FILE__, __LINE__, #a " >= " #b, "实际 %lld 与 %lld",    \
                      utestA, utestB);                                         \
        }                                                                      \
    } while (0)

/* 无符号量比较：避免把 uint64_t 塞进 long long 时符号位翻转。 */
#define UTEST_ASSERT_EQ_U64(actual, expected)                                  \
    do {                                                                       \
        unsigned long long utestA = (unsigned long long)(actual);              \
        unsigned long long utestE = (unsigned long long)(expected);            \
        if (utestA != utestE) {                                                \
            UtestFail(__FILE__, __LINE__, #actual " == " #expected,            \
                      "实际 %llu，期望 %llu", utestA, utestE);                  \
        }                                                                      \
    } while (0)

#define UTEST_ASSERT_STREQ(actual, expected)                                   \
    do {                                                                       \
        const char *utestA = (actual);                                         \
        const char *utestE = (expected);                                       \
        if (utestA == NULL || utestE == NULL || strcmp(utestA, utestE) != 0) {  \
            UtestFail(__FILE__, __LINE__, #actual " == " #expected,            \
                      "实际 \"%s\"，期望 \"%s\"",                               \
                      (utestA != NULL) ? utestA : "(null)",                    \
                      (utestE != NULL) ? utestE : "(null)");                   \
        }                                                                      \
    } while (0)

/* UTEST_ASSERT_STREQ 需要 strcmp，请在各测试文件里自己 include <string.h>。 */

#endif /* UTEST_H */
