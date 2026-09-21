/*
 * Unit tests for the wire format and SCM_CREDENTIALS parsing.
 * No sockets are involved here -- everything is a synthetic buffer, which is
 * exactly what makes the credential parser testable in isolation (the real
 * kernel integration is covered by tests/integration).
 */
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "ipc/ipc.h"
#include "ipc_proto.h"
#include "utest.h"

static void mk_hdr(ipc_hdr_t *h, ipc_msg_type_t type)
{
    memset(h, 0, sizeof(*h));
    h->version     = IPC_PROTO_VERSION;
    h->type        = (uint8_t)type;
    h->event       = 42;
    h->payload_len = 3;
    h->req_id      = 0x1122334455667788ull;
    h->instance_id = 0xaabbccddeeff0011ull;
    strcpy(h->ns, "core");
    strcpy(h->src, "A1");
    strcpy(h->dst, "B2");
}

/* ------------------------------------------------------------------ */

UT_TEST(proto, roundtrip_all_types)
{
    static const ipc_msg_type_t types[] = { IPC_TYPE_POST, IPC_TYPE_REQ,
                                            IPC_TYPE_REP };
    size_t                      k;

    for (k = 0; k < sizeof(types) / sizeof(types[0]); k++) {
        ipc_hdr_t in, out;
        uint8_t   buf[IPC_HDR_SIZE];

        mk_hdr(&in, types[k]);
        UT_EQ_U64(ipc_hdr_encode(&in, buf, sizeof(buf)), IPC_HDR_SIZE);
        UT_EQ_INT(ipc_hdr_decode(buf, IPC_HDR_SIZE, &out), IPC_OK);
        UT_EQ_INT(out.version, in.version);
        UT_EQ_INT(out.type, in.type);
        UT_EQ_STR(out.ns, "core");
        UT_EQ_STR(out.src, "A1");
        UT_EQ_STR(out.dst, "B2");
        UT_EQ_U64(out.event, 42);
        UT_EQ_U64(out.payload_len, 3);
        UT_EQ_U64(out.req_id, 0x1122334455667788ull);
        UT_EQ_U64(out.instance_id, 0xaabbccddeeff0011ull);
    }
}

UT_TEST(proto, wire_is_big_endian_and_padding_free)
{
    ipc_hdr_t in;
    uint8_t   buf[IPC_HDR_SIZE];

    mk_hdr(&in, IPC_TYPE_REQ);
    in.event       = 0x01020304u;
    in.payload_len = 0x00000258u;
    in.req_id      = 0x0102030405060708ull;
    in.instance_id = 0x1112131415161718ull;
    UT_EQ_U64(ipc_hdr_encode(&in, buf, sizeof(buf)), IPC_HDR_SIZE);
    UT_CHECK(buf[0] == 'U' && buf[1] == 'I' && buf[2] == 'P' && buf[3] == 'C');
    UT_EQ_INT(buf[4], IPC_PROTO_VERSION);
    UT_EQ_INT(buf[5], IPC_TYPE_REQ);
    UT_EQ_INT(buf[6], 0);
    UT_EQ_INT(buf[7], IPC_HDR_SIZE);
    UT_EQ_INT(buf[88], 0x01);
    UT_EQ_INT(buf[89], 0x02);
    UT_EQ_INT(buf[90], 0x03);
    UT_EQ_INT(buf[91], 0x04);
    UT_EQ_INT(buf[92], 0x00);
    UT_EQ_INT(buf[93], 0x00);
    UT_EQ_INT(buf[94], 0x02);
    UT_EQ_INT(buf[95], 0x58);
    UT_EQ_INT(buf[96], 0x01);
    UT_EQ_INT(buf[103], 0x08);
    UT_EQ_INT(buf[104], 0x11);
    UT_EQ_INT(buf[111], 0x18);
}

UT_TEST(proto, encode_rejects_bad_input)
{
    ipc_hdr_t in;
    uint8_t   buf[IPC_HDR_SIZE];

    mk_hdr(&in, IPC_TYPE_REQ);
    UT_EQ_U64(ipc_hdr_encode(&in, buf, IPC_HDR_SIZE - 1), 0); /* small cap */
    UT_EQ_U64(ipc_hdr_encode(NULL, buf, sizeof(buf)), 0);
    UT_EQ_U64(ipc_hdr_encode(&in, NULL, sizeof(buf)), 0);

    mk_hdr(&in, (ipc_msg_type_t)0);
    UT_EQ_U64(ipc_hdr_encode(&in, buf, sizeof(buf)), 0);
    mk_hdr(&in, (ipc_msg_type_t)4);
    UT_EQ_U64(ipc_hdr_encode(&in, buf, sizeof(buf)), 0);
    mk_hdr(&in, (ipc_msg_type_t)255);
    UT_EQ_U64(ipc_hdr_encode(&in, buf, sizeof(buf)), 0);

    mk_hdr(&in, IPC_TYPE_REQ);
    in.version = 2;
    UT_EQ_U64(ipc_hdr_encode(&in, buf, sizeof(buf)), 0);

    mk_hdr(&in, IPC_TYPE_REQ);
    in.payload_len = IPC_PAYLOAD_HARD_MAX + 1;
    UT_EQ_U64(ipc_hdr_encode(&in, buf, sizeof(buf)), 0);

    mk_hdr(&in, IPC_TYPE_REQ);
    memset(in.ns, 'x', sizeof(in.ns)); /* no room for the terminator */
    UT_EQ_U64(ipc_hdr_encode(&in, buf, sizeof(buf)), 0);

    mk_hdr(&in, IPC_TYPE_REQ);
    memset(in.src, 'x', sizeof(in.src));
    UT_EQ_U64(ipc_hdr_encode(&in, buf, sizeof(buf)), 0);

    mk_hdr(&in, IPC_TYPE_REQ);
    in.dst[0] = '\0';
    UT_EQ_U64(ipc_hdr_encode(&in, buf, sizeof(buf)), 0);

    mk_hdr(&in, IPC_TYPE_REQ);
    UT_EQ_U64(ipc_hdr_encode(&in, buf, sizeof(buf)), IPC_HDR_SIZE);
    mk_hdr(&in, IPC_TYPE_REQ);
    in.payload_len = IPC_PAYLOAD_HARD_MAX; /* boundary value is allowed */
    UT_EQ_U64(ipc_hdr_encode(&in, buf, sizeof(buf)), IPC_HDR_SIZE);
}

UT_TEST(proto, decode_rejects_short_and_corrupt)
{
    ipc_hdr_t in, out;
    uint8_t   buf[IPC_HDR_SIZE];

    mk_hdr(&in, IPC_TYPE_REQ);
    UT_EQ_U64(ipc_hdr_encode(&in, buf, sizeof(buf)), IPC_HDR_SIZE);

    UT_EQ_INT(ipc_hdr_decode(buf, 0, &out), IPC_ERR_PROTO);
    UT_EQ_INT(ipc_hdr_decode(buf, IPC_HDR_SIZE - 1, &out), IPC_ERR_PROTO);
    UT_EQ_INT(ipc_hdr_decode(NULL, IPC_HDR_SIZE, &out), IPC_ERR_INVAL);
    UT_EQ_INT(ipc_hdr_decode(buf, IPC_HDR_SIZE, NULL), IPC_ERR_INVAL);

    {
        int i;
        for (i = 0; i < 4; i++) { /* every magic byte matters */
            uint8_t save = buf[i];
            buf[i]       = (uint8_t)(save ^ 0xff);
            UT_EQ_INT(ipc_hdr_decode(buf, IPC_HDR_SIZE, &out), IPC_ERR_PROTO);
            buf[i] = save;
        }
    }
    buf[4] = 99; /* version */
    UT_EQ_INT(ipc_hdr_decode(buf, IPC_HDR_SIZE, &out), IPC_ERR_PROTO);
    buf[4] = IPC_PROTO_VERSION;

    buf[5] = 0; /* type */
    UT_EQ_INT(ipc_hdr_decode(buf, IPC_HDR_SIZE, &out), IPC_ERR_PROTO);
    buf[5] = 9;
    UT_EQ_INT(ipc_hdr_decode(buf, IPC_HDR_SIZE, &out), IPC_ERR_PROTO);
    buf[5] = IPC_TYPE_REQ;

    buf[6] = 1; /* reserved flags must be zero */
    UT_EQ_INT(ipc_hdr_decode(buf, IPC_HDR_SIZE, &out), IPC_ERR_PROTO);
    buf[6] = 0;

    buf[7] = 56; /* hdr_len */
    UT_EQ_INT(ipc_hdr_decode(buf, IPC_HDR_SIZE, &out), IPC_ERR_PROTO);
    buf[7] = IPC_HDR_SIZE;

    memset(buf + 8, 'z', IPC_NS_MAX); /* namespace full, no terminator */
    UT_EQ_INT(ipc_hdr_decode(buf, IPC_HDR_SIZE, &out), IPC_ERR_PROTO);
    buf[8] = 'c';
    buf[9] = 'o';
    buf[10] = 'r';
    buf[11] = 'e';
    buf[12] = '\0';
    memset(buf + 13, 0, IPC_NS_MAX - 5);

    buf[8] = '\0'; /* empty namespace */
    UT_EQ_INT(ipc_hdr_decode(buf, IPC_HDR_SIZE, &out), IPC_ERR_PROTO);
    buf[8] = 'c';

    buf[24] = '\0'; /* empty source module */
    UT_EQ_INT(ipc_hdr_decode(buf, IPC_HDR_SIZE, &out), IPC_ERR_PROTO);
    buf[24] = 'A';

    memset(buf + 56, 'q', IPC_NAME_MAX); /* destination, no terminator */
    UT_EQ_INT(ipc_hdr_decode(buf, IPC_HDR_SIZE, &out), IPC_ERR_PROTO);
    buf[56] = 'B';
    buf[57] = '2';
    buf[58] = '\0';
    memset(buf + 59, 0, IPC_NAME_MAX - 3);

    buf[92] = 0xff; /* payload_len 0xff000258 > hard max */
    UT_EQ_INT(ipc_hdr_decode(buf, IPC_HDR_SIZE, &out), IPC_ERR_PROTO);
}

/* ------------------------------------------------------------------ */
/* SCM_CREDENTIALS                                                     */
/* ------------------------------------------------------------------ */

static void put_cred_cmsg(struct msghdr *mh, uint8_t *ctl, size_t ctlcap,
                          pid_t pid, uid_t uid, gid_t gid)
{
    struct cmsghdr *c;
    struct ucred    uc;

    memset(ctl, 0, ctlcap);
    mh->msg_control    = ctl;
    mh->msg_controllen = CMSG_SPACE(sizeof(struct ucred));
    c = CMSG_FIRSTHDR(mh);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type  = SCM_CREDENTIALS;
    c->cmsg_len   = CMSG_LEN(sizeof(struct ucred));
    uc.pid        = pid;
    uc.uid        = uid;
    uc.gid        = gid;
    memcpy(CMSG_DATA(c), &uc, sizeof(uc));
}

UT_TEST(cred, parses_kernel_style_credentials)
{
    struct msghdr mh;
    uint8_t       ctl[CMSG_SPACE(sizeof(struct ucred))];
    ipc_cred_t    out;

    memset(&mh, 0, sizeof(mh));
    put_cred_cmsg(&mh, ctl, sizeof(ctl), 1234, 1001, 1004);
    UT_EQ_INT(ipc_cred_from_msg(&mh, 0, &out), IPC_OK);
    UT_EQ_INT(out.present, 1);
    UT_EQ_INT(out.pid, 1234);
    UT_EQ_INT(out.uid, 1001);
    UT_EQ_INT(out.gid, 1004);
}

UT_TEST(cred, missing_control_buffer_is_rejected)
{
    struct msghdr mh;
    ipc_cred_t    out;

    memset(&mh, 0, sizeof(mh));
    UT_EQ_INT(ipc_cred_from_msg(&mh, 0, &out), IPC_ERR_CRED);
    UT_EQ_INT(out.present, 0);
}

UT_TEST(cred, ctrl_truncated_flag_is_rejected)
{
    struct msghdr mh;
    uint8_t       ctl[CMSG_SPACE(sizeof(struct ucred))];
    ipc_cred_t    out;

    memset(&mh, 0, sizeof(mh));
    put_cred_cmsg(&mh, ctl, sizeof(ctl), 7, 1002, 1004);
    /* The credentials may have been the truncated part: never trust this. */
    UT_EQ_INT(ipc_cred_from_msg(&mh, MSG_CTRUNC, &out), IPC_ERR_CRED);
}

UT_TEST(cred, only_credential_cmsgs_count)
{
    struct msghdr mh;
    uint8_t       ctl[256];
    struct cmsghdr *c;
    ipc_cred_t    out;

    /* A non-credential cmsg (SCM_RIGHTS-like) must not be mistaken for one. */
    memset(&mh, 0, sizeof(mh));
    memset(ctl, 0, sizeof(ctl));
    mh.msg_control    = ctl;
    mh.msg_controllen = CMSG_SPACE(sizeof(int));
    c                 = CMSG_FIRSTHDR(&mh);
    c->cmsg_level     = SOL_SOCKET;
    c->cmsg_type      = SCM_RIGHTS;
    c->cmsg_len       = CMSG_LEN(sizeof(int));
    UT_EQ_INT(ipc_cred_from_msg(&mh, 0, &out), IPC_ERR_CRED);
}

UT_TEST(cred, credentials_found_after_other_cmsgs)
{
    struct msghdr mh;
    uint8_t       ctl[256];
    struct cmsghdr *c1;
    struct cmsghdr *c2;
    struct ucred    uc;
    ipc_cred_t      out;

    memset(&mh, 0, sizeof(mh));
    memset(ctl, 0, sizeof(ctl));
    mh.msg_control    = ctl;
    mh.msg_controllen = (socklen_t)(CMSG_SPACE(sizeof(int)) +
                                    CMSG_SPACE(sizeof(struct ucred)));
    c1                = CMSG_FIRSTHDR(&mh);
    c1->cmsg_level    = SOL_SOCKET;
    c1->cmsg_type     = SCM_RIGHTS;
    c1->cmsg_len      = CMSG_LEN(sizeof(int));
    c2                = CMSG_NXTHDR(&mh, c1);
    UT_CHECK(c2 != NULL);
    c2->cmsg_level = SOL_SOCKET;
    c2->cmsg_type  = SCM_CREDENTIALS;
    c2->cmsg_len   = CMSG_LEN(sizeof(struct ucred));
    uc.pid         = 99;
    uc.uid         = 1003;
    uc.gid         = 1004;
    memcpy(CMSG_DATA(c2), &uc, sizeof(uc));

    UT_EQ_INT(ipc_cred_from_msg(&mh, 0, &out), IPC_OK);
    UT_EQ_INT(out.uid, 1003);
    UT_EQ_INT(out.pid, 99);
}

UT_TEST(cred, short_credential_payload_is_rejected)
{
    struct msghdr mh;
    uint8_t       ctl[CMSG_SPACE(sizeof(struct ucred))];
    struct cmsghdr *c;
    ipc_cred_t    out;

    memset(&mh, 0, sizeof(mh));
    put_cred_cmsg(&mh, ctl, sizeof(ctl), 1, 1001, 1004);
    c = CMSG_FIRSTHDR(&mh);
    /* Still a plausible cmsghdr, but four bytes short of a full ucred. */
    c->cmsg_len = CMSG_LEN(sizeof(struct ucred)) - 4;
    UT_EQ_INT(ipc_cred_from_msg(&mh, 0, &out), IPC_ERR_CRED);
}

UT_TEST(cred, inconsistent_cmsg_len_is_reported_as_protocol_error)
{
    struct msghdr mh;
    uint8_t       ctl[CMSG_SPACE(sizeof(struct ucred))];
    struct cmsghdr *c;
    ipc_cred_t    out;

    memset(&mh, 0, sizeof(mh));
    put_cred_cmsg(&mh, ctl, sizeof(ctl), 1, 1001, 1004);
    c = CMSG_FIRSTHDR(&mh);
    c->cmsg_len = sizeof(struct cmsghdr) - 1; /* nonsense: stop and report */
    UT_EQ_INT(ipc_cred_from_msg(&mh, 0, &out), IPC_ERR_PROTO);
}

/* ------------------------------------------------------------------ */

UT_TEST(payload, to_cstr_sanitises_and_truncates)
{
    char        buf[8];
    const char *weird = "ab\x01\xff" "cd";

    ipc_payload_to_cstr(NULL, 0, buf, sizeof(buf));
    UT_EQ_STR(buf, "");
    ipc_payload_to_cstr("hello", 5, buf, sizeof(buf));
    UT_EQ_STR(buf, "hello");
    ipc_payload_to_cstr(weird, 6, buf, sizeof(buf));
    UT_EQ_STR(buf, "ab..cd");
    ipc_payload_to_cstr("0123456789", 10, buf, sizeof(buf));
    UT_EQ_STR(buf, "0123456"); /* cap - 1 */
    ipc_payload_to_cstr("x", 1, buf, 0); /* must not touch memory */
}
