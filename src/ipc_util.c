/*
 * Copyright (c) 2026 <版权方待定>
 * 许可协议：<待定>
 *
 * ipc_util.c -- 库内部通用小工具的实现。
 *
 * 本文件不依赖上下文，也不分配除 IpcReadFile 之外的任何内存，
 * 因此它的每个函数都可以被白盒单测直接覆盖。
 */
#include "ipc_util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ipc/ipc.h"

/* ------------------------------------------------------------------ */
/* 字符串                                                             */
/* ------------------------------------------------------------------ */

size_t IpcStrlcpy(char *dst, const char *src, size_t cap)
{
    size_t len;
    size_t n;

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
    n = (len < (cap - 1)) ? len : (cap - 1);
    if (n > 0) {
        memcpy(dst, src, n);
    }
    dst[n] = '\0';
    return len;
}

void IpcPayloadToCStr(const void *data, size_t len, char *out, size_t cap)
{
    const unsigned char *p = (const unsigned char *)data;
    size_t               i;
    size_t               n;

    if (out == NULL || cap == 0) {
        return;
    }
    if (p == NULL || len == 0) {
        out[0] = '\0';
        return;
    }
    n = (len < (cap - 1)) ? len : (cap - 1);
    for (i = 0; i < n; i++) {
        out[i] = (p[i] >= 0x20 && p[i] < 0x7f) ? (char)p[i] : '.';
    }
    out[n] = '\0';
}

/* ------------------------------------------------------------------ */
/* 错误码                                                             */
/* ------------------------------------------------------------------ */

int32_t IpcErrnoToResult(int32_t err)
{
    switch (err) {
        case 0:
            return IPC_OK;
        case EAGAIN:
#if defined(EWOULDBLOCK) && (EWOULDBLOCK != EAGAIN)
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

const char *IpcErrnoString(int32_t err)
{
    const char *s = strerror(err);

    return (s != NULL) ? s : "unknown";
}

/*
 * 公开接口：返回码转可读字符串。
 * 契约（写死在 ipc.h 里）：未知值返回 "unknown"，**绝不返回 NULL**。
 * 所以下面这个 default 分支不是「不该发生」，而是接口承诺的一部分。
 */
const char *IpcResultToString(int32_t result)
{
    switch (result) {
        case IPC_OK:
            return "ok";
        case IPC_ERR_INVAL:
            return "invalid argument";
        case IPC_ERR_NOMEM:
            return "out of memory";
        case IPC_ERR_IO:
            return "I/O error";
        case IPC_ERR_AGAIN:
            return "peer queue full (not enqueued)";
        case IPC_ERR_NOENT:
            return "no such module in config";
        case IPC_ERR_OFFLINE:
            return "module has no live endpoint";
        case IPC_ERR_PERM:
            return "permission denied";
        case IPC_ERR_BUSY:
            return "module already registered";
        case IPC_ERR_CRED:
            return "bad or missing sender credentials";
        case IPC_ERR_PROTO:
            return "malformed message";
        case IPC_ERR_TIMEOUT:
            return "timed out waiting for reply";
        case IPC_ERR_STOPPED:
            return "context stopped";
        case IPC_ERR_DEADLOCK:
            return "synchronous send from a dispatch callback";
        case IPC_ERR_MSGSIZE:
            return "payload exceeds maxPayload";
        case IPC_ERR_CONFIG:
            return "configuration rejected";
        case IPC_ERR_TOOMANY:
            return "no free pending slot";
        case IPC_ERR_STATE:
            return "wrong lifecycle state";
        default:
            return "unknown";
    }
}

/* ------------------------------------------------------------------ */
/* 时间与实例代际号                                                    */
/* ------------------------------------------------------------------ */

uint64_t IpcMonoNs(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return ((uint64_t)ts.tv_sec * 1000000000ull) + (uint64_t)ts.tv_nsec;
}

uint64_t IpcMix64(uint64_t x)
{
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

/*
 * boot_id 的哈希，进程内缓存一次。
 * 缓存是刻意为之：这个文件每次注册都读一遍没有意义，而 boot_id 在一次
 * 开机期间不会变。加锁的代价不需要付 —— 最坏情况是两个线程各读一次，
 * 得到相同的结果，没有危害。
 */
static uint64_t BootHash(void)
{
    static uint64_t cached;
    int32_t         err = IPC_OK;
    char           *text;
    uint64_t        hash = 0xcbf29ce484222325ull;
    size_t          i;

    if (cached != 0) {
        return cached;
    }
    text = IpcReadFile("/proc/sys/kernel/random/boot_id", 256, &err);
    if (text == NULL) {
        /* 读不到就用一个固定值：代际号仍然有单调时钟和 pid 兜底。 */
        cached = 0x1234567890abcdefull;
        return cached;
    }
    for (i = 0; text[i] != '\0'; i++) {
        hash ^= (uint64_t)(unsigned char)text[i];
        hash *= 0x100000001b3ull;
    }
    free(text);
    cached = (hash != 0) ? hash : 1;
    return cached;
}

uint64_t IpcGenInstanceId(void)
{
    uint64_t v;

    v = IpcMix64(BootHash() ^ IpcMonoNs());
    v ^= IpcMix64((uint64_t)getpid() + 0x51ed270b5f3d1f01ull);
    return (v != 0) ? v : 1; /* 0 保留给「未初始化」 */
}

/* ------------------------------------------------------------------ */
/* 文件                                                               */
/* ------------------------------------------------------------------ */

char *IpcReadFile(const char *path, size_t maxBytes, int32_t *outErr)
{
    int     fd;
    char   *buf;
    size_t  cap;
    size_t  len = 0;

    if (outErr != NULL) {
        *outErr = IPC_OK;
    }
    if (path == NULL) {
        if (outErr != NULL) {
            *outErr = IPC_ERR_INVAL;
        }
        return NULL;
    }
    if (maxBytes == 0) {
        if (outErr != NULL) {
            *outErr = IPC_ERR_INVAL;
        }
        return NULL;
    }
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (outErr != NULL) {
            *outErr = IpcErrnoToResult(errno);
        }
        return NULL;
    }
    cap = (maxBytes < 65536u) ? maxBytes : 65536u;
    buf = (char *)malloc(cap + 1);
    if (buf == NULL) {
        (void)close(fd);
        if (outErr != NULL) {
            *outErr = IPC_ERR_NOMEM;
        }
        return NULL;
    }
    for (;;) {
        ssize_t n;

        if (len == cap) {
            char  *grown;
            size_t newCap;

            if (cap >= maxBytes) {
                /*
                 * 已经用满允许的上限。**必须再探一个字节**才能判「超限」：
                 * 直接报 MSGSIZE 会把「文件恰好等于上限」误判成超限 ——
                 * 白盒单测正好钉着这条边界（读 16 字节失败、读正好等于
                 * 文件大小必须成功）。
                 */
                char    probe;
                ssize_t extra = read(fd, &probe, 1);

                if (extra < 0 && errno == EINTR) {
                    continue;
                }
                if (extra < 0) {
                    free(buf);
                    (void)close(fd);
                    if (outErr != NULL) {
                        *outErr = IpcErrnoToResult(errno);
                    }
                    return NULL;
                }
                if (extra == 0) {
                    break; /* 正好等于上限：合法，按成功返回 */
                }
                free(buf);
                (void)close(fd);
                if (outErr != NULL) {
                    *outErr = IPC_ERR_MSGSIZE; /* 文件比允许的上限还大 */
                }
                return NULL;
            }
            newCap = cap * 2; /* 指数增长，但不超过调用方给的上限 */
            if (newCap > maxBytes) {
                newCap = maxBytes;
            }
            grown = (char *)realloc(buf, newCap + 1);
            if (grown == NULL) {
                free(buf);
                (void)close(fd);
                if (outErr != NULL) {
                    *outErr = IPC_ERR_NOMEM;
                }
                return NULL;
            }
            buf = grown;
            cap = newCap;
        }
        n = read(fd, buf + len, cap - len);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            free(buf);
            (void)close(fd);
            if (outErr != NULL) {
                *outErr = IpcErrnoToResult(errno);
            }
            return NULL;
        }
        if (n == 0) {
            break;
        }
        len += (size_t)n;
    }
    (void)close(fd);
    buf[len] = '\0';
    return buf;
}
