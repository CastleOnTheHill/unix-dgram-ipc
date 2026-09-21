/*
 * ipc_proto.h -- wire format and SCM_CREDENTIALS parsing.
 *
 * Pure data-in / data-out code with no socket access, so it can be unit
 * tested with synthetic buffers (see tests/unit/test_proto.c).
 *
 * Wire header: fixed width, explicit big-endian, serialised field by field.
 * A raw C struct is deliberately NOT used on the wire because of padding,
 * alignment and endianness.
 *
 *  off  size  field
 *  ---  ----  -----------------------------------------------------------
 *    0     4  magic "UIPC"
 *    4     1  version
 *    5     1  type (ipc_msg_type_t)
 *    6     1  flags (reserved, must be 0)
 *    7     1  hdr_len (== IPC_HDR_SIZE)
 *    8    16  namespace, NUL padded
 *   24    32  source module, NUL padded
 *   56    32  destination module, NUL padded
 *   88     4  event
 *   92     4  payload_len
 *   96     8  req_id
 *  104     8  sender instance_id
 *  ---  ----
 *        112
 */
#ifndef IPC_PROTO_H
#define IPC_PROTO_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "ipc/ipc.h"

#define IPC_PROTO_VERSION 1u

typedef struct {
    uint8_t  version;
    uint8_t  type;
    uint8_t  flags;
    char     ns[IPC_NS_MAX];
    char     src[IPC_NAME_MAX];
    char     dst[IPC_NAME_MAX];
    uint32_t event;
    uint32_t payload_len;
    uint64_t req_id;
    uint64_t instance_id;
} ipc_hdr_t;

/* Serialise into buf (>= IPC_HDR_SIZE).  Returns IPC_HDR_SIZE, or 0 when an
 * argument is invalid (too-long name, bad type, oversized payload). */
size_t ipc_hdr_encode(const ipc_hdr_t *h, uint8_t *buf, size_t cap);

/* Parse.  Rejects short buffers, bad magic, bad version, bad type, non-zero
 * reserved flags, wrong hdr_len, unterminated names, empty names, unknown
 * type, and reserved type/flags combinations.  Returns IPC_OK on success. */
int ipc_hdr_decode(const uint8_t *buf, size_t len, ipc_hdr_t *out);

/* ------------------------------------------------------------------ */
/* Credentials                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    int   present;  /* 1 when an SCM_CREDENTIALS control message was found */
    pid_t pid;
    uid_t uid;
    gid_t gid;
} ipc_cred_t;

/* Extract SCM_CREDENTIALS from a received message.
 *
 * Returns:
 *   IPC_OK       - out->present == 1 and fields filled
 *   IPC_ERR_CRED - no SCM_CREDENTIALS at all, or truncated / too-short one
 *   IPC_ERR_PROTO- malformed control-message walk (cmsg_len inconsistent)
 *
 * `msg_flags` must be the flags returned by recvmsg(); MSG_CTRUNC is treated
 * as IPC_ERR_CRED because the credentials may have been the truncated part.
 *
 * Takes a non-const msghdr because CMSG_FIRSTHDR/CMSG_NXTHDR are declared
 * with a non-const pointer in glibc; the function never writes through it. */
int ipc_cred_from_msg(struct msghdr *mh, int msg_flags, ipc_cred_t *out);

/* ------------------------------------------------------------------ */
/* Reply addressing                                                    */
/* ------------------------------------------------------------------ */

struct ipc_ctx;

/* Everything ipc_reply() needs to answer a IPC_TYPE_REQ.  It is derived from
 * the received header only: the *claimed* source module is used solely to look
 * up a configured path, never as proof of identity (the proof is
 * SCM_CREDENTIALS, checked before the handler runs). */
typedef struct {
    struct ipc_ctx *ctx;
    char            dst[IPC_NAME_MAX];      /* reply goes here == request src */
    uint64_t        req_id;
    uint64_t        peer_instance;          /* echoed back to the requester  */
    int             replied;                /* reply at most once            */
} ipc_reply_ctx_t;

/* Convenience: render a payload as a NUL-terminated, printable string in
 * `out` (truncating).  Used by the test driver and logs only. */
void ipc_payload_to_cstr(const void *data, size_t len, char *out, size_t cap);

#endif /* IPC_PROTO_H */
