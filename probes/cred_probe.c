/*
 * cred_probe.c -- what exactly does SCM_CREDENTIALS carry?
 *
 * handoff.md item 8 asks whether the old framework's UID check used the real
 * or the effective UID.  That cannot be answered from the old source (not
 * available), but the *kernel* side can: SCM_CREDENTIALS is filled by the
 * kernel, not by the sender, and either it reports the sender's real UID or
 * its effective UID.  This probe measures it.
 *
 * Method: fork a child, setresuid(real=R, effective=E, saved=E) with R != E,
 * have it send one datagram, and print what the receiver sees.  Requires
 * CAP_SETUID, so run it as root.
 *
 * Part 2 answers the other half of the question with a measurement instead of
 * an assertion: can an *unprivileged* sender attach its own SCM_CREDENTIALS and
 * claim somebody else's UID?  A child drops to a single unprivileged UID and
 * sendmsg()s with SCM_CREDENTIALS naming uid 0; the probe reports what the
 * kernel did with it (reject, override, or -- the interesting case -- honour).
 *
 * Run: sudo ./build/bin/cred_probe [R E]
 */
#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define CTRL_SIZE CMSG_SPACE(sizeof(struct ucred))

/* ------------------------------------------------------------------ */
/* Part 2: can an unprivileged sender attach its own SCM_CREDENTIALS?  */
/* ------------------------------------------------------------------ */

static int make_bound(const char *path, int server);

/*
 * Part 1 shows which UID the kernel *fills in*.  It does not by itself show
 * that the field cannot be *supplied* by the sender -- and the identity check
 * in libipc is only as strong as that second fact.  So it is measured here
 * rather than asserted: a child drops to an unprivileged UID and sendmsg()s
 * with an explicit SCM_CREDENTIALS naming uid 0.
 *
 * Three outcomes are possible and the probe names whichever happened:
 *   - sendmsg() fails EPERM        -> kernel refused the forged credential
 *   - sendmsg() succeeds, the receiver sees the real uid -> kernel overrode it
 *   - sendmsg() succeeds, the receiver sees uid 0       -> the lie was honoured
 *
 * The third would invalidate the whole identity model, so it is reported
 * loudly instead of being folded into a pass/fail.
 */
static int probe_forged_cred(uid_t low)
{
    char  spath[128];
    char  cpath[128];
    int   sfd = -1;
    int   cfd = -1;
    pid_t pid = -1;
    int   rc  = 1;

    snprintf(spath, sizeof(spath), "/tmp/ipc_cred_forge_s_%ld", (long)getpid());
    snprintf(cpath, sizeof(cpath), "/tmp/ipc_cred_forge_c_%ld", (long)getpid());

    sfd = make_bound(spath, 1);
    cfd = make_bound(cpath, 0);
    {
        struct timeval tv;
        tv.tv_sec  = 10;
        tv.tv_usec = 0;
        if (setsockopt(sfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
            perror("SO_RCVTIMEO");
            goto out;
        }
    }

    pid = fork();
    if (pid < 0) {
        perror("fork");
        goto out;
    }
    if (pid == 0) {
        char            ctrl[CMSG_SPACE(sizeof(struct ucred))];
        struct msghdr   mh;
        struct iovec    iov;
        struct cmsghdr *c;
        struct ucred    claim;
        struct sockaddr_un sun;
        char            byte = 'y';
        size_t          plen = strlen(spath);

        /* Become unprivileged before making the claim, otherwise the test
         * would only show that root may do what root may do. */
        if (geteuid() == 0) {
            if (setresuid(low, low, low) != 0) {
                fprintf(stderr, "child setresuid(%u) failed: %s\n",
                        (unsigned)low, strerror(errno));
                _exit(3);
            }
        }
        printf("FORGE sender uid=%u euid=%u claiming uid=0\n", (unsigned)getuid(),
               (unsigned)geteuid());

        claim.pid = (pid_t)getpid();
        claim.uid = 0; /* the lie */
        claim.gid = 0;

        memset(&sun, 0, sizeof(sun));
        sun.sun_family = AF_UNIX;
        memcpy(sun.sun_path, spath, plen + 1);

        iov.iov_base = &byte;
        iov.iov_len  = 1;
        memset(&mh, 0, sizeof(mh));
        mh.msg_name       = &sun;
        mh.msg_namelen    = (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
                                        plen + 1);
        mh.msg_iov        = &iov;
        mh.msg_iovlen     = 1;
        mh.msg_control    = ctrl;
        mh.msg_controllen = sizeof(ctrl);
        c                 = CMSG_FIRSTHDR(&mh);
        c->cmsg_level     = SOL_SOCKET;
        c->cmsg_type      = SCM_CREDENTIALS;
        c->cmsg_len       = CMSG_LEN(sizeof(claim));
        memcpy(CMSG_DATA(c), &claim, sizeof(claim));

        if (sendmsg(cfd, &mh, 0) < 0) {
            printf("FORGED_SEND_REJECTED errno=%d (%s)\n", errno,
                   strerror(errno));
            printf("CONCLUSION kernel refused the forged credential\n");
            _exit(0);
        }
        _exit(0);
    }

    {
        char            data[64];
        char            ctrl[CTRL_SIZE];
        struct iovec    iov;
        struct msghdr   mh;
        struct cmsghdr *cmsg;
        ssize_t         n;
        int             status = 0;

        iov.iov_base = data;
        iov.iov_len  = sizeof(data);
        memset(&mh, 0, sizeof(mh));
        mh.msg_iov        = &iov;
        mh.msg_iovlen     = 1;
        mh.msg_control    = ctrl;
        mh.msg_controllen = sizeof(ctrl);

        n = recvmsg(sfd, &mh, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                waitpid(pid, &status, 0);
                pid = -1;
                /* Exit 10 is the child's "the kernel rejected my sendmsg()"
                 * signal: no datagram was queued, which is the good outcome. */
                if (WIFEXITED(status) && WEXITSTATUS(status) == 10) {
                    printf("CONCLUSION forged credential refused, no datagram "
                           "queued\n");
                    rc = 0;
                } else {
                    printf("RESULT forged_datagram_not_received (child exit=%d)\n",
                           WIFEXITED(status) ? WEXITSTATUS(status) : -1);
                }
            } else {
                perror("recvmsg");
            }
            goto out;
        }
        waitpid(pid, &status, 0);
        pid = -1;

        cmsg = CMSG_FIRSTHDR(&mh);
        if (cmsg == NULL || cmsg->cmsg_level != SOL_SOCKET ||
            cmsg->cmsg_type != SCM_CREDENTIALS ||
            cmsg->cmsg_len < CMSG_LEN(sizeof(struct ucred))) {
            printf("RESULT forged_datagram_without_credentials\n");
            goto out;
        }
        {
            struct ucred uc;

            memcpy(&uc, CMSG_DATA(cmsg), sizeof(uc));
            printf("RESULT forged_attempt_seen_as uid=%u gid=%u\n",
                   (unsigned)uc.uid, (unsigned)uc.gid);
            if (uc.uid == 0) {
                printf("CONCLUSION KERNEL HONOURED THE FORGED CREDENTIAL -- the "
                       "identity check is not a security boundary\n");
            } else {
                printf("CONCLUSION kernel overrode the forged credential with "
                       "the sender's own uid\n");
                rc = 0;
            }
        }
    }

out:
    if (pid > 0) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
    }
    unlink(spath);
    unlink(cpath);
    if (sfd >= 0) {
        close(sfd);
    }
    if (cfd >= 0) {
        close(cfd);
    }
    return rc;
}

static int make_bound(const char *path, int server)
{
    int                fd;
    struct sockaddr_un sun;
    size_t             plen;

    fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        perror("socket");
        exit(1);
    }
    if (server) {
        int one = 1;
        if (setsockopt(fd, SOL_SOCKET, SO_PASSCRED, &one, sizeof(one)) != 0) {
            perror("SO_PASSCRED");
            exit(1);
        }
    }
    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    plen           = strlen(path);
    if (plen >= sizeof(sun.sun_path)) {
        fprintf(stderr, "socket path too long\n");
        exit(1);
    }
    memcpy(sun.sun_path, path, plen + 1);
    if (bind(fd, (struct sockaddr *)&sun,
             (socklen_t)(offsetof(struct sockaddr_un, sun_path) + plen + 1)) !=
        0) {
        perror("bind");
        exit(1);
    }
    if (server) {
        chmod(path, 0666); /* so the child can send even across UIDs */
    }
    return fd;
}

int main(int argc, char **argv)
{
    uid_t real = (argc > 1) ? (uid_t)strtoul(argv[1], NULL, 10) : 65530;
    uid_t eff  = (argc > 2) ? (uid_t)strtoul(argv[2], NULL, 10) : 65531;
    char  spath[128], cpath[128];
    int   sfd, cfd;
    int   rc    = 1;
    pid_t pid   = -1;

    snprintf(spath, sizeof(spath), "/tmp/ipc_cred_probe_s_%ld", (long)getpid());
    snprintf(cpath, sizeof(cpath), "/tmp/ipc_cred_probe_c_%ld", (long)getpid());

    /* Children terminate with _exit(), which does not flush stdio.  Line
     * buffering keeps their observations visible even when stdout is a pipe. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    printf("== cred_probe: real=%u effective=%u (sender will split them)\n",
           (unsigned)real, (unsigned)eff);
    printf("== running as uid=%u euid=%u (needs root to setresuid)\n",
           (unsigned)getuid(), (unsigned)geteuid());

    sfd = make_bound(spath, 1);
    cfd = make_bound(cpath, 0);
    /* Bounded receive.  Without this a child that fails setresuid() (the
     * obvious mistake is running the probe without sudo) exits without sending
     * anything and the parent blocks in recvmsg() forever -- no error, no
     * output, just a hung process. */
    {
        struct timeval tv;
        tv.tv_sec  = 10;
        tv.tv_usec = 0;
        if (setsockopt(sfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
            perror("SO_RCVTIMEO");
            goto out;
        }
    }

    pid = fork();
    if (pid < 0) {
        perror("fork");
        goto out;
    }
    if (pid == 0) {
        /* child: split real/effective UID, then send one datagram */
        if (setresuid(real, eff, eff) != 0) {
            fprintf(stderr,
                    "setresuid(%u,%u) failed: %s -- re-run as root\n",
                    (unsigned)real, (unsigned)eff, strerror(errno));
            _exit(3);
        }
        {
            struct sockaddr_un sun;
            size_t             plen = strlen(spath);

            memset(&sun, 0, sizeof(sun));
            sun.sun_family = AF_UNIX;
            memcpy(sun.sun_path, spath, plen + 1);
            if (sendto(cfd, "x", 1, 0, (struct sockaddr *)&sun,
                       (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
                                   plen + 1)) < 0) {
                fprintf(stderr, "child sendto: %s\n", strerror(errno));
                _exit(4);
            }
        }
        _exit(0);
    }

    {
        char            data[64];
        char            ctrl[CTRL_SIZE];
        struct iovec    iov;
        struct msghdr   mh;
        struct cmsghdr *cmsg;
        ssize_t         n;
        int             status = 0;

        iov.iov_base = data;
        iov.iov_len  = sizeof(data);
        memset(&mh, 0, sizeof(mh));
        mh.msg_iov        = &iov;
        mh.msg_iovlen     = 1;
        mh.msg_control    = ctrl;
        mh.msg_controllen = sizeof(ctrl);

        n = recvmsg(sfd, &mh, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* Say what actually happened instead of hanging or printing a
                 * bare errno: the usual cause is a missing sudo. */
                waitpid(pid, &status, 0);
                pid = -1;
                printf("RESULT no_datagram_within_timeout\n");
                printf("CHILD_EXIT status=%d%s\n",
                       WIFEXITED(status) ? WEXITSTATUS(status) : -1,
                       (WIFEXITED(status) && WEXITSTATUS(status) == 3)
                           ? " (setresuid failed: re-run as root)"
                           : "");
            } else {
                perror("recvmsg");
            }
            goto out;
        }
        waitpid(pid, &status, 0);
        pid = -1;

        cmsg = CMSG_FIRSTHDR(&mh);
        if (cmsg == NULL) {
            printf("RESULT no_credentials_received\n");
            goto out;
        }
        {
            struct ucred uc;

            if (cmsg->cmsg_level != SOL_SOCKET ||
                cmsg->cmsg_type != SCM_CREDENTIALS ||
                cmsg->cmsg_len < CMSG_LEN(sizeof(uc))) {
                printf("RESULT unexpected_cmsg level=%d type=%d len=%zu\n",
                       cmsg->cmsg_level, cmsg->cmsg_type,
                       (size_t)cmsg->cmsg_len);
                goto out;
            }
            memcpy(&uc, CMSG_DATA(cmsg), sizeof(uc));
            printf("RESULT SCM_CREDENTIALS pid=%d uid=%u gid=%u\n", uc.pid,
                   (unsigned)uc.uid, (unsigned)uc.gid);
            if (uc.uid == real) {
                printf("CONCLUSION kernel reports the REAL uid\n");
            } else if (uc.uid == eff) {
                printf("CONCLUSION kernel reports the EFFECTIVE uid\n");
            } else {
                printf("CONCLUSION unexpected uid\n");
            }
            printf("CHILD_EXIT status=%d\n",
                   WIFEXITED(status) ? WEXITSTATUS(status) : -1);
            rc = 0;
        }
    }

    /* Part 2 needs the same sockets and the same root privileges; reaching
     * here means Part 1 got that far. */
    printf("\n== Part 2: unprivileged sender forging SCM_CREDENTIALS\n");
    if (probe_forged_cred(65530) != 0 && rc == 0) {
        rc = 1;
    }

out:
    /* Clean up on every path, including the ones that used to return early and
     * leave /tmp/ipc_cred_probe_{s,c}_<pid> behind. */
    if (pid > 0) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
    }
    unlink(spath);
    unlink(cpath);
    close(sfd);
    close(cfd);
    return rc;
}
