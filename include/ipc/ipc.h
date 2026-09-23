/*
 * ipc.h -- AF_UNIX SOCK_DGRAM direct-connect IPC framework (public API).
 *
 * Prototype implementation of the design described in handoff.md:
 * modules talk straight to each other over Unix domain datagram sockets;
 * there is no central forwarding server.
 *
 * NOTE ON COMPATIBILITY: no source of the three legacy C IPC frameworks was
 * available when this prototype was written.  The four functions below
 * (register / post / send / broadcast) mirror the *documented* legacy
 * semantics only.  Everything else -- exact signatures, return codes,
 * callback threading, data ownership -- is an explicitly stated assumption
 * recorded in this header and in README.md.  Legacy compatibility is
 * therefore "not verified".
 */
/*
 * ======================== 中文注释说明（评审用） ========================
 * 本文件原有的英文注释全部保留，下面每一处中文注释是新增的逐项说明，
 * 目的是便于评审「哪些接口/字段可以删掉」。中文注释统一紧跟在其说明的
 * 声明之前，或直接追加在该行英文注释之后（用 ｜ 分隔）。
 * 若不需要中文，`git checkout include/ipc/ipc.h` 即可整体还原。
 *
 * 三条贯穿全文件的约定，先看这里再看细节：
 *  - 遗留接口只有 4 个：ipc_register / ipc_post / ipc_send / ipc_broadcast。
 *    其余全部是本原型新增的扩展（reply / 事件循环 / 统计 / 访问器），
 *    扩展可以删，删了不影响「兼容遗留」这个目标。
 *  - 凡是标注「遗留语义」的，都是为了对齐旧框架的文档行为而保留；
 *    标注「本原型新增」的，都是设计选择，砍起来没有兼容性代价。
 *  - 上限宏（IPC_NAME_MAX 等）不是建议值，是**线上报头的字段宽度**，
 *    动它们等于动协议格式。
 * ====================================================================
 */
#ifndef IPC_IPC_H
#define IPC_IPC_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Compile-time limits                                                 */
/* ------------------------------------------------------------------ */

/* Module identifier: 31 chars + NUL (fixed-width field in the wire header). */
/* 模块标识：31 字符 + NUL。这是线上报头的定长字段，改它等于改协议格式。 */
#define IPC_NAME_MAX 32
/* Framework namespace: 15 chars + NUL. */
/* 命名空间：15 字符 + NUL。用于把同一台机器上的多套部署隔离开。 */
#define IPC_NS_MAX 16
/* Transport path length (sizeof(((struct sockaddr_un *)0)->sun_path) is 108). */
/* socket 路径长度上限 108 字节，直接取自 sockaddr_un.sun_path，不可改。 */
#define IPC_SUN_PATH_MAX 108
/* Serialised header size.  Fixed width, no padding, big-endian. */
/* 串行化报头固定 112 字节、无填充、大端序。收发双方按此硬编码解析。 */
#define IPC_HDR_SIZE 112
/* Absolute hard cap for a payload; enforced even if config asks for more. */
/* 单条有效载荷的**硬上限** 64 KiB：配置里写更大也会被这个值压住。 */
#define IPC_PAYLOAD_HARD_MAX 65536u
/* Default runtime payload cap (bytes). */
/* 运行时默认载荷上限 8 KiB（注册时可用 opts.max_payload 覆盖，上不超硬上限）。 */
#define IPC_PAYLOAD_DEFAULT 8192u

/* 配置文件默认路径（注册时 opts.conf_path 传 NULL 就用它）。 */
#define IPC_CONF_DEFAULT "/etc/ipc-modules.conf"

/* ------------------------------------------------------------------ */
/* Error codes                                                         */
/* ------------------------------------------------------------------ */

/* 全部错误码都是负数，IPC_OK 为 0。共 18 个。 */
typedef enum {
    IPC_OK               = 0,
    IPC_ERR_INVAL        = -1,  /* bad argument ｜ 参数非法（空指针、非法枚举等） */
    IPC_ERR_NOMEM        = -2,  /* 内存分配失败 */
    IPC_ERR_IO           = -3,  /* 系统调用失败（sendto/socket/epoll 等） */
    IPC_ERR_AGAIN        = -4,  /* peer queue full / would block ｜ 对端队列满，消息**未入队** */
    IPC_ERR_NOENT        = -5,  /* module not present in the static table ｜ 配置表里查无此模块 */
    IPC_ERR_OFFLINE      = -6,  /* module known but has no live socket ｜ 模块存在但当前没有活 socket */
    IPC_ERR_PERM         = -7,  /* 权限不足（socket 文件模式/属主不匹配） */
    IPC_ERR_BUSY         = -8,  /* module already registered (lock held) ｜ 模块已被占用（flock 被人持有） */
    IPC_ERR_CRED         = -9,  /* missing / mismatching SCM_CREDENTIALS ｜ 内核凭据缺失或不匹配 */
    IPC_ERR_PROTO        = -10, /* malformed datagram ｜ 报文格式非法（报头/长度） */
    IPC_ERR_TIMEOUT      = -11, /* 等待回复超时（只有 ipc_send_timeout 会返回） */
    IPC_ERR_STOPPED      = -12, /* 上下文已停止 */
    IPC_ERR_DEADLOCK     = -13, /* ipc_send() re-entered from a handler ｜ INLINE 回调内调 ipc_send */
    IPC_ERR_MSGSIZE      = -14, /* 长度超过上限，**发送前**即拒绝 */
    IPC_ERR_CONFIG       = -15, /* 配置文件解析失败 / 条目重复 / 身份不匹配 */
    IPC_ERR_TOOMANY      = -16, /* pending-request table exhausted ｜ 并发同步请求槽位用尽 */
    IPC_ERR_STATE        = -17  /* API used in the wrong lifecycle state ｜ 生命周期阶段用错 */
} ipc_err_t;

/* 错误码转可读字符串。纯便利函数，删掉不影响功能。 */
const char *ipc_strerror(int rc);

/* ------------------------------------------------------------------ */
/* Message types and delivery classes                                  */
/* ------------------------------------------------------------------ */

/* 报文的三种类型。POST 是单程，REQ/REP 构成一问一答。 */
typedef enum {
    IPC_TYPE_POST = 1, /* fire and forget, no reply expected ｜ 即发即忘，不等回复 */
    IPC_TYPE_REQ  = 2, /* synchronous request, reply expected ｜ 同步请求，等待回复 */
    IPC_TYPE_REP  = 3  /* reply to IPC_TYPE_REQ ｜ 对 REQ 的回复 */
} ipc_msg_type_t;

/* 回调在哪个线程执行。这是本原型新增的设计选择，不是遗留需求。 */
typedef enum {
    /* Handler runs on the single receive thread.  Cheapest in memory and
     * preserves per-source ordering.  ipc_send() MUST NOT be called from a
     * handler: it is detected and fails with IPC_ERR_DEADLOCK instead of
     * deadlocking.  This is the default. */
    /* 【默认】回调在唯一的接收线程上同步执行：
     * 优点——零队列开销、同一来源天然保序；
     * 限制——回调里禁止调 ipc_send（会返回 IPC_ERR_DEADLOCK）。 */
    IPC_DISPATCH_INLINE = 0,
    /* Handler runs on one of `workers` bounded worker threads pulling from a
     * bounded queue.  ipc_send() is allowed from a handler.  A full queue
     * drops the message and bumps stats.cb_dropped. */
    /* 回调在 worker 线程池上执行：
     * 优点——回调里可以调 ipc_send；
     * 代价——同一来源可能乱序，且队列满时**静默丢消息**（只累加 cb_dropped）。 */
    IPC_DISPATCH_POOL   = 1
} ipc_dispatch_t;

/* ------------------------------------------------------------------ */
/* Static module table (config)                                        */
/* ------------------------------------------------------------------ */

/* 静态模块表的一行：谁（模块）在哪个路径上、以什么 UID 服务。 */
typedef struct {
    char     ns[IPC_NS_MAX];       /* 命名空间（定长，含 NUL） */
    char     module[IPC_NAME_MAX]; /* 模块标识（定长，含 NUL） */
    uid_t    uid;          /* UID authorised to register/service this module ｜ 允许注册/服务该模块的 UID */
    char     path[IPC_SUN_PATH_MAX]; /* 该模块 socket 的绝对路径（定长，含 NUL） */
} ipc_config_entry_t;

/* 不透明句柄：调用方拿不到内部结构。 */
typedef struct ipc_config ipc_config_t;

/* Parse "<ns> <module> <uid> <path>" lines, '#' comments, blank lines.
 * Duplicate (ns, module) or duplicate path => IPC_ERR_CONFIG. */
/* 从文件解析配置：每行 "<ns> <module> <uid> <path>"，# 开头为注释，空行忽略。
 * (ns,module) 重复或 path 重复都直接报 IPC_ERR_CONFIG。
 * NULL 路径要看下面那个函数 —— 这组配置接口主要是为了测试和工具。 */
int  ipc_config_load(const char *path, ipc_config_t **out);
/* 从内存字符串解析，语义同上。**本原型新增**，只为单元测试方便。 */
int  ipc_config_parse(const char *text, ipc_config_t **out); /* for tests */
/* 释放配置对象。 */
void ipc_config_free(ipc_config_t *cfg);

/* 表内条目数。 */
int  ipc_config_count(const ipc_config_t *cfg);
/* 按下标取条目，越界返回 NULL。 */
const ipc_config_entry_t *ipc_config_at(const ipc_config_t *cfg, int idx);
/* 按 (ns, module) 正查条目，找不到返回 NULL。 */
const ipc_config_entry_t *ipc_config_lookup(const ipc_config_t *cfg,
                                            const char *ns, const char *module);
/* Reverse lookup: which module owns this socket path?  NULL if none. */
/* 按路径反查是哪个模块拥有它。**本原型新增**：用于收报文时反查对端身份。 */
const ipc_config_entry_t *ipc_config_lookup_path(const ipc_config_t *cfg,
                                                 const char *path);

/* ------------------------------------------------------------------ */
/* Handler / message                                                   */
/* ------------------------------------------------------------------ */

/* 不透明句柄（实际定义在 ipc_internal.h）。 */
typedef struct ipc_msg ipc_msg_t;

/* 交给回调的报文视图。注意：里面所有指针都**只在回调期间有效**。 */
struct ipc_msg {
    const char    *ns;         /* 命名空间（指向上下文内部，勿存） */
    const char    *src;        /* sender module id ｜ 发送方模块标识 */
    const char    *dst;        /* target module id (== our own id) ｜ 目标模块（就是自己） */
    uint32_t       event;      /* 业务事件号，语义由业务层定义 */
    ipc_msg_type_t type;       /* POST / REQ / REP */
    uint64_t       req_id;     /* 请求序号；仅 REQ/REP 有意义 */
    uint64_t       instance_id;/* opaque generation id of the *sender* process ｜ 发送方进程的实例代际号 */
    const void    *data;       /* valid only for the duration of the call ｜ 载荷，仅回调期间有效 */
    size_t         len;        /* 载荷长度 */
    void          *opaque;     /* internal; do not touch ｜ 库内部用，勿动 */
};

/* Runs on the receive thread (INLINE) or a worker thread (POOL).
 * `data` is owned by the library and is only valid inside the call; copy it
 * if the handler needs to keep it.  Calling ipc_reply() at most once, and
 * only for messages with type == IPC_TYPE_REQ, is allowed. */
/* 业务回调签名。约束：
 *  - 执行线程：INLINE 模式=接收线程，POOL 模式=worker 线程；
 *  - msg->data 归库所有，**只在本次调用内有效**，要留就自己 memcpy；
 *  - 对 type == IPC_TYPE_REQ 的报文，允许调且只允许调一次 ipc_reply()。 */
typedef void (*ipc_handler_fn)(const ipc_msg_t *msg, void *user);

/* ------------------------------------------------------------------ */
/* Statistics                                                          */
/* ------------------------------------------------------------------ */

/* 统一计数快照。**本原型新增**（遗留接口没有统计）。
 * 全部是只增计数器，没有瞬时值；发送侧与接收侧口径分开。 */
typedef struct {
    uint64_t send_attempts;    /* post + send + broadcast target attempts ｜ 尝试发送的目标次数 */
    uint64_t send_enqueued;    /* sendto() succeeded ｜ sendto 成功 */
    uint64_t send_failed;      /* sendto() failed for a reason other than "peer offline" ｜ 失败（不含对端离线） */
    uint64_t broadcast_targets;/* 广播展开后的目标总数 */
    uint64_t broadcast_skipped;/* target offline / unreachable ｜ 广播里被跳过的离线目标 */
    uint64_t recv_read;        /* datagrams read off the socket ｜ 从 socket 上读到的报文数 */
    uint64_t recv_rejected;    /* dropped before the handler (see reasons) ｜ 进回调前被丢弃的总数 */
    uint64_t recv_rej_cred;    /*   SCM_CREDENTIALS missing or mismatched ｜ 凭据缺失/不匹配 */
    uint64_t recv_rej_proto;   /*   malformed header / size ｜ 报头或长度非法 */
    uint64_t recv_rej_trunc;   /*   MSG_TRUNC or MSG_CTRUNC ｜ 被截断（含接收方 max_payload 配小的情况） */
    uint64_t recv_delivered;   /* handed to a business handler ｜ 已交给业务回调 */
    uint64_t cb_invoked;       /* handler calls started ｜ 回调开始执行次数 */
    uint64_t cb_dropped;       /* dropped because the callback queue was full ｜ POOL 队列满丢弃 */
    uint64_t reply_sent;       /* 回复已发出 */
    uint64_t reply_matched;    /* REP completed a pending request ｜ 回复成功匹配上等待中的请求 */
    uint64_t reply_unmatched;  /* REP with no (matching) pending request ｜ 回复无人认领 */
    uint64_t pending_rejected; /* ipc_send() refused: table full ｜ 同步请求槽位已满被拒 */
    uint64_t eagain_count;     /* EAGAIN/ENOBUFS observed on send ｜ 发送时观察到队列满 */
    uint64_t deadlock_probes;  /* ipc_send() refused with IPC_ERR_DEADLOCK ｜ 死锁检测拦下次数 */
} ipc_stats_t;

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

/* 注册参数。字段较多，但只有 module 是必填，其余都有默认值。
 * 想删字段的话，最可疑的是下面标了「调优」的几个。 */
typedef struct {
    const char    *module;        /* this process' module id (required) ｜ 【必填】本进程的模块标识 */
    const char    *ns;            /* framework namespace; NULL means "the only
                                   * namespace in which this module name
                                   * occurs".  Ambiguous => IPC_ERR_CONFIG.  */
                                  /* 【调优】命名空间；NULL=自动推导，
                                   * 若同名模块出现在多个命名空间则报 IPC_ERR_CONFIG */
    const char    *conf_path;     /* NULL => IPC_CONF_DEFAULT ｜ 【调优】配置路径 */
    const char    *group;         /* shared group name for the socket file,
                                   * e.g. "ipc-members".  NULL => leave the
                                   * group as the process default.            */
                                  /* 【调优】socket 文件的属组，NULL=用进程默认属组 */
    int            allow_uid_split;/* 0 (default) => real UID, effective UID and
                                   * the configured UID must all agree.  1 =>
                                   * check the effective UID only.          */
                                  /* 【调优/安全】0=真实UID、有效UID、配置UID三者必须一致；
                                   * 1=只校验有效UID（为 setuid 场景放宽） */
    ipc_dispatch_t dispatch;      /* default IPC_DISPATCH_INLINE ｜ 回调派发模式 */
    int            workers;       /* POOL only, clamp 1..8, default 2 ｜ 【调优】仅 POOL，钳制 1..8 */
    int            cb_queue_max;  /* POOL only, default 64 ｜ 【调优】仅 POOL，回调队列深度 */
    int            max_pending;   /* concurrent ipc_send() slots, default 64 ｜ 【调优】并发同步请求槽位 */
    uint32_t       max_payload;   /* default IPC_PAYLOAD_DEFAULT.
                                   *
                                   * This is a SENDER-side limit: a send larger
                                   * than *our* value fails with
                                   * IPC_ERR_MSGSIZE before hitting the wire.
                                   * The receiver's buffer is
                                   * IPC_HDR_SIZE + its own max_payload, so a
                                   * receiver configured smaller silently drops
                                   * bigger datagrams (counted as
                                   * recv_rej_trunc) while the sender still
                                   * sees IPC_OK.  Treat "all modules in a
                                   * namespace agree on max_payload" as a
                                   * deployment-level requirement. */
                                  /* 【重要】这是**发送方上限**：超过自己这个值会在上线前
                                   * 直接返回 IPC_ERR_MSGSIZE。接收方的缓冲区是
                                   * IPC_HDR_SIZE + 它自己的 max_payload，所以接收方配得
                                   * 更小时会**静默丢弃**大报文（计入 recv_rej_trunc），
                                   * 而发送方仍然看到 IPC_OK。
                                   * 因此「同一命名空间内所有模块的 max_payload 必须一致」
                                   * 是部署级约束。 */
    int            broadcast_include_self; /* default 0 ｜ 广播是否包含自己 */
    int            sndbuf;        /* SO_SNDBUF, 0 => kernel default ｜ 【调优】0=内核默认 */
    int            rcvbuf;        /* SO_RCVBUF, 0 => kernel default
                                   * 【调优】注意：实测它**不**控制 AF_UNIX 接收队列深度，
                                   * 队列容量跟发送方 SO_SNDBUF 走，见 probes/PROBE_NOTES.md */
} ipc_register_opts_t;

/* 全零初始化宏。注意全零即"全部取默认值"，可以放心用。 */
#define IPC_REGISTER_OPTS_INIT { 0 }

typedef struct ipc_ctx ipc_ctx_t;

/* Legacy interface #1: become `module`.
 * Full lifecycle: config lookup, identity check, non-blocking exclusive lock,
 * residue handling, socket creation/bind/permission, epoll registration.
 * Any failure is rolled back completely. */
/* 【遗留接口 1/4】注册成为 module。一次调用完成整个建立流程：
 * 查配置 → 校验身份 → 加非阻塞独占锁 → 清理残留 → 建 socket/bind/设权限 →
 * 注册到 epoll。任何一步失败都会完整回滚（不留半成品）。 */
int ipc_register(const ipc_register_opts_t *opts, ipc_ctx_t **out);

/* Graceful teardown: stop accepting, resolve pending, close, unlink while
 * still holding the lock, release the lock.
 *
 * Idempotent and safe to call concurrently.  The first caller performs the
 * teardown; a second (or concurrent) caller blocks until it has finished and
 * then returns IPC_OK, so "it returned" always means "the resources are
 * released" -- for every caller, not just the first.
 *
 * This function does NOT free the context: a second call, or any accessor such
 * as ipc_get_stats(), would then be a use-after-free.  Release the allocation
 * with ipc_ctx_free() once no thread can still reference it. */
/* 【重要】优雅拆除：停止接收 → 了结挂起的同步请求 → 关 fd → **持锁状态下**
 * unlink socket 文件 → 放锁。
 *
 * 幂等且可并发调用：第一个调用者真正执行拆除，其余调用者会阻塞到拆除完成
 * 再返回 IPC_OK。所以「它返回了」对每个调用者都意味着资源已释放。
 *
 * **本函数不释放内存。** 要在没人再能引用它之后调 ipc_ctx_free()。
 * （早期版本在这里 free(ctx)，导致连调两次即 UAF，已修正。） */
int ipc_unregister(ipc_ctx_t *ctx);

/* Release a context's memory.  Only legal after ipc_unregister() has
 * completed on it; otherwise IPC_ERR_STATE is returned and nothing is freed.
 * The pointer is invalid afterwards. */
/* 【本原型新增】释放上下文内存。只能在 ipc_unregister() 完成之后调用，
 * 否则返回 IPC_ERR_STATE 且什么都不释放。调用后指针即失效。 */
int ipc_ctx_free(ipc_ctx_t *ctx);

/* 安装业务回调。可在收循环启动前后调用；一般注册完立刻设置。 */
int ipc_set_handler(ipc_ctx_t *ctx, ipc_handler_fn fn, void *user);

/* 以下 5 个是只读访问器。**本原型新增**，纯粹为了测试/日志/诊断，
 * 删掉不影响任何功能（库内部用不到它们）。 */
const char *ipc_module_id(const ipc_ctx_t *ctx);   /* 本模块标识 */
const char *ipc_namespace(const ipc_ctx_t *ctx);   /* 本模块命名空间 */
const char *ipc_socket_path(const ipc_ctx_t *ctx); /* 本模块 socket 路径 */
int         ipc_socket_fd(const ipc_ctx_t *ctx);   /* 本模块监听 fd */
uint64_t    ipc_instance_id(const ipc_ctx_t *ctx); /* 本进程实例代际号 */

/* ------------------------------------------------------------------ */
/* Legacy interfaces #2..#4                                            */
/* ------------------------------------------------------------------ */

/* Legacy interface #2: asynchronous send, no business reply awaited.
 * Non-blocking by contract: never blocks.  IPC_ERR_AGAIN means the peer queue
 * was full and the message was NOT enqueued; IPC_ERR_OFFLINE means the peer
 * has no live socket.  Message length is checked BEFORE anything is sent. */
/* 【遗留接口 2/4】异步发送，不等业务回复。
 * 契约规定**绝不阻塞**（内部所有发送路径用 MSG_DONTWAIT）；
 * IPC_ERR_AGAIN = 对端队列满且消息**未入队**；
 * IPC_ERR_OFFLINE = 对端没有活 socket；
 * 长度检查在任何发送动作**之前**完成。 */
int ipc_post(ipc_ctx_t *ctx, const char *dst, uint32_t event,
             const void *data, size_t len);

/* Legacy interface #3: synchronous send, waits for the reply.
 * Preserves the legacy contract of waiting indefinitely by default; use
 * ipc_send_timeout() for a bounded variant.  `reply_buf`/`reply_cap` receive
 * the reply payload; pass NULL to discard it (the reply is still counted).
 * `out_len` is optional.
 *
 * Contract details:
 *  - The request is registered in the pending table BEFORE sendto(), so a
 *    fast peer can never lose the reply.  If sendto() fails the slot is
 *    released and no wait happens.
 *  - A reply is accepted only if its source module AND echoed instance id
 *    match the request; stale replies from a previous instance of the peer,
 *    or replies from a wrong module, are rejected.
 *  - If the context is stopped while waiting, IPC_ERR_STOPPED is returned.
 *  - Called from an INLINE handler thread => IPC_ERR_DEADLOCK. */
/* 【遗留接口 3/4】同步发送，等回复。默认沿用遗留契约**无限等待**；
 * 要时限用 ipc_send_timeout()。reply_buf/reply_cap 收回复载荷，
 * 传 NULL 表示丢弃内容（回复仍计入统计）；out_len 可传 NULL。
 *
 * 契约细节（这些是关键，改实现时别丢）：
 *  - pending 槽在 sendto() **之前**就登记好，所以对端再快也丢不了回复；
 *    sendto() 失败则释放槽位并且不进入等待。
 *  - 只有「来源模块正确」且「回显的 instance_id 与请求一致」的回复才被接受；
 *    对端重启后残留的陈旧回复、或别的模块发来的回复，一律拒绝
 *    （计入 reply_unmatched）。
 *  - 等待期间上下文被 stop → 返回 IPC_ERR_STOPPED。
 *  - 在 INLINE 回调线程里调用 → IPC_ERR_DEADLOCK。 */
int ipc_send(ipc_ctx_t *ctx, const char *dst, uint32_t event,
             const void *data, size_t len,
             void *reply_buf, size_t reply_cap, size_t *out_len);

/* Same, but bounded.  timeout_ms < 0 behaves like ipc_send(). */
/* 【本原型新增】带时限版本，其余语义与 ipc_send 完全一致；
 * timeout_ms < 0 等价于 ipc_send()（即无限等待）。 */
int ipc_send_timeout(ipc_ctx_t *ctx, const char *dst, uint32_t event,
                     const void *data, size_t len,
                     void *reply_buf, size_t reply_cap, size_t *out_len,
                     int timeout_ms);

/* Legacy interface #4: send to every module in our namespace.
 * Not atomic: modules that are offline are skipped and counted, they are
 * never queued for later.  Returns the number of targets that accepted the
 * datagram, or a negative error if the arguments are invalid.  A partial
 * success is NOT an error.  Use ipc_get_stats() for the skip count. */
/* 【遗留接口 4/4】向本命名空间内**所有**模块广播。
 * 不是原子操作：离线的模块直接跳过并计数，**不会排队补发**。
 * 返回值 = 成功接收该报文的目的小数；参数非法才返回负错误码。
 * 部分成功**不算错误**；跳过数看 stats.broadcast_skipped。 */
int ipc_broadcast(ipc_ctx_t *ctx, uint32_t event, const void *data, size_t len);

/* Reply to a IPC_TYPE_REQ received by our handler.  May be called once per
 * message, from the handler.  Returns 0 on success. */
/* 【本原型新增，但遗留方案必然隐含此能力】回复收到的 IPC_TYPE_REQ。
 * 每条报文最多调一次，且必须在回调内调用；成功返回 0。 */
int ipc_reply(const ipc_msg_t *msg, const void *data, size_t len);

/* ------------------------------------------------------------------ */
/* Event loop                                                          */
/* ------------------------------------------------------------------ */

/* Blocking receive loop until ipc_stop()/ipc_unregister().  Spawns one
 * receive thread and (POOL) `workers` worker threads.  Must not be combined
 * with ipc_poll() on the same context.
 *
 * Returns IPC_OK for an ordinary stop, or a negative error when the receive
 * loop died on its own (epoll failure, out of memory, broken socket).  A
 * caller that cannot tell the two apart will treat a dead context as a clean
 * shutdown, so check the return value. */
/* 阻塞式接收循环，直到 ipc_stop()/ipc_unregister()。
 * 内部起 1 个接收线程（POOL 模式再加 workers 个 worker 线程）。
 * **不能**与 ipc_poll() 在同一个上下文上混用。
 *
 * 返回值必须检查：正常停止返回 IPC_OK；接收循环自己死掉
 * （epoll 出错、OOM、socket 损坏）返回负错误码。
 * 不查就会把「已经死了的上下文」当成优雅退出。 */
int ipc_run(ipc_ctx_t *ctx);

/* Single-process drain step: process everything currently queued without
 * blocking, plus at most `timeout_ms` of waiting if nothing was queued.
 * Returns the number of datagrams processed, or a negative error.
 * Intended for embedders and tests that own their own loop.
 *
 * NOTE: ipc_poll() and ipc_send()/ipc_send_timeout() cannot be mixed in the
 * same context.  ipc_send() needs a running ipc_run() loop (nobody else can
 * read the reply off the socket), while ipc_poll() refuses to run once that
 * loop is started.  A poll-driven embedder therefore has to use ipc_post()
 * plus a reply handler, or drive its own receive loop. */
/* 【本原型新增】单步排空：先把当前已排队的报文全部处理完（不阻塞），
 * 若一条都没有，最多再等 timeout_ms。返回值=处理条数，或负错误码。
 * 给「自己拥有主循环」的嵌入方和测试用。
 *
 * 注意：**ipc_poll() 与 ipc_send()/ipc_send_timeout() 不能在同一上下文混用。**
 * ipc_send 需要有 ipc_run 在跑（否则没人从 socket 上把回复读回来），
 * 而 ipc_poll 在该循环启动后就拒绝工作。自建循环的嵌入方只能用
 * ipc_post() + 回复回调，或者自己驱动接收循环。 */
int ipc_poll(ipc_ctx_t *ctx, int timeout_ms);

/* 请求停止并唤醒 ipc_run()；任意线程可调，异步安全。 */
int ipc_stop(ipc_ctx_t *ctx);   /* wakes ipc_run(); safe from any thread */
/* 1 once a stop was requested or the receive loop ended, 0 while running. */
/* 已请求停止或接收循环已结束返回 1，运行中返回 0。只读查询。 */
int ipc_is_stopped(const ipc_ctx_t *ctx);

/* 取一份统计快照（结构体拷贝，非原子一致快照）。 */
void ipc_get_stats(const ipc_ctx_t *ctx, ipc_stats_t *out);

#ifdef __cplusplus
}
#endif
#endif /* IPC_IPC_H */
