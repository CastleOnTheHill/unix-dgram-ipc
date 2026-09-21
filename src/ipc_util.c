#include "ipc_util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static ipc_log_level_t g_level = IPC_LOG_WARN;
static int             g_level_init;

const char *ipc_strerror(int rc)
{
    switch (rc) {
    case IPC_OK:           return "ok";
    case IPC_ERR_INVAL:    return "invalid argument";
    case IPC_ERR_NOMEM:    return "out of memory";
    case IPC_ERR_IO:       return "I/O error";
    case IPC_ERR_AGAIN:    return "peer queue full (would block)";
    case IPC_ERR_NOENT:    return "unknown module";
    case IPC_ERR_OFFLINE:  return "module offline (no live socket)";
    case IPC_ERR_PERM:     return "permission denied";
    case IPC_ERR_BUSY:     return "already registered";
    case IPC_ERR_CRED:     return "bad or missing sender credentials";
    case IPC_ERR_PROTO:    return "malformed message";
    case IPC_ERR_TIMEOUT:  return "timed out";
    case IPC_ERR_STOPPED:  return "stopped";
    case IPC_ERR_DEADLOCK: return "synchronous send from an inline handler";
    case IPC_ERR_MSGSIZE:  return "message too large";
    case IPC_ERR_CONFIG:   return "configuration error";
    case IPC_ERR_TOOMANY:  return "too many concurrent requests";
    case IPC_ERR_STATE:    return "wrong lifecycle state";
    default:               return "unknown error";
    }
}

static void level_init(void)
{
    const char *e;

    if (g_level_init) {
        return;
    }
    g_level_init = 1;
    e = getenv("IPC_LOG_LEVEL");
    if (e == NULL) {
        return;
    }
    if (strcmp(e, "error") == 0) {
        g_level = IPC_LOG_ERROR;
    } else if (strcmp(e, "warn") == 0) {
        g_level = IPC_LOG_WARN;
    } else if (strcmp(e, "info") == 0) {
        g_level = IPC_LOG_INFO;
    } else if (strcmp(e, "debug") == 0) {
        g_level = IPC_LOG_DEBUG;
    }
}

void ipc_log_set_level(ipc_log_level_t lvl)
{
    level_init();
    g_level     = lvl;
    g_level_init = 1;
}

ipc_log_level_t ipc_log_get_level(void)
{
    level_init();
    return g_level;
}

void ipc_logf(ipc_log_level_t lvl, const char *fmt, ...)
{
    static const char *tag[] = { "ERROR", "WARN ", "INFO ", "DEBUG" };
    va_list ap;
    char buf[512];

    level_init();
    if ((int)lvl > (int)g_level) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fprintf(stderr, "[ipc %s] pid=%ld %s\n", tag[(int)lvl], (long)getpid(), buf);
    fflush(stderr);
}

int ipc_errno_to_rc(int err)
{
    switch (err) {
    case 0:
        return IPC_OK;
    case EAGAIN:
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK:
#endif
    case ENOBUFS:
    case EINTR:
        return IPC_ERR_AGAIN;
    case EACCES:
    case EPERM:
    case EROFS:
        return IPC_ERR_PERM;
    case ENOENT:
        return IPC_ERR_NOENT;
    case ECONNREFUSED:
    case ECONNRESET:
    case ENOTCONN:
        return IPC_ERR_OFFLINE;
    case EMSGSIZE:
    case E2BIG:
        return IPC_ERR_MSGSIZE;
    case ENOMEM:
    case ENFILE:
    case EMFILE:
        return IPC_ERR_NOMEM;
    case EINVAL:
    case EFAULT:
    case EAFNOSUPPORT:
    case EPROTOTYPE:
        return IPC_ERR_INVAL;
    case EBUSY:
    case EEXIST:
        return IPC_ERR_BUSY;
    default:
        return IPC_ERR_IO;
    }
}

size_t ipc_strlcpy(char *dst, const char *src, size_t cap)
{
    size_t len, n;

    if (src == NULL) {
        if (dst != NULL && cap > 0) {
            dst[0] = '\0';
        }
        return 0;
    }
    len = strlen(src);
    if (dst == NULL || cap == 0) {
        return len;
    }
    n = (len < cap - 1) ? len : cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
    return len; /* strlcpy semantics: always the full source length */
}

uint64_t ipc_mono_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint64_t ipc_real_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint64_t ipc_mix64(uint64_t x)
{
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

static uint64_t boot_hash(void)
{
    static uint64_t cached;
    int             err = IPC_OK;
    char           *b;

    if (cached != 0) {
        return cached;
    }
    b = ipc_read_file("/proc/sys/kernel/random/boot_id", 256, &err);
    if (b == NULL) {
        cached = 0x1234567890abcdefull;
        return cached;
    }
    {
        size_t i;
        uint64_t h = 0xcbf29ce484222325ull;
        for (i = 0; b[i] != '\0'; i++) {
            h ^= (uint64_t)(unsigned char)b[i];
            h *= 0x100000001b3ull;
        }
        cached = h ? h : 1;
    }
    free(b);
    return cached;
}

uint64_t ipc_gen_instance_id(void)
{
    uint64_t v = ipc_mix64(boot_hash() ^ ipc_mono_ns());
    v ^= ipc_mix64((uint64_t)getpid() + 0x51ed270b5f3d1f01ull);
    return v ? v : 1;
}

char *ipc_read_file(const char *path, size_t max_bytes, int *out_err)
{
    int    fd;
    char  *buf;
    size_t cap = max_bytes ? max_bytes : 65536;
    size_t len = 0;

    if (out_err != NULL) {
        *out_err = IPC_OK;
    }
    if (path == NULL) {
        if (out_err) *out_err = IPC_ERR_INVAL;
        return NULL;
    }
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (out_err) *out_err = ipc_errno_to_rc(errno);
        return NULL;
    }
    buf = malloc(cap + 1);
    if (buf == NULL) {
        close(fd);
        if (out_err) *out_err = IPC_ERR_NOMEM;
        return NULL;
    }
    for (;;) {
        ssize_t n;
        if (len == cap) {
            char *nb = realloc(buf, cap * 2 + 1);
            if (nb == NULL) {
                free(buf);
                close(fd);
                if (out_err) *out_err = IPC_ERR_NOMEM;
                return NULL;
            }
            buf = nb;
            cap *= 2;
        }
        n = read(fd, buf + len, cap - len);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            free(buf);
            close(fd);
            if (out_err) *out_err = ipc_errno_to_rc(errno);
            return NULL;
        }
        if (n == 0) {
            break;
        }
        len += (size_t)n;
    }
    close(fd);
    buf[len] = '\0';
    return buf;
}
