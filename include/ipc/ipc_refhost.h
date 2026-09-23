/*
 * Copyright (c) 2026 <版权方待定>
 * 许可协议：<待定>
 */
/*
 * ipc_refhost.h -- 参考宿主（reference host），**仅供自测，正式接入时不要用**。
 *
 * =====================================================================
 * 这个头文件存在的唯一理由
 * =====================================================================
 * ipc.h 定义的传输层刻意不创建线程、不保存回调 —— 因为那三样东西
 * （独立 select 线程 / 回调线程池 / 回调注册表 + 事件分发）在老系统里
 * 已经固定好了，本次改造要原样复用。
 *
 * 但库自己得能独立跑起来才能测试。所以这里放一份**最小可用**的宿主实现，
 * 把上面三样东西按最朴素的方式写一遍：
 *
 *   - 一个 select 线程：select(IpcGetSelectFd()) → IpcHandleReadable()
 *   - 一个固定深度队列 + N 个 worker 线程：业务回调在这里执行
 *   - 一张 (event 号 → 回调) 的注册表
 *
 * =====================================================================
 * 正式接入时怎么办
 * =====================================================================
 * **整个头文件和对应实现文件都不需要**。要做的事情只有两件：
 *
 *   1. 老系统的 select 线程改成：把 IpcGetSelectFd(ctx) 加进它自己的
 *      fd_set；该 fd 可读时调一次 IpcHandleReadable(ctx, maxCount)。
 *
 *   2. 注册时把 IpcModuleOptions.dispatch 指向老系统的事件分发入口
 *      （写一个几行的薄适配函数，把 IpcMessage 翻译成老系统需要的参数），
 *      dispatchUser 指向老系统的注册表对象。老系统的分发内部照旧投递到
 *      它自己的线程池。
 *
 * 换句话说：本文件是「用库的人怎么写宿主」的示例代码，不是交付物的一部分。
 * 评审时如果只想看正式接口，直接忽略本文件即可。
 *
 * 命名同样遵循《OpenHarmony C 语言编程规范》。
 */
#ifndef IPC_REFHOST_H
#define IPC_REFHOST_H

#include "ipc/ipc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 不透明句柄。 */
typedef struct IpcRefHost IpcRefHost;

/*
 * 业务回调签名。
 *
 * 执行线程：worker 线程池里的某一条，**不是** select 线程。
 * 因此本回调里可以安全地调用 IpcSend / IpcSendTimeout（会阻塞该 worker，
 * 不影响 select 线程继续把回复读回来）。
 *
 * message 的指针只在本次调用期间有效；要留数据必须自己 memcpy。
 * 返回值：IPC_OK 表示处理成功；返回负值只会计入 dispatchFailed，
 *         不影响后续报文的处理。
 */
typedef int (*IpcRefHandlerFunc)(const IpcMessage *message, void *user);

/* 参考宿主的可调参数。全零初始化取默认值。 */
typedef struct {
    int workers;        /* worker 线程数，钳制到 1..8，默认 2 */
    int queueMax;       /* 回调队列深度，默认 64；满了丢报文并计数 */
    int selectTimeoutMs;/* select 的超时（毫秒），默认 200；<=0 表示不设超时 */
} IpcRefHostOptions;

#define IPC_REFHOST_OPTIONS_INIT { 0 }

/*
 * 建一个参考宿主对象。此时不创建任何线程，只是把注册表和队列准备好。
 * 之后需要：
 *   - 把 IpcRefHostDispatch 填进 IpcModuleOptions.dispatch，
 *     把本对象填进 IpcModuleOptions.dispatchUser，再去 IpcRegister()；
 *   - 用 IpcRefHostAddHandler() 登记业务回调；
 *   - 最后 IpcRefHostRun() 才起线程并阻塞。
 *
 * 成功时 *outHost 为新建对象，失败时为 NULL。
 */
int IpcRefHostCreate(const IpcRefHostOptions *options, IpcRefHost **outHost);

/*
 * 现成的 IpcDispatchFunc 实现：把报文投递到本宿主的队列，由 worker 执行。
 * 用法就是
 *     options.dispatch     = IpcRefHostDispatch;
 *     options.dispatchUser = host;
 * 直接赋进去。
 *
 * 队列满时丢弃该报文并返回 IPC_ERR_AGAIN（库会计入 dispatchFailed）。
 */
int IpcRefHostDispatch(IpcContext *ctx, const IpcMessage *message,
                       void *hostUser);

/*
 * 登记业务回调。
 * event 相同的重复登记返回 IPC_ERR_BUSY（不覆盖，避免静默把别人的回调顶掉）。
 * 允许在 Run 之前或之后调用。
 */
int IpcRefHostAddHandler(IpcRefHost *host, uint32_t event,
                         IpcRefHandlerFunc handler, void *user);

/* 注销一个业务回调；找不到返回 IPC_ERR_NOENT。 */
int IpcRefHostRemoveHandler(IpcRefHost *host, uint32_t event);

/*
 * 起 select 线程与 worker 线程池，然后**阻塞**直到 IpcRefHostStop() 被调用
 * 或 IpcRequestStop() 生效。
 *
 * 返回 IPC_OK 表示正常停止；负值表示 select 循环自己出错了
 * （端点坏了、内存不够）。**返回值要检查**：不检查就会把「已经死掉的宿主」
 * 当成正常退出。
 */
int IpcRefHostRun(IpcRefHost *host);

/*
 * 请求停止并从 Run 返回。任意线程可调，幂等。
 *
 * 实现上先调 IpcRequestStop()（唤醒所有在 IpcSend 里等回复的业务线程），
 * 再用自管道唤醒 select 线程，最后收掉 worker 池。
 */
int IpcRefHostStop(IpcRefHost *host);

/*
 * 释放宿主对象。调用前必须已经 Run 返回或没 Run 过。
 * **不负责**注销模块：IpcUnregister / IpcDestroy 由调用方自己按顺序调。
 */
void IpcRefHostDestroy(IpcRefHost *host);

#ifdef __cplusplus
}
#endif
#endif /* IPC_REFHOST_H */
