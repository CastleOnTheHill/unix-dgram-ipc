/*
 * Copyright (c) 2026 <版权方待定>
 * 许可协议：<待定>
 *
 * ipc_pending.h -- 同步请求（REQ/REP）的等待表。**不对外安装**。
 *
 * 职责边界：本文件只管理「谁在等哪条回复」，不含传输、不含统计汇总。
 * 表本身不感知 socket，可以脱离内核做完整的白盒单测（包括超时和放弃）。
 */
#ifndef IPC_PENDING_H
#define IPC_PENDING_H

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "ipc/ipc.h"

/*
 * 一个等待槽位。
 *
 * 匹配条件三件套，缺一不可（这三条都是踩过坑换来的）：
 *   1. 回复的 reqId 等于请求的 reqId；
 *   2. 回复的 src 等于当初请求的目标模块（防止别的模块冒充回复）；
 *   3. 回复回显的 instanceId 等于**本进程当前**的实例代际号
 *      （防止对端重启后残留的、上一代实例发出的陈旧回复被认领）。
 */
typedef struct {
    int32_t  inUse;
    uint64_t reqId;
    char     dstModuleId[IPC_NAME_MAX]; /* 期望的回复来源模块 */
    uint64_t expectedInstanceId;        /* 必须回显的实例代际号 */
    int32_t  done;
    int32_t  result;
    void    *replyBuf; /* 调用者的缓冲；可以传 NULL 表示丢弃内容 */
    size_t   replyCap;
    size_t   replyLen;   /* 实际写入的字节数 */
    uint64_t deadlineNs; /* 0 表示无限等待 */
    size_t   fullLen;    /* 回复的完整长度，用于诊断截断 */
} IpcPendingSlot;

typedef struct {
    IpcPendingSlot     *slots;
    int32_t             capacity;
    int32_t             liveSlots; /* inUse 的槽位数，供拆除时等待排空 */
    uint64_t            nextReqId;
    pthread_mutex_t     lock;
    pthread_cond_t      cv;
    _Atomic int32_t    *abortFlag; /* 指向 ctx->stopped：非 0 时所有等待者立即返回 */
} IpcPending;

#define IPC_PENDING_DEFAULT_CAPACITY 64

/*
 * 初始化。capacity <= 0 时取 IPC_PENDING_DEFAULT_CAPACITY；
 * abortFlag 可以为 NULL（表示没有外部中断源，只受超时控制）。
 * 成功返回 IPC_OK。
 */
int32_t IpcPendingInit(IpcPending *pending, int32_t capacity,
                       _Atomic int32_t *abortFlag);

void IpcPendingDestroy(IpcPending *pending);

/*
 * 登记一个等待槽位。**必须在 sendto() 之前调用** —— 对端再快，也不能出现
 * 「回复已经到了、而请求方还没开始等」这个窗口。
 *
 * timeoutMs < 0 表示无限等待；0 表示不等待（只探一次）。
 * 成功时 *outIndex 是槽位下标，*outReqId 是要写进请求报头的序号。
 * 表满返回 IPC_ERR_TOOMANY，且**不建立任何槽位**。
 */
int32_t IpcPendingAdd(IpcPending *pending, const char *dstModuleId,
                      uint64_t expectedInstanceId, int32_t timeoutMs, void *replyBuf,
                      size_t replyCap, int32_t *outIndex, uint64_t *outReqId);

/*
 * 等待槽位被填好。返回：
 *   IPC_OK            —— 回复已到，*outLen 是实际写入的字节数
 *   IPC_ERR_TIMEOUT   —— 超时
 *   IPC_ERR_STOPPED   —— 外部中断（IpcRequestStop）
 *
 * outFullLen（可传 NULL）带回回复的**完整长度**。它和 *outLen 不相等时说明
 * 回复被 replyCap 截断了 —— 调用方应当就此记一条 warning，而不是把截断后的
 * 内容当成完整回复用下去。
 *
 * 本函数**不释放**槽位，调用者要显式 IpcPendingRelease。
 */
int32_t IpcPendingWait(IpcPending *pending, int32_t index, size_t *outLen,
                       size_t *outFullLen);

/* 释放槽位。对同一个下标重复调用是安全的（幂等）。 */
void IpcPendingRelease(IpcPending *pending, int32_t index);

/*
 * 收到一条 REP 时由接收路径调用：尝试把它交给某个等待中的请求。
 * *outMatched 置 1 表示有人认领；置 0 表示无人认领（迟到、来源不符、
 * 实例代际不符），调用者应计入 replyUnmatched。
 */
void IpcPendingComplete(IpcPending *pending, const char *srcModuleId,
                        uint64_t instanceId, uint64_t reqId, const void *payload,
                        size_t len, int32_t *outMatched);

/*
 * 唤醒所有等待者，让它们返回 IPC_ERR_STOPPED。
 * IpcRequestStop() 调它；wait 循环也会自己看 abortFlag，两者互为兜底。
 */
void IpcPendingWakeAll(IpcPending *pending);

/*
 * 阻塞直到所有槽位都被释放。
 *
 * IpcUnregister 用它来实现「先了结挂起的同步请求，再关 fd」：如果不等就关 fd，
 * 正在收尾的发送线程会看到 EBADF，虽然无害但会污染日志、也会让「注销返回了
 * 就等于资源已释放」这句话不成立。
 *
 * 前提是 abortFlag 已经被置位（否则等待者永远不会退出），调用顺序不能反。
 */
void IpcPendingWaitDrained(IpcPending *pending);

#endif /* IPC_PENDING_H */
