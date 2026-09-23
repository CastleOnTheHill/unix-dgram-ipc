/*
 * Copyright (c) 2026 <版权方待定>
 * 许可协议：<待定>
 *
 * lab.h -- 测试用的临时实验目录。**仅供测试，不随交付物发布**。
 *
 * =====================================================================
 * 它解决的三个具体麻烦
 * =====================================================================
 *   1. 建端点需要绝对路径 + 真实属主 + flock —— 必须是个真实的文件系统目录，
 *      不能是内存里的假对象；
 *   2. 每个用例都要一份自己的配置表，且 uid 字段必须是**当前进程的 uid**
 *      （否则注册会在身份自检那一步失败，而那是另一个用例要测的东西）；
 *   3. 用完必须能清干净：socket 文件、锁文件、配置文件都要删掉，否则下一次
 *      运行的残留会被 ClearResidue 当成「上一实例的残留」处理，
 *      于是测试之间产生了隐式耦合。
 *
 * 清理只作用于**本文件自己创建的**目录（mkdtemp 生成的、前缀固定），
 * 不做任何递归删除，也不接受调用者传进来的任意路径。
 */
#ifndef IPC_TEST_LAB_H
#define IPC_TEST_LAB_H

#include <stddef.h>
#include <stdint.h>

/* 实验目录名前缀与根目录。两者一起构成「这个目录归测试所有」的判据。 */
#define IPC_LAB_ROOT "/tmp"
#define IPC_LAB_PREFIX "ipc-lab-"

/* 建一个实验目录。成功返回 0 并把绝对路径写进 outDir。 */
int32_t IpcLabCreate(char *outDir, size_t cap);

/* 覆盖写一个文本文件。成功返回 0。 */
int32_t IpcLabWriteFile(const char *path, const char *text);

/* <dir>/modules.conf */
void IpcLabConfPath(const char *dir, char *out, size_t cap);

/* <dir>/<module>.sock —— 端点路径，绝对且长度远小于 IPC_PATH_MAX。 */
void IpcLabSockPath(const char *dir, const char *module, char *out, size_t cap);

/*
 * 生成一份配置表文本：count 个模块，全部在同一个命名空间、uid 都是
 * 当前进程的 real uid。模块名由 modules 数组给出。
 * 返回写入的字节数（0 表示缓冲不够）。
 */
size_t IpcLabBuildConf(const char *dir, const char *ns, const char *const *modules,
                       int32_t count, char *out, size_t cap);

/*
 * 删除实验目录里的内容再删掉目录本身。
 *
 * 只删「自己建的那种目录」：路径必须位于 IPC_LAB_ROOT 下、末段以
 * IPC_LAB_PREFIX 开头。别的一律拒绝并打印一行说明 —— 这个函数只会被测试
 * 调用，但一个「接受任意路径然后递归删」的辅助函数迟早会有人拿它去删别的东西。
 */
void IpcLabRemove(const char *dir);

/* ------------------------------------------------------------------ */
/* 日志捕获                                                           */
/* ------------------------------------------------------------------ */

/*
 * 测试**不应该**靠肉眼看 stderr 判断「有没有打警告」。所以这里把库的日志
 * 出口接过来存进一个固定大小的环里，让断言能直接读：
 *
 *     IpcLabCaptureBegin();
 *     ... 做点会记日志的事 ...
 *     UTEST_ASSERT(IpcLabCaptureCount() >= 1);
 *     UTEST_ASSERT(IpcLabCaptureContains("no endpoint yet"));
 *
 * 环容量固定；满了丢最早的并计数，不扩容 —— 测试里的日志量本就很小，
 * 而一个会 malloc 的日志回调违反库对日志回调的约束（不得长阻塞、不得回调
 * 进本模块的其它接口）。
 */
#define IPC_LAB_CAPTURE_MAX 32

/* 开始捕获：清空环，并安装捕获出口、把级别开到 DEBUG。 */
void IpcLabCaptureBegin(void);

/* 取消捕获：恢复默认出口（WARN 及以上写 stderr）与默认级别。 */
void IpcLabCaptureEnd(void);

/* 把日志出口换成「什么都不做」，级别开到 ERROR。用于不想看任何输出的用例。 */
void IpcLabSilenceLog(void);

int32_t     IpcLabCaptureCount(void);
int32_t     IpcLabCaptureDropped(void);
int32_t     IpcLabCaptureLevel(int32_t index);
const char *IpcLabCaptureModule(int32_t index);
const char *IpcLabCaptureText(int32_t index);

/* 有没有**某一条**日志同时含 needleA 与 needleB（needleB 可传 NULL）。 */
int32_t IpcLabCaptureContains(const char *needleA, const char *needleB);

#endif /* IPC_TEST_LAB_H */
