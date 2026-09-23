/*
 * Copyright (c) 2026 <版权方待定>
 * 许可协议：<待定>
 *
 * 本仓库尚未添加 LICENSE 文件，落地前需要补齐《OpenHarmony C 语言编程规范》
 * 规则 3.1 要求的「文件头注释必须包含版权许可」。
 */
/*
 * ipc.h -- 基于 AF_UNIX SOCK_DGRAM 的进程间通信传输层（对外唯一头文件）。
 *
 * =====================================================================
 * 一、本模块是什么
 * =====================================================================
 * 替换老系统中「AF_UNIX + SOCK_STREAM + 中心转发服务器」的通信底座，改为
 * 「AF_UNIX + SOCK_DGRAM + 模块间直连」。只做传输，不做业务。
 *
 * 本模块负责四件事：
 *   1. 安全注册：把本进程注册成配置表里的某个模块，拿到一个独占的
 *      数据报端点（配置查表 / UID 身份校验 / flock 独占 / 残留清理 /
 *      bind + 权限设置）。
 *   2. 发送：把业务数据编码成「定长大端报头 + 载荷」，直接投给目标模块的
 *      端点；提供异步 post、同步 send、广播 broadcast、回复 reply。
 *   3. 接收：从本端点收报文，逐条做凭据校验（SCM_CREDENTIALS）与协议校验，
 *      把**通过全部校验**的报文交给宿主注入的事件分发入口。
 *   4. 同步请求匹配：维护 REQ/REP 的等待表，保证多并发请求不串包、不错配。
 *
 * 本模块**明确不做**下面这些事，它们由接入方（老系统）的既有代码负责：
 *   - 不创建任何线程。没有 select 线程、没有回调线程池、没有自己的事件循环。
 *   - 不保存业务回调，不做事件分发。合法报文一律交给宿主注入的 dispatch。
 *   - 不解释 event 号的业务含义。
 *
 * 这样一来，本模块是纯「被动式传输层」：宿主问它要一个 fd 去 select，
 * 可读时回调它一次，它把该干的事干完再回来。线程模型完全由宿主决定。
 *
 * =====================================================================
 * 二、与老系统的接缝（这是本次改造的核心）
 * =====================================================================
 * 老系统已经固定好了三样东西，本次改造**原样复用**：
 *   (a) 一个独立的 select 线程；
 *   (b) 一个执行业务回调的线程池；
 *   (c) 回调注册表的保存逻辑与事件分发逻辑。
 *
 * 本模块只在这三样之间插了一层传输，接缝就是「两个函数 + 一个回调」：
 *
 *   ┌──────────────────────── 宿主（老系统） ────────────────────────┐
 *   │                                                                │
 *   │   (c) 回调注册表 + 事件分发   ←── IpcDispatchFunc（宿主注入）   │
 *   │            ▲                                                   │
 *   │            │                                                   │
 *   │   (a) 独立 select 线程                                         │
 *   │       select( IpcGetSelectFd(ctx) )                            │
 *   │            │ 可读                                              │
 *   │            ▼                                                   │
 *   │       IpcHandleReadable(ctx, maxCount)   ← 本模块收包+校验+分发 │
 *   │            │                                                   │
 *   │            └── dispatch 由宿主决定放哪跑 ──┐                    │
 *   │                                          ▼                     │
 *   │   (b) 回调线程池  ←── 业务回调在这里执行（可安全调 IpcSend）    │
 *   │                                                                │
 *   └────────────────────────────────────────────────────────────────┘
 *                                  ▲
 *                                  │ 本模块（传输层）
 *                                  ▼
 *                    内核 AF_UNIX  SOCK_DGRAM
 *
 * 线程归属的推论（很重要，评审时请重点看这一条）：
 *   - IpcGetSelectFd() 返回的 fd 归宿主 select 线程使用；本模块不碰它。
 *   - IpcHandleReadable() 必须在宿主那个独立 select 线程上调用。
 *   - 业务回调到底跑在 select 线程上还是线程池上，**由宿主的 dispatch 决定**；
 *     本模块不预设、不干预。
 *   - 若宿主的 dispatch 是「内联执行回调」，那么回调里调 IpcSend() 会让
 *     select 线程卡死、回复永远读不回来 —— 本模块能检测到这个错误用法并
 *     返回 IPC_ERR_DEADLOCK（见 IpcSend 的说明），不会静默死锁。
 *   - 若宿主的 dispatch 是「投递到线程池」，则回调在池线程执行，不受限制。
 *     这也是老系统本来的做法，所以正常路径下不会触发。
 *
 * =====================================================================
 * 三、命名与编码规范
 * =====================================================================
 * 遵循《OpenHarmony C 语言编程规范》（本地参照 .refs/OpenHarmony-c-coding-style-guide.md）：
 *
 *   类别                          风格             本模块示例
 *   ----------------------------  ---------------  ----------------------------
 *   函数 / 结构体 / 枚举 / 联合体  大驼峰           IpcRegister / IpcMessage
 *   变量 / 参数 / 结构体字段        小驼峰           maxPayload / peerUid
 *   宏 / 常量 / 枚举值             全大写 + 下划线   IPC_NAME_MAX / IPC_OK
 *   全局变量                       g_ 前缀          （本头文件不引入全局变量）
 *   文件名                        小写 + 下划线     ipc.h / ipc_ctx.c
 *
 * 两条已知的、有意为之的偏离，先说清楚免得评审时当成疏漏：
 *   1. 规范 §3 要求「使用英文进行注释」，本文件按项目要求改用中文注释。
 *      两种风格不要混用；若将来要回归规范，整文件一起改。
 *   2. 规范 §2.4 要求返回类型与函数名同行，本文件对较长的函数签名做了
 *      换行并对齐参数。
 *
 * =====================================================================
 * 四、兼容性纪律（沿用上一版的结论，不要放松）
 * =====================================================================
 * 写这套代码时**没有拿到老框架的任何源码或头文件**。因此：
 *   - 老接口兼容性始终是「未验证」，任何报告、PPT、汇报都不得写成已验证。
 *   - 本文中标了「【假设】」的地方都是推断出来的接口形状，拿到老头文件后
 *     必须逐条核对并回改。
 *   - 本文末尾「附：对接待确认清单」列出了需要老系统提供方回答的全部问题。
 */
#ifndef IPC_IPC_H
#define IPC_IPC_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/* 1. 编译期上限与常量                                                */
/* ================================================================== */

/*
 * 下面四个宏不是「建议值」，而是**线上报头的字段宽度**。
 * 改它们等于改协议格式，收发双方必须同步改，否则解析必错。
 */
#define IPC_NAME_MAX            32       /* 模块标识：31 字符 + NUL */
#define IPC_NS_MAX              16       /* 命名空间：15 字符 + NUL */
#define IPC_PATH_MAX            108      /* socket 路径，取自 sockaddr_un.sun_path */
#define IPC_HDR_SIZE            112      /* 串行化报头字节数，固定宽、无填充、大端 */

#define IPC_PAYLOAD_HARD_MAX    65536u   /* 单条载荷硬上限 64 KiB，配置写更大也压到这个值 */
#define IPC_PAYLOAD_DEFAULT     8192u    /* 运行时默认载荷上限 8 KiB */

/* 协议版本。接收端拒绝版本号不匹配的报文，不尝试向后兼容解析。 */
#define IPC_PROTOCOL_VERSION    1u

/* 配置文件默认路径（IpcModuleOptions.confPath 传 NULL 时使用）。 */
#define IPC_CONF_DEFAULT        "/etc/ipc-modules.conf"

/*
 * 模块注册参数的全零初始化宏。
 * 全零 == 全部取默认值，可以放心使用，不要手工逐个字段赋值。
 */
#define IPC_MODULE_OPTIONS_INIT { 0 }

/* ================================================================== */
/* 2. 返回码                                                          */
/* ================================================================== */

/*
 * 全部错误码为负值，IPC_OK 为 0。
 * 函数返回类型写 int 而不是本枚举，是为了让老系统的适配层可以直接用 int
 * 承接返回值、按 <0 / ==0 判断，不必引入新类型。
 */
typedef enum {
    IPC_OK                  = 0,     /* 成功 */
    IPC_ERR_INVAL           = -1,    /* 参数非法：空指针、非法枚举、名字超长 */
    IPC_ERR_NOMEM           = -2,    /* 内存分配失败 */
    IPC_ERR_IO              = -3,    /* 系统调用失败：socket/bind/sendto/recvmsg/fcntl */
    IPC_ERR_AGAIN           = -4,    /* 对端接收队列满，消息**未入队**；可稍后重试 */
    IPC_ERR_NOENT           = -5,    /* 配置表里查无此模块 */
    IPC_ERR_OFFLINE         = -6,    /* 模块在配置表里，但当前没有活着的端点 */
    IPC_ERR_PERM            = -7,    /* 权限不足：UID 不匹配、端点文件模式/属主异常 */
    IPC_ERR_BUSY            = -8,    /* 模块已被其它进程注册（flock 被持有） */
    IPC_ERR_CRED            = -9,    /* 内核凭据缺失、被截断，或与配置的授权 UID 不符 */
    IPC_ERR_PROTO           = -10,   /* 报文格式非法：magic/版本/长度/类型校验失败 */
    IPC_ERR_TIMEOUT         = -11,   /* 等待回复超时，仅 IpcSendTimeout 会返回 */
    IPC_ERR_STOPPED         = -12,   /* 上下文已停止（IpcRequestStop 之后） */
    IPC_ERR_DEADLOCK        = -13,   /* 在 dispatch 内联线程里调 IpcSend，会死锁，提前拒绝 */
    IPC_ERR_MSGSIZE         = -14,   /* 载荷超过 maxPayload，**发送动作之前**即拒绝 */
    IPC_ERR_CONFIG          = -15,   /* 配置解析失败：条目重复、路径重复、身份歧义 */
    IPC_ERR_TOOMANY         = -16,   /* 并发同步请求槽位用尽（maxPending） */
    IPC_ERR_STATE           = -17    /* 生命周期阶段用错，例如没注销就销毁上下文 */
} IpcResult;

/*
 * 返回码转可读字符串。纯诊断用，日志和测试打印用；
 * 传入未知值时返回 "unknown"，不返回 NULL。
 */
const char *IpcResultToString(int result);

/* ================================================================== */
/* 3. 报文类型                                                        */
/* ================================================================== */

/*
 * 报文分三种。POST 是单程；REQ / REP 构成一问一答，靠 reqId 关联。
 */
typedef enum {
    IPC_MSG_TYPE_POST = 1,   /* 即发即忘，不等回复 */
    IPC_MSG_TYPE_REQ  = 2,   /* 同步请求，发送方在 IpcSend 里等回复 */
    IPC_MSG_TYPE_REP  = 3    /* 对某条 REQ 的回复，只有本模块内部会构造 */
} IpcMsgType;

/* ================================================================== */
/* 4. 静态模块表（配置）                                              */
/* ================================================================== */

/*
 * 配置表的一行：谁（命名空间 + 模块标识）在哪个路径上、以什么 UID 服务。
 *
 * 命名空间（ns）用于在同一台机器上隔离多套部署，本设计保留：
 * 同一个模块名可以出现在不同的命名空间里、互不影响；配置里的 (ns, moduleId)
 * 必须唯一，path 也必须唯一。
 */
typedef struct {
    char  ns[IPC_NS_MAX];          /* 命名空间，定长含 NUL */
    char  moduleId[IPC_NAME_MAX];  /* 模块标识，定长含 NUL */
    uid_t uid;                     /* 允许注册/服务该模块的 UID */
    char  path[IPC_PATH_MAX];      /* 该模块端点的绝对路径，定长含 NUL */
} IpcConfigEntry;

/* 不透明句柄，内部结构不对外暴露。 */
typedef struct IpcConfig IpcConfig;

/*
 * 从文件加载配置表。
 * 语法：每行 "<ns> <moduleId> <uid> <path>"；'#' 起头为注释行；空行忽略。
 * 重复的 (ns, moduleId) 或重复的 path 一律整表拒绝，返回 IPC_ERR_CONFIG。
 * 成功时 *outConfig 为新建对象，失败时为 NULL。
 *
 * 说明：这是**库自带的**配置解析。若老系统已有等价的模块名单加载逻辑，
 * 适配层可以直接改用老逻辑，本组接口不必强绑。
 */
int IpcConfigLoad(const char *path, IpcConfig **outConfig);

/*
 * 从内存字符串解析，语义与 IpcConfigLoad 完全一致。
 * 存在的主要理由是单元测试和工具，不想依赖磁盘上的文件。
 */
int IpcConfigParse(const char *text, IpcConfig **outConfig);

/* 释放配置对象。传 NULL 安全返回。 */
void IpcConfigDestroy(IpcConfig *config);

/* 表内条目数；config 为 NULL 时返回 0。 */
int IpcConfigGetCount(const IpcConfig *config);

/* 按下标取条目，越界返回 NULL。 */
const IpcConfigEntry *IpcConfigGetEntry(const IpcConfig *config, int index);

/* 按 (ns, moduleId) 正查，找不到返回 NULL。 */
const IpcConfigEntry *IpcConfigFindModule(const IpcConfig *config,
                                          const char *ns,
                                          const char *moduleId);

/*
 * 按路径反查是哪个模块拥有它，找不到返回 NULL。
 * 收报文时用它反查对端 —— 但注意：反查结果只用来「查配置、查授权 UID」，
 * **不能**当作对端身份证明；身份证明只有内核给的 SCM_CREDENTIALS。
 */
const IpcConfigEntry *IpcConfigFindByPath(const IpcConfig *config,
                                          const char *path);

/* ================================================================== */
/* 5. 报文视图与宿主分发入口                                          */
/* ================================================================== */

/* 不透明句柄，先声明，供 dispatch 签名使用。 */
typedef struct IpcContext IpcContext;

/*
 * 交给宿主的报文视图。
 *
 * 生命周期约定（必须遵守，否则就是悬垂指针）：
 *   - 结构体本身在栈上，随便用；
 *   - ns / src / dst / data 指向库内部缓冲，**只在 dispatch 本次调用期间有效**；
 *   - 要把数据留下来，必须自己 memcpy 一份；
 *   - opaque 是库内部用的回复上下文，业务侧不要读写。
 *
 * 注意：src 是报文**自称**的来源，它已经过了内核凭据校验（peerUid 与配置里
 * src 对应的授权 UID 一致）才会被交到这里，所以可以信任；但不要再拿 src 去
 * 做别的授权判断，授权判断一律以 peerUid 为准。
 */
typedef struct {
    const char *ns;          /* 命名空间 */
    const char *src;         /* 发送方模块标识（已通过凭据校验） */
    const char *dst;         /* 目标模块标识，即本上下文注册的模块 */
    uint32_t    event;       /* 业务事件号，含义由业务层定义 */
    IpcMsgType  type;        /* POST / REQ / REP */
    uint64_t    reqId;       /* 请求序号，仅 REQ/REP 有意义 */
    uint64_t    instanceId;  /* 发送方进程实例代际号，用于拒绝上一实例的陈旧回复 */
    const void *data;        /* 载荷，仅本次调用期间有效 */
    size_t      len;         /* 载荷长度 */
    uid_t       peerUid;     /* 内核填的发送方 real UID，不可伪造 */
    pid_t       peerPid;     /* 内核填的发送方 PID，仅供日志诊断 */
    void       *opaque;      /* 库内部用，业务侧勿动 */
} IpcMessage;

/*
 * 宿主事件分发入口。本模块收到报文、做完**全部**校验（凭据 + 协议 + 长度）
 * 之后调用它，把业务事件交给宿主已有的注册表和分发逻辑。
 *
 * 参数：
 *   ctx      —— 收到该报文的本模块上下文。一个进程注册多个模块时用它区分；
 *               也是宿主在回调里做 IpcReply / IpcPost 的入口。
 *   message  —— 报文视图，只在本次调用期间有效。
 *   hostUser —— 注册时由 IpcModuleOptions.dispatchUser 原样带回。
 *
 * 返回值：IPC_OK 表示宿主已接手；返回负值会被计入统计 dispatchFailed，
 *         但**不影响**库继续处理后续报文（丢弃策略由宿主自己决定、自己计数）。
 *
 * 执行线程：由宿主实现决定。老系统的做法是投递到线程池，本模块不干预。
 *
 * 允许在这个函数里调用：IpcReply（仅 REQ）、IpcPost、IpcBroadcast、IpcSend。
 *   —— 其中 IpcSend 只有在线程池里执行时才安全；若宿主把 dispatch 写成
 *      「在本线程内联调回调」，则 IpcSend 会返回 IPC_ERR_DEADLOCK 而不是死锁。
 */
typedef int (*IpcDispatchFunc)(IpcContext *ctx, const IpcMessage *message,
                               void *hostUser);

/* ================================================================== */
/* 6. 统计                                                            */
/* ================================================================== */

/*
 * 统计快照。全部是只增计数器，没有瞬时值。
 * 发送侧和接收侧口径分开，便于回答「发送成功」和「交付成功」这两个不同问题。
 * 取快照是结构体拷贝，不是原子一致快照，只用于日志和测试断言。
 */
typedef struct {
    uint64_t sendAttempts;      /* 尝试发送的目标次数（post + send + broadcast 展开） */
    uint64_t sendEnqueued;      /* sendto 成功 */
    uint64_t sendFailed;        /* 发送失败，且原因不是「对端离线」 */
    uint64_t broadcastTargets;  /* 广播展开后的目标总数 */
    uint64_t broadcastSkipped;  /* 广播中被跳过的离线目标 */
    uint64_t recvRead;          /* 从端点读到的报文数 */
    uint64_t recvRejected;      /* 进 dispatch 之前被丢弃的总数 */
    uint64_t recvRejCred;       /*   其中：凭据缺失/截断/与配置 UID 不符 */
    uint64_t recvRejProto;      /*   其中：报头或长度非法 */
    uint64_t recvRejTrunc;      /*   其中：被截断（含接收方 maxPayload 配小的情况） */
    uint64_t recvDelivered;     /* 已交给宿主 dispatch */
    uint64_t dispatchInvoked;   /* dispatch 调用次数 */
    uint64_t dispatchFailed;    /* dispatch 返回负值的次数 */
    uint64_t replySent;         /* 回复已发出 */
    uint64_t replyMatched;      /* 回复成功匹配上等待中的请求 */
    uint64_t replyUnmatched;    /* 回复无人认领（迟到、来源不符、实例代际不符） */
    uint64_t pendingRejected;   /* IpcSend 因等待表满被拒 */
    uint64_t pendingTimeout;    /* IpcSendTimeout 超时次数 */
    uint64_t eagainCount;       /* 发送时观察到对端队列满的次数 */
    uint64_t deadlockProbes;    /* IpcSend 被死锁检测提前拦下的次数 */
} IpcStatistics;

/* ================================================================== */
/* 7. 注册参数与生命周期                                              */
/* ================================================================== */

/*
 * 注册参数。只有 moduleId 必填，其余字段留 0 / NULL 即取默认值；
 * 建议一律用 IPC_MODULE_OPTIONS_INIT 初始化后再覆盖需要的字段。
 */
typedef struct {
    const char *moduleId;       /* 【必填】本进程要注册的模块标识 */

    /*
     * 【假设】命名空间。传 NULL 表示「按 moduleId 自动推导」：在配置表里
     * 只找到唯一一处同名 moduleId 时取它的 ns；找到多处则报 IPC_ERR_CONFIG
     * （歧义）。老系统若固定单命名空间，适配层直接写死即可。
     */
    const char *ns;

    /* 配置文件路径，NULL 表示 IPC_CONF_DEFAULT。 */
    const char *confPath;

    /*
     * 端点文件的属组，例如 "ipc-members"，用于跨 UID 组内互通。
     * 传 NULL 表示不显式改属组，沿用进程默认。
     * 注意：附加组不会自动决定新建文件的属组，所以这里要靠显式设置。
     */
    const char *groupName;

    /*
     * UID 校验的宽严程度。
     *   0（默认）—— real UID、effective UID、配置里的授权 UID 三者必须一致；
     *   1         —— 只校验 effective UID（为 setuid 场景放宽）。
     * 【假设】老系统原先用哪个，需要拿到源码后核对，见文末 Q6。
     */
    int allowUidSplit;

    /*
     * 载荷上限。这是**发送方上限**：超过自己这个值会在上线之前直接返回
     * IPC_ERR_MSGSIZE。
     *
     * 接收方的缓冲区是 IPC_HDR_SIZE + 它自己的 maxPayload，所以接收方配得
     * 更小时会**静默丢弃**大报文（计入 recvRejTrunc），而发送方仍然看到
     * IPC_OK。因此「同一命名空间内所有模块的 maxPayload 必须一致」是
     * **部署级约束**，不是调优项。取 0 表示 IPC_PAYLOAD_DEFAULT；
     * 超过 IPC_PAYLOAD_HARD_MAX 会被压到硬上限。
     */
    uint32_t maxPayload;

    /* 并发同步请求槽位数（同时允许多少个 IpcSend 在等回复）。0 表示默认 64。 */
    int maxPending;

    /* 发送缓冲 SO_SNDBUF，0 表示用内核默认。 */
    int sendBufSize;

    /*
     * 接收缓冲 SO_RCVBUF，0 表示用内核默认。
     * 【实测已证】它**不**控制 AF_UNIX 数据报的接收队列深度：容量跟**发送方**
     * 的 SO_SNDBUF 走，按 skb truesize 计费。所以调这个值不要期待
     * 「能顶住更多在途报文」。详见 probes/PROBE_NOTES.md。
     */
    int recvBufSize;

    /* 广播是否包含自己。0（默认）不包含。 */
    int broadcastIncludeSelf;

    /*
     * 【正式接入时必填】宿主事件分发入口。传 NULL 表示本模块不做任何业务
     * 分发（只收发、只做同步请求匹配）——自测时可以这么用，正式接入不行。
     */
    IpcDispatchFunc dispatch;

    /* 原样回传给 dispatch 的第三个参数，用来关联宿主的注册表对象。 */
    void *dispatchUser;
} IpcModuleOptions;

/*
 * 注册成为 options->moduleId 指定的模块，并建立一个独占的数据报端点。
 *
 * 一次调用完成整条建立流程，任何一步失败都完整回滚，不留半成品：
 *   查配置 → 校验身份（real / effective / 配置 UID）→ 加非阻塞独占锁（flock）
 *   → 持锁检查并处理残留路径 → 建非阻塞 + CLOEXEC 的 DGRAM socket
 *   → 打开 SO_PASSCRED → bind → 显式设置属组与模式 → 回读校验
 *
 * 安全边界（不要误解，也不要向别人夸大）：
 *   - 库内的 UID 自检只是「防误用」，**不是安全边界**；
 *   - 真正的强制保护来自两处：目录/文件权限，以及接收端的凭据校验；
 *   - 同 UID 的进程之间互相信任，不要求同 UID 内再做模块级隔离。
 *
 * 成功时 *outContext 为新建上下文，失败时为 NULL。
 * 同一进程可以对同一个库实例注册多个不同模块，各自独立。
 */
int IpcRegister(const IpcModuleOptions *options, IpcContext **outContext);

/*
 * 优雅注销：停止接活 → 了结挂起的同步请求 → 关 fd → **持锁状态下**
 * unlink 本实例的端点文件 → 放锁。
 *
 * 幂等且可并发调用：第一个调用者真正执行拆除，其余调用者阻塞到拆除完成
 * 再返回 IPC_OK。所以「它返回了」对每个调用者都意味着资源已经释放。
 *
 * **本函数不释放内存。** 等所有线程都不再可能引用这个上下文之后，
 * 再调 IpcDestroy() 释放。
 *
 * 【实现注意，上一版在这里踩过坑】早期版本在本函数里 free(ctx)，导致连调
 * 两次就是 use-after-free（ASan 复现过）。守卫必须是「在对象还活着的时候
 * 同步等待」，不能只靠一个标记位。
 */
int IpcUnregister(IpcContext *ctx);

/*
 * 释放上下文内存。只能在 IpcUnregister() 完成之后调用；否则返回
 * IPC_ERR_STATE 并且什么都不释放。调用后指针即失效。
 */
int IpcDestroy(IpcContext *ctx);

/* ================================================================== */
/* 8. 接收侧：交给宿主的 select 线程驱动                              */
/* ================================================================== */

/*
 * 返回本端点用于 select()/poll() 的 fd。
 *
 * 该 fd 是非阻塞、CLOEXEC 的数据报 socket。宿主那个独立的 select 线程
 * 直接把它加进自己的 fd_set / pollfd 数组即可。
 * 本模块**不持有**这个 fd 的线程归属，也**不会**自己去读它。
 *
 * 返回 >=0 为 fd，负值为错误码（上下文非法或已注销）。
 */
int IpcGetSelectFd(const IpcContext *ctx);

/*
 * 端点可读时的处理函数：把当前已经排队的报文读干净（全程非阻塞），
 * 逐条做凭据校验与协议校验，合法报文交给宿主的 dispatch。
 *
 * 在哪里调用：宿主那个独立 select 线程，在 IpcGetSelectFd() 返回的 fd
 * 可读时调用。**必须在 select 线程上调用**，不要在业务线程上随便调。
 *
 * maxCount：本次最多处理多少条。<=0 表示不限（一路读到 EAGAIN）。
 *   担心在 select 线程上滞留过久时，可以给一个上限（例如 64）。
 *
 * 返回值：
 *     >=0 —— 本次成功交给宿主的报文条数；
 *     <0  —— 错误码。IPC_ERR_STOPPED 表示已请求停止，宿主应当收摊；
 *            其它负值表示端点出了故障（例如 fd 被外部关掉）。
 *
 * 剩余报文：若返回值 == maxCount 且 maxCount > 0，说明可能还有剩余。
 * 对 level-triggered 的 select/poll 不用管，下一次 select 会立刻返回；
 * 只有在边沿触发（EPOLLET）的宿主上才需要再调一次。
 *
 * 本函数**不阻塞**：读不到就返回 0，不会等在那里。
 */
int IpcHandleReadable(IpcContext *ctx, int maxCount);

/*
 * 请求停止：上下文转入停止态，不再接受新的发送，并把所有正在 IpcSend /
 * IpcSendTimeout 里等回复的线程唤醒（它们返回 IPC_ERR_STOPPED）。
 *
 * 可由任意线程调用，异步安全。**它不会去动宿主的线程**——让 select 线程
 * 退出、让线程池收摊，是宿主的责任（本模块没有那些线程）。
 *
 * 幂等。重复调用返回 IPC_OK。
 */
int IpcRequestStop(IpcContext *ctx);

/* 已请求停止或端点已失效返回 1，正常工作中返回 0。只读查询。 */
int IpcIsStopped(const IpcContext *ctx);

/* ================================================================== */
/* 9. 发送侧                                                          */
/* ================================================================== */

/*
 * 异步发送，不等业务回复。
 *
 * 契约：**绝不阻塞**（内部所有发送路径使用 MSG_DONTWAIT）。
 *   - IPC_OK           —— 报文已交给内核；
 *   - IPC_ERR_AGAIN    —— 对端队列满，消息**未入队**，调用方自行决定是否重试；
 *   - IPC_ERR_OFFLINE  —— 目标当前没有活着的端点；
 *   - IPC_ERR_NOENT    —— 目标不在配置表里；
 *   - IPC_ERR_MSGSIZE  —— 载荷超过 maxPayload，**在发送动作之前**就拒绝。
 *
 * 注意：IPC_OK 只代表「内核收下了」，不代表「对端业务处理了」。
 * 想知道交付结果请用统计里的 recvDelivered，或改用 IpcSend。
 * 尤其注意：接收方 maxPayload 配小导致静默丢弃时，发送方仍然看到 IPC_OK。
 */
int IpcPost(IpcContext *ctx, const char *dstModuleId, uint32_t event,
            const void *data, size_t len);

/*
 * 同步发送，等回复。
 *
 * 沿用老接口的契约：**默认无限等待**，对端不回复就一直等。要时限请用
 * IpcSendTimeout()。
 *
 * replyBuf / replyCap 接收回复载荷；传 NULL 表示丢弃内容（回复仍然计入统计）。
 * outLen 可传 NULL。回复比 replyCap 长时按截断处理并计入统计。
 *
 * 关键契约（改实现时别丢，这几条都是踩过坑换来的）：
 *   1. 请求槽位在 sendto() **之前**就登记好，所以对端再快也丢不了回复；
 *      sendto() 失败则释放槽位并且不进入等待。
 *   2. 只有「来源模块正确」且「回显的 instanceId 与请求一致」的回复才被接受。
 *      这样对端重启后残留的陈旧回复、以及别的模块冒充的回复都会被拒绝
 *      （计入 replyUnmatched）。请求方自己重启也一样安全。
 *   3. 等待期间上下文被 IpcRequestStop() → 返回 IPC_ERR_STOPPED。
 *   4. 在 dispatch 内联线程上调用 → 返回 IPC_ERR_DEADLOCK，绝不静默死锁。
 *   5. 等待表满（maxPending）→ IPC_ERR_TOOMANY，且**不发送**。
 *
 * 谁负责把回复读回来：宿主的 select 线程。也就是说，调用本函数的业务线程
 * 会阻塞，但 select 线程必须还在跑；这正是老系统「select 线程 + 回调线程池」
 * 的结构天然满足的前提。
 */
int IpcSend(IpcContext *ctx, const char *dstModuleId, uint32_t event,
            const void *data, size_t len,
            void *replyBuf, size_t replyCap, size_t *outLen);

/*
 * 带时限的同步发送，其余语义与 IpcSend 完全一致。
 * timeoutMs < 0 等价于 IpcSend()（无限等待）；0 表示只探一次、不等待。
 * 超时返回 IPC_ERR_TIMEOUT。
 */
int IpcSendTimeout(IpcContext *ctx, const char *dstModuleId, uint32_t event,
                   const void *data, size_t len,
                   void *replyBuf, size_t replyCap, size_t *outLen,
                   int timeoutMs);

/*
 * 向本命名空间内的**所有**模块广播（按配置表的固定集合逐目标发送）。
 *
 * 不是原子操作，也不承诺所有目标同时成功：
 *   - 离线模块直接跳过并计数，**不会排队补发**；
 *   - 返回值 = 成功收下该报文的目的小数；
 *   - 参数非法返回负错误码；**部分成功不算错误**；
 *   - 跳过数看统计 broadcastSkipped。
 *
 * 不自动重试，避免重复业务副作用。
 *
 * 【实测已证的性质，属于设计性质、不是缺陷】拥塞是 sender-wide 的：
 * 一个停滞的对端会让发送方对**所有**目标返回 EAGAIN，而且 EAGAIN 出现在
 * 目的地解析之前，所以这种情况下 broadcastSkipped 保持 0，而每个目标都
 * 计入 eagainCount。引用这个行为时要说明清楚。
 */
int IpcBroadcast(IpcContext *ctx, uint32_t event, const void *data, size_t len);

/*
 * 回复一条收到的 IPC_MSG_TYPE_REQ。
 *
 * 必须在 dispatch 内、针对 message->type == IPC_MSG_TYPE_REQ 的报文调用，
 * 且每条报文**最多调一次**（第二次返回 IPC_ERR_STATE）。
 * 回复的目标地址取自称的 src（已通过凭据校验），并会回显对方的 instanceId。
 */
int IpcReply(const IpcMessage *message, const void *data, size_t len);

/* ================================================================== */
/* 10. 诊断访问器                                                     */
/* ================================================================== */

/*
 * 下面几个都是只读诊断接口，库内部不使用它们，删掉不影响任何功能。
 * 返回的字符串都指向上下文内部存储，上下文存活期间有效，不要 free。
 */
const char *IpcGetModuleId(const IpcContext *ctx);   /* 本模块标识 */
const char *IpcGetNamespace(const IpcContext *ctx);  /* 本命名空间 */
const char *IpcGetSocketPath(const IpcContext *ctx); /* 本端点路径 */
uint64_t    IpcGetInstanceId(const IpcContext *ctx); /* 本进程实例代际号 */

/* 取一份统计快照；outStatistics 为 NULL 时返回 IPC_ERR_INVAL。 */
int IpcGetStatistics(const IpcContext *ctx, IpcStatistics *outStatistics);

/* ================================================================== */
/* 附：报头线格式（协议文档，改这里必须同步改收发两端）                */
/* ================================================================== */
/*
 * 定长、显式大端、逐字段串行化。**故意不用裸 C struct**：填充字节、
 * 对齐和端序都会让双方解析不一致。
 *
 *  偏移  长度  字段
 *  ----  ----  -----------------------------------------------
 *     0     4  magic "UIPC"
 *     4     1  version（当前 1）
 *     5     1  type（IpcMsgType）
 *     6     1  flags（保留，必须为 0，非 0 一律拒绝）
 *     7     1  hdrSize（必须等于 IPC_HDR_SIZE）
 *     8    16  ns，NUL 填充
 *    24    32  src 模块标识，NUL 填充
 *    56    32  dst 模块标识，NUL 填充
 *    88     4  event
 *    92     4  payloadLen
 *    96     8  reqId
 *   104     8  sender instanceId
 *  ----  ----
 *        112
 *
 * 接收端拒绝：短头、magic 不符、版本不符、type 未知、flags 非 0、
 * hdrSize 不符、名字没有 NUL 结尾、名字为空、payloadLen 与实际长度不符、
 * 报文被截断（MSG_TRUNC / MSG_CTRUNC）。
 */

/* ================================================================== */
/* 附：对接待确认清单（拿到老系统源码/头文件后逐条核对并回改）        */
/* ================================================================== */
/*
 * 本文件里凡标了「【假设】」的地方都来自推断。以下问题需要老系统提供方回答：
 *
 *   Q1.  老系统的回调签名到底是什么？参数里有没有「来源模块」？
 *        —— 决定 IpcMessage 要给 dispatch 呈现哪些字段。
 *   Q2.  老系统的回调注册表是按 event 号索引，还是按 (module, event) 索引？
 *        —— 决定 dispatch 是否需要库把 src 也传进去（现已传）。
 *   Q3.  老系统的事件分发入口能不能被外部直接调用？还是必须走它的某个
 *        dispatch 函数？—— 决定适配层是薄适配，还是要抄一份分发逻辑。
 *   Q4.  老系统 select 线程的循环长什么样？它是否已经处理了「处理不完、
 *        下一轮继续」？—— 决定 IpcHandleReadable 的 maxCount 该给多少。
 *   Q5.  老系统的同步 send 是怎么关联请求与回复的？有没有它自己的 reqId
 *        语义需要我们沿用？—— 决定是否要暴露 reqId 分配钩子。
 *   Q6.  老系统判断 UID 用的是 real UID 还是 effective UID？
 *        —— 直接对应 allowUidSplit 的默认值。
 *   Q7.  老系统 post 在队列满时的语义是什么？返回错误，还是静默丢弃？
 *        —— 决定 IPC_ERR_AGAIN 是否要改成「静默丢弃 + 计数」。
 *   Q8.  老系统 broadcast 部分成功时的返回语义？返回成功目标数，还是 bool？
 *   Q9.  三套框架的模块编号、事件编号是否会冲突？单靠 namespace 是否够用？
 *        —— 决定要不要在 ns 之外再加一层「框架标识」。
 *   Q10. 最大消息长度、最大并发请求数、峰值通信量的实际数值是多少？
 *        —— 决定 maxPayload / maxPending 的默认值是否合理。
 *   Q11. 老系统的配置文件是什么格式？能否直接用我们的格式，还是要在适配层
 *        做一次转换？—— 决定 IpcConfig* 这一组是保留还是删掉。
 *   Q12. 老系统有没有自己的「模块上线/下线」通知？需要在 peer 掉线时回调
 *        宿主吗？—— 决定要不要加一个 OnPeerState 钩子。
 */

#ifdef __cplusplus
}
#endif
#endif /* IPC_IPC_H */
