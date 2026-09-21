/*
 * Unit tests for the small shared helpers: errno mapping, bounded string
 * copy, the instance-id generator and the bounded file reader.
 *
 * The errno mapping table has a single purpose: making sure "peer does not
 * exist" is never reported as a generic I/O error, because broadcast and
 * post treat offline peers specially.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ipc/ipc.h"
#include "ipc_util.h"
#include "utest.h"

UT_TEST(util, errno_mapping_is_specific)
{
    UT_EQ_INT(ipc_errno_to_rc(0), IPC_OK);
    UT_EQ_INT(ipc_errno_to_rc(EAGAIN), IPC_ERR_AGAIN);
    UT_EQ_INT(ipc_errno_to_rc(ENOBUFS), IPC_ERR_AGAIN);
    UT_EQ_INT(ipc_errno_to_rc(EINTR), IPC_ERR_AGAIN);
    UT_EQ_INT(ipc_errno_to_rc(EACCES), IPC_ERR_PERM);
    UT_EQ_INT(ipc_errno_to_rc(EPERM), IPC_ERR_PERM);
    UT_EQ_INT(ipc_errno_to_rc(ENOENT), IPC_ERR_NOENT);
    UT_EQ_INT(ipc_errno_to_rc(ECONNREFUSED), IPC_ERR_OFFLINE);
    UT_EQ_INT(ipc_errno_to_rc(ECONNRESET), IPC_ERR_OFFLINE);
    UT_EQ_INT(ipc_errno_to_rc(EMSGSIZE), IPC_ERR_MSGSIZE);
    UT_EQ_INT(ipc_errno_to_rc(ENOMEM), IPC_ERR_NOMEM);
    UT_EQ_INT(ipc_errno_to_rc(EMFILE), IPC_ERR_NOMEM);
    UT_EQ_INT(ipc_errno_to_rc(EINVAL), IPC_ERR_INVAL);
    UT_EQ_INT(ipc_errno_to_rc(EBUSY), IPC_ERR_BUSY);
    UT_EQ_INT(ipc_errno_to_rc(EEXIST), IPC_ERR_BUSY);
    /* Unknown errno must still not look like success. */
    UT_CHECK(ipc_errno_to_rc(0x7fff) < 0);
}

UT_TEST(util, strerror_text_covers_every_code)
{
    static const int codes[] = {
        IPC_OK,        IPC_ERR_INVAL,    IPC_ERR_NOMEM,    IPC_ERR_IO,
        IPC_ERR_AGAIN, IPC_ERR_NOENT,    IPC_ERR_OFFLINE,  IPC_ERR_PERM,
        IPC_ERR_BUSY,  IPC_ERR_CRED,     IPC_ERR_PROTO,    IPC_ERR_TIMEOUT,
        IPC_ERR_STOPPED, IPC_ERR_DEADLOCK, IPC_ERR_MSGSIZE, IPC_ERR_CONFIG,
        IPC_ERR_TOOMANY, IPC_ERR_STATE
    };
    size_t i;

    for (i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
        const char *s = ipc_strerror(codes[i]);
        UT_CHECK(s != NULL);
        UT_CHECK(strlen(s) > 0);
        UT_CHECK(strcmp(s, "unknown error") != 0);
    }
    UT_EQ_STR(ipc_strerror(-9999), "unknown error");
}

UT_TEST(util, strlcpy_always_terminates)
{
    char b[8];

    UT_EQ_U64(ipc_strlcpy(b, "abc", sizeof(b)), 3);
    UT_EQ_STR(b, "abc");
    UT_EQ_U64(ipc_strlcpy(b, "0123456789", sizeof(b)), 10); /* source length */
    UT_EQ_STR(b, "0123456");
    UT_EQ_U64(ipc_strlcpy(b, "", sizeof(b)), 0);
    UT_EQ_STR(b, "");
    UT_EQ_U64(ipc_strlcpy(b, "ignored", 0), 7); /* cap 0: no write at all */
    ipc_strlcpy(NULL, "x", 4);                  /* must not crash */
    UT_EQ_STR(b, "");
}

UT_TEST(util, mix64_is_deterministic_and_avalanches)
{
    uint64_t a = ipc_mix64(1);
    uint64_t b = ipc_mix64(1);
    uint64_t c = ipc_mix64(2);

    UT_EQ_U64(a, b);
    UT_CHECK(a != c);
    /* A one-bit input change should flip many output bits. */
    {
        uint64_t x = ipc_mix64(0x0000000000000000ull);
        uint64_t y = ipc_mix64(0x0000000000000001ull);
        int      diff = 0, k;
        for (k = 0; k < 64; k++) {
            if (((x >> k) & 1) != ((y >> k) & 1)) {
                diff++;
            }
        }
        UT_CHECK_MSG(diff > 16, "only %d bits changed", diff);
    }
}

UT_TEST(util, instance_ids_are_unique_and_never_zero)
{
    uint64_t seen[256];
    int      i, j;

    for (i = 0; i < 256; i++) {
        seen[i] = ipc_gen_instance_id();
        UT_CHECK(seen[i] != 0);
    }
    for (i = 0; i < 256; i++) {
        for (j = i + 1; j < 256; j++) {
            UT_CHECK_MSG(seen[i] != seen[j], "duplicate instance id %llu",
                         (unsigned long long)seen[i]);
        }
    }
}

UT_TEST(util, monotonic_clock_moves_forward)
{
    uint64_t a = ipc_mono_ns();
    /* unsigned on purpose: the accumulator overflows by construction and
     * signed overflow would be UB (UBSan flags it), while the wrap-around of
     * an unsigned type is perfectly defined.  The loop only has to burn time. */
    volatile unsigned spin = 0;
    uint64_t          b;

    for (unsigned i = 0; i < 100000u; i++) {
        spin += i;
    }
    (void)spin;
    b = ipc_mono_ns();
    UT_CHECK(b >= a);
    UT_CHECK(ipc_real_ns() > 1600000000000000000ull); /* after 2020 */
}

UT_TEST(util, read_file_handles_the_normal_cases)
{
    char  path[] = "/tmp/ipc_util_test_XXXXXX";
    int   fd;
    char *text;
    int   err = 0;
    char  chunk[512];

    fd = mkstemp(path);
    UT_CHECK(fd >= 0);
    UT_EQ_INT(write(fd, "hello\n", 6), 6);
    close(fd);

    text = ipc_read_file(path, 1024, &err);
    UT_CHECK(text != NULL);
    UT_EQ_INT(err, IPC_OK);
    UT_EQ_STR(text, "hello\n");
    free(text);

    /* Grow past the initial hint to exercise the realloc path. */
    fd = open(path, O_WRONLY | O_TRUNC);
    UT_CHECK(fd >= 0);
    memset(chunk, 'y', sizeof(chunk));
    for (int i = 0; i < 8; i++) {
        UT_EQ_INT(write(fd, chunk, sizeof(chunk)), (int)sizeof(chunk));
    }
    close(fd);
    text = ipc_read_file(path, 16, &err);
    UT_CHECK(text != NULL);
    UT_EQ_U64(strlen(text), 8 * sizeof(chunk));
    free(text);

    unlink(path);

    text = ipc_read_file(path, 1024, &err);
    UT_CHECK(text == NULL);
    UT_EQ_INT(err, IPC_ERR_NOENT);

    text = ipc_read_file(NULL, 16, &err);
    UT_CHECK(text == NULL);
    UT_EQ_INT(err, IPC_ERR_INVAL);
}
