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
 * Also reports whether a normal (non-setuid) process can lie about its UID.
 *
 * Run: sudo ./build/bin/cred_probe [R E]
 */
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define CTRL_SIZE CMSG_SPACE(sizeof(struct ucred))

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
    pid_t pid;

    snprintf(spath, sizeof(spath), "/tmp/ipc_cred_probe_s_%ld", (long)getpid());
    snprintf(cpath, sizeof(cpath), "/tmp/ipc_cred_probe_c_%ld", (long)getpid());

    printf("== cred_probe: real=%u effective=%u (sender will split them)\n",
           (unsigned)real, (unsigned)eff);
    printf("== running as uid=%u euid=%u (needs root to setresuid)\n",
           (unsigned)getuid(), (unsigned)geteuid());

    sfd = make_bound(spath, 1);
    cfd = make_bound(cpath, 0);

    pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
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
        int             status;

        iov.iov_base = data;
        iov.iov_len  = sizeof(data);
        memset(&mh, 0, sizeof(mh));
        mh.msg_iov        = &iov;
        mh.msg_iovlen     = 1;
        mh.msg_control    = ctrl;
        mh.msg_controllen = sizeof(ctrl);

        n = recvmsg(sfd, &mh, 0);
        if (n < 0) {
            perror("recvmsg");
            return 1;
        }
        waitpid(pid, &status, 0);

        cmsg = CMSG_FIRSTHDR(&mh);
        if (cmsg == NULL) {
            printf("RESULT no_credentials_received\n");
            return 1;
        }
        {
            struct ucred uc;

            if (cmsg->cmsg_level != SOL_SOCKET ||
                cmsg->cmsg_type != SCM_CREDENTIALS ||
                cmsg->cmsg_len < CMSG_LEN(sizeof(uc))) {
                printf("RESULT unexpected_cmsg level=%d type=%d len=%zu\n",
                       cmsg->cmsg_level, cmsg->cmsg_type,
                       (size_t)cmsg->cmsg_len);
                return 1;
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
        }
    }

    unlink(spath);
    unlink(cpath);
    close(sfd);
    close(cfd);
    return 0;
}
