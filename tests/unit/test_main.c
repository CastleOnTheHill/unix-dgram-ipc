#include "utest.h"

#include <stdlib.h>
#include <string.h>

#define UT_MAX 512

typedef struct {
    const char *suite;
    const char *name;
    ut_fn       fn;
} ut_case_t;

static ut_case_t g_cases[UT_MAX];
static int       g_count;

int ut_current_failures;

void ut_register(const char *suite, const char *name, ut_fn fn)
{
    if (g_count == UT_MAX) {
        fprintf(stderr, "utest: too many tests (max %d)\n", UT_MAX);
        exit(2);
    }
    g_cases[g_count].suite = suite;
    g_cases[g_count].name  = name;
    g_cases[g_count].fn    = fn;
    g_count++;
}

void ut_fail(const char *file, int line, const char *what)
{
    ut_current_failures++;
    fprintf(stdout, "    FAIL %s:%d: %s\n", file, line, what);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    const char *filter = (argc > 1) ? argv[1] : NULL;
    int         i;
    int         passed = 0;
    int         failed = 0;
    int         skipped = 0;

    for (i = 0; i < g_count; i++) {
        char label[160];

        snprintf(label, sizeof(label), "%s.%s", g_cases[i].suite,
                 g_cases[i].name);
        if (filter != NULL && strstr(label, filter) == NULL) {
            skipped++;
            continue;
        }
        ut_current_failures = 0;
        fprintf(stdout, "  [ run ] %s\n", label);
        fflush(stdout);
        g_cases[i].fn();
        if (ut_current_failures == 0) {
            passed++;
            fprintf(stdout, "    ok\n");
        } else {
            failed++;
        }
        fflush(stdout);
    }

    fprintf(stdout, "\n%d passed, %d failed, %d filtered out\n", passed, failed,
            skipped);
    return failed == 0 ? 0 : 1;
}
