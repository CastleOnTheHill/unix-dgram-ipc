#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "ipc_internal.h"

__thread ipc_ctx_t *ipc__tls_handler_ctx;

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static int split_path(const char *path, char *dir, size_t dcap,
                      const char **name)
{
    const char *s;
    size_t      n;

    if (path == NULL || path[0] != '/') {
        return IPC_ERR_INVAL;
    }
    s = strrchr(path, '/');
    if (s == NULL || s == path) {
        if (strcmp(path, "/") == 0) {
            return IPC_ERR_INVAL;
        }
        n = 1;
    } else {
        n = (size_t)(s - path);
    }
    if (n >= dcap) {
        return IPC_ERR_INVAL;
    }
    memcpy(dir, path, n);
    dir[n] = '\0';
    *name  = s + 1;
    if (**name == '\0') {
        return IPC_ERR_INVAL;
    }
    return IPC_OK;
}

/* Validate the directory that holds our socket:
 *  - exists, is a directory
 *  - owned by our UID
 *  - not writable by group or others (otherwise a hostile process in the
 *    shared group could swap our socket path for a symlink between bind()
 *    and chmod(), which we cannot close from the fd because sockfs ignores
 *    fchmod/fchown -- see probes/PROBE_NOTES.md)
 *  - searchable by us */
static int check_own_dir(const char *dir)
{
    struct stat st;

    if (lstat(dir, &st) != 0) {
        IPC_LOGE("socket directory %s: %s", dir, strerror(errno));
        return errno == ENOENT ? IPC_ERR_NOENT : ipc_errno_to_rc(errno);
    }
    if (!S_ISDIR(st.st_mode)) {
        IPC_LOGE("%s is not a directory", dir);
        return IPC_ERR_PERM;
    }
    if (st.st_uid != geteuid()) {
        IPC_LOGE("socket directory %s owned by uid %u, not ours (%u)", dir,
                 (unsigned)st.st_uid, (unsigned)geteuid());
        return IPC_ERR_PERM;
    }
    if (st.st_mode & (S_IWGRP | S_IWOTH)) {
        IPC_LOGE("socket directory %s is group/other writable (mode %04o)", dir,
                 (unsigned)(st.st_mode & 07777));
        return IPC_ERR_PERM;
    }
    if ((st.st_mode & S_IXUSR) == 0) {
        IPC_LOGE("socket directory %s is not searchable by its owner", dir);
        return IPC_ERR_PERM;
    }
    return IPC_OK;
}

/* Resolve the shared group to give the socket inode, and verify that we are
 * really a member of it.
 *
 * Rationale (handoff 5.2): holding a supplementary group does not by itself
 * decide the group of a newly created file, so the group has to be applied
 * explicitly.  Doing it through the fd would be nicer but sockfs ignores
 * fchown(), so the path form is used. */
static int resolve_group(const char *name, gid_t *out)
{
    struct group *gr;
    gid_t         groups[64];
    int           n, i;

    if (name == NULL) {
        *out = (gid_t)-1; /* leave the process default */
        return IPC_OK;
    }
    errno = 0;
    gr    = getgrnam(name);
    if (gr == NULL) {
        IPC_LOGE("group '%s' not found", name);
        return IPC_ERR_PERM;
    }
    *out = gr->gr_gid;
    if (getegid() == *out) {
        return IPC_OK;
    }
    n = getgroups((int)(sizeof(groups) / sizeof(groups[0])), groups);
    if (n < 0) {
        IPC_LOGE("getgroups: %s", strerror(errno));
        return ipc_errno_to_rc(errno);
    }
    for (i = 0; i < n; i++) {
        if (groups[i] == *out) {
            return IPC_OK;
        }
    }
    IPC_LOGE("process is not a member of group '%s' (gid %u); cannot chgrp the "
             "socket", name, (unsigned)*out);
    return IPC_ERR_PERM;
}

/* Validate the configuration file itself.
 *
 * The config is the trust root: it decides which UID may occupy which socket
 * path.  A service that can write it can register itself as any module it
 * likes, so a group- or world-writable config (or one owned by an unrelated
 * UID) is refused at registration.
 *
 * This is a different object from the one check_own_dir() guards: that one
 * covers the socket *directory*.  Both are needed.
 *
 * The stat() and the later read are not one atomic operation; the ownership
 * and mode check still closes the realistic case (a config the service itself
 * can modify), and the remaining window requires an attacker who can already
 * replace files in the config's directory. */
static int check_config_file(const char *path)
{
    struct stat st;
    uid_t       me = geteuid();

    if (stat(path, &st) != 0) {
        IPC_LOGE("config %s: %s", path, strerror(errno));
        return errno == ENOENT ? IPC_ERR_NOENT : ipc_errno_to_rc(errno);
    }
    if (!S_ISREG(st.st_mode)) {
        IPC_LOGE("refusing config %s: not a regular file", path);
        return IPC_ERR_PERM;
    }
    if (st.st_mode & (S_IWGRP | S_IWOTH)) {
        IPC_LOGE("refusing config %s: mode %04o is writable by group or others",
                 path, (unsigned)(st.st_mode & 07777));
        return IPC_ERR_PERM;
    }
    if (st.st_uid != 0 && st.st_uid != me) {
        IPC_LOGE("refusing config %s: owned by uid %u, which is neither root "
                 "nor ours (%u)", path, (unsigned)st.st_uid, (unsigned)me);
        return IPC_ERR_PERM;
    }
    return IPC_OK;
}

/* ------------------------------------------------------------------ */
/* misc accessors                                                      */
/* ------------------------------------------------------------------ */

int ipc__check_alive(ipc_ctx_t *ctx)
{
    if (ctx == NULL) {
        return IPC_ERR_INVAL;
    }
    if (atomic_load(&ctx->teardown)) {
        return IPC_ERR_STOPPED;
    }
    if (ctx->fd < 0) {
        return IPC_ERR_STATE;
    }
    if (getpid() != ctx->owner_pid) {
        /* The context was inherited across fork().  The child must not use it:
         * it would share the pending table and the socket with the parent.
         * CLOEXEC only covers exec(), not fork(). */
        return IPC_ERR_STATE;
    }
    return IPC_OK;
}

const char *ipc__peer_path(ipc_ctx_t *ctx, const char *dst, uid_t *uid_out)
{
    const ipc_config_entry_t *e;

    e = ipc_config_lookup(ctx->cfg, ctx->ns, dst);
    if (e == NULL) {
        return NULL;
    }
    if (uid_out != NULL) {
        *uid_out = e->uid;
    }
    return e->path;
}

const char *ipc_module_id(const ipc_ctx_t *ctx)
{
    return ctx ? ctx->module : NULL;
}

const char *ipc_namespace(const ipc_ctx_t *ctx)
{
    return ctx ? ctx->ns : NULL;
}

const char *ipc_socket_path(const ipc_ctx_t *ctx)
{
    return ctx ? ctx->path : NULL;
}

int ipc_socket_fd(const ipc_ctx_t *ctx)
{
    return ctx ? ctx->fd : -1;
}

uint64_t ipc_instance_id(const ipc_ctx_t *ctx)
{
    return ctx ? ctx->instance_id : 0;
}

int ipc_set_handler(ipc_ctx_t *ctx, ipc_handler_fn fn, void *user)
{
    int rc;

    if (ctx == NULL) {
        return IPC_ERR_INVAL;
    }
    rc = ipc__check_alive(ctx);
    if (rc != IPC_OK) {
        return rc;
    }
    if (atomic_load(&ctx->loop_thread_started)) {
        return IPC_ERR_STATE; /* do not swap a handler under the loop */
    }
    ctx->handler      = fn;
    ctx->handler_user = user;
    return IPC_OK;
}

void ipc_get_stats(const ipc_ctx_t *ctx, ipc_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (ctx == NULL) {
        return;
    }
#define IPC_X(f) out->f = atomic_load_explicit(&ctx->st.f, memory_order_relaxed);
    IPC_STAT_FIELDS(IPC_X)
#undef IPC_X
}

/* ------------------------------------------------------------------ */
/* registration                                                        */
/* ------------------------------------------------------------------ */

static void ctx_release_resources(ipc_ctx_t *ctx, int unlink_path)
{
    if (ctx->epoll_fd >= 0) {
        close(ctx->epoll_fd);
        ctx->epoll_fd = -1;
    }
    if (ctx->stop_evfd >= 0) {
        close(ctx->stop_evfd);
        ctx->stop_evfd = -1;
    }
    if (ctx->fd >= 0) {
        close(ctx->fd);
        ctx->fd = -1;
    }
    if (unlink_path && ctx->path[0] != '\0') {
        if (unlink(ctx->path) != 0 && errno != ENOENT) {
            IPC_LOGW("could not unlink %s: %s", ctx->path, strerror(errno));
        }
    }
    if (ctx->lock_fd >= 0) {
        /* Closing releases the flocks.  The lock file itself is left in place
         * on purpose: removing it would let a second process create a second,
         * independent lock on the same name. */
        close(ctx->lock_fd);
        ctx->lock_fd = -1;
    }
}

/* Handle a path that already exists at bind time.
 *
 * We hold the exclusive lock, so no *live* registrant can exist; whatever we
 * find is residue left by a process that was killed.  Anything that is not a
 * plain socket owned by us is refused: a regular file, a directory or a
 * symlink at our socket path means something we do not understand is going
 * on, and an unconditional unlink would be a foot-gun. */
static int handle_residue(ipc_ctx_t *ctx)
{
    struct stat st;

    if (lstat(ctx->path, &st) != 0) {
        if (errno == ENOENT) {
            return IPC_OK;
        }
        IPC_LOGE("lstat %s: %s", ctx->path, strerror(errno));
        return ipc_errno_to_rc(errno);
    }
    if (!S_ISSOCK(st.st_mode)) {
        IPC_LOGE("refusing to remove %s: not a socket (mode %06o)", ctx->path,
                 (unsigned)(st.st_mode & 07777));
        return IPC_ERR_PERM;
    }
    if (st.st_uid != geteuid()) {
        IPC_LOGE("refusing to remove %s: owned by uid %u, not ours (%u)",
                 ctx->path, (unsigned)st.st_uid, (unsigned)geteuid());
        return IPC_ERR_PERM;
    }
    IPC_LOGW("removing stale socket residue at %s", ctx->path);
    if (unlink(ctx->path) != 0) {
        IPC_LOGE("unlink %s: %s", ctx->path, strerror(errno));
        return ipc_errno_to_rc(errno);
    }
    return IPC_OK;
}

int ipc_register(const ipc_register_opts_t *opts, ipc_ctx_t **out)
{
    ipc_ctx_t                *ctx = NULL;
    const ipc_config_entry_t *ent = NULL;
    const char               *conf_path;
    char                      dir[IPC_SUN_PATH_MAX];
    const char               *name = NULL;
    struct sockaddr_un        sun;
    gid_t                     gid = (gid_t)-1;
    mode_t                    old_umask;
    int                       rc;
    int                       i;
    int                       matches = 0;

    if (out == NULL || opts == NULL || opts->module == NULL ||
        opts->module[0] == '\0') {
        return IPC_ERR_INVAL;
    }
    *out = NULL;
    if (strlen(opts->module) >= IPC_NAME_MAX) {
        return IPC_ERR_INVAL;
    }

    ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return IPC_ERR_NOMEM;
    }
    ctx->fd        = -1;
    ctx->lock_fd   = -1;
    ctx->epoll_fd  = -1;
    ctx->stop_evfd = -1;
    ctx->owner_pid = getpid();
    ipc_strlcpy(ctx->module, opts->module, sizeof(ctx->module));

    /* ---- options, defaults and clamping --------------------------- */
    ctx->opt                     = *opts;
    ctx->opt.dispatch            = (opts->dispatch == IPC_DISPATCH_POOL)
                                       ? IPC_DISPATCH_POOL
                                       : IPC_DISPATCH_INLINE;
    ctx->opt.workers             = opts->workers > 0 ? opts->workers : 2;
    if (ctx->opt.workers > 8) {
        ctx->opt.workers = 8;
    }
    ctx->opt.cb_queue_max        = opts->cb_queue_max > 0 ? opts->cb_queue_max
                                                         : 64;
    ctx->opt.max_pending         = opts->max_pending > 0 ? opts->max_pending
                                                        : 64;
    ctx->max_payload             = opts->max_payload ? opts->max_payload
                                                     : IPC_PAYLOAD_DEFAULT;
    if (ctx->max_payload > IPC_PAYLOAD_HARD_MAX) {
        ctx->max_payload = IPC_PAYLOAD_HARD_MAX;
    }
    ctx->opt.max_payload         = ctx->max_payload;
    ctx->opt.broadcast_include_self = opts->broadcast_include_self ? 1 : 0;
    ctx->opt.allow_uid_split     = opts->allow_uid_split ? 1 : 0;

    if (pthread_mutex_init(&ctx->life_lock, NULL) != 0 ||
        pthread_cond_init(&ctx->life_cv, NULL) != 0) {
        rc = IPC_ERR_IO;
        goto fail;
    }

    /* ---- static module table -------------------------------------- */
    conf_path = ctx->opt.conf_path ? ctx->opt.conf_path : IPC_CONF_DEFAULT;
    rc        = check_config_file(conf_path);
    if (rc != IPC_OK) {
        goto fail;
    }
    rc        = ipc_config_load(conf_path, &ctx->cfg);
    if (rc != IPC_OK) {
        goto fail;
    }
    for (i = 0; i < ipc_config_count(ctx->cfg); i++) {
        const ipc_config_entry_t *e = ipc_config_at(ctx->cfg, i);
        if (strcmp(e->module, ctx->module) != 0) {
            continue;
        }
        if (ctx->opt.ns != NULL && strcmp(e->ns, ctx->opt.ns) != 0) {
            continue;
        }
        ent = e;
        matches++;
    }
    if (matches == 0) {
        IPC_LOGE("module '%s' is not in %s", ctx->module, conf_path);
        rc = IPC_ERR_NOENT;
        goto fail;
    }
    if (matches > 1) {
        IPC_LOGE("module id '%s' occurs in %d namespaces of %s; pass opts.ns",
                 ctx->module, matches, conf_path);
        rc = IPC_ERR_CONFIG;
        goto fail;
    }
    ipc_strlcpy(ctx->ns, ent->ns, sizeof(ctx->ns));
    ipc_strlcpy(ctx->path, ent->path, sizeof(ctx->path));
    ctx->uid = ent->uid;
    snprintf(ctx->lock_path, sizeof(ctx->lock_path), "%s.lock", ctx->path);

    /* ---- identity -------------------------------------------------- */
    /* Default policy: real UID, effective UID and the configured UID must all
     * agree.  Allowing them to differ would make the credentials peers see
     * (SCM_CREDENTIALS, see probes/PROBE_NOTES.md) differ from the identity
     * checked here.  allow_uid_split=1 relaxes this to the effective UID. */
    if (!ctx->opt.allow_uid_split) {
        if (getuid() != ent->uid || geteuid() != ent->uid) {
            IPC_LOGE("uid mismatch for module %s: config=%u real=%u effective=%u",
                     ctx->module, (unsigned)ent->uid, (unsigned)getuid(),
                     (unsigned)geteuid());
            rc = IPC_ERR_PERM;
            goto fail;
        }
    } else if (geteuid() != ent->uid) {
        IPC_LOGE("effective uid %u cannot register module %s (config uid=%u)",
                 (unsigned)geteuid(), ctx->module, (unsigned)ent->uid);
        rc = IPC_ERR_PERM;
        goto fail;
    }

    /* ---- directory, group ----------------------------------------- */
    rc = split_path(ctx->path, dir, sizeof(dir), &name);
    if (rc != IPC_OK) {
        IPC_LOGE("bad socket path in config: %s", ctx->path);
        goto fail;
    }
    (void)name;
    rc = check_own_dir(dir);
    if (rc != IPC_OK) {
        goto fail;
    }
    rc = resolve_group(ctx->opt.group, &gid);
    if (rc != IPC_OK) {
        goto fail;
    }

    /* ---- lifetime lock -------------------------------------------- */
    ctx->lock_fd = open(ctx->lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (ctx->lock_fd < 0) {
        IPC_LOGE("cannot open lock file %s: %s", ctx->lock_path,
                 strerror(errno));
        rc = ipc_errno_to_rc(errno);
        goto fail;
    }
    if (flock(ctx->lock_fd, LOCK_EX | LOCK_NB) != 0) {
        IPC_LOGE("module %s is already registered (lock %s held): %s",
                 ctx->module, ctx->lock_path, strerror(errno));
        rc = (errno == EWOULDBLOCK) ? IPC_ERR_BUSY : ipc_errno_to_rc(errno);
        goto fail;
    }

    /* ---- residue, must happen while the lock is held ---------------- */
    rc = handle_residue(ctx);
    if (rc != IPC_OK) {
        goto fail;
    }

    /* ---- socket ---------------------------------------------------- */
    ctx->fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (ctx->fd < 0) {
        IPC_LOGE("socket(AF_UNIX, SOCK_DGRAM): %s", strerror(errno));
        rc = ipc_errno_to_rc(errno);
        goto fail;
    }
    {
        int one = 1;
        if (setsockopt(ctx->fd, SOL_SOCKET, SO_PASSCRED, &one, sizeof(one)) !=
            0) {
            IPC_LOGE("SO_PASSCRED: %s", strerror(errno));
            rc = ipc_errno_to_rc(errno);
            goto fail;
        }
    }
    if (ctx->opt.sndbuf > 0 &&
        setsockopt(ctx->fd, SOL_SOCKET, SO_SNDBUF, &ctx->opt.sndbuf,
                   sizeof(ctx->opt.sndbuf)) != 0) {
        IPC_LOGW("SO_SNDBUF=%d: %s", ctx->opt.sndbuf, strerror(errno));
    }
    if (ctx->opt.rcvbuf > 0) {
        int eff = 0;
        socklen_t el = sizeof(eff);
        if (setsockopt(ctx->fd, SOL_SOCKET, SO_RCVBUF, &ctx->opt.rcvbuf,
                       sizeof(ctx->opt.rcvbuf)) != 0) {
            IPC_LOGW("SO_RCVBUF=%d: %s", ctx->opt.rcvbuf, strerror(errno));
        } else if (getsockopt(ctx->fd, SOL_SOCKET, SO_RCVBUF, &eff, &el) == 0) {
            IPC_LOGW("SO_RCVBUF=%d requested (kernel reports %d) -- measured "
                     "behaviour: on AF_UNIX datagram sockets this does NOT set "
                     "the receive-queue depth, see probes/PROBE_NOTES.md",
                     ctx->opt.rcvbuf, eff);
        }
    }

    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    memcpy(sun.sun_path, ctx->path, strlen(ctx->path) + 1);
    /* The socket is created by bind() with (0777 & ~umask).  Tighten the umask
     * so it is never group/other writable, not even for the microseconds
     * between bind() and chmod().  sockfs ignores fchmod()/fchown(), so the
     * path forms below are the only ones that actually take effect. */
    old_umask = umask(0077);
    rc = bind(ctx->fd, (struct sockaddr *)&sun,
              (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
                          strlen(ctx->path) + 1));
    umask(old_umask);
    if (rc != 0) {
        IPC_LOGE("bind %s: %s", ctx->path, strerror(errno));
        rc = ipc_errno_to_rc(errno);
        goto fail;
    }
    ctx->bounds_created = 1;

    if (gid != (gid_t)-1 && chown(ctx->path, (uid_t)-1, gid) != 0) {
        IPC_LOGE("chown %s to gid %u: %s", ctx->path, (unsigned)gid,
                 strerror(errno));
        rc = ipc_errno_to_rc(errno);
        goto fail;
    }
    if (chmod(ctx->path, 0620) != 0) {
        IPC_LOGE("chmod %s: %s", ctx->path, strerror(errno));
        rc = ipc_errno_to_rc(errno);
        goto fail;
    }
    {
        /* Verify what actually landed on the filesystem.  Cheap, and it turns
         * a silent permission surprise into a loud failure. */
        struct stat st;
        if (lstat(ctx->path, &st) != 0) {
            rc = ipc_errno_to_rc(errno);
            goto fail;
        }
        if (!S_ISSOCK(st.st_mode) || (st.st_mode & 07777) != 0620 ||
            st.st_uid != geteuid() ||
            (gid != (gid_t)-1 && st.st_gid != gid)) {
            IPC_LOGE("socket %s has unexpected attributes: mode %04o uid %u "
                     "gid %u (wanted socket 0620 uid %u gid %d)", ctx->path,
                     (unsigned)(st.st_mode & 07777), (unsigned)st.st_uid,
                     (unsigned)st.st_gid, (unsigned)geteuid(), (int)gid);
            rc = IPC_ERR_PERM;
            goto fail;
        }
    }

    /* ---- receive plumbing ------------------------------------------ */
    ctx->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (ctx->epoll_fd < 0) {
        rc = ipc_errno_to_rc(errno);
        goto fail;
    }
    ctx->stop_evfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (ctx->stop_evfd < 0) {
        rc = ipc_errno_to_rc(errno);
        goto fail;
    }
    {
        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.events  = EPOLLIN;
        ev.data.fd = ctx->fd;
        if (epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, ctx->fd, &ev) != 0) {
            rc = ipc_errno_to_rc(errno);
            goto fail;
        }
        ev.events  = EPOLLIN;
        ev.data.fd = ctx->stop_evfd;
        if (epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, ctx->stop_evfd, &ev) != 0) {
            rc = ipc_errno_to_rc(errno);
            goto fail;
        }
    }

    rc = ipc_pending_init(&ctx->pend, ctx->opt.max_pending);
    if (rc != IPC_OK) {
        goto fail;
    }
    rc = ipc_queue_init(&ctx->queue, ctx->opt.cb_queue_max);
    if (rc != IPC_OK) {
        goto fail;
    }
    if (ctx->opt.dispatch == IPC_DISPATCH_POOL) {
        rc = ipc_loop_start_workers(ctx);
        if (rc != IPC_OK) {
            goto fail;
        }
    }

    ctx->instance_id = ipc_gen_instance_id();
    IPC_LOGI("module %s/%s registered on %s (uid=%u instance=0x%016llx "
             "dispatch=%s payload_max=%u)",
             ctx->ns, ctx->module, ctx->path, (unsigned)geteuid(),
             (unsigned long long)ctx->instance_id,
             ctx->opt.dispatch == IPC_DISPATCH_POOL ? "pool" : "inline",
             ctx->max_payload);
    *out = ctx;
    return IPC_OK;

fail:
    if (ctx != NULL) {
        ipc_loop_shutdown_workers(ctx);
        ctx_release_resources(ctx, ctx->bounds_created);
        if (ctx->pend.slots != NULL) {
            ipc_pending_destroy(&ctx->pend);
        }
        if (ctx->queue.items != NULL) {
            ipc_queue_destroy(&ctx->queue);
        }
        pthread_cond_destroy(&ctx->life_cv);
        pthread_mutex_destroy(&ctx->life_lock);
        if (ctx->cfg != NULL) {
            ipc_config_free(ctx->cfg);
        }
        free(ctx);
    }
    return rc;
}

int ipc_unregister(ipc_ctx_t *ctx)
{
    int rc;

    if (ctx == NULL) {
        return IPC_ERR_INVAL;
    }
    if (ctx->fd >= 0 && getpid() != ctx->owner_pid) {
        /* A forked child must not tear the parent's context down.  Checked
         * before anything is claimed so it cannot leave the context looking
         * half-torn-down. */
        return IPC_ERR_STATE;
    }

    /* Idempotent *and* safe to call concurrently.
     *
     * Exactly one caller performs the teardown; every other caller waits until
     * it has finished and then returns IPC_OK.  Waiting rather than returning
     * early is what makes the documented meaning of this function -- "on
     * return, the resources are released" -- true for the second caller too.
     *
     * The context is NOT freed here.  Freeing it would turn a second call
     * (or any accessor: ipc_get_stats, ipc_module_id, ...) into a
     * use-after-free, and the teardown flag cannot protect an object that no
     * longer exists.  The allocation is released by ipc_ctx_free(). */
    pthread_mutex_lock(&ctx->life_lock);
    if (atomic_load(&ctx->teardown)) {
        while (!atomic_load(&ctx->teardown_done)) {
            pthread_cond_wait(&ctx->life_cv, &ctx->life_lock);
        }
        pthread_mutex_unlock(&ctx->life_lock);
        return IPC_OK;
    }
    atomic_store(&ctx->teardown, 1);
    pthread_mutex_unlock(&ctx->life_lock);

    /* 1. stop taking part in the protocol */
    ipc_stop(ctx);

    /* 2. let the loop thread notice and finish */
    pthread_mutex_lock(&ctx->life_lock);
    while (atomic_load(&ctx->loop_thread_started)) {
        pthread_cond_wait(&ctx->life_cv, &ctx->life_lock);
    }
    pthread_mutex_unlock(&ctx->life_lock);
    if (ctx->have_loop_thread) {
        pthread_join(ctx->loop_thread, NULL);
        ctx->have_loop_thread = 0;
    }

    /* 3. drain the callback queue, then stop the workers */
    {
        ipc_task_t t;
        while (ipc_queue_pop(&ctx->queue, &t, 0) == IPC_OK) {
            ipc_task_clear(&t);
        }
    }
    ipc_loop_shutdown_workers(ctx);

    /* 4. resolve outstanding synchronous requests (waiters wake with
     *    IPC_ERR_STOPPED) */
    ipc_pending_shutdown(&ctx->pend);

    /* 5. close the socket, then clean the path while still holding the lock */
    rc = IPC_OK;
    ctx_release_resources(ctx, ctx->bounds_created);

    /* 6. the lock is now released; destroy the rest of the payload */
    ipc_pending_destroy(&ctx->pend);
    ipc_queue_destroy(&ctx->queue);
    if (ctx->cfg != NULL) {
        ipc_config_free(ctx->cfg);
        ctx->cfg = NULL;
    }
    IPC_LOGI("module %s/%s unregistered", ctx->ns, ctx->module);

    /* Publish "teardown complete" while the condition variable still exists,
     * so a concurrent second caller can return.  The mutex and the condition
     * variable outlive this call on purpose: destroying them here would race
     * with a waiter that has been woken but has not re-acquired the mutex yet.
     * ipc_ctx_free() destroys them once nobody can be waiting any more. */
    pthread_mutex_lock(&ctx->life_lock);
    atomic_store(&ctx->teardown_done, 1);
    pthread_cond_broadcast(&ctx->life_cv);
    pthread_mutex_unlock(&ctx->life_lock);
    return rc;
}

int ipc_ctx_free(ipc_ctx_t *ctx)
{
    if (ctx == NULL) {
        return IPC_ERR_INVAL;
    }
    if (!atomic_load(&ctx->teardown_done)) {
        IPC_LOGE("ipc_ctx_free() before ipc_unregister() completed; refusing to "
                 "free a live context");
        return IPC_ERR_STATE;
    }
    pthread_cond_destroy(&ctx->life_cv);
    pthread_mutex_destroy(&ctx->life_lock);
    free(ctx);
    return IPC_OK;
}
