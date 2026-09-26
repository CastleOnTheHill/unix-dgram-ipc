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
    test_refhost.c  #   参考宿主自身：出队拷贝后名字指针必须重指（槽复用回归）
  tools/            # 黑盒测试用的独立进程（自带 main）
    ipc_testmod.c   #   可脚本化的模块进程，journal 出结论
                    #   --group <name> 把端点设成属组可写（0660）。跨 uid 用例
                    #   只能靠它：端点模式是库自己设的（不指定属组就是 0600），
                    #   把实验目录放开到 0777 完全不起作用 —— 目录权限管的是
                    #   「能不能在目录里建文件」，管不了「能不能往这个 socket 发送」。
  integration/      # 黑盒测试的 shell 层
    common.sh       #   记账 + JWait 有界轮询 + 进程包装
    lab_guard.sh    #   实验目录路径护栏（含自身对照自检）
    run_all.sh      #   入口，三态退出码
    t01..t08        #   各条用例
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

### 三类「看起来像实现 bug、其实是测试自己写错」的写法（2026-09-25 全部真踩过）

第一次把整套测试跑起来时，7 条用例里 4 条失败。逐条定性之后发现**没有一条
是实现错了** —— 全是下面三类写法。写新用例时对照着看：

1. **正则等的是收件方，而 journal 记的是来源。** journal 里 `RECV` 那一行是
   `RECV <来源模块> <event> <长度> <载荷>`，即 `RECV alpha 7 5 hello` 表示
   「alpha 发来的」。t01 写成 `JWait ... '^RECV beta'`，于是**结构上永远
   匹配不上**，每次都白烧一个完整截止；更坏的是它顺手把后面的断言推到了
   别的时刻，把下面两类问题一起盖住了。
2. **断言在对方已经退出之后才执行。** `ipc_testmod` 收摊时会 unlink 自己的
   端点，所以「端点文件还在不在」这类断言**必须落在对方的存活窗口内**。
   t01 把 `test -S beta.sock` 放在等 alpha 结束之后，而 beta 的脚本是靠
   `sleep 1500` 撑着的一个自我了断进程 —— 读到必然是「文件已经没了」。
   要么把判定挪进存活窗口，要么改成**结构性判据**（t01 改成从两条 `READY`
   行里取端点路径，断言「各自以自己模块名结尾且互不相同」）。顺带记一条：
   能打出 `READY` 就说明 `IpcRegister` 成功了，而它内部已经 lstat 回读校验过
   「这个路径确实是个 socket、模式与属主正确」（`src/ipc_io.c` 的
   `ApplyOwnership`）—— 库自己验过的证据比 shell 再 `-S` 一次更硬。
3. **读对方统计之前没等 `STATS` 行。** `JStatIs` 读的是「最后一行 STATS」。
   对方脚本是「`sleep N` 之后才 `stats`」时，早读拿到空值，报出来是
   「期望 1，实际 `<空>`」，看起来像计数错。t06 里本来就有这一步等待，
   t02 漏了、t01 漏了 —— 凡是读对方的统计，前面都要有
   `JWait <journal> '^STATS '`。

还有一条通用纪律：**载荷长度别写第二次字面量**。t05 期望
`RECV alpha 7 11 stillalive`，而 `stillalive` 是 10 字节 —— 断言的
「长度」和「载荷」各写一遍就一定会漂。改成载荷放进变量、长度用 `${#PAYLOAD}`
算出来，让它不可能对不上。

### 竞态用例：把「偶发」变成「必然」

一个只在负载下偶尔失败的用例没有判定力 —— 掷一次骰子不叫验证。
如果怀疑的是**丢唤醒 / 丢停止**这类调度竞态，用 `taskset -c 0` 把被测进程
钉到单核：创建者会一直跑到阻塞或时间片用完为止，新线程因此几乎总是晚一步
被调度，窗口就从「偶发」变成「必然」。t08 就是这么写的，它顺手把
「`Stop()` 早于 `Run()` 到达时停止请求被吃掉」这个 bug 从「偶发的
4s 内没结束」变成了 **40/40 稳定复现**。

所以涉及竞态的用例**必须真的重复多轮**，并且要在超时时**先取证再杀进程**
（`/proc/<pid>/task/*/wchan` 就够：一条停在 `core_sys_select`、其余停在
`futex` 等 join，就是丢停止的特征），否则失败报告里只有一句「没退出」，
下一个看的人还得从头猜一遍。

### 环境变量：骨架自己兜住，别指望调用方守规矩

实测踩过一次，而且代价很大：`Makefile` 的 `integration` 目标传的是
`IPC_TEST_BIN=$(BUILD)/bin`（**相对路径**），而 `StartMod` 是先 `cd` 到实验
目录再 `exec` —— 相对路径在 `cd` 之后就失效了。后果是**每个用例的 journal
全是空的**，连 `REGFAIL` 都没有，现象看起来像「模块起不来」；而真正的错误
（`exec` 找不到文件，退出码 127）被写进了 `<journal>.out`，断言根本不看那个
文件。整条套件 7/7 全红，报告里却只有一句「beta 没能进入 READY」。

之所以一直没发现，是因为手工跑时 shell 里 `export` 的是绝对路径。
**两边约定不一致时，必须由测试骨架自己抹平**：

- `common.sh` 在加载时就把 `IPC_TEST_BIN` 解析成绝对路径（按**当前**工作
  目录解析一次，之后怎么 `cd` 都不受影响）；
- `Makefile` 侧同时改用 `$(abspath ...)` —— 骨架兜住的只是最后一道。

同类的还有 `IPC_LAB`：文档与流水线都承诺「换一棵 lab root 就能把不同构建树
的实验目录分开」（ASan 那遍用 `/tmp/ipc-lab-asan`），而 `LabNew` 里写的是
`${1:-/tmp}`，于是**这个变量从来没生效过**。凡是文档承诺了的环境变量，
都该有一条能证明它真的生效的检查。

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

## 六、当前状态：整套测试**已真正跑过**（2026-09-26 最新一轮）

`scripts/wsl-verify.sh` 在 WSL2（Ubuntu 22.04 / 真 GCC 11.4.0 / root）里跑完
整条流水线，结论 **`PASS 9 / FAIL 0 / BLOCKED 0`**：

| 步骤 | 判定 | 结论 |
|---|---|---|
| preflight | PASS | `cc (Ubuntu 11.4.0-1ubuntu1~22.04.3)` / `uid=0` |
| lib | PASS | **0 warning / 0 error** |
| separation | PASS | `libipc.a` 里没有测试用实现 |
| coverage-selftest | PASS | 4 个对照里 1 个达标、3 个被正确拒绝 |
| unit-selftest | PASS | 挂起被判为失败（退出码 1） |
| unit | PASS | 118 / 118 |
| integration | PASS | 8 / 8（t01–t08） |
| coverage | PASS | **行覆盖率 1306/1493 = 87.47%**（门槛 80%） |
| asan | PASS | ASan + UBSan 无诊断（集成 8/8 也在该树下复跑过） |

结论原文归档在 `.workbuddy/verify/VERDICT-2026-09-25-allgreen-run1.txt`
（2026-09-25 首轮，114/114、1303/1490）与 `.workbuddy/verify/VERDICT-2026-09-26-FINAL.txt`
（2026-09-26 最新一轮，118/118、1306/1493）。

2026-09-26 那一轮新增的 4 条用例（`test_refhost` 的出队拷贝、`test_forward`
的跨上下文死锁检测与 maxCount 洪泛、`test_log` 的模块级级别消毒）**每一条都
做过「注回旧写法必须报红」的对照**：`.workbuddy/verify/mutation-check.sh`
把 4 处修复分别改回旧写法，要求恰好这 4 条用例变红、其余 114 条不受影响 ——
实测 `4 失败`，且失败名字与预期逐一对应。没有这步，新用例只是「看起来
被测过了」。

**「测试通过」「覆盖率达标」「交付物 0 警告」这三句话现在终于有资格说了。**
但下面几条限制必须跟结论一起写，不许省：

- **兼容性仍然「未验证」**：没有旧系统源码，无法验证与老发送方/接收方的
  线格式与语义兼容。这条不会因为测试变绿而变化。
- 覆盖率是 **`src/` 口径**（lcov 已剔除 `tests/` 与系统头），87.47% 是**行**
  覆盖率，不是分支覆盖率。
- 「本机不可运行」依然成立：上面所有结论**全部**来自 WSL 里的 Linux 原生
  文件系统；Windows 侧的 `build/` 没有可执行产物。
- 不许把「编译通过」当「测试通过」（这个坑踩过），也不许把「设计上应该
  覆盖了」当覆盖率数字。

重跑（需先在沙箱里放行 `wsl.exe`）：

```bash
wsl.exe -d Ubuntu-22.04 -u root -e bash \
    /mnt/d/code/unix_odmain_ipc/scripts/wsl-verify.sh
```

它会先把源码镜像到 Linux 原生 FS，再依次跑
构建 → 交付物分离性 → 门槛自检 → 单元 → 集成 → 覆盖率 → ASan，
每一步判成 `PASS` / `FAIL` / `BLOCKED` 写进 `VERDICT.txt` ——
**「没跑」不许写成 `PASS`**，任何一步 BLOCKED 都不算通过。

想手工分步跑：

```bash
make test                                  # 0 警告 + 单元 + 集成
make coverage                              # 真实行覆盖率（门槛 80%，lcov）
make check-separation                      # 交付物分离性
BUILD=build-asan IPC_LAB=/opt/ipc-lab-asan \
    OPT='-O1 -g -fsanitize=address,undefined' make test
```

> 本机没有 C 工具链时，`make dev-check` 会跑一组**只做静态判定**的降级
> 检查（用 ziglang 交叉编译 → 编译/链接/符号表/shell 语法/Makefile 结构），
> 每个都带能报脏的对照组。它给不出「测试通过」「覆盖率达标」——
> 那两句必须真的跑起来才有，两者不能互相替代。其中 Makefile 那一项是
> `scripts/mkcheck.py`（只依赖 Python 3 标准库），任何 clone 下来都能跑。
> ⚠ 注意降级检查器用的是 **clang（zig cc）**，它不实现 `-Wformat-truncation`、
> 认 `(void)` 抑制 `warn_unused_result` —— 对这两类诊断会**误报清零**。
> 它的结论只能叫「语法/可移植性冒烟通过」，权威门槛只认真 GCC。

> 本机没有 C 工具链时，`make dev-check` 会跑一组**只做静态判定**的降级
> 检查（用 ziglang 交叉编译 → 编译/链接/符号表/shell 语法/Makefile 结构），
> 每个都带能报脏的对照组。它给不出「测试通过」「覆盖率达标」——
> 那两句必须真的跑起来才有，两者不能互相替代。其中 Makefile 那一项是
> `scripts/mkcheck.py`（只依赖 Python 3 标准库），任何 clone 下来都能跑。
