/*
 * Copyright (c) 2026 <版权方待定>
 * 许可协议：<待定>
 */
/*
 * refhost.h -- 参考宿主（reference host），**仅供自测，正式接入时不要用**。
 *
 * =====================================================================
 * 这个文件存在的唯一理由
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
 * 为什么它在 tests/ 下，而不是 include/ipc/ 下
 * =====================================================================
 * 它是「为了测试的实现」，不是交付物。放在 tests/support/ 让
 * `libipc.a 只由 src/ 下的 .c 生成` 这条规则**按目录就能成立**，不用靠人记。
 * 这条界线有机械检查兜底：`make check-separation` 会翻 libipc.a 的符号表，
 * 出现 IpcRefHost* 就报错。
 *
 * =====================================================================
 * 正式接入时怎么办
 * =====================================================================
 * **整个文件都不需要**。要做的事情只有两件：
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
 * 命名同样遵循《OpenHarmony C 语言编程规范》（函数大驼峰、全局变量 g_ 前缀）。
 */
#ifndef IPC_REFHOST_H
#define IPC_REFHOST_H

#include <stdint.h>

#include "ipc/ipc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 编译期上限。都是「小规模自测够用」的量级，不是设计参数。 */
#define IPC_REFHOST_MAX_CONTEXTS 8   /* 一个宿主最多驱动几个模块上下文 */
#define IPC_REFHOST_MAX_HANDLERS 16  /* 注册表最多几项 */
#define IPC_REFHOST_QUEUE_MAX 256    /* 回调队列深度上限 */
#define IPC_REFHOST_MAX_WORKERS 8    /* worker 线程数上限 */

/* 不透明句柄。 */
typedef struct IpcRefHost IpcRefHost;

/*
 * 业务回调签名。
 *
 * 执行线程：worker 线程池里的某一条，**不是** select 线程。
 * 因此本回调里可以安全地调用 IpcSend / IpcSendTimeout（会阻塞该 worker，
 * 不影响 select 线程继续把回复读回来）。
 *
 * message 的指针只在本次调用期间有效；要留数据必须自己 memcpy。本宿主
 * 已经把 message 深拷过一份再交给回调，所以回调期间 message 是安全的，
 * 但回调返回之后 job 就被回收了。
 *
 * 参数取非 const：因为业务可能需要调 IpcReply，而它要写 message->reply.replied
 * 那个记账字段。这是 ipc.h 里就定下的语义（见 IpcReply 的注释）。
 *
 * 返回值：IPC_OK 表示处理成功；返回负值只会计入 dispatchFailed，
 *         不影响后续报文的处理。
 */
typedef int32_t (*IpcRefHandlerFunc)(IpcMessage *message, void *user);

/* 参考宿主的可调参数。全零初始化取默认值。 */
typedef struct {
    int32_t workers;         /* worker 线程数，钳制到 1..8，默认 2 */
    int32_t queueMax;        /* 回调队列深度，钳制到 1..256，默认 64；满了丢报文并计数 */
    int32_t selectTimeoutMs; /* select 的超时（毫秒），默认 200；<=0 表示不设超时 */
    int32_t maxBatch;        /* 每轮最多处理几条可读报文，默认 64；<=0 表示不限 */
} IpcRefHostOptions;

#define IPC_REFHOST_OPTIONS_INIT { 0 }

/*
 * 建一个参考宿主对象。此时不创建任何线程，只是把注册表和队列准备好。
 * 之后需要：
 *   - 把 IpcRefHostDispatch 填进 IpcModuleOptions.dispatch，
 *     把本对象填进 IpcModuleOptions.dispatchUser，再去 IpcRegister()；
 *   - IpcRefHostBind() 把注册好的上下文交给宿主（Run 要知道 select 谁的 fd）；
 *   - 用 IpcRefHostAddHandler() 登记业务回调；
 *   - 最后 IpcRefHostRun() 才起线程并阻塞。
 *
 * 成功时 *outHost 为新建对象，失败时为 NULL。
 */
int32_t IpcRefHostCreate(const IpcRefHostOptions *options, IpcRefHost **outHost);

/*
 * 把一个已经注册好的模块上下文交给宿主去 select。
 *
 * 这一步是必须的，而且顺序不能反：Run() 需要 fd 才能 select，而 fd 要等
 * IpcRegister() 成功之后才有。老系统同理 —— 它是在模块注册完成后把新 fd
 * 加进自己的 fd_set。
 *
 * 同一个宿主可以绑多个上下文（同进程多模块，正是 Q2 答复里那种情况）。
 * 超过 IPC_REFHOST_MAX_CONTEXTS 返回 IPC_ERR_TOOMANY。
 */
int32_t IpcRefHostBind(IpcRefHost *host, IpcContext *ctx);

/*
 * 现成的 IpcDispatchFunc 实现：把报文投递到本宿主的队列，由 worker 执行。
 * 用法就是
 *     options.dispatch     = IpcRefHostDispatch;
 *     options.dispatchUser = host;
 * 直接赋进去。
 *
 * 队列满时丢弃该报文并返回 IPC_ERR_AGAIN（库会计入 dispatchFailed）。
 */
int32_t IpcRefHostDispatch(IpcContext *ctx, const IpcMessage *message,
                           void *hostUser);

/*
 * 另一种 dispatch 形态：**在调用者线程上内联执行**回调。
 *
 * 它存在的理由只有一个：它是 ipc.h 里那条死锁检测要识别的宿主写法。
 * 用它在白盒测试里验证「在 dispatch 回调里调 IpcSend 会拿到
 * IPC_ERR_DEADLOCK，而不是把 select 线程卡死」。
 *
 * 队列满不适用（没有队列）。找不到 handler 时返回 IPC_ERR_NOENT。
 */
int32_t IpcRefHostDispatchInline(IpcContext *ctx, const IpcMessage *message,
                                 void *hostUser);

/*
 * 登记业务回调。
 * event 相同的重复登记返回 IPC_ERR_BUSY（不覆盖，避免静默把别人的回调顶掉）。
 * 允许在 Run 之前或之后调用。
 */
int32_t IpcRefHostAddHandler(IpcRefHost *host, uint32_t event,
                             IpcRefHandlerFunc handler, void *user);

/* 注销一个业务回调；找不到返回 IPC_ERR_NOENT。 */
int32_t IpcRefHostRemoveHandler(IpcRefHost *host, uint32_t event);

/*
 * 起 select 线程与 worker 线程池，然后**阻塞**直到 IpcRefHostStop() 被调用
 * 或 IpcRequestStop() 生效。
 *
 * 返回 IPC_OK 表示正常停止；负值表示 select 循环自己出错了
 * （端点坏了、内存不够）。**返回值要检查**：不检查就会把「已经死掉的宿主」
 * 当成正常退出。
 */
int32_t IpcRefHostRun(IpcRefHost *host);

/*
 * 请求停止并从 Run 返回。任意线程可调，幂等。
 *
 * 实现上先调 IpcRequestStop()（唤醒所有在 IpcSend 里等回复的业务线程），
 * 再用自管道唤醒 select 线程，最后收掉 worker 池。
 *
 * 本函数**不阻塞**、也不回收线程 —— 收线程是 Run() 的活。所以从 worker
 * 回调里调它是安全的（在回调里 join 自己那条线程会死锁）。
 */
int32_t IpcRefHostStop(IpcRefHost *host);

/* ------------------------------------------------------------------ */
/* 诊断（测试断言用）                                                 */
/* ------------------------------------------------------------------ */

/* 当前队列里还没被 worker 取走的 job 数。 */
int32_t IpcRefHostQueueDepth(IpcRefHost *host, int32_t *outDepth);

/* 因为队列满而被丢掉、以及因为找不到 handler 而被丢掉的 job 数。 */
int32_t IpcRefHostDropped(IpcRefHost *host, uint64_t *outQueueFull,
                          uint64_t *outNoHandler);

/* handler 返回非 IPC_OK 的次数。 */
int32_t IpcRefHostHandlerErrors(IpcRefHost *host, uint64_t *outErrors);

/*
 * 释放宿主对象。调用前必须已经 Run 返回或没 Run 过。
 * **不负责**注销模块：IpcUnregister / IpcDestroy 由调用方自己按顺序调。
 */
void IpcRefHostDestroy(IpcRefHost *host);

#ifdef __cplusplus
}
#endif
#endif /* IPC_REFHOST_H */
