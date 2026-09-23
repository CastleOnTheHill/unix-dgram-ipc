# 测试套件布局与纪律

本目录下的所有东西都**不是交付物**。交付物只有一个：`libipc.a`，它由
`src/` 下的 `.c` 生成，别的都不许进去。这条界线靠三层保证：

| 层 | 手段 | 谁在检查 |
|---|---|---|
| 构建规则 | `libipc.a` 的依赖里只有 `src/*.c` | `Makefile` |
| 目录 | 参考宿主在 `tests/support/`，不在 `include/ipc/` | 目录结构本身 |
| 符号表 | 翻 `libipc.a` 导出符号，出现 `IpcRefHost*` / `utest` / `main` 就报错 | `make check-separation` |

---

## 一、白盒与黑盒的分界线

这不是一个说法，是一个可以机械检查的事实：

| | 白盒 `tests/unit/` | 黑盒 `tests/tools/` + `tests/integration/` |
|---|---|---|
| 允许包含 | `ipc/ipc.h`、`ipc_internal.h`、`src/*.h` | **只** `ipc/ipc.h` + `refhost.h` |
| 能看到 | 内部结构体、内部函数、统计字段、线格式编码器 | 只有公开契约与统计快照 |
| 进程模型 | 单进程，同 uid | 多进程，可真跨 uid |
| 能不能构造畸形输入 | 能（用 `IpcProtoEncode` 手工造头） | 不能（只能发库产出的合法报文） |
| 覆盖率 | 主要贡献者 | 补充（进程边界、flock、跨 uid） |

`tests/tools/` 下的工具**只许**包含公开头文件 —— 这一条由
`.workbuddy/checks/zcc.sh` 里的一条 grep 检查（它同时也检查反向：
`tests/unit/` 里必须**确实**有文件在用内部头，否则「白盒」就只是个目录名）。

---

## 二、目录

```
tests/
  support/          # 测试用实现（既不进交付物，也不是测试本身）
    utest.h/.c      #   极简断言框架 + constructor 自动登记
    refhost.h/.c    #   参考宿主：独立 select 线程 + 回调线程池 + 事件注册表
    lab.h/.c        #   临时实验目录 + 日志捕获环
  unit/             # 白盒单测，产出 build/run_unit
    main.c          #   退出码三态：0 全过 / 1 有用例失败 / 2 没跑成
    test_util.c     #   工具函数（含 splitmix64 的独立参考值）
    test_proto.c    #   线格式编解码、手工构造的 cmsghdr
    test_config.c   #   配置解析规范
    test_pending.c  #   等待表（含三线程用例）
    test_log.c      #   日志出口与级别过滤
    test_lifecycle.c#   注册/注销/销毁/诊断访问器（真建 socket）
    test_forward.c  #   真收发：post/send/reply/broadcast/死锁检测/畸形输入
  tools/            # 黑盒测试用的独立进程（自带 main）
    ipc_testmod.c   #   可脚本化的模块进程，journal 出结论
  integration/      # 黑盒测试的 shell 层
    common.sh       #   记账 + JWait 有界轮询 + 进程包装
    lab_guard.sh    #   实验目录路径护栏（含自身对照自检）
    run_all.sh      #   入口，三态退出码
    t01..t07        #   各条用例
```

---

## 三、同步纪律（改集成测试前必读）

集成测试**只用两种**东西做同步：

1. **journal 行** —— `ipc_testmod` 每写一行就 flush，所以「行出现了」就等于
   「那件事已经发生了」。用 `JWait <file> <正则> <截止毫秒>` 有界轮询。
2. **进程退出** —— `WaitMod <pid> <截止毫秒>`。

**不许用固定 sleep 做判定。** 固定 sleep 会同时带来两种错误：快机器上白等，
慢机器上偶发失败；而且失败时看不出是「没发生」还是「没等到」。

要 sleep 只有一个合法理由：**被测的东西就是时间**（比如超时用例）。那种
情况在代码里要写明理由。

> 上一个项目踩过的坑：有界轮询必须盯住**断言实际读的那个完整表达式**，
> 不能只盯它的一项。有一个用例在「差 2 条、恰好等于队列深度」时就醒过来，
> 于是稳定假失败。`JWait` 的判据永远是调用方给的那条正则。

---

## 四、退出码

`run_all.sh` 是三态的，不允许含糊：

| 码 | 含义 |
|---|---|
| 0 | 全部用例通过 |
| 1 | 有用例失败 |
| 2 | **没跑成**（BLOCKED）：环境不满足（非 root、缺被测程序、护栏自检失败） |

为什么单给 BLOCKED 一个码：把「没测」报成「通过」是这个项目里最容易被自己
骗到的一件事。有了 2，CI 上会红；红的时候看一眼就知道是环境问题还是代码
问题。

非 root 时默认把整条套件判为 BLOCKED（退出 2），**不列出任何 pass**。
想只跑不需要 root 的那部分：

```bash
IPC_INTEG_NONROOT=1 bash tests/integration/run_all.sh
```

这时需要 root 的用例标 BLOCKED，整条套件仍然是 BLOCKED（退出 2）——
也就是说，部分运行**不会**被说成全部通过。

一个测试进程如果结束了却没打出 `RESULT <name> <verdict>` 行，算 **FAIL**：
「跑完了但没给结论」和「通过」在输出上必须能区分开。

---

## 五、必须跑在 Linux 原生文件系统上

```bash
make test                       # 单元 + 集成
make BUILD=build-asan OPT='-O1 -g -fsanitize=address,undefined' test
```

**绝不要在 `/mnt/c` 下跑集成测试。** drvfs 没有真实属主、没有真实 `flock`，
而这两样正好是好几条用例的判据（t05 靠 flock，t07 靠属主）。跑出来的
结果没有意义 —— `run_all.sh` 检测到 `IPC_LAB` 在 `/mnt/*` 下会直接拒绝。

`build/` 与 `build-asan/` **分树**，互不混用。改过代码之后 sanitizer 那一遍
必须重跑，不许引用上一次的结论。

---

## 六、⚠ 当前状态：这套测试**从未被执行过**

截至 2026-09-23：

- 单元测试与集成测试的源码**全部写完**，并且在 `-Werror` 下 **0 警告**通过
  编译、链接成功（用 ziglang 交叉编译到 `x86_64-linux-gnu` 验证的）；
- 但开发这台机器上 **跑不了 Linux 产物**：`wsl.exe` 被安全策略拦截，
  Windows 上没有任何 C 工具链，交叉编译出的 ELF 在本机无法执行。
- 因此 **「测试通过」和「覆盖率达标」这两句话目前无权说。**
  不许把「编译通过」当成「测试通过」，也不许把「设计上应该覆盖了」
  当成覆盖率数字。

要拿到真实结论，需要先在 Linux 原生文件系统上跑：

```bash
make test                                  # 0 警告 + 单元 + 集成
make coverage                              # 真实行覆盖率（门槛 80%，lcov）
make checkout-separation                   # 交付物分离性
BUILD=build-asan IPC_LAB=/opt/ipc-lab-asan \
    OPT='-O1 -g -fsanitize=address,undefined' make test
```

在此之前，任何报告里的「测试通过」都必须带着这条限制一起写。
