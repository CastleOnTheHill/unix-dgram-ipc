/*
 * Copyright (c) 2026 <版权方待定>
 * 许可协议：<待定>
 *
 * test_proto.c -- ipc_proto 的白盒单测。**仅供测试**。
 *
 * 这一组的重点是两件事：
 *   1. 线格式的**逐字节**正确性（偏移、大端、NUL 填充）。头文件里那张
 *      偏移表就是这里的判据，两边必须一致；
 *   2. 接收侧的拒绝清单。报头是发送方说了算的，所以「什么情况下必须拒绝」
 *      比「正常情况能不能解析」重要得多。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "ipc_proto.h"
#include "ipc/ipc.h"
#include "utest.h"

/* 头文件里那张线格式表的偏移，抄成常量让改动时两边都有人提醒。 */
#define OFF_MAGIC      0
#define OFF_VERSION    4
#define OFF_TYPE       5
#define OFF_FLAGS      6
#define OFF_HDRSIZE    7
#define OFF_NS         8
#define OFF_SRC        24
#define OFF_DST        56
#define OFF_EVENT      88
#define OFF_PAYLOADLEN 92
#define OFF_REQID      96
#define OFF_INSTANCEID 104

static void MakeValidHeader(IpcProtoHeader *header)
{
    memset(header, 0, sizeof(*header));
    header->version    = (uint8_t)IPC_PROTOCOL_VERSION;
    header->type       = (uint8_t)IPC_MSG_TYPE_POST;
    header->flags      = 0;
    header->payloadLen = 5;
    header->event      = 0x01020304u;
    header->reqId      = 0x1122334455667788ull;
    header->instanceId = 0x99aabbccddeeff00ull;
    (void)snprintf(header->ns, sizeof(header->ns), "ns1");
    (void)snprintf(header->src, sizeof(header->src), "sender");
    (void)snprintf(header->dst, sizeof(header->dst), "receiver");
}

/* ------------------------------------------------------------------ */
/* 编码                                                               */
/* ------------------------------------------------------------------ */

UTEST_CASE(proto, encode_lays_out_exactly_as_documented)
{
    IpcProtoHeader header;
    uint8_t         buf[IPC_HDR_SIZE];
    size_t          written;

    MakeValidHeader(&header);
    memset(buf, 0xAA, sizeof(buf));
    written = IpcProtoEncode(&header, buf, sizeof(buf));

    UTEST_ASSERT_EQ_U64(written, IPC_HDR_SIZE);

    /* magic "UIPC" */
    UTEST_ASSERT_EQ(buf[OFF_MAGIC + 0], 'U');
    UTEST_ASSERT_EQ(buf[OFF_MAGIC + 1], 'I');
    UTEST_ASSERT_EQ(buf[OFF_MAGIC + 2], 'P');
    UTEST_ASSERT_EQ(buf[OFF_MAGIC + 3], 'C');
    UTEST_ASSERT_EQ(buf[OFF_VERSION], IPC_PROTOCOL_VERSION);
    UTEST_ASSERT_EQ(buf[OFF_TYPE], IPC_MSG_TYPE_POST);
    UTEST_ASSERT_EQ(buf[OFF_FLAGS], 0);
    UTEST_ASSERT_EQ(buf[OFF_HDRSIZE], IPC_HDR_SIZE);

    /* 名字字段：内容对，且**其余部分被 NUL 填满**（不是留着调用者的垃圾）。 */
    UTEST_ASSERT_EQ(buf[OFF_NS], 'n');
    UTEST_ASSERT_EQ(buf[OFF_NS + 1], 's');
    UTEST_ASSERT_EQ(buf[OFF_NS + 2], '1');
    UTEST_ASSERT_EQ(buf[OFF_NS + 3], '\0');
    UTEST_ASSERT_EQ(buf[OFF_NS + 4], '\0');
    UTEST_ASSERT_EQ(buf[OFF_NS + IPC_NS_MAX - 1], '\0');
    UTEST_ASSERT_EQ(buf[OFF_SRC + 6], '\0');
    UTEST_ASSERT_EQ(buf[OFF_DST + 8], '\0');

    /* 数值字段：**大端**。逐字节确认，避免「本机能跑、换架构就错」。 */
    UTEST_ASSERT_EQ(buf[OFF_EVENT + 0], 0x01);
    UTEST_ASSERT_EQ(buf[OFF_EVENT + 1], 0x02);
    UTEST_ASSERT_EQ(buf[OFF_EVENT + 2], 0x03);
    UTEST_ASSERT_EQ(buf[OFF_EVENT + 3], 0x04);
    UTEST_ASSERT_EQ(buf[OFF_PAYLOADLEN + 3], 5);
    UTEST_ASSERT_EQ(buf[OFF_REQID + 0], 0x11);
    UTEST_ASSERT_EQ(buf[OFF_REQID + 7], 0x88);
    UTEST_ASSERT_EQ(buf[OFF_INSTANCEID + 0], 0x99);
    UTEST_ASSERT_EQ(buf[OFF_INSTANCEID + 7], 0x00);

    /* 报头是定长的，第 111 字节之后不该再写任何东西：再写一个字节的探针。 */
    UTEST_ASSERT_EQ(buf[IPC_HDR_SIZE - 1], 0x00); /* instanceId 最低字节 */
}

UTEST_CASE(proto, encode_rejects_invalid_input)
{
    IpcProtoHeader header;
    uint8_t         buf[IPC_HDR_SIZE];

    MakeValidHeader(&header);

    UTEST_ASSERT_EQ_U64(IpcProtoEncode(NULL, buf, sizeof(buf)), 0);
    UTEST_ASSERT_EQ_U64(IpcProtoEncode(&header, NULL, sizeof(buf)), 0);
    UTEST_ASSERT_EQ_U64(IpcProtoEncode(&header, buf, IPC_HDR_SIZE - 1), 0);

    MakeValidHeader(&header);
    header.version = 2;
    UTEST_ASSERT_EQ_U64(IpcProtoEncode(&header, buf, sizeof(buf)), 0);

    MakeValidHeader(&header);
    header.type = 7; /* 未知类型 */
    UTEST_ASSERT_EQ_U64(IpcProtoEncode(&header, buf, sizeof(buf)), 0);

    MakeValidHeader(&header);
    header.flags = 1; /* v1 的保留位必须为 0 */
    UTEST_ASSERT_EQ_U64(IpcProtoEncode(&header, buf, sizeof(buf)), 0);

    MakeValidHeader(&header);
    header.payloadLen = IPC_PAYLOAD_HARD_MAX + 1u;
    UTEST_ASSERT_EQ_U64(IpcProtoEncode(&header, buf, sizeof(buf)), 0);

    /* 三个名字字段：空串与超长都必须拒绝，dst 也不许为空
     * （接收端要靠 dst 做「是不是发给我的」校验）。 */
    MakeValidHeader(&header);
    header.ns[0] = '\0';
    UTEST_ASSERT_EQ_U64(IpcProtoEncode(&header, buf, sizeof(buf)), 0);

    MakeValidHeader(&header);
    header.src[0] = '\0';
    UTEST_ASSERT_EQ_U64(IpcProtoEncode(&header, buf, sizeof(buf)), 0);

    MakeValidHeader(&header);
    header.dst[0] = '\0';
    UTEST_ASSERT_EQ_U64(IpcProtoEncode(&header, buf, sizeof(buf)), 0);

    /* 恰好填满字段宽度（没有 NUL 的位置）也算写不下。 */
    MakeValidHeader(&header);
    memset(header.ns, 'a', IPC_NS_MAX);
    UTEST_ASSERT_EQ_U64(IpcProtoEncode(&header, buf, sizeof(buf)), 0);

    MakeValidHeader(&header);
    memset(header.src, 'a', IPC_NAME_MAX);
    UTEST_ASSERT_EQ_U64(IpcProtoEncode(&header, buf, sizeof(buf)), 0);
}

/* ------------------------------------------------------------------ */
/* 解码                                                               */
/* ------------------------------------------------------------------ */

UTEST_CASE(proto, decode_roundtrips_every_field)
{
    IpcProtoHeader in;
    IpcProtoHeader out;
    uint8_t        buf[IPC_HDR_SIZE];

    MakeValidHeader(&in);
    in.type = (uint8_t)IPC_MSG_TYPE_REQ;
    UTEST_ASSERT_EQ_U64(IpcProtoEncode(&in, buf, sizeof(buf)), IPC_HDR_SIZE);

    memset(&out, 0xEE, sizeof(out));
    UTEST_ASSERT_EQ(IpcProtoDecode(buf, sizeof(buf), &out), IPC_OK);

    UTEST_ASSERT_EQ(out.version, in.version);
    UTEST_ASSERT_EQ(out.type, in.type);
    UTEST_ASSERT_EQ(out.flags, in.flags);
    UTEST_ASSERT_STREQ(out.ns, in.ns);
    UTEST_ASSERT_STREQ(out.src, in.src);
    UTEST_ASSERT_STREQ(out.dst, in.dst);
    UTEST_ASSERT_EQ_U64(out.event, in.event);
    UTEST_ASSERT_EQ_U64(out.payloadLen, in.payloadLen);
    UTEST_ASSERT_EQ_U64(out.reqId, in.reqId);
    UTEST_ASSERT_EQ_U64(out.instanceId, in.instanceId);

    /* 三条消息类型都要能往返。 */
    in.type = (uint8_t)IPC_MSG_TYPE_POST;
    UTEST_ASSERT_EQ_U64(IpcProtoEncode(&in, buf, sizeof(buf)), IPC_HDR_SIZE);
    UTEST_ASSERT_EQ(IpcProtoDecode(buf, sizeof(buf), &out), IPC_OK);
    UTEST_ASSERT_EQ(out.type, IPC_MSG_TYPE_POST);

    in.type = (uint8_t)IPC_MSG_TYPE_REP;
    UTEST_ASSERT_EQ_U64(IpcProtoEncode(&in, buf, sizeof(buf)), IPC_HDR_SIZE);
    UTEST_ASSERT_EQ(IpcProtoDecode(buf, sizeof(buf), &out), IPC_OK);
    UTEST_ASSERT_EQ(out.type, IPC_MSG_TYPE_REP);
}

UTEST_CASE(proto, decode_rejects_malformed_headers)
{
    IpcProtoHeader header;
    IpcProtoHeader out;
    uint8_t         buf[IPC_HDR_SIZE];

    UTEST_ASSERT_EQ(IpcProtoDecode(NULL, IPC_HDR_SIZE, &out), IPC_ERR_INVAL);
    UTEST_ASSERT_EQ(IpcProtoDecode(buf, IPC_HDR_SIZE, NULL), IPC_ERR_INVAL);

    MakeValidHeader(&header);
    UTEST_ASSERT_EQ_U64(IpcProtoEncode(&header, buf, sizeof(buf)), IPC_HDR_SIZE);

    /* 短头：连报头都不够长。 */
    UTEST_ASSERT_EQ(IpcProtoDecode(buf, IPC_HDR_SIZE - 1, &out), IPC_ERR_PROTO);

    /* magic 不符：改第 0 字节。每一类拒绝都要独立构造，不能只测一种。 */
    {
        uint8_t bad[IPC_HDR_SIZE];

        memcpy(bad, buf, sizeof(bad));
        bad[OFF_MAGIC] = 'X';
        UTEST_ASSERT_EQ(IpcProtoDecode(bad, sizeof(bad), &out), IPC_ERR_PROTO);

        memcpy(bad, buf, sizeof(bad));
        bad[OFF_VERSION] = 9;
        UTEST_ASSERT_EQ(IpcProtoDecode(bad, sizeof(bad), &out), IPC_ERR_PROTO);

        memcpy(bad, buf, sizeof(bad));
        bad[OFF_TYPE] = 0;
        UTEST_ASSERT_EQ(IpcProtoDecode(bad, sizeof(bad), &out), IPC_ERR_PROTO);

        memcpy(bad, buf, sizeof(bad));
        bad[OFF_FLAGS] = 0x80;
        UTEST_ASSERT_EQ(IpcProtoDecode(bad, sizeof(bad), &out), IPC_ERR_PROTO);

        memcpy(bad, buf, sizeof(bad));
        bad[OFF_HDRSIZE] = 64;
        UTEST_ASSERT_EQ(IpcProtoDecode(bad, sizeof(bad), &out), IPC_ERR_PROTO);
    }
}

UTEST_CASE(proto, decode_rejects_bad_name_fields)
{
    IpcProtoHeader header;
    IpcProtoHeader out;
    uint8_t         buf[IPC_HDR_SIZE];
    uint8_t         bad[IPC_HDR_SIZE];

    MakeValidHeader(&header);
    UTEST_ASSERT_EQ_U64(IpcProtoEncode(&header, buf, sizeof(buf)), IPC_HDR_SIZE);

    /* 字段里一个 NUL 都没有：不能当字符串用，否则会读到 event 的字节上去。 */
    memcpy(bad, buf, sizeof(bad));
    memset(bad + OFF_NS, 'a', IPC_NS_MAX);
    UTEST_ASSERT_EQ(IpcProtoDecode(bad, sizeof(bad), &out), IPC_ERR_PROTO);

    memcpy(bad, buf, sizeof(bad));
    memset(bad + OFF_SRC, 'a', IPC_NAME_MAX);
    UTEST_ASSERT_EQ(IpcProtoDecode(bad, sizeof(bad), &out), IPC_ERR_PROTO);

    memcpy(bad, buf, sizeof(bad));
    memset(bad + OFF_DST, 'a', IPC_NAME_MAX);
    UTEST_ASSERT_EQ(IpcProtoDecode(bad, sizeof(bad), &out), IPC_ERR_PROTO);

    /* 空字段（第一个字节就是 NUL）同样要拒。 */
    memcpy(bad, buf, sizeof(bad));
    bad[OFF_NS] = '\0';
    UTEST_ASSERT_EQ(IpcProtoDecode(bad, sizeof(bad), &out), IPC_ERR_PROTO);

    memcpy(bad, buf, sizeof(bad));
    bad[OFF_SRC] = '\0';
    UTEST_ASSERT_EQ(IpcProtoDecode(bad, sizeof(bad), &out), IPC_ERR_PROTO);

    memcpy(bad, buf, sizeof(bad));
    bad[OFF_DST] = '\0';
    UTEST_ASSERT_EQ(IpcProtoDecode(bad, sizeof(bad), &out), IPC_ERR_PROTO);
}

UTEST_CASE(proto, decode_rejects_oversized_payload_length)
{
    IpcProtoHeader header;
    IpcProtoHeader out;
    uint8_t         buf[IPC_HDR_SIZE];
    uint8_t         bad[IPC_HDR_SIZE];

    MakeValidHeader(&header);
    UTEST_ASSERT_EQ_U64(IpcProtoEncode(&header, buf, sizeof(buf)), IPC_HDR_SIZE);

    /* 手工把 payloadLen 改成 0xFFFFFFFF。走编码器是做不到的（它自己也拦），
     * 所以直接改字节 —— 模拟一个恶意发送方。 */
    memcpy(bad, buf, sizeof(bad));
    bad[OFF_PAYLOADLEN + 0] = 0xFF;
    bad[OFF_PAYLOADLEN + 1] = 0xFF;
    bad[OFF_PAYLOADLEN + 2] = 0xFF;
    bad[OFF_PAYLOADLEN + 3] = 0xFF;
    UTEST_ASSERT_EQ(IpcProtoDecode(bad, sizeof(bad), &out), IPC_ERR_PROTO);

    /* 恰好等于硬上限要能过（边界两侧都测）。 */
    memcpy(bad, buf, sizeof(bad));
    bad[OFF_PAYLOADLEN + 0] = 0x00;
    bad[OFF_PAYLOADLEN + 1] = 0x01;
    bad[OFF_PAYLOADLEN + 2] = 0x00;
    bad[OFF_PAYLOADLEN + 3] = 0x00; /* 65536 */
    UTEST_ASSERT_EQ(IpcProtoDecode(bad, sizeof(bad), &out), IPC_OK);
    UTEST_ASSERT_EQ_U64(out.payloadLen, IPC_PAYLOAD_HARD_MAX);
}

/* ------------------------------------------------------------------ */
/* 凭据                                                               */
/* ------------------------------------------------------------------ */

/*
 * 手工搭一个带 SCM_CREDENTIALS 的 msghdr。
 * 不靠真实 socket 是刻意的：这样能构造出内核**永远不会发**的形状
 * （两条凭据、长度自相矛盾），而那些正是接收侧必须拒掉的情况。
 */
typedef struct {
    uint8_t       ctrl[2 * ((CMSG_SPACE(sizeof(struct ucred)) + 15) / 16) * 16];
    struct msghdr msg;
} CredFixture;

static void CredFixtureInit(CredFixture *fixture, size_t ctrlLen)
{
    memset(fixture, 0, sizeof(*fixture));
    fixture->msg.msg_control    = fixture->ctrl;
    fixture->msg.msg_controllen = ctrlLen;
}

/*
 * 往链上第 index 条位置写一条凭据。**必须按 index 递增顺序调用**：
 * CMSG_NXTHDR 靠前一条的 cmsg_len 才能走到下一条，跳着写是走不动的。
 * totalCount 是这次要放几条，用来设定 msg_controllen 这个上界。
 */
static void CredFixtureAdd(CredFixture *fixture, int32_t index, int32_t totalCount,
                           pid_t pid, uid_t uid, gid_t gid)
{
    struct cmsghdr *cmsg;
    int32_t         i;

    fixture->msg.msg_controllen = CMSG_SPACE(sizeof(struct ucred)) * (size_t)totalCount;
    cmsg                        = CMSG_FIRSTHDR(&fixture->msg);
    for (i = 0; i < index && cmsg != NULL; i++) {
        cmsg = CMSG_NXTHDR(&fixture->msg, cmsg);
    }
    if (cmsg == NULL) {
        return;
    }
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type  = SCM_CREDENTIALS;
    cmsg->cmsg_len   = CMSG_LEN(sizeof(struct ucred));
    {
        struct ucred uc;

        memset(&uc, 0, sizeof(uc));
        uc.pid = pid;
        uc.uid = uid;
        uc.gid = gid;
        memcpy(CMSG_DATA(cmsg), &uc, sizeof(uc));
    }
}

UTEST_CASE(proto, cred_from_msg_accepts_single_credential)
{
    CredFixture fixture;
    IpcCred     cred;

    CredFixtureInit(&fixture, CMSG_SPACE(sizeof(struct ucred)));
    CredFixtureAdd(&fixture, 0, 1, 4242, 1000, 1000);
    UTEST_ASSERT_EQ(IpcProtoCredFromMsg(&fixture.msg, 0, &cred), IPC_OK);
    UTEST_ASSERT_EQ(cred.present, 1);
    UTEST_ASSERT_EQ(cred.pid, 4242);
    UTEST_ASSERT_EQ_U64(cred.uid, 1000);
    UTEST_ASSERT_EQ_U64(cred.gid, 1000);
}

UTEST_CASE(proto, cred_from_msg_rejects_missing_or_truncated)
{
    CredFixture fixture;
    IpcCred     cred;

    CredFixtureInit(&fixture, CMSG_SPACE(sizeof(struct ucred)));

    /* 参数检查先来。 */
    UTEST_ASSERT_EQ(IpcProtoCredFromMsg(NULL, 0, &cred), IPC_ERR_INVAL);
    UTEST_ASSERT_EQ(IpcProtoCredFromMsg(&fixture.msg, 0, NULL), IPC_ERR_INVAL);

    /* MSG_CTRUNC：控制缓冲被截断，凭据可能正是被切掉的那部分 → 一律不可信。 */
    CredFixtureAdd(&fixture, 0, 1, 1, 0, 0);
    UTEST_ASSERT_EQ(IpcProtoCredFromMsg(&fixture.msg, MSG_CTRUNC, &cred), IPC_ERR_CRED);

    /* 没有控制数据。 */
    CredFixtureInit(&fixture, CMSG_SPACE(sizeof(struct ucred)));
    UTEST_ASSERT_EQ(IpcProtoCredFromMsg(&fixture.msg, 0, &cred), IPC_ERR_CRED);

    /* 有控制数据但不是凭据（例如别人塞了个 SCM_RIGHTS）。 */
    CredFixtureInit(&fixture, CMSG_SPACE(sizeof(int)));
    {
        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&fixture.msg);

        cmsg->cmsg_level           = SOL_SOCKET;
        cmsg->cmsg_type            = SCM_RIGHTS;
        cmsg->cmsg_len             = CMSG_LEN(sizeof(int));
        fixture.msg.msg_controllen = CMSG_SPACE(sizeof(int));
        UTEST_ASSERT_EQ(IpcProtoCredFromMsg(&fixture.msg, 0, &cred), IPC_ERR_CRED);
        /* out 必须被重置，不能留上一次的残留。 */
        UTEST_ASSERT_EQ(cred.present, 0);
    }
}

UTEST_CASE(proto, cred_from_msg_rejects_two_credentials)
{
    CredFixture fixture;
    IpcCred     cred;

    CredFixtureInit(&fixture, 2 * CMSG_SPACE(sizeof(struct ucred)));
    CredFixtureAdd(&fixture, 0, 2, 111, 0, 0);
    CredFixtureAdd(&fixture, 1, 2, 222, 0, 0);
    /* SO_PASSCRED 只会让内核填一条。出现第二条就说明这个报文不是内核原样送来的。 */
    UTEST_ASSERT_EQ(IpcProtoCredFromMsg(&fixture.msg, 0, &cred), IPC_ERR_CRED);
}

UTEST_CASE(proto, cred_from_msg_rejects_self_contradictory_length)
{
    CredFixture fixture;
    IpcCred     cred;

    /* cmsg_len 比 cmsghdr 本身还小：矛盾，立刻停手报 PROTO。 */
    CredFixtureInit(&fixture, CMSG_SPACE(sizeof(struct ucred)));
    CredFixtureAdd(&fixture, 0, 1, 1, 0, 0);
    CMSG_FIRSTHDR(&fixture.msg)->cmsg_len = 4;
    UTEST_ASSERT_EQ(IpcProtoCredFromMsg(&fixture.msg, 0, &cred), IPC_ERR_PROTO);

    /* cmsg_len 够放 cmsghdr，但放不下完整的 ucred：凭据本身被截断。 */
    CredFixtureInit(&fixture, CMSG_SPACE(sizeof(struct ucred)));
    CredFixtureAdd(&fixture, 0, 1, 1, 0, 0);
    CMSG_FIRSTHDR(&fixture.msg)->cmsg_len = CMSG_LEN(sizeof(struct ucred)) - 4;
    UTEST_ASSERT_EQ(IpcProtoCredFromMsg(&fixture.msg, 0, &cred), IPC_ERR_CRED);
}

UTEST_CASE(proto, cred_size_macro_matches_the_documented_invariant)
{
    /*
     * ipc.h / ipc_proto.h 都写明 IPC_CTRL_SIZE 恰好放一条 ucred，多放别的东西
     * 就会 MSG_CTRUNC。这条不变式是接收端分配的固定尺寸，必须被钉住。
     */
    UTEST_ASSERT_EQ_U64(IPC_CTRL_SIZE, CMSG_SPACE(sizeof(struct ucred)));
    UTEST_ASSERT_GE(IPC_CTRL_SIZE, CMSG_LEN(sizeof(struct ucred)));
}
