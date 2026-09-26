/*
 * Copyright (c) 2026 <版权方待定>
 * 许可协议：<待定>
 *
 * ipc_proto.c -- 线格式编解码与凭据解析的实现。
 *
 * 全部是纯函数：不碰 fd、不碰上下文，可以脱离 socket 直接做白盒单测。
 */
#include "ipc_proto.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* 大端读写（显式移位，不做 struct 覆写）                              */
/* ------------------------------------------------------------------ */

static void PutU32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t GetU32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void PutU64(uint8_t *p, uint64_t v)
{
    PutU32(p, (uint32_t)(v >> 32));
    PutU32(p + 4, (uint32_t)(v & 0xffffffffull));
}

static uint64_t GetU64(const uint8_t *p)
{
    return ((uint64_t)GetU32(p) << 32) | (uint64_t)GetU32(p + 4);
}

/* ------------------------------------------------------------------ */
/* 定长名字字段                                                       */
/* ------------------------------------------------------------------ */

/*
 * 写入一个定长名字字段：先整段清零再拷贝，保证 NUL 填充。
 * 源串为空、或含 NUL 后长度达到/超过字段宽度，都算失败。
 */
static int32_t PutName(uint8_t *dst, size_t cap, const char *src)
{
    size_t n;

    if (src == NULL) {
        return IPC_ERR_PROTO;
    }
    n = strlen(src);
    if (n == 0 || n >= cap) {
        return IPC_ERR_PROTO;
    }
    memset(dst, 0, cap);
    memcpy(dst, src, n);
    return IPC_OK;
}

/*
 * 读出一个定长名字字段，要求字段内**存在** NUL 且内容非空。
 *
 * 为什么必须要求 NUL 在字段内：如果 32 字节被 32 个有效字符填满，说明发送方
 * 写的时候就没有终止符（或者被裁剪过），此时把整段当字符串用会越过字段边界
 * 读到后面 event / payloadLen 的字节上去。宁可报协议错。
 */
static int32_t GetName(const uint8_t *src, size_t cap, char *out)
{
    size_t i;

    for (i = 0; i < cap; i++) {
        if (src[i] == '\0') {
            break;
        }
    }
    if (i == 0 || i == cap) {
        return IPC_ERR_PROTO; /* 空字段，或字段内没有终止符 */
    }
    memcpy(out, src, i);
    out[i] = '\0';
    return IPC_OK;
}

static int32_t TypeIsValid(uint8_t type)
{
    return (type == (uint8_t)IPC_MSG_TYPE_POST || type == (uint8_t)IPC_MSG_TYPE_REQ ||
            type == (uint8_t)IPC_MSG_TYPE_REP)
               ? 1
               : 0;
}

/* ------------------------------------------------------------------ */
/* 报头                                                               */
/* ------------------------------------------------------------------ */

size_t IpcProtoEncode(const IpcProtoHeader *header, uint8_t *buf, size_t cap)
{
    if (header == NULL || buf == NULL || cap < IPC_HDR_SIZE) {
        return 0;
    }
    if (header->version != (uint8_t)IPC_PROTOCOL_VERSION) {
        return 0;
    }
    if (!TypeIsValid(header->type)) {
        return 0;
    }
    if (header->flags != 0) {
        return 0; /* v1 保留位必须为 0，非 0 一律拒绝，不偷偷忽略 */
    }
    if (header->payloadLen > IPC_PAYLOAD_HARD_MAX) {
        return 0;
    }
    if (PutName(buf + 8, IPC_NS_MAX, header->ns) != IPC_OK) {
        return 0;
    }
    if (PutName(buf + 24, IPC_NAME_MAX, header->src) != IPC_OK) {
        return 0;
    }
    if (PutName(buf + 56, IPC_NAME_MAX, header->dst) != IPC_OK) {
        return 0;
    }

    buf[0] = (uint8_t)'U';
    buf[1] = (uint8_t)'I';
    buf[2] = (uint8_t)'P';
    buf[3] = (uint8_t)'C';
    buf[4] = header->version;
    buf[5] = header->type;
    buf[6] = header->flags;
    buf[7] = (uint8_t)IPC_HDR_SIZE;

    PutU32(buf + 88, header->event);
    PutU32(buf + 92, header->payloadLen);
    PutU64(buf + 96, header->reqId);
    PutU64(buf + 104, header->instanceId);

    return (size_t)IPC_HDR_SIZE;
}

int32_t IpcProtoDecode(const uint8_t *buf, size_t len, IpcProtoHeader *out)
{
    if (buf == NULL || out == NULL) {
        return IPC_ERR_INVAL;
    }
    if (len < (size_t)IPC_HDR_SIZE) {
        return IPC_ERR_PROTO; /* 短头 */
    }
    if (buf[0] != (uint8_t)'U' || buf[1] != (uint8_t)'I' || buf[2] != (uint8_t)'P' ||
        buf[3] != (uint8_t)'C') {
        return IPC_ERR_PROTO;
    }
    if (buf[4] != (uint8_t)IPC_PROTOCOL_VERSION) {
        return IPC_ERR_PROTO; /* 版本不符：不尝试向后兼容解析 */
    }
    if (!TypeIsValid(buf[5])) {
        return IPC_ERR_PROTO;
    }
    if (buf[6] != 0) {
        return IPC_ERR_PROTO;
    }
    if (buf[7] != (uint8_t)IPC_HDR_SIZE) {
        return IPC_ERR_PROTO;
    }

    memset(out, 0, sizeof(*out));
    out->version = buf[4];
    out->type    = buf[5];
    out->flags   = buf[6];

    if (GetName(buf + 8, IPC_NS_MAX, out->ns) != IPC_OK) {
        return IPC_ERR_PROTO;
    }
    if (GetName(buf + 24, IPC_NAME_MAX, out->src) != IPC_OK) {
        return IPC_ERR_PROTO;
    }
    if (GetName(buf + 56, IPC_NAME_MAX, out->dst) != IPC_OK) {
        return IPC_ERR_PROTO;
    }

    out->event       = GetU32(buf + 88);
    out->payloadLen  = GetU32(buf + 92);
    out->reqId       = GetU64(buf + 96);
    out->instanceId  = GetU64(buf + 104);

    if (out->payloadLen > IPC_PAYLOAD_HARD_MAX) {
        return IPC_ERR_PROTO; /* 声明的长度越界：早拒，不去分配 */
    }
    return IPC_OK;
}

/* ------------------------------------------------------------------ */
/* 凭据                                                               */
/* ------------------------------------------------------------------ */

int32_t IpcProtoCredFromMsg(struct msghdr *msg, int32_t msgFlags, IpcCred *out)
{
    struct cmsghdr *cmsg;
    int32_t         found = 0;

    if (out == NULL) {
        return IPC_ERR_INVAL;
    }
    memset(out, 0, sizeof(*out));

    if (msg == NULL) {
        return IPC_ERR_INVAL;
    }
    if ((msgFlags & MSG_CTRUNC) != 0) {
        /*
         * 控制缓冲被截断：凭据有可能正是被切掉的那部分，因此这里看到的一切
         * 都不可信。不能「没看到凭据就当没有凭据」地放行。
         */
        return IPC_ERR_CRED;
    }
    if (msg->msg_control == NULL || msg->msg_controllen == 0) {
        return IPC_ERR_CRED; /* 开了 SO_PASSCRED 就不该出现这种情况 */
    }

    for (cmsg = CMSG_FIRSTHDR(msg); cmsg != NULL; cmsg = CMSG_NXTHDR(msg, cmsg)) {
        if (cmsg->cmsg_len == 0) {
            /*
             * 全零的控制缓冲：调用方给了空间，但里面一个字都没填。
             * 这是「**没有**凭据」，不是「报文格式非法」—— 两者对调用方的
             * 含义不同（前者是缺失、可能重试；后者是对端不老实或内存被踩），
             * 所以不能都揉成 PROTO。零长度也说明后面不可能再有有效条目，
             * 直接结束遍历，落到末尾的 !found 分支报 CRED。
             * 注意区分：cmsg_len **非零**但小于 cmsghdr 才是自相矛盾 → PROTO。
             */
            break;
        }
        if (cmsg->cmsg_len < sizeof(struct cmsghdr)) {
            return IPC_ERR_PROTO; /* cmsg_len 自相矛盾：停手，不再往下解析 */
        }
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_CREDENTIALS) {
            continue; /* 不认识的辅助数据，忽略 */
        }
        if (cmsg->cmsg_len < CMSG_LEN(sizeof(struct ucred))) {
            return IPC_ERR_CRED; /* 凭据本身被截断 */
        }
        if (found) {
            /* 内核只会填一条。出现第二条说明这个报文不是内核原样送来的。 */
            return IPC_ERR_CRED;
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
        return IPC_ERR_CRED;
    }
    return IPC_OK;
}
