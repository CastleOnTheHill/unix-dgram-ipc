#include "ipc_proto.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* big-endian helpers (explicit; no struct overlay)                    */
/* ------------------------------------------------------------------ */

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t get_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void put_u64(uint8_t *p, uint64_t v)
{
    put_u32(p, (uint32_t)(v >> 32));
    put_u32(p + 4, (uint32_t)(v & 0xffffffffu));
}

static uint64_t get_u64(const uint8_t *p)
{
    return ((uint64_t)get_u32(p) << 32) | (uint64_t)get_u32(p + 4);
}

/* Copy a fixed-width name field, guaranteeing NUL termination.  Returns 0 on
 * success, -1 if `src` does not fit into `cap` bytes including the NUL. */
static int put_name(uint8_t *dst, size_t cap, const char *src)
{
    size_t n;

    if (src == NULL) {
        return -1;
    }
    n = strlen(src);
    if (n == 0 || n >= cap) {
        return -1;
    }
    memset(dst, 0, cap);
    memcpy(dst, src, n);
    return 0;
}

/* Read a fixed-width name field and require that it is NUL terminated
 * somewhere inside the field and not empty. */
static int get_name(const uint8_t *src, size_t cap, char *out)
{
    size_t i;

    for (i = 0; i < cap; i++) {
        if (src[i] == '\0') {
            break;
        }
    }
    if (i == 0 || i == cap) {
        return -1; /* empty, or no terminator inside the field */
    }
    memcpy(out, src, i);
    out[i] = '\0';
    return 0;
}

/* ------------------------------------------------------------------ */
/* header                                                              */
/* ------------------------------------------------------------------ */

size_t ipc_hdr_encode(const ipc_hdr_t *h, uint8_t *buf, size_t cap)
{
    if (h == NULL || buf == NULL || cap < IPC_HDR_SIZE) {
        return 0;
    }
    if (h->type != IPC_TYPE_POST && h->type != IPC_TYPE_REQ &&
        h->type != IPC_TYPE_REP) {
        return 0;
    }
    if (h->version != IPC_PROTO_VERSION) {
        return 0;
    }
    if (h->payload_len > IPC_PAYLOAD_HARD_MAX) {
        return 0;
    }
    if (put_name(buf + 8, IPC_NS_MAX, h->ns) != 0) {
        return 0;
    }
    if (put_name(buf + 24, IPC_NAME_MAX, h->src) != 0) {
        return 0;
    }
    if (put_name(buf + 56, IPC_NAME_MAX, h->dst) != 0) {
        return 0;
    }

    buf[0] = 'U';
    buf[1] = 'I';
    buf[2] = 'P';
    buf[3] = 'C';
    buf[4] = h->version;
    buf[5] = h->type;
    buf[6] = h->flags;
    buf[7] = (uint8_t)IPC_HDR_SIZE;

    put_u32(buf + 88, h->event);
    put_u32(buf + 92, h->payload_len);
    put_u64(buf + 96, h->req_id);
    put_u64(buf + 104, h->instance_id);

    return IPC_HDR_SIZE;
}

int ipc_hdr_decode(const uint8_t *buf, size_t len, ipc_hdr_t *out)
{
    if (buf == NULL || out == NULL) {
        return IPC_ERR_INVAL;
    }
    if (len < IPC_HDR_SIZE) {
        return IPC_ERR_PROTO; /* short header */
    }
    if (buf[0] != 'U' || buf[1] != 'I' || buf[2] != 'P' || buf[3] != 'C') {
        return IPC_ERR_PROTO;
    }
    if (buf[4] != IPC_PROTO_VERSION) {
        return IPC_ERR_PROTO;
    }
    if (buf[5] != IPC_TYPE_POST && buf[5] != IPC_TYPE_REQ &&
        buf[5] != IPC_TYPE_REP) {
        return IPC_ERR_PROTO;
    }
    if (buf[6] != 0) {
        return IPC_ERR_PROTO; /* reserved flags must be zero in v1 */
    }
    if (buf[7] != (uint8_t)IPC_HDR_SIZE) {
        return IPC_ERR_PROTO;
    }

    memset(out, 0, sizeof(*out));
    out->version = buf[4];
    out->type    = buf[5];
    out->flags   = buf[6];

    if (get_name(buf + 8, IPC_NS_MAX, out->ns) != 0) {
        return IPC_ERR_PROTO;
    }
    if (get_name(buf + 24, IPC_NAME_MAX, out->src) != 0) {
        return IPC_ERR_PROTO;
    }
    if (get_name(buf + 56, IPC_NAME_MAX, out->dst) != 0) {
        return IPC_ERR_PROTO;
    }

    out->event       = get_u32(buf + 88);
    out->payload_len = get_u32(buf + 92);
    out->req_id      = get_u64(buf + 96);
    out->instance_id = get_u64(buf + 104);

    if (out->payload_len > IPC_PAYLOAD_HARD_MAX) {
        return IPC_ERR_PROTO; /* declared length out of range: reject early */
    }
    return IPC_OK;
}

/* ------------------------------------------------------------------ */
/* credentials                                                         */
/* ------------------------------------------------------------------ */

int ipc_cred_from_msg(struct msghdr *mh, int msg_flags, ipc_cred_t *out)
{
    struct cmsghdr *cmsg;
    int found = 0;

    if (out == NULL) {
        return IPC_ERR_INVAL;
    }
    memset(out, 0, sizeof(*out));

    if (mh == NULL) {
        return IPC_ERR_INVAL;
    }
    if (msg_flags & MSG_CTRUNC) {
        /* The control buffer was too small: the credentials may have been the
         * part that got truncated, so we cannot trust anything here. */
        return IPC_ERR_CRED;
    }
    if (mh->msg_control == NULL || mh->msg_controllen == 0) {
        return IPC_ERR_CRED;
    }

    for (cmsg = CMSG_FIRSTHDR(mh); cmsg != NULL; cmsg = CMSG_NXTHDR(mh, cmsg)) {
        if (cmsg->cmsg_len < sizeof(struct cmsghdr)) {
            return IPC_ERR_PROTO; /* inconsistent cmsg_len: stop parsing */
        }
        if (cmsg->cmsg_level != SOL_SOCKET ||
            cmsg->cmsg_type != SCM_CREDENTIALS) {
            continue;
        }
        if (cmsg->cmsg_len < CMSG_LEN(sizeof(struct ucred))) {
            return IPC_ERR_CRED; /* truncated credentials */
        }
        {
            struct ucred uc;
            memcpy(&uc, CMSG_DATA(cmsg), sizeof(uc));
            out->pid     = uc.pid;
            out->uid     = uc.uid;
            out->gid     = uc.gid;
            out->present = 1;
            found        = 1;
        }
    }

    if (!found) {
        return IPC_ERR_CRED; /* SO_PASSCRED is on: this must never happen */
    }
    return IPC_OK;
}

void ipc_payload_to_cstr(const void *data, size_t len, char *out, size_t cap)
{
    size_t i, n;

    if (out == NULL || cap == 0) {
        return;
    }
    if (data == NULL || len == 0) {
        out[0] = '\0';
        return;
    }
    n = len < (cap - 1) ? len : (cap - 1);
    for (i = 0; i < n; i++) {
        unsigned char c = ((const unsigned char *)data)[i];
        out[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
    }
    out[n] = '\0';
}
