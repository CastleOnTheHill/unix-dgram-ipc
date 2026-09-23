/*
 * forge_peer.c -- raw datagram driver for the negative tests.
 *
 * Bypasses the library's send path on purpose: it builds a datagram byte by
 * byte and sendmsg()s it, optionally from a socket that is not registered at
 * all.  That is what lets the integration tests exercise the cases a
 * well-behaved peer never produces:
 *
 *   - a reply whose echoed instance id belongs to a previous process
 *   - a datagram claiming a source module that does not own the sending UID
 *   - an unconfigured source module
 *   - a destination that is not the receiver
 *   - short / mis-magicked / wrong-version / truncated headers
 *   - a payload length that disagrees with the datagram size
 *   - a payload larger than the receiver's cap
 *   - a datagram that carries extra ancillary data, which crowds out the
 *     credentials the receiver reserved room for (MSG_CTRUNC)
 *
 * Usage:
 *   forge_peer --conf FILE --to-module B1 [--ns core] [--type rep]
 *              [--src A1] [--dst B1] [--event N] [--req-id N]
 *              [--instance-id 0x...] [--payload TEXT] [--payload-size N]
 *              [--raw-hex AABBCC] [--count N] [--repeat-delay-ms N]
 *   forge_peer --conf FILE --to /path/socket.sock ...   (explicit target)
 *
 * SAFETY: because `--to` bypasses the config table, an explicit target is only
 * accepted when it either appears in --conf or lives under $IPC_LAB.  This is
 * a deliberately experimental tool that can send arbitrary bytes to an
 * arbitrary AF_UNIX datagram socket, so it stays inside the lab.  It still
 * cannot forge SCM_CREDENTIALS -- the kernel fills those in, which is the whole
 * point of the tests that use it.
 *
 * Prints one line per datagram: "FORGE n=%d to=%s bytes=%zd rc=%d errno=%d".
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "ipc/ipc.h"
#include "ipc_proto.h" /* header layout + encode: this tool builds frames by hand */

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static size_t parse_hex(const char *s, unsigned char *out, size_t cap)
{
    size_t n = 0;

    while (s[0] != '\0' && s[1] != '\0' && n < cap) {
        int hi = hexval(s[0]);
        int lo = hexval(s[1]);
        if (hi < 0 || lo < 0) {
            break;
        }
        out[n++] = (unsigned char)((hi << 4) | lo);
        s += 2;
    }
    return n;
}

static void usage(const char *a0)
{
    fprintf(stderr,
            "usage: %s --conf FILE (--to-module ID | --to PATH) [options]\n"
            "  --ns NAME          namespace, default 'core'\n"
            "  --type TYPE        post|req|rep, default post\n"
            "  --src ID           claimed source module, default 'A1'\n"
            "  --dst ID           claimed destination, default the target\n"
            "  --event N          event id, default 1\n"
            "  --req-id N         request id, default 0\n"
            "  --instance-id HEX  sender instance id, default 0\n"
            "  --payload TEXT     payload, default empty\n"
            "  --payload-size N   payload of N 'P' bytes (overrides --payload)\n"
            "  --raw-hex HEX      send these bytes verbatim, no header build\n"
            "  --count N          send N datagrams, default 1\n"
            "  --delay-ms N       sleep between datagrams, default 0\n"
            "  --send-fd PATH     attach PATH as SCM_RIGHTS alongside the payload,\n"
            "                     which crowds out the credentials the receiver\n"
            "                     reserved control-buffer room for (MSG_CTRUNC)\n"
            "  --bad-version N / --bad-magic / --bad-type N / --bad-hdrlen N\n"
            "                     --bad-flags N / --declare-len N\n"
            "                     --truncate N   (header mutation helpers)\n",
            a0);
}

int main(int argc, char **argv)
{
    const char             *conf = NULL, *to_path = NULL, *to_module = NULL;
    const char             *ns = "core", *src = "A1", *dst = NULL;
    const char             *payload = "";
    const char             *raw_hex = NULL;
    int                     type = IPC_TYPE_POST;
    uint32_t                event = 1;
    uint64_t                req_id = 0, instance = 0;
    long                    count = 1, delay_ms = 0;
    long                    payload_size = -1;
    long                    bad_version = -1, bad_type = -1, bad_hdrlen = -1;
    long                    bad_flags = -1, declare_len = -1, truncate = -1;
    const char             *send_fd_path = NULL;
    int                     bad_magic = 0;
    int                     explicit_to = 0;
    ipc_config_t           *cfg = NULL;
    int                     i;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        int         takes_value = 1;

        if (strcmp(a, "--conf") == 0 && v) conf = v;
        else if (strcmp(a, "--to") == 0 && v) { to_path = v; explicit_to = 1; }
        else if (strcmp(a, "--to-module") == 0 && v) to_module = v;
        else if (strcmp(a, "--ns") == 0 && v) ns = v;
        else if (strcmp(a, "--src") == 0 && v) src = v;
        else if (strcmp(a, "--dst") == 0 && v) dst = v;
        else if (strcmp(a, "--event") == 0 && v) event = (uint32_t)strtoul(v, NULL, 10);
        else if (strcmp(a, "--req-id") == 0 && v) req_id = strtoull(v, NULL, 0);
        else if (strcmp(a, "--instance-id") == 0 && v) instance = strtoull(v, NULL, 0);
        else if (strcmp(a, "--payload") == 0 && v) payload = v;
        else if (strcmp(a, "--payload-size") == 0 && v) payload_size = strtol(v, NULL, 10);
        else if (strcmp(a, "--raw-hex") == 0 && v) raw_hex = v;
        else if (strcmp(a, "--count") == 0 && v) count = strtol(v, NULL, 10);
        else if (strcmp(a, "--delay-ms") == 0 && v) delay_ms = strtol(v, NULL, 10);
        else if (strcmp(a, "--send-fd") == 0 && v) send_fd_path = v;
        else if (strcmp(a, "--bad-version") == 0 && v) bad_version = strtol(v, NULL, 10);
        else if (strcmp(a, "--bad-type") == 0 && v) bad_type = strtol(v, NULL, 10);
        else if (strcmp(a, "--bad-hdrlen") == 0 && v) bad_hdrlen = strtol(v, NULL, 10);
        else if (strcmp(a, "--bad-flags") == 0 && v) bad_flags = strtol(v, NULL, 10);
        else if (strcmp(a, "--declare-len") == 0 && v) declare_len = strtol(v, NULL, 10);
        else if (strcmp(a, "--truncate") == 0 && v) truncate = strtol(v, NULL, 10);
        else if (strcmp(a, "--bad-magic") == 0) { bad_magic = 1; takes_value = 0; }
        else if (strcmp(a, "--type") == 0 && v) {
            if (strcmp(v, "req") == 0) type = IPC_TYPE_REQ;
            else if (strcmp(v, "rep") == 0) type = IPC_TYPE_REP;
            else type = IPC_TYPE_POST;
        } else {
            usage(argv[0]);
            return 2;
        }
        i += takes_value;
    }

    if (conf == NULL || (to_path == NULL && to_module == NULL)) {
        usage(argv[0]);
        return 2;
    }
    if (ipc_config_load(conf, &cfg) != IPC_OK) {
        fprintf(stderr, "cannot load %s\n", conf);
        return 1;
    }
    if (to_path == NULL) {
        const ipc_config_entry_t *e = ipc_config_lookup(cfg, ns, to_module);
        if (e == NULL) {
            fprintf(stderr, "no such module %s/%s in %s\n", ns, to_module, conf);
            ipc_config_free(cfg);
            return 1;
        }
        to_path = e->path;
        if (dst == NULL) {
            dst = to_module;
        }
    } else if (explicit_to) {
        /* Guard rail: an explicit target is not validated by the config table,
         * which would make this the one tool in the tree that can address an
         * arbitrary AF_UNIX datagram socket.  Accept it only when it is a
         * configured path, or when it lives under the test lab root. */
        const char *lab     = getenv("IPC_LAB");
        int         allowed = (ipc_config_lookup_path(cfg, to_path) != NULL);

        if (!allowed && lab != NULL && lab[0] != '\0') {
            size_t ll = strlen(lab);
            allowed   = (strncmp(to_path, lab, ll) == 0 &&
                         (to_path[ll] == '/' || to_path[ll] == '\0'));
        }
        if (!allowed) {
            fprintf(stderr,
                    "refusing --to %s: an explicit target must be listed in %s "
                    "or live under $IPC_LAB (%s)\n",
                    to_path, conf, (lab && lab[0]) ? lab : "unset");
            ipc_config_free(cfg);
            return 2;
        }
    }
    if (dst == NULL) {
        dst = to_module ? to_module : "?";
    }
    /* sun_path is 108 bytes.  Every other probe in this directory checks this;
     * this one used not to, and --to comes straight from the command line. */
    if (strlen(to_path) >= IPC_SUN_PATH_MAX) {
        fprintf(stderr, "target path too long (%zu bytes, max %d)\n",
                strlen(to_path), IPC_SUN_PATH_MAX - 1);
        ipc_config_free(cfg);
        return 1;
    }

    {
        unsigned char *buf = NULL;
        size_t         len = 0;
        int            fd;
        struct sockaddr_un sun;
        long           n;

        if (raw_hex != NULL) {
            buf = malloc(strlen(raw_hex) / 2 + 1);
            if (buf == NULL) {
                ipc_config_free(cfg);
                return 1;
            }
            len = parse_hex(raw_hex, buf, strlen(raw_hex) / 2 + 1);
        } else {
            ipc_hdr_t      h;
            const void    *pl = payload;
            char          *big = NULL;
            size_t         plen;

            if (payload_size >= 0) {
                big = malloc((size_t)payload_size + 1);
                if (big == NULL) return 1;
                memset(big, 'P', (size_t)payload_size);
                pl   = big;
                plen = (size_t)payload_size;
                payload = big;
            } else {
                plen = strlen(payload);
            }

            memset(&h, 0, sizeof(h));
            h.version     = IPC_PROTO_VERSION;
            h.type        = (uint8_t)type;
            h.event       = event;
            h.req_id      = req_id;
            h.instance_id = instance;
            /* Declare the *real* length here, so the encoder's own range check
             * cannot stop us: a corrupted --declare-len is then injected into
             * the encoded bytes below, which is the only way to produce a frame
             * the encoder would have refused to build. */
            h.payload_len = (uint32_t)plen;
            snprintf(h.ns, sizeof(h.ns), "%s", ns);
            snprintf(h.src, sizeof(h.src), "%s", src);
            snprintf(h.dst, sizeof(h.dst), "%s", dst);

            buf = malloc(IPC_HDR_SIZE + plen + 1);
            if (buf == NULL) {
                free(big);
                ipc_config_free(cfg);
                return 1;
            }
            if (ipc_hdr_encode(&h, buf, IPC_HDR_SIZE) != IPC_HDR_SIZE) {
                fprintf(stderr, "header encode failed\n");
                free(buf);
                free(big);
                ipc_config_free(cfg);
                return 1;
            }
            memcpy(buf + IPC_HDR_SIZE, pl, plen);
            len = IPC_HDR_SIZE + plen;

            /* deliberate corruptions, applied after a correct encode */
            if (bad_magic)   buf[0] = 'X';
            if (bad_version >= 0) buf[4] = (unsigned char)bad_version;
            if (bad_type >= 0)    buf[5] = (unsigned char)bad_type;
            if (bad_flags >= 0)   buf[6] = (unsigned char)bad_flags;
            if (bad_hdrlen >= 0)  buf[7] = (unsigned char)bad_hdrlen;
            if (declare_len >= 0) {
                buf[92] = (unsigned char)((declare_len >> 24) & 0xff);
                buf[93] = (unsigned char)((declare_len >> 16) & 0xff);
                buf[94] = (unsigned char)((declare_len >> 8) & 0xff);
                buf[95] = (unsigned char)(declare_len & 0xff);
            }
            if (truncate >= 0 && (size_t)truncate < len) {
                len = (size_t)truncate;
            }
            free(big);
        }

        fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            fprintf(stderr, "socket: %s\n", strerror(errno));
            free(buf);
            ipc_config_free(cfg);
            return 1;
        }
        memset(&sun, 0, sizeof(sun));
        sun.sun_family = AF_UNIX;
        memcpy(sun.sun_path, to_path, strlen(to_path) + 1);

        /* Optional ancillary payload.  With SO_PASSCRED on the *receiver*, the
         * kernel appends SCM_CREDENTIALS to every datagram; the receiver
         * reserves exactly CMSG_SPACE(sizeof(struct ucred)) for control data.
         * Anything else we attach therefore spills over and makes the kernel
         * report MSG_CTRUNC -- which is how a real MSG_CTRUNC is produced
         * without touching the receiver. */
        {
            int   extra_fd = -1;
            char  cbuf[CMSG_SPACE(sizeof(int))];
            struct msghdr  mh;
            struct iovec   iov;

            if (send_fd_path != NULL) {
                extra_fd = open(send_fd_path, O_RDONLY | O_CLOEXEC);
                if (extra_fd < 0) {
                    fprintf(stderr, "open %s: %s\n", send_fd_path,
                            strerror(errno));
                    close(fd);
                    free(buf);
                    ipc_config_free(cfg);
                    return 1;
                }
            }

            memset(&mh, 0, sizeof(mh));
            memset(cbuf, 0, sizeof(cbuf));
            iov.iov_base       = buf;
            iov.iov_len        = len;
            mh.msg_name        = &sun;
            mh.msg_namelen     = (socklen_t)(offsetof(struct sockaddr_un,
                                                     sun_path) +
                                             strlen(to_path) + 1);
            mh.msg_iov         = &iov;
            mh.msg_iovlen      = 1;
            if (extra_fd >= 0) {
                struct cmsghdr *c = (struct cmsghdr *)cbuf;

                c->cmsg_level      = SOL_SOCKET;
                c->cmsg_type       = SCM_RIGHTS;
                c->cmsg_len        = CMSG_LEN(sizeof(int));
                memcpy(CMSG_DATA(c), &extra_fd, sizeof(int));
                mh.msg_control     = cbuf;
                mh.msg_controllen  = sizeof(cbuf);
            }

            for (n = 0; n < count; n++) {
                ssize_t w = sendmsg(fd, &mh, 0);

                printf("FORGE n=%ld to=%s bytes=%zd rc=%d errno=%d%s\n", n + 1,
                       to_path, w, w < 0 ? -1 : 0, w < 0 ? errno : 0,
                       extra_fd >= 0 ? " with_fd=1" : "");
                fflush(stdout);
                if (delay_ms > 0 && n + 1 < count) {
                    struct timespec ts;
                    ts.tv_sec  = delay_ms / 1000;
                    ts.tv_nsec = (delay_ms % 1000) * 1000000L;
                    nanosleep(&ts, NULL);
                }
            }
            if (extra_fd >= 0) {
                close(extra_fd);
            }
        }
        close(fd);
        free(buf);
    }

    ipc_config_free(cfg);
    return 0;
}
