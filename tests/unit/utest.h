/*
 * utest.h -- the smallest test framework that still gives useful output.
 * No external dependency: the whole point of this project is a small,
 * auditable C component.
 *
 * A test is declared with UT_TEST(suite, name) { ... }.  Tests register
 * themselves through a constructor, so no central list has to be maintained.
 */
#ifndef UTEST_H
#define UTEST_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef void (*ut_fn)(void);

void ut_register(const char *suite, const char *name, ut_fn fn);
void ut_fail(const char *file, int line, const char *what);

/* Per-test failure counter; the runner inspects it. */
extern int ut_current_failures;

#define UT_TEST(suite, name)                                                   \
    static void suite##_##name(void);                                          \
    __attribute__((constructor)) static void ut_reg_##suite##_##name(void)     \
    {                                                                          \
        ut_register(#suite, #name, suite##_##name);                            \
    }                                                                          \
    static void suite##_##name(void)

#define UT_FAIL(what) ut_fail(__FILE__, __LINE__, what)

#define UT_CHECK(cond)                                                         \
    do {                                                                       \
        if (!(cond)) {                                                         \
            ut_fail(__FILE__, __LINE__, #cond);                                \
        }                                                                      \
    } while (0)

#define UT_CHECK_MSG(cond, ...)                                                \
    do {                                                                       \
        if (!(cond)) {                                                         \
            char ut__buf[256];                                                 \
            snprintf(ut__buf, sizeof(ut__buf), __VA_ARGS__);                   \
            ut_fail(__FILE__, __LINE__, ut__buf);                              \
        }                                                                      \
    } while (0)

#define UT_EQ_INT(a, b)                                                        \
    do {                                                                       \
        long long ut__a = (long long)(a);                                      \
        long long ut__b = (long long)(b);                                      \
        if (ut__a != ut__b) {                                                  \
            char ut__buf[256];                                                 \
            snprintf(ut__buf, sizeof(ut__buf), "%s == %s (%lld != %lld)",      \
                     #a, #b, ut__a, ut__b);                                    \
            ut_fail(__FILE__, __LINE__, ut__buf);                              \
        }                                                                      \
    } while (0)

#define UT_EQ_U64(a, b)                                                        \
    do {                                                                       \
        unsigned long long ut__a = (unsigned long long)(a);                    \
        unsigned long long ut__b = (unsigned long long)(b);                    \
        if (ut__a != ut__b) {                                                  \
            char ut__buf[256];                                                 \
            snprintf(ut__buf, sizeof(ut__buf), "%s == %s (%llu != %llu)",      \
                     #a, #b, ut__a, ut__b);                                    \
            ut_fail(__FILE__, __LINE__, ut__buf);                              \
        }                                                                      \
    } while (0)

#define UT_EQ_STR(a, b)                                                        \
    do {                                                                       \
        const char *ut__a = (a);                                               \
        const char *ut__b = (b);                                               \
        if (ut__a == NULL || ut__b == NULL || strcmp(ut__a, ut__b) != 0) {     \
            char ut__buf[256];                                                 \
            snprintf(ut__buf, sizeof(ut__buf), "%s == %s (\"%s\" != \"%s\")",  \
                     #a, #b, ut__a ? ut__a : "(null)", ut__b ? ut__b : "(null)"); \
            ut_fail(__FILE__, __LINE__, ut__buf);                              \
        }                                                                      \
    } while (0)

#endif /* UTEST_H */
