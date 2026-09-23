/*
 * Copyright (c) 2026 <版权方待定>
 * 许可协议：<待定>
 *
 * ipc_io.c -- 端点的建立/拆除与统一发送出口。
 *
 * 本文件是整库跟内核打交道最密集的地方，也是历次踩坑最集中的地方。
 * 下面每一处「为什么这么做」的注释都不是装饰，改写前请先读完：
 *   - 权限/属主必须走**路径形式**设置（fchmod/fchown 对已 bind 的 socket fd
 *     是静默 no-op）；
 *   - bind 会应用进程 umask，所以要用 umask(0077) 包住它，之后**再显式设模式**；
 *   - 独占锁必须放在一个**单独的、永不被删的**锁文件上；
 *   - 所有发送都要 MSG_DONTWAIT。
 */
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stddef.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

#include "ipc_internal.h"

/*
 * 锁文件后缀。锁文件**故意永远不删**：
 *   flock 锁挂在 open file description 上。如果拿 socket 文件本身当锁对象，
 *   拆除时一 unlink，路径就指向新 inode 了 —— 下一个注册者建出来的新文件
 *   是一个全新的 inode，可以立刻加锁成功，于是「同一路径两个实例」这件事
 *   就再也挡不住了。锁对象必须活得比任何一个注册实例都久。
 */
#define IPC_LOCK_SUFFIX ".lock"

/*
 * 端点文件模式。属组可用时给组写权限（同组内可以互相投递），
 * 否则只留属主。注意 AF_UNIX 数据报的**发送方**需要对端点文件有写权限，
 * 所以「只读」在这里没有意义。
 */
#define IPC_MODE_PRIVATE 0600
#define IPC_MODE_GROUP   0660

/* ------------------------------------------------------------------ */
/* 复位辅助                                                           */
/* ------------------------------------------------------------------ */

/*
 * 把 sun_path 安全地填进 sockaddr_un。
 * 长度必须显式校验：sun_path 只有 108 字节，而配置里的路径上限是 107，
 * 中间只差一个 NUL —— 少一次判断就是一次栈溢出。
 */
static int32_t FillSockAddr(struct sockaddr_un *addr, socklen_t *outLen,
                            const char *path)
{
    size_t len;

    if (addr == NULL || outLen == NULL || path == NULL) {
        return IPC_ERR_INVAL;
    }
    len = strlen(path);
    if (len == 0 || len >= sizeof(addr->sun_path)) {
        return IPC_ERR_INVAL;
    }
    memset(addr, 0, sizeof(*addr));
    addr->sun_family = AF_UNIX;
    memcpy(addr->sun_path, path, len + 1);
    *outLen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + len + 1);
    return IPC_OK;
}

/* ------------------------------------------------------------------ */
/* 独占锁                                                             */
/* ------------------------------------------------------------------ */

static int32_t AcquireExclusiveLock(IpcContext *ctx)
{
    int32_t fd;

    fd = open(ctx->lockPath, O_RDWR | O_CREAT | O_CLOEXEC, IPC_MODE_PRIVATE);
    if (fd < 0) {
        IPC_LOGE(ctx, "cannot open lock file '%s': %s", ctx->lockPath,
                 IpcErrnoString(errno));
        return IpcErrnoToResult(errno);
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        int32_t err = errno;

        (void)close(fd);
        if (err == EWOULDBLOCK || err == EAGAIN) {
            /*
             * 注意这条的真话：这里只知道「锁被持有」，**不等于**「所有者还活着」。
             * flock 属于 open file description，进程死了只要还有继承来的 fd
             * 开着，锁就不放（CLOEXEC 只对 exec 有效，对 fork 无效）。
             * 所以这里宁可报 BUSY 让上层去查，也不做「猜它死了就抢锁」的动作。
             */
            IPC_LOGW(ctx, "endpoint '%s' is locked by another process", ctx->path);
            return IPC_ERR_BUSY;
        }
        IPC_LOGE(ctx, "flock '%s' failed: %s", ctx->lockPath, IpcErrnoString(err));
        return IpcErrnoToResult(err);
    }
    ctx->lockFd = fd;
    return IPC_OK;
}

static void ReleaseExclusiveLock(IpcContext *ctx)
{
    if (ctx->lockFd >= 0) {
        (void)close(ctx->lockFd); /* 关掉 fd 就等于放锁；锁文件本身不删 */
        ctx->lockFd = -1;
    }
}

/* ------------------------------------------------------------------ */
/* 残留处理                                                           */
/* ------------------------------------------------------------------ */

/*
 * 持锁状态下处理路径上的残留。
 *
 * 为什么可以放心删：能走到这里说明我们已经拿到了那个独占锁，而**任何一个
 * 活着的注册实例都必然持着同一把锁**。所以此刻路径上如果有东西，它一定是
 * 上一个实例崩溃后留下的残留，不是别人的活端点。
 *
 * 但仍然只删「确实是 socket」的东西：路径上如果是个普通文件或目录，
 * 那说明配置或部署有问题，此时删掉就是毁别人的数据。
 */
static int32_t ClearResidue(IpcContext *ctx)
{
    struct stat st;

    if (lstat(ctx->path, &st) != 0) {
        if (errno == ENOENT) {
            return IPC_OK; /* 干净的首次启动 */
        }
        IPC_LOGE(ctx, "lstat '%s' failed: %s", ctx->path, IpcErrnoString(errno));
        return IpcErrnoToResult(errno);
    }
    if (!S_ISSOCK(st.st_mode)) {
        IPC_LOGE(ctx, "refusing to remove '%s': it is not a socket (mode=0%o)",
                 ctx->path, (unsigned)(st.st_mode & 07777));
        return IPC_ERR_PERM;
    }
    if (unlink(ctx->path) != 0) {
        IPC_LOGE(ctx, "cannot remove stale socket '%s': %s", ctx->path,
                 IpcErrnoString(errno));
        return IpcErrnoToResult(errno);
    }
    IPC_LOGI(ctx, "removed stale endpoint '%s' left by a previous instance",
             ctx->path);
    return IPC_OK;
}

/* ------------------------------------------------------------------ */
/* 权限与属主                                                         */
/* ------------------------------------------------------------------ */

/*
 * 设置属组与模式，然后**回读校验**。
 *
 * 顺序不能反：chown 可能清掉权限位，所以先 chown 再 chmod。
 * 全部走路径形式，因为 fchmod/fchown 作用在已 bind 的 socket fd 上
 * 会被内核**静默忽略** —— 不报错，也不生效，只看返回值发现不了。
 *
 * 回读校验不是多余动作：它把「我以为设上了」变成「确认设上了」。
 * 少了这一步，一次被内核吞掉的 chmod 会一直潜伏到上线才暴露。
 */
static int32_t ApplyOwnership(IpcContext *ctx)
{
    struct stat st;
    mode_t      wantMode = ctx->haveGroup ? IPC_MODE_GROUP : IPC_MODE_PRIVATE;

    if (ctx->haveGroup) {
        if (chown(ctx->path, (uid_t)-1, ctx->groupGid) != 0) {
            IPC_LOGE(ctx, "chown '%s' to gid %u failed: %s", ctx->path,
                     (unsigned)ctx->groupGid, IpcErrnoString(errno));
            return IpcErrnoToResult(errno);
        }
    }
    if (chmod(ctx->path, wantMode) != 0) {
        IPC_LOGE(ctx, "chmod '%s' to 0%o failed: %s", ctx->path, (unsigned)wantMode,
                 IpcErrnoString(errno));
        return IpcErrnoToResult(errno);
    }

    if (lstat(ctx->path, &st) != 0) {
        IPC_LOGE(ctx, "verification lstat '%s' failed: %s", ctx->path,
                 IpcErrnoString(errno));
        return IpcErrnoToResult(errno);
    }
    if (!S_ISSOCK(st.st_mode)) {
        IPC_LOGE(ctx, "verification failed: '%s' is not a socket", ctx->path);
        return IPC_ERR_IO;
    }
    if ((st.st_mode & 07777) != (mode_t)wantMode) {
        IPC_LOGE(ctx, "verification failed: '%s' mode is 0%o, expected 0%o", ctx->path,
                 (unsigned)(st.st_mode & 07777), (unsigned)wantMode);
        return IPC_ERR_PERM;
    }
    if (st.st_uid != ctx->realUid) {
        IPC_LOGE(ctx, "verification failed: '%s' owner is %u, expected %u", ctx->path,
                 (unsigned)st.st_uid, (unsigned)ctx->realUid);
        return IPC_ERR_PERM;
    }
    if (ctx->haveGroup && st.st_gid != ctx->groupGid) {
        IPC_LOGE(ctx, "verification failed: '%s' group is %u, expected %u", ctx->path,
                 (unsigned)st.st_gid, (unsigned)ctx->groupGid);
        return IPC_ERR_PERM;
    }
    return IPC_OK;
}

/* ------------------------------------------------------------------ */
/* 建立 / 拆除                                                        */
/* ------------------------------------------------------------------ */

int32_t IpcIoCreateEndpoint(IpcContext *ctx)
{
    int32_t          rc;
    int32_t          fd;
    int32_t          on = 1;
    struct sockaddr_un addr;
    socklen_t        addrLen = 0;
    mode_t           oldUmask;

    if (ctx == NULL) {
        return IPC_ERR_INVAL;
    }

    /* 1) 锁文件路径。长度在配置解析阶段已经保证过，这里再挡一次。 */
    {
        size_t pathLen = strlen(ctx->path);

        if (pathLen + sizeof(IPC_LOCK_SUFFIX) > sizeof(ctx->lockPath)) {
            IPC_LOGE(ctx, "endpoint path too long for a lock file name");
            return IPC_ERR_INVAL;
        }
        (void)IpcStrlcpy(ctx->lockPath, ctx->path, sizeof(ctx->lockPath));
        (void)IpcStrlcpy(ctx->lockPath + pathLen, IPC_LOCK_SUFFIX,
                         sizeof(ctx->lockPath) - pathLen);
    }

    /* 2) 拿独占锁。拿不到就到此为止，绝不碰路径上的东西。 */
    rc = AcquireExclusiveLock(ctx);
    if (rc != IPC_OK) {
        return rc;
    }

    /* 3) 持锁清理残留。 */
    rc = ClearResidue(ctx);
    if (rc != IPC_OK) {
        goto Fail;
    }

    /* 4) 建 socket。显式设非阻塞 + CLOEXEC，不依赖 socket() 的扩展标志，
     *    这样在不支持 SOCK_CLOEXEC 的内核上也一样正确。 */
    fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) {
        IPC_LOGE(ctx, "socket(AF_UNIX, SOCK_DGRAM) failed: %s", IpcErrnoString(errno));
        rc = IpcErrnoToResult(errno);
        goto Fail;
    }
    ctx->fd = fd;

    if (fcntl(fd, F_SETFD, FD_CLOEXEC) != 0 ||
        fcntl(fd, F_SETFL, O_NONBLOCK) != 0) {
        IPC_LOGE(ctx, "fcntl on endpoint failed: %s", IpcErrnoString(errno));
        rc = IpcErrnoToResult(errno);
        goto Fail;
    }

    rc = FillSockAddr(&addr, &addrLen, ctx->path);
    if (rc != IPC_OK) {
        IPC_LOGE(ctx, "endpoint path is not usable: '%s'", ctx->path);
        goto Fail;
    }

    /*
     * 5) SO_PASSCRED 必须在**第一条报文到达之前**打开。
     *    打开之前收到的报文不带凭据，而本库对没有凭据的报文一律拒绝，
     *    所以顺序错了不会漏掉什么，只会让首个报文被丢弃 —— 但那是可避免的
     *    线上故障，顺序还是要对。
     */
    if (setsockopt(fd, SOL_SOCKET, SO_PASSCRED, &on, sizeof(on)) != 0) {
        IPC_LOGE(ctx, "SO_PASSCRED failed (needs a Linux socket): %s",
                 IpcErrnoString(errno));
        rc = IpcErrnoToResult(errno);
        goto Fail;
    }

    /* 6) 可选的缓冲区调优。注意：调 SO_RCVBUF 不会让接收队列变深。 */
    if (ctx->sndBufSize > 0 &&
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &ctx->sndBufSize,
                   sizeof(ctx->sndBufSize)) != 0) {
        IPC_LOGW(ctx, "SO_SNDBUF=%d ignored: %s", ctx->sndBufSize, IpcErrnoString(errno));
    }
    if (ctx->rcvBufSize > 0 &&
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &ctx->rcvBufSize,
                   sizeof(ctx->rcvBufSize)) != 0) {
        IPC_LOGW(ctx, "SO_RCVBUF=%d ignored: %s", ctx->rcvBufSize, IpcErrnoString(errno));
    }

    /*
     * 7) bind。用 umask(0077) 包住：bind 会给新文件套上进程 umask，
     *    在 umask 022 的环境里会短暂出现 0777&~022=0755 的窗口。
     *    这不能代替后面的显式 chmod，只是把窗口关掉。
     */
    oldUmask = umask(0077);
    if (bind(fd, (struct sockaddr *)&addr, addrLen) != 0) {
        int32_t err = errno;

        (void)umask(oldUmask);
        IPC_LOGE(ctx, "bind '%s' failed: %s", ctx->path, IpcErrnoString(err));
        rc = IpcErrnoToResult(err);
        goto Fail;
    }
    (void)umask(oldUmask);
    ctx->createdSocket = 1;

    /* 8) 属组与模式必须**路径形式**设置并回读校验。 */
    rc = ApplyOwnership(ctx);
    if (rc != IPC_OK) {
        goto Fail;
    }

    IpcLogIdentity(ctx, "endpoint ready");
    return IPC_OK;

Fail:
    IpcIoDestroyEndpoint(ctx);
    return rc;
}

void IpcIoDestroyEndpoint(IpcContext *ctx)
{
    if (ctx == NULL) {
        return;
    }
    if (ctx->fd >= 0) {
        (void)close(ctx->fd);
        ctx->fd = -1;
    }
    /*
     * unlink 必须在**持锁状态下**做：否则可能出现「我们判断这个路径属于自己，
     * 但在 unlink 之前锁就放了、另一个实例抢先 bind」这种把别人的活端点删掉的
     * 情况。顺序：先删（还持锁）→ 再放锁。
     */
    if (ctx->createdSocket) {
        if (unlink(ctx->path) != 0 && errno != ENOENT) {
            IPC_LOGW(ctx, "cannot remove endpoint '%s': %s", ctx->path,
                     IpcErrnoString(errno));
        }
        ctx->createdSocket = 0;
    }
    ReleaseExclusiveLock(ctx);
}

/* ------------------------------------------------------------------ */
/* 发送                                                               */
/* ------------------------------------------------------------------ */

void IpcIoFillHeader(const IpcContext *ctx, IpcProtoHeader *header, uint8_t type,
                     const char *dstModuleId, uint32_t event, uint32_t payloadLen,
                     uint64_t reqId)
{
    if (ctx == NULL || header == NULL) {
        return;
    }
    memset(header, 0, sizeof(*header));
    header->version    = (uint8_t)IPC_PROTOCOL_VERSION;
    header->type       = type;
    header->flags      = 0;
    (void)IpcStrlcpy(header->ns, ctx->ns, sizeof(header->ns));
    (void)IpcStrlcpy(header->src, ctx->moduleId, sizeof(header->src));
    (void)IpcStrlcpy(header->dst, dstModuleId, sizeof(header->dst));
    header->event      = event;
    header->payloadLen = payloadLen;
    header->reqId      = reqId;
    header->instanceId = ctx->instanceId;
}

int32_t IpcIoSendTo(IpcContext *ctx, const char *dstPath,
                    const IpcProtoHeader *header, const void *payload, size_t len)
{
    uint8_t            headerBytes[IPC_HDR_SIZE];
    struct sockaddr_un addr;
    struct iovec       iov[2];
    struct msghdr      msg;
    socklen_t          addrLen = 0;
    ssize_t            sent;
    size_t             want;
    int32_t            rc;
    size_t             iovCount;

    if (ctx == NULL || dstPath == NULL || header == NULL) {
        return IPC_ERR_INVAL;
    }
    if (len > 0 && payload == NULL) {
        return IPC_ERR_INVAL;
    }
    if (len > (size_t)ctx->maxPayload) {
        return IPC_ERR_MSGSIZE; /* 在发送动作之前就拒绝 */
    }
    if (IpcProtoEncode(header, headerBytes, sizeof(headerBytes)) != (size_t)IPC_HDR_SIZE) {
        return IPC_ERR_PROTO;
    }
    rc = FillSockAddr(&addr, &addrLen, dstPath);
    if (rc != IPC_OK) {
        IPC_LOGE(ctx, "target path '%s' is not usable", dstPath);
        return rc;
    }

    iovCount   = (len > 0) ? 2u : 1u;
    iov[0].iov_base = headerBytes;
    iov[0].iov_len  = (size_t)IPC_HDR_SIZE;
    iov[1].iov_base = (void *)(uintptr_t)payload;
    iov[1].iov_len  = len;

    memset(&msg, 0, sizeof(msg));
    msg.msg_name    = &addr;
    msg.msg_namelen = addrLen;
    msg.msg_iov     = iov;
    msg.msg_iovlen  = iovCount;

    /*
     * MSG_DONTWAIT：绝不阻塞。阻塞版 sendto 在队列满时会**睡下去**，
     * 而 IpcPost 的契约是不阻塞。
     * MSG_NOSIGNAL：对端消失时不要收 SIGPIPE 把宿主进程打死
     * （对 DGRAM 本来也不会，但代价是零，留着更稳）。
     */
    sent = sendmsg(ctx->fd, &msg, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (sent < 0) {
        int32_t err = errno;

        rc = IpcErrnoToResult(err);
        if (rc == IPC_ERR_AGAIN) {
            IPC_STAT_INC(ctx, eagainCount);
            IPC_LOGW(ctx, "peer queue full, '%s' not enqueued", dstPath);
            return rc;
        }
        if (rc == IPC_ERR_NOENT || rc == IPC_ERR_OFFLINE) {
            /* ENOENT：路径没了（从未注册或已被删除）
             * ECONNREFUSED：路径还在但没人在上面 bind
             * 两者对调用方是同一件事：这个模块当前没有活端点。
             * 这也是 Q12 要求的那条 warning。 */
            IPC_LOGW(ctx, "module is offline: '%s' has no live endpoint", dstPath);
            return IPC_ERR_OFFLINE;
        }
        IPC_LOGE(ctx, "sendmsg to '%s' failed: %s", dstPath, IpcErrnoString(err));
        return rc;
    }

    want = (size_t)IPC_HDR_SIZE + ((iovCount == 2u) ? len : 0u);
    if ((size_t)sent != want) {
        /*
         * 数据报是全有或全无。出现短写说明这个路径上的东西不是我们在等的那种
         * socket（比如被换成了流式 socket），必须报错而不是当作成功。
         */
        IPC_LOGE(ctx, "short sendmsg to '%s': %zd of %zu bytes", dstPath, sent, want);
        return IPC_ERR_IO;
    }
    return IPC_OK;
}
