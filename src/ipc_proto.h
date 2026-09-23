/*
 * Copyright (c) 2026 <版权方待定>
 * 许可协议：<待定>
 *
 * ipc_proto.h -- 线格式编解码。**不对外安装**。
 *
 * 报头是不含填充的定长 112 字节、显式大端、逐字段串行化。
 * 本层是纯函数：不碰 fd、不碰上下文、除了调用者给的缓冲不写别的地方，
 * 因此可以直接做白盒单测，不需要建 socket。
 */
#ifndef IPC_PROTO_H
#define IPC_PROTO_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "ipc/ipc.h"

/*
 * 反序列化后的报头。字段名用小驼峰，与文末线格式表一一对应。
 * 注意本结构体**只用于库内部**，绝不做 struct 覆写（overlay）式收发 ——
 * 填充字节、对齐和端序都会让两端解析不一致，而这三样在不同编译器、
 * 不同平台上都会变。
 */
typedef struct {
    uint8_t  version;
    uint8_t  type;
    uint8_t  flags;
    char     ns[IPC_NS_MAX];
    char     src[IPC_NAME_MAX];
    char     dst[IPC_NAME_MAX];
    uint32_t event;
    uint32_t payloadLen;
    uint64_t reqId;
    uint64_t instanceId;
} IpcProtoHeader;

/*
 * 内核给的发送方凭据。**这是本库唯一的身份依据**，不可伪造：
 * 它由内核在发送时填好，发送方自己改不了。
 *
 * 注意两点：
 *   - uid 是发送方的 **real UID**。内核不提供对端的 effective UID，
 *     所以接收侧的身份校验永远按 real UID 走，与 allowUidSplit 无关。
 *   - pid 只用于日志诊断，**不得**用于任何授权判断（可以被复用）。
 */
typedef struct {
    pid_t   pid;
    uid_t   uid;
    gid_t   gid;
    int32_t present; /* 1 = 成功取到凭据 */
} IpcCred;

/*
 * 序列化。成功返回 IPC_HDR_SIZE；任何字段非法返回 0。
 * 非法清单：type 未知、version 不符、flags 非 0、payloadLen 超硬上限、
 * ns / src / dst 有一个为空或写不下（含 NUL 后超出字段宽度）。
 *
 * dst 也要求非空：接收端要拿它做「这条是不是发给我的」这一道校验，
 * 空 dst 会让这道校验失去意义。广播是逐目标填 dst 的，不缺值。
 */
size_t IpcProtoEncode(const IpcProtoHeader *header, uint8_t *buf, size_t cap);

/*
 * 反序列化。接收端拒绝：短头、magic 不符、版本不符、type 未知、flags 非 0、
 * hdrSize 不符、名字没有 NUL 结尾、名字为空、payloadLen 超硬上限。
 * 成功返回 IPC_OK。
 *
 * 本函数**不校验** payloadLen 与实际收到的字节数是否一致 —— 那个信息在
 * 这里拿不到，由收包层在拿到 msg_flags 与实际长度后自行比对。
 */
int32_t IpcProtoDecode(const uint8_t *buf, size_t len, IpcProtoHeader *out);

/*
 * 从 recvmsg 的辅助数据里取出发送方凭据。
 *
 * 失败一律返回 IPC_ERR_CRED：MSG_CTRUNC（控制缓冲被截断，凭据可能正是被切掉
 * 的那部分，所以这里的一切都不可信）、没有控制数据、没有 SCM_CREDENTIALS
 * 项、凭据长度不足、以及出现**多于一条**凭据（那说明有人在拼报文，
 * 而 SO_PASSCRED 只会让内核填一条）。
 * **宁可拒绝报文，也不要放行一个身份不明的发送方。**
 */
int32_t IpcProtoCredFromMsg(struct msghdr *msg, int32_t msgFlags, IpcCred *out);

/* 需要给 recvmsg 准备的控制缓冲大小：恰好放一条 ucred。 */
#define IPC_CTRL_SIZE ((size_t)CMSG_SPACE(sizeof(struct ucred)))

#endif /* IPC_PROTO_H */
