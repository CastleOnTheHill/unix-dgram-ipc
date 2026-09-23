/*
 * ref_inherit.c -- UNIX socket and flock reference-inheritance semantics.
 *
 * handoff.md 6.3 requires explicit handling of reference inheritance through
 * fork()/dup(): "CLOEXEC cannot solve fork inheritance; a child holding a
 * reference can extend the life of the socket and the lock".
 *
 * This probe measures exactly what that means, so the library contract rests
 * on observed behaviour instead of a guess.  Each step prints one line; the
 * integration suite greps the conclusions.
 *
 * Run: ./build/bin/ref_inherit
 */
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static char g_dir[128];

static int bind_at(const char *path)
{
    int                fd;
    struct sockaddr_un sun;

    unlink(path);
    fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        perror("socket");
        exit(1);
    }
    if (strlen(path) >= sizeof(sun.sun_path)) {
        fprintf(stderr, "socket path too long: %s\n", path);
        exit(1);
    }
    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    memcpy(sun.sun_path, path, strlen(path) + 1);
    if (bind(fd, (struct sockaddr *)&sun,
             (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
                         strlen(path) + 1)) != 0) {
        perror("bind");
        exit(1);
    }
    chmod(path, 0666);
    return fd;
}

/* Returns 0 on success, -errno on failure. */
static int send_one(const char *dst, const char *what)
{
    int                fd;
    struct sockaddr_un sun;
    ssize_t            n;

    fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -errno;
    }
    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    memcpy(sun.sun_path, dst, strlen(dst) + 1);
    n = sendto(fd, what, strlen(what), 0, (struct sockaddr *)&sun,
               (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
                           strlen(dst) + 1));
    close(fd);
    return n < 0 ? -errno : 0;
}

/* ------------------------------------------------------------------ */
/* 1. socket inode outliving the path and the parent's reference       */
/* ------------------------------------------------------------------ */

static void test_socket_reference(void)
{
    char  sock[160];
    char  other[160];
    int   parent_fd;
    int   hold[2];
    pid_t child;

    snprintf(sock, sizeof(sock), "%s/mod.sock", g_dir);
    snprintf(other, sizeof(other), "%s/other.sock", g_dir);

    printf("== 1. socket inode vs. path vs. inherited reference\n");
    parent_fd = bind_at(sock);
    if (pipe(hold) != 0) {
        perror("pipe");
        return;
    }
    child = fork();
    if (child == 0) {
        char            c;
        struct sockaddr_un sun;
        socklen_t          l  = sizeof(sun);
        int                rc;
        int                ok;

        close(hold[1]);
        if (read(hold[0], &c, 1) != 1) {
            _exit(1);
        }
        /* The parent has closed its own fd and unlinked the path by now. */
        rc = send_one(sock, "child");
        printf("CHILD send_to_removed_path rc=%d errno=%d\n", rc, rc < 0 ? -rc : 0);
        ok = (getsockname(parent_fd, (struct sockaddr *)&sun, &l) == 0);
        printf("CHILD inherited_fd_still_valid=%d\n", ok);
        close(hold[0]);
        _exit(0);
    }
    close(hold[0]);
    close(parent_fd);
    if (unlink(sock) != 0) {
        printf("PARENT unlink_errno=%d\n", errno);
    }
    {
        int rc = send_one(sock, "peer");
        printf("PARENT send_after_unlink rc=%d errno=%d\n", rc, rc < 0 ? -rc : 0);
    }
    {
        char    c = 'x';
        ssize_t w = write(hold[1], &c, 1);
        (void)w;
    }
    close(hold[1]);
    waitpid(child, NULL, 0);

    /* A fresh bind at the same path is a brand new inode. */
    {
        int     fd2 = bind_at(sock);
        char    buf[64];
        ssize_t n;
        int     rc = send_one(sock, "peer2");

        n = recv(fd2, buf, sizeof(buf), MSG_DONTWAIT);
        printf("PARENT new_bind send_rc=%d received=%zd (expect 0 and 5)\n", rc,
               n);
        close(fd2);
        unlink(sock);
    }
    unlink(other);
}

/* ------------------------------------------------------------------ */
/* 2. flock outliving the process that acquired it                     */
/* ------------------------------------------------------------------ */

static void test_flock_reference(void)
{
    char  lockfile[160];
    char  result[192];
    int   lock_fd;
    pid_t locker;

    snprintf(lockfile, sizeof(lockfile), "%s/mod.lock", g_dir);
    snprintf(result, sizeof(result), "%s/flock_result.txt", g_dir);
    unlink(result);

    printf("\n== 2. flock survives the owner's exit while an inherited fd is "
           "open\n");

    /* The locker process acquires the lock and then disappears.  Its child
     * inherits the descriptor and reports into a file, because an orphan's
     * stdout is not reliably captured by whoever ran the probe. */
    locker = fork();
    if (locker == 0) {
        pid_t me     = getpid();
        pid_t mine;

        lock_fd = open(lockfile, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (lock_fd < 0) {
            _exit(2);
        }
        if (flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
            _exit(3);
        }
        printf("PARENT lock_acquired=1 pid=%ld\n", (long)me);
        fflush(stdout);

        mine = fork();
        if (mine == 0) {
            FILE *f = fopen(result, "a");
            int   i, probe, rc;

            /* Wait until the process holding the lock is gone. */
            for (i = 0; i < 500 && getppid() == me; i++) {
                usleep(10000);
            }
            if (f == NULL) {
                _exit(4);
            }
            fprintf(f, "CHILD locker_gone=%d\n", getppid() != me);

            probe = open(lockfile, O_RDWR | O_CLOEXEC);
            rc    = (probe >= 0) ? flock(probe, LOCK_EX | LOCK_NB) : -1;
            fprintf(f,
                    "CHILD lock_while_inherited_fd_open rc=%d errno=%d "
                    "(EWOULDBLOCK=%d)\n",
                    rc, rc == 0 ? 0 : errno, EWOULDBLOCK);
            if (probe >= 0) {
                close(probe);
            }

            /* Drop the last reference: only now is the lock actually free. */
            close(lock_fd);
            probe = open(lockfile, O_RDWR | O_CLOEXEC);
            rc    = (probe >= 0) ? flock(probe, LOCK_EX | LOCK_NB) : -1;
            fprintf(f, "CHILD lock_after_last_reference_closed rc=%d (expect 0)\n",
                    rc);
            if (probe >= 0) {
                close(probe);
            }
            fclose(f);
            _exit(0);
        }
        fflush(stdout);
        _exit(0); /* vanish without ever releasing the lock */
    }

    {
        int i;
        waitpid(locker, NULL, 0);
        /* Poll for the two expected lines instead of sleeping a fixed time. */
        for (i = 0; i < 500; i++) {
            FILE *f = fopen(result, "r");
            int   lines = 0;

            if (f != NULL) {
                char buf[256];
                while (fgets(buf, sizeof(buf), f) != NULL) {
                    lines++;
                }
                fclose(f);
            }
            if (lines >= 3) {
                break;
            }
            usleep(10000);
        }
        {
            FILE *f = fopen(result, "r");
            char  buf[256];

            if (f != NULL) {
                while (fgets(buf, sizeof(buf), f) != NULL) {
                    fputs(buf, stdout);
                }
                fclose(f);
            } else {
                printf("RESULT flock_result_unavailable\n");
            }
        }
        fflush(stdout);
    }
}

/* ------------------------------------------------------------------ */
/* cleanup                                                             */
/* ------------------------------------------------------------------ */

/* The probe used to leave /tmp/ipc_ref_inherit_<pid>/ behind on every run --
 * harmless because the name carries the pid, but it means t13 (which runs this
 * as root) quietly accumulates scratch trees on the test host.  Nothing here
 * is shared, so removing the whole tree is safe. */
static void cleanup_dir(void)
{
    static const char *const names[] = { "mod.sock", "other.sock", "mod.lock",
                                         "flock_result.txt" };
    char                      p[sizeof(g_dir) + 32];
    size_t                    i;
    int                       failed = 0;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (snprintf(p, sizeof(p), "%s/%s", g_dir, names[i]) >= (int)sizeof(p)) {
            continue;
        }
        if (unlink(p) != 0 && errno != ENOENT) {
            failed = 1;
        }
    }
    if (rmdir(g_dir) != 0) {
        failed = 1;
    }
    if (failed) {
        printf("RESULT cleanup_incomplete dir=%s errno=%d\n", g_dir, errno);
    } else {
        printf("RESULT cleanup_ok dir=%s\n", g_dir);
    }
}

int main(void)
{
    /* Children terminate with _exit(), which does not flush stdio.  Line
     * buffering keeps every observation visible. */
    setvbuf(stdout, NULL, _IOLBF, 0);
    snprintf(g_dir, sizeof(g_dir), "/tmp/ipc_ref_inherit_%ld", (long)getpid());
    if (mkdir(g_dir, 0700) != 0 && errno != EEXIST) {
        perror("mkdir");
        return 1;
    }

    /* Section 2 forks a process that forks again and then vanishes, so the
     * grandchild is reparented.  Without this it is adopted by init and can
     * outlive the probe while still holding mod.lock; with it, the grandchild
     * lands back on us and we can actually wait for it.  Not fatal if the
     * kernel refuses: the probe still measures what it measures, it just may
     * leave an orphan (and says so). */
    if (prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) != 0) {
        printf("NOTE subreaper_unavailable errno=%d (an orphan may survive)\n",
               errno);
    }

    test_socket_reference();
    test_flock_reference();

    /* Reap anything that was reparented to us, bounded.  The grandchild writes
     * its last line and then exits, so this normally returns immediately. */
    {
        int i;
        for (i = 0; i < 500; i++) {
            pid_t w = waitpid(-1, NULL, WNOHANG);
            if (w == -1) {
                break; /* ECHILD: nothing left to wait for */
            }
            if (w > 0) {
                continue;
            }
            usleep(10000);
        }
    }

    cleanup_dir();
    return 0;
}
