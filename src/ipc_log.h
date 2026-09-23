/*
 * Copyright (c) 2026 <版权方待定>
 * 许可协议：<待定>
 *
 * ipc_log.h -- 全局日志出口。**不对外安装**。
 *
 * 单独拆出头文件是为了打断循环依赖：配置加载发生在任何 IpcContext 之前，
 * 所以它只能走「全局出口」，拿不到上下文里的出口。ipc_internal.h 在此基础上
 * 再提供带上下文的 IpcLogEmit()。
 */
#ifndef IPC_LOG_H
#define IPC_LOG_H

#include <stdarg.h>
#include <stdint.h>

/*
 * 给「接受 printf 风格格式串」的函数打上格式检查标记。
 *
 * 为什么值得引入一个编译器扩展：本库对外接口一律用定长整型（int32_t /
 * uint32_t / uint64_t），而它们对应的格式串各不相同（%d / %u / %PRId64）。
 * 打上这个标记之后，**格式串写错会在编译期报错**，而不是在运行期打印出
 * 一串垃圾。这正是「类型纪律」那条要求能落地的机械保障。
 *
 * 用 __GNUC__ 门控：非 GCC/Clang 的编译器上退化成空宏，代码仍然可编译，
 * 只是少一层检查。规范未禁止扩展，但仍保持可移植。
 */
#if defined(__GNUC__) || defined(__clang__)
#define IPC_PRINTF_LIKE(formatIndex, firstArgIndex)                            \
    __attribute__((format(printf, formatIndex, firstArgIndex)))
#else
#define IPC_PRINTF_LIKE(formatIndex, firstArgIndex)
#endif

/*
 * 全局出口的诊断输出。level 取 IpcLogLevel；moduleId 允许为 NULL
 * （配置加载阶段还没有模块身份）。
 *
 * 低于当前全局级别时本函数**立刻返回**，连 va_list 都不构造 —— 现场把级别
 * 调到 WARN 之后，DEBUG 日志对 select 线程的开销是零，而不是「格式化完再丢」。
 */
void IpcLogGlobalEmit(int32_t level, const char *moduleId, const char *format, ...)
    IPC_PRINTF_LIKE(3, 4);

/*
 * 同上，但接收已经取好的 va_list。
 *
 * 存在的理由：带上下文的 IpcLogEmit() 需要把 va_list **原样透传**给宿主的
 * 回调，不能先自己格式化一遍（那会多一次拷贝，而且真实系统的日志接口本来
 * 就是 va_list 形态的）。于是分成「取参数」和「用参数」两层。
 *
 * 第三个参数是 0，表示「变参由 va_list 传入」而不是从第 N 个参数开始。
 */
void IpcLogGlobalEmitVa(int32_t level, const char *moduleId, const char *format,
                        va_list args) IPC_PRINTF_LIKE(3, 0);

/* 取出当前生效的全局级别（把「未设置」解析成默认值之后的结果）。 */
int32_t IpcLogGlobalLevel(void);

#endif /* IPC_LOG_H */
