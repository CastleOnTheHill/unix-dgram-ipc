#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
mkcheck.py —— 没有 make 的时候，对 Makefile 做结构检查。

为什么需要它
---------------------------------------------------------------------
本机的 Windows PATH 上没有任何 C 工具链，也没有 make（`command -v make`
为空），所以 Makefile 的改动**无法真的跑一遍**。可是 Makefile 里的错
（recipe 用空格缩进、.PHONY 里写了不存在的目标、`$(MAKE) ... 目标名` 里
目标名拼错）都只在 make 真的跑到那一行时才暴露 —— 属于「改完看起来没事、
下次别人跑才炸」那一类。

所以这里做六件不需要 make 也能做的事：
  1. recipe 缩进：紧跟规则行的行，必须是 TAB 开头。用空格是经典的
     "missing separator"，而且**只报一行**、看起来像别的问题。
  2. `.PHONY` 里声明的每个目标，必须有对应的规则行。拼错的目标名
     在 .PHONY 里是完全无声的。
  3. 规则里 `$(MAKE) ... <目标>` 递归调用的目标名，必须存在。
  4. 仓库里其它脚本（scripts/*.sh、tests/**/*.sh）调用的
     `make <目标>`，目标名必须存在 —— 这是最容易出的错：
     脚本里写了 `make integration`，而 Makefile 里叫 `integ`。
  5. 文档（**只扫围栏代码块**）里写的 `make <目标>`，同上。
     这一条真抓到过：`tests/README.md` 里写的是
     `make checkout-separation`，真实目标是 `check-separation`。
  6. `bash .../<脚本> <参数>` 里的参数 —— 仅当那个脚本会把参数原样转给
     make 时（靠脚本内容自己判断，不是硬编码文件名）。这一条也真抓到过：
     README 里写 `bash scripts/wsl-run.sh check`，而 `check` 不是目标。

它给不出什么
---------------------------------------------------------------------
给不出「make 能跑通」。变量展开、模式规则、条件分支一律不模拟。
它只是把「拼写与缩进」这一层的错抓出来。

为什么它在 scripts/ 里，而别的降级检查器不在
---------------------------------------------------------------------
`.workbuddy/checks/` 下的另外两个（zcc.sh 要 ziglang、symcheck.py 要
pycparser）依赖外部工具，所以不进仓库。这个只依赖 Python 3 标准库，
任何 clone 下来都能跑，所以留在仓库里。**只留一份** —— 本项目已经吃过
「同一个东西存两份、然后悄悄不一致」的亏（`include/ipc/ipc_refhost.h` 与
`tests/support/refhost.h` 就是这么出问题的）。

纪律：先证明它能报脏
---------------------------------------------------------------------
`--control` 模式用一组已知坏掉的 Makefile / 脚本断言它确实会报，
再用已知正常的断言它不乱报。
一个从不报错的检查比没有检查更糟 —— 本仓库的覆盖率门槛就是这么坏过一次
（awk 扫错的字段，永远 exit 0）。这个检查器自己也被对照组抓出过 4 个 bug，
其中一个是漏了 `re.MULTILINE`，导致每个脚本只有第一条 make 调用被检查。

用法
---------------------------------------------------------------------
    python3 scripts/mkcheck.py [Makefile] [--control] [-q]
"""

import os
import re
import subprocess
import sys

# ---------------------------------------------------------------- 解析

VAR_ASSIGN = re.compile(r"^[A-Za-z_][A-Za-z0-9_.]*\s*[:?+]?=")
DIRECTIVE = re.compile(r"^(ifeq|ifneq|ifdef|ifndef|else|endif|include|-include|"
                       r"sinclude|export|unexport|override|define|endef|vpath)\b")
RULE = re.compile(r"^([^\t#:=][^:=]*?)\s*:(?!=)")
# 行内注释：`#` 前面得有空白才算注释起点（`a#b` 不是注释）。
INLINE_COMMENT = re.compile(r"(?:^|\s)#.*$")
# 合法目标名的形状。见 is_target_word() 的说明。
TARGET_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_.\-]*$")


def strip_quotes(text):
    """把 '...' 与 "..." 里的内容换成空串，避免值里的空格被当成词边界。"""
    out = []
    i = 0
    n = len(text)
    while i < n:
        ch = text[i]
        if ch in "'\"":
            j = text.find(ch, i + 1)
            if j < 0:
                break
            out.append(" ")
            i = j + 1
        else:
            out.append(ch)
            i += 1
    return "".join(out)


def is_target_word(word):
    """这个词像不像一个 make 目标名？（用来滤掉选项、变量赋值、重定向）"""
    if not word:
        return False
    if word.startswith("-"):
        return False                       # 选项：-s、--no-print-directory
    if "=" in word or "<" in word or ">" in word:
        return False                       # VAR=value、重定向
    # 目标名必须是标识符形状。这一条同时兜掉了 `$` 开头的变量、
    # `/tmp/xxx.log` 这类路径、`2>`，以及散文里的中文词 —— 上一版就是因为
    # 没这道闸，报出了「调用了 `make /tmp/step_lib.log`」这种结论。
    return bool(TARGET_RE.match(word))


class Makefile(object):
    def __init__(self, path):
        self.path = path
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            raw = fh.read()
        self.lines = raw.split("\n")
        self.targets = {}        # 目标名 -> 行号（从 1 开始）
        self.phony = []          # [(目标名, 行号)]
        self.make_calls = []     # [(调用的目标名, 行号)]
        self.bad_indent = []     # [(行号, 行内容)]
        self.script_refs = []    # 脚本里引用到的目标名（用来判断这条检查查了多少）
        self.doc_refs = []       # 文档代码块里引用到的目标名
        self.forward_refs = []   # 转发脚本调用里的参数（也是 make 目标）
        self._parse()

    @staticmethod
    def _logical(lines):
        """把**非** recipe 行的续行（行尾反斜杠）拼成一条逻辑行。

        .PHONY 之类的声明经常跨行写，不拼起来就会把行尾那个 '\\' 当成
        一个目标名 —— 这正是第一版报出来的假问题。
        recipe 行不参与拼接（它们只在里面找 $(MAKE)，逐行看更简单）。
        """
        out = []
        buf = None
        buf_start = 0
        for idx, line in enumerate(lines):
            lineno = idx + 1
            if buf is not None:
                buf += " " + line.strip()
                if not line.rstrip().endswith("\\"):
                    out.append((buf_start, buf))
                    buf = None
                continue
            if not line.startswith("\t") and line.rstrip().endswith("\\"):
                buf = line.rstrip()[:-1]
                buf_start = lineno
                continue
            out.append((lineno, line))
        if buf is not None:
            out.append((buf_start, buf))
        return out

    def _parse(self):
        # expect_recipe：下一行**应该**是一条命令（因为上一行是规则行）。
        # 这个标志必须记在「刚看到规则行」之后，而不是记在读取规则行的那一刻 ——
        # 第一版就是后者，于是规则行自己被判为「不是 recipe」，紧跟它的那条
        # 空格缩进行就永远查不出来。
        expect_recipe = False
        for lineno, line in self._logical(self.lines):
            if line.startswith("\t"):
                # 正常的 recipe 行
                expect_recipe = True
                self._scan_make_call(line, lineno)
                continue

            if line.strip() == "" or line.lstrip().startswith("#"):
                # 空行/注释：make 忽略它们，不打断 recipe 归属
                continue

            # 到这里：非 recipe、非空、非注释
            if expect_recipe and line[:1] == " ":
                # 上一行是规则行，这一行缩进了却不是 TAB —— 几乎可以肯定
                # 作者想写命令。make 会在这里报 'missing separator'。
                self.bad_indent.append((lineno, line))

            stripped = line.lstrip()
            # .PHONY: x y 长得像规则行（目标名是 `.PHONY`），必须在 RULE 之前
            # 截住，否则它会被当成一个目标注册，幽灵目标就查不出来了。
            if stripped.startswith(".PHONY") or stripped.startswith("PHONY"):
                self._parse_phony(line, lineno)
                expect_recipe = False
                continue

            if VAR_ASSIGN.match(line) or DIRECTIVE.match(line):
                expect_recipe = False
                continue

            m = RULE.match(line)
            if m:
                for name in m.group(1).split():
                    self.targets.setdefault(name, lineno)
                expect_recipe = True
            else:
                expect_recipe = False

    def _parse_phony(self, line, lineno):
        rhs = line.split(":", 1)[1] if ":" in line else ""
        for name in rhs.split():
            if name == "\\":
                continue
            self.phony.append((name, lineno))

    def _scan_make_call(self, line, lineno):
        """从 recipe 行里抠出 `$(MAKE) ... <目标>` 的目标名。"""
        text = strip_quotes(line)
        for m in re.finditer(r"\$\(MAKE\)", text):
            rest = text[m.end():]
            for word in rest.split():
                if not is_target_word(word):
                    continue
                self.make_calls.append((word, lineno))
                break   # 只取第一个像目标名的词


# `make` 后面跟的词的形状，已经由 is_target_word() 严格把关；这里只需要
# 确认 `make` 是一个**独立的词**（前面是空白或分隔符），而不是 `xmake`。
#
# 为什么允许「前面是空白」而不要求「前面是命令分隔符」：
# 本仓库的脚本用 `if Step "make lib" make lib > log; then` 这种写法 —— 真正
# 执行的 make 出现在显示串之后，前面只有空白。按「必须紧跟 ; & | (」的严格
# 规则，12 个脚本里只能提取到 2 个引用，也就是这条检查**几乎在空转**。
# 真正把散文挡在外面的是另外两道：先剥注释行，再把引号内的内容抹掉 ——
# 实测这两道之后，剩下能匹配到的就都是真的调用了。
#
# 已知残留局限：一句**没有引号、也不在注释里**的英文散文（例如
# `echo run a make target`）会被误报。这种输出人一眼能看出是假警报，
# 而漏报一个拼错的目标名要等别人跑才炸 —— 代价不对等，所以选了前者。
MAKE_CALL = re.compile(r"(?:^|[\s;&|()])make\s+([^\n;|&]*)", re.MULTILINE)

# 这个脚本是不是把命令行参数**原样转给 make**？
# 探的是内容而不是文件名：`make "${TARGETS[@]}"` / `make "$@"` 这两种写法
# 都算。scripts/wsl-run.sh 就是这种（它的 contract 是「参数 = make 目标」）。
FORWARD_TO_MAKE = re.compile(
    r"\bmake\s+(?:\"\$\{[A-Za-z_][A-Za-z0-9_]*\[@\]\}\"|\"\$@\")")

# 文档/脚本里的一次 `bash .../<脚本> <参数>` 调用。
SCRIPT_INVOKE = re.compile(r"\bbash\s+\S*/(\S+\.sh)\s+([^\n#|&;]*)")


def forwards_to_make(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()
    except OSError:
        return False
    return bool(FORWARD_TO_MAKE.search(text))


def script_invocations(text, forwarders):
    """从一段文本里找出「调用转发脚本」的地方，返回 [(脚本名, [参数])]。

    README 里写着 `bash scripts/wsl-run.sh check`，而 wsl-run.sh 会把 `check`
    原样交给 make —— 但 Makefile 里没有 `check` 这个目标（叫 `unit`）。
    照文档敲的人会直接撞上 "No rule to make target"。这一类「文档里的命令
    根本不存在」的错，和写错 make 目标名是同一件事，所以放在一起查。
    """
    out = []
    for m in SCRIPT_INVOKE.finditer(text):
        name, rest = m.group(1), m.group(2)
        if name not in forwarders:
            continue
        args = [w for w in rest.split() if is_target_word(w)]
        if args:
            out.append((name, args))
    return out


def strip_inline_comment(line):
    """去掉行内注释：`#` 前面有空白（或整行就是注释）才算注释起点。

    不剥它的话，`make all    # builds probes into ./build/bin` 里注释中的
    `builds` / `probes` / `into` 全会被当成目标名 —— 实测就是这么从
    probes/PROBE_NOTES.md 报出三个假问题的。
    `#` 紧贴前一个字符（`a#b`）不算注释，所以不能简单地按第一个 `#` 切。
    """
    return INLINE_COMMENT.sub("", line)


def shell_scannable_lines(path):
    """脚本里「可以被当成命令看」的行：去掉注释、抹掉引号内容
    （行内注释的判定见 strip_inline_comment）。"""
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            raw = fh.read().split("\n")
    except OSError:
        return []
    return [strip_inline_comment(strip_quotes(line)) for line in raw
            if not line.lstrip().startswith("#")]


def make_targets_referenced_by(path):
    """扫一个 shell 脚本，找出它调用的 `make <目标>`。"""
    found = []
    for line in shell_scannable_lines(path):
        for m in MAKE_CALL.finditer(line):
            for word in m.group(1).split():
                if is_target_word(word):
                    found.append(word)
    return found


def make_targets_in_markdown(path):
    """从 .md 的围栏代码块里提取 `make <目标>`。

    **只扫代码块**。正文里的 "make" 是散文，扫它必然一堆假警报
    （试过：README 里「在 $HOME 里 make」这种句子会命中）。围栏块里的
    内容按定义就是命令，所以这一层可以放心扫。

    加这一层是有具体收获的：`tests/README.md` 里写着
    `make checkout-separation`，而真实目标是 `check-separation` ——
    照文档敲的人会直接撞上 "No rule to make target"。

    遇到含 `mkcheck: skip-rest` 的行就停止扫描本文件。文档里那些「以下章节
    描述的是另一个分支 / 历史版本」的部分，就是这个标记的用途。
    """
    found = []
    for line in markdown_scannable_lines(path):
        for m in MAKE_CALL.finditer(line):
            for word in m.group(1).split():
                if is_target_word(word):
                    found.append(word)
    return found


SKIP_REST = "mkcheck: skip-rest"


def markdown_scannable_lines(path):
    """文档里「可以被当成命令看」的行：围栏代码块内部（排除块内注释和
    行内注释行），遇到 skip-rest 标记就停。"""
    lines = []
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            raw = fh.read().split("\n")
    except OSError:
        return lines
    inside = False
    for line in raw:
        if SKIP_REST in line:
            break
        if line.lstrip().startswith("```"):
            inside = not inside
            continue
        if inside and not line.lstrip().startswith("#"):
            lines.append(strip_inline_comment(strip_quotes(line)))
    return lines

# 这些 .md 描述的是**修复前**的状态，不随仓库发布（在 .gitignore 里）。
# 拿它们里面的旧目标名来报错只会制造噪音。有 git 的时候用 `git ls-files`
# 天然就排除了；没有 git（例如 rsync 出来的镜像里 .git 不在）时靠这张表。
UNSHIPPED_MD = ("REVIEW.md", "handoff.md")


def tracked_markdown(repo):
    """找出**随仓库发布**的 .md。

    优先问 git（`git ls-files '*.md'`）—— 那正好就是「会被人读到的文档」
    的定义，不用自己维护白名单。git 不可用时退回目录扫描，并排除
    UNSHIPPED_MD 里那几个历史文件。
    """
    try:
        out = subprocess.check_output(
            ["git", "ls-files", "*.md"], cwd=repo,
            stderr=subprocess.DEVNULL)
        files = [os.path.join(repo, ln.strip())
                 for ln in out.decode("utf-8", "replace").split("\n")
                 if ln.strip()]
        if files:
            return files
    except (OSError, subprocess.CalledProcessError):
        pass

    files = []
    for root, dirs, names in os.walk(repo):
        dirs[:] = [d for d in dirs
                   if d not in (".git", ".workbuddy", ".refs", "build",
                                "build-asan", "build-cov")]
        for name in sorted(names):
            if name.endswith(".md") and name not in UNSHIPPED_MD:
                files.append(os.path.join(root, name))
    return files


# ---------------------------------------------------------------- 检查

def check(makefile_path, scripts, docs=()):
    mk = Makefile(makefile_path)
    problems = []

    for lineno, line in mk.bad_indent:
        problems.append(
            "line %d: recipe 用空格缩进（make 会报 'missing separator'）: %r"
            % (lineno, line[:60]))

    for name, lineno in mk.phony:
        if name not in mk.targets:
            problems.append(
                "line %d: .PHONY 声明了 %r，但整个文件里没有这个目标的规则"
                "（拼错的目标名在 .PHONY 里是无声的）" % (lineno, name))

    for name, lineno in mk.make_calls:
        if name not in mk.targets:
            problems.append(
                "line %d: 递归调用 $(MAKE) %s —— 没有这个目标" % (lineno, name))

    for script in scripts:
        for name in make_targets_referenced_by(script):
            mk.script_refs.append(name)
            if name not in mk.targets:
                problems.append(
                    "%s: 调用了 `make %s` —— Makefile 里没有这个目标"
                    % (script, name))

    for doc in docs:
        for name in make_targets_in_markdown(doc):
            mk.doc_refs.append(name)
            if name not in mk.targets:
                problems.append(
                    "%s: 代码块里写了 `make %s` —— Makefile 里没有这个目标"
                    % (doc, name))

    # 再来一层：`bash .../<转发脚本> <参数>` 里的参数，其实就是 make 目标。
    # 只有**会把参数转给 make** 的脚本才这么算（靠脚本内容自己判断）。
    forwarders = {}
    for s in scripts:
        if forwards_to_make(s):
            forwarders[os.path.basename(s)] = s
    for src in list(scripts) + list(docs):
        lines = (shell_scannable_lines(src) if src.endswith(".sh")
                 else markdown_scannable_lines(src))
        for script_name, args in script_invocations("\n".join(lines),
                                                    forwarders):
            for a in args:
                mk.forward_refs.append(a)
                if a not in mk.targets:
                    problems.append(
                        "%s: 写了 `bash %s %s` —— %s 会把参数原样转给 make，"
                        "而 Makefile 里没有 %r 这个目标"
                        % (src, script_name, a, script_name, a))

    return mk, problems


# ---------------------------------------------------------------- 对照组

CONTROL_BAD = """\
# 对照：已知坏掉的 Makefile
.PHONY: all clean ghost
all: lib
lib:
\techo lib
clean:
    rm -rf build
recurse:
\t$(MAKE) nosuchtarget
"""

CONTROL_GOOD = """\
# 对照：已知正常的 Makefile
BUILD ?= build
.PHONY: all clean clean-all recurse
all: lib
lib:
\t@echo lib
clean:
\trm -rf $(BUILD)
clean-all:
\trm -rf build build-asan build-cov
recurse:
\t$(MAKE) --no-print-directory clean
\t$(MAKE) COV_INFO=/tmp/x.info cov-threshold
cov-threshold:
\t@echo threshold
"""


def run_control(tmpdir):
    """先证明检查器能报脏，再证明它不乱报。"""
    os.makedirs(tmpdir, exist_ok=True)
    fails = []

    bad = os.path.join(tmpdir, "Makefile.bad")
    with open(bad, "w", encoding="utf-8") as fh:
        fh.write(CONTROL_BAD)
    mk, problems = check(bad, [])
    joined = "\n".join(problems)
    expect_bad = [
        ("空格缩进的 recipe", "missing separator"),
        (".PHONY 里的幽灵目标", "ghost"),
        ("不存在的递归目标", "nosuchtarget"),
    ]
    for label, needle in expect_bad:
        if needle not in joined:
            fails.append("坏 Makefile：漏报了「%s」（找不到 %r）" % (label, needle))

    good = os.path.join(tmpdir, "Makefile.good")
    with open(good, "w", encoding="utf-8") as fh:
        fh.write(CONTROL_GOOD)
    mk, problems = check(good, [])
    if problems:
        fails.append("好 Makefile 竟然被报错：\n    " + "\n    ".join(problems))

    # 再证明「脚本调用 make」这条检查也能报脏，而且**不是空转**。
    #
    # 这段脚本是照着本仓库真实的写法造出来的：
    #   - `if Step "make X" make Y` 是 wsl-verify.sh 的惯用式，真正执行的是
    #     后面那个 Y；如果提取逻辑漏掉它，这条检查就形同虚设。
    #   - 注释里 / 引号里各埋一个假目标（`#   make nosuchcomment`、
    #     `echo "   make nosuchprose"`）。**引号里那个前面特意留了空格** ——
    #     不留空格的话，引号本身就会挡住匹配，那样即使没做剥引号也照样过，
    #     这个对照就白设了。
    #   - `make "${TARGETS[@]}"` 是 wsl-run.sh 的写法，目标名在变量里，
    #     提取不到是正常的，不该报错。
    scr = os.path.join(tmpdir, "script.sh")
    with open(scr, "w", encoding="utf-8") as fh:
        fh.write(
            "#!/bin/sh\n"
            "#   make nosuchcomment\n"
            "make integ\n"
            "make -s lib\n"
            "make BUILD=x nosuch\n"
            'echo "   make nosuchprose"\n'
            'if Step "make nosuchdisplay" make ghosttarget'
            " > /tmp/step.log 2>&1; then :; fi\n"
            'make "${TARGETS[@]}"\n')
    mk, problems = check(good, [scr])
    joined = "\n".join(problems)
    for needle in ("integ", "nosuch", "ghosttarget"):
        if needle not in joined:
            fails.append("脚本里的坏目标 %r 漏报了" % needle)
    for needle in ("nosuchcomment", "nosuchprose", "nosuchdisplay", "lib"):
        if needle in joined:
            fails.append("脚本里的 %r 被误报（注释/引号里的散文没被剥掉？）"
                         % needle)
    # 顺带证明「提取到 0 个引用」这件事会被发现 —— 脚本里明明有 5 处 make。
    if len(mk.script_refs) < 4:
        fails.append("只提取到 %d 个目标引用，明显偏少（提取逻辑坏了？）"
                     % len(mk.script_refs))

    # 再证明「文档围栏代码块」这一层也能报脏，而且**不扫正文**。
    # 正文那句 `make nosuchprose` 是故意留的诱饵：如果不区分代码块与正文，
    # 它一定会被误报。
    md = os.path.join(tmpdir, "doc.md")
    with open(md, "w", encoding="utf-8") as fh:
        fh.write("# 标题\n"
                 "\n"
                 "正文里提到 make nosuchprose 这句不该被当成命令。\n"
                 "\n"
                 "```bash\n"
                 "make lib\n"
                 "make all        # builds nosuchinline into ./build/bin\n"
                 "make nosuchmd\n"
                 "```\n"
                 "\n"
                 "```\n"
                 "make anotherbad\n"
                 "```\n")
    mk, problems = check(good, [], [md])
    joined = "\n".join(problems)
    for needle in ("nosuchmd", "anotherbad"):
        if needle not in joined:
            fails.append("文档代码块里的坏目标 %r 漏报了" % needle)
    for needle in ("nosuchprose", "nosuchinline", "lib"):
        if needle in joined:
            fails.append("文档里的 %r 被误报（正文/行内注释没被排除？）"
                         % needle)

    # skip-rest 标记：标记之后的代码块不该再被扫。
    doc2 = os.path.join(tmpdir, "doc2.md")
    with open(doc2, "w", encoding="utf-8") as fh:
        fh.write("```\nmake lib\n```\n"
                 "<!-- mkcheck: skip-rest -->\n"
                 "```\nmake nosuchafter\n```\n")
    mk, problems = check(good, [], [doc2])
    joined = "\n".join(problems)
    if "nosuchafter" in joined:
        fails.append("skip-rest 标记之后的代码块不该再被扫")
    if "lib" in joined:
        fails.append("skip-rest 之前的合法目标 lib 被误报")

    # 「文档里的命令根本不存在」：`bash .../fwd.sh <参数>`，而 fwd.sh 会把
    # 参数原样转给 make。
    fwd = os.path.join(tmpdir, "fwd.sh")
    with open(fwd, "w", encoding="utf-8") as fh:
        fh.write('#!/bin/sh\nTARGETS=("$@")\nmake "${TARGETS[@]}"\n')
    if not forwards_to_make(fwd):
        fails.append("forwards_to_make 没认出转发脚本（wsl-run.sh 那种写法）")
    doc3 = os.path.join(tmpdir, "doc3.md")
    with open(doc3, "w", encoding="utf-8") as fh:
        fh.write("```bash\n"
                 "bash scripts/fwd.sh lib\n"
                 "bash scripts/fwd.sh nosuchforward\n"
                 "```\n")
    mk, problems = check(good, [fwd], [doc3])
    joined = "\n".join(problems)
    if "nosuchforward" not in joined:
        fails.append("转发脚本的坏参数漏报了")
    if "lib" in joined:
        fails.append("转发脚本的合法参数 lib 被误报")

    if fails:
        print("== mkcheck --control：失败")
        for f in fails:
            print("!! " + f)
        return 1
    print("== mkcheck --control：通过")
    print("   坏 Makefile -> 3 类问题全部报出（缩进 / 幽灵目标 / 递归目标）")
    print("   好 Makefile -> 0 问题")
    print("   脚本：坏目标（含 Step 惯用式里的）报出；")
    print("         注释里 / 引号里 / 变量里的散文不误报")
    print("   文档：围栏代码块里的坏目标报出；正文里的散文不误报；")
    print("         skip-rest 标记之后的内容不再扫；")
    print("         `bash .../<转发脚本> <参数>` 里的坏参数报出")
    return 0


# ---------------------------------------------------------------- 入口

def main(argv):
    args = [a for a in argv[1:]]
    if "--control" in args:
        args.remove("--control")
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            return run_control(os.path.join(tmp, "mk"))
    quiet = False
    if "-q" in args:
        args.remove("-q")
        quiet = True

    # 兜底路径：本脚本在 <repo>/scripts/ 下，所以往上两级就是仓库根。
    repo_guess = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    if args:
        mk_path = args[0]
    elif os.path.isfile("Makefile"):
        mk_path = "Makefile"
    else:
        mk_path = os.path.join(repo_guess, "Makefile")
    if not os.path.isfile(mk_path):
        print("!! 找不到 %s" % mk_path)
        return 2
    # 脚本从 Makefile 所在的目录去找 —— 这样 `make -C <dir>` 和
    # 从仓库根直接跑都成立。
    repo = os.path.dirname(os.path.abspath(mk_path))

    scripts = []
    for sub in ("scripts", os.path.join("tests", "integration")):
        d = os.path.join(repo, sub)
        if not os.path.isdir(d):
            continue
        for name in sorted(os.listdir(d)):
            if name.endswith(".sh"):
                scripts.append(os.path.join(d, name))

    # 文档只收**随仓库发布**的那些（见 tracked_markdown 的说明）。
    docs = tracked_markdown(repo)

    mk, problems = check(mk_path, scripts, docs)

    if scripts and not mk.script_refs:
        # 脚本一个引用都没提取到，多半是提取逻辑坏了（而不是脚本真的
        # 不调 make）。这种情况必须吭声，否则这条检查是空转的。
        # 注意这一条在 -q 下也要跑：它是「检查是否空转」的检查本身。
        problems.append(
            "扫了 %d 个脚本却一个 make 目标引用都没提取到 —— "
            "提取逻辑可能坏了，这条检查等于没查" % len(scripts))

    if not quiet:
        print("== mkcheck: %s" % os.path.relpath(mk_path, repo))
        print("   目标 %d 个，.PHONY 声明 %d 个，递归 $(MAKE) 调用 %d 处" %
              (len(mk.targets), len(mk.phony), len(mk.make_calls)))
        print("   扫了 %d 个脚本 / %d 个 .md，提取到 %d + %d + %d 个 make 目标引用"
              % (len(scripts), len(docs), len(mk.script_refs),
                 len(mk.doc_refs), len(mk.forward_refs)))

    if problems:
        print("!! 发现 %d 个问题：" % len(problems))
        for p in problems:
            print("   - " + p)
        return 1

    if not quiet:
        print("   recipe 缩进 / .PHONY / 递归目标 / 脚本目标 / 文档目标 /"
              " 转发脚本参数 —— 6 类检查全过")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
