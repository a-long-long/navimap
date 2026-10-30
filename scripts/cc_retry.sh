#!/usr/bin/env bash
# cc_retry.sh —— 编译器的**重试包装器**（Windows/MSYS2 专用）。
#
# 用法（CMake 会这样调）：
#     cc_retry.sh <编译器> <编译参数...>
#
# ★ 为什么需要它（本机踩的坑，很隐蔽）：
#
#   这台机器上装了 Defender 实时防护（MsMpEng 常驻数百 MB）。它会**扫描**
#   新启动的 cc1plus.exe；扫描窗口内，cc1plus 会被**直接杀掉**，
#   于是 gcc 表现为：
#       退出码 1，stderr **一个字都没有**。
#   没有任何错误信息 ⇒ 看起来像"代码编译不过"，其实代码完全没问题。
#
#   证据（都实测过）：
#     · 项目里**原有**的 main_window.cpp（本次改动没碰过）同样编不过
#       ⇒ 排除"我的代码写错了"；
#     · cc1plus.exe **单独直接跑** 100% 成功 ⇒ 编译器本身没坏；
#     · 同一命令加 sleep 0.3 后连跑 10 次 **10/10 成功**
#       ⇒ 是时序/扫描窗口问题，不是参数问题；
#     · 参数、路径、工作目录、PATH 形式都逐项排除过。
#
#   所以：失败且**没有 stderr 输出** ⇒ 判定为"被杀"，重试；
#         失败且**有 stderr 输出** ⇒ 是真编译错误，立刻报出来（不重试掩盖）。
#   成功时原样透传 stdout/stderr 与退出码 —— 编译器的告警不能丢。
set -uo pipefail

if [ "${NM_CC_RETRY:-1}" = "0" ]; then
    exec "$@"
fi

MAX_TRY=8
ERR_FILE=$(mktemp 2>/dev/null || echo "/tmp/nm_cc_err.$$")
trap 'rm -f "$ERR_FILE"' EXIT

# ★ PATH 必须是 **MSYS 形式**（/d/msys64/ucrt64/bin 这种，用 ':' 分隔）。
#   实测：MSYS 形式 ⇒ gcc 正常；Windows 形式（D:\...，';' 分隔）⇒ gcc 静默失败。
#   原因是 gcc 派生 cc1plus 时会把 PATH 原样传下去；MSYS 形式的 PATH 里
#   每个条目都对应一个真实目录，Windows 的 DLL 搜索按 ';' 分隔、遇到 ':'
#   分隔的整串就当一个路径，找不到 DLL ⇒ 子进程启动即死、且不报错。
#   （MSYS 的 env 在 spawn 原生程序时会把 POSIX 形式的 PATH 转成 Windows
#    形式并追加 ';'，所以这里必须用 env 而不是 export。）
#
# ★★ PATH 里**绝不能放 Qt 的 bin 目录**（2026-10-25 实测，很隐蔽）：
#   D:\Qt\6.11.2\mingw_64\bin 里躺着另一套 MinGW 运行期 DLL
#   （libstdc++-6.dll / libgcc_s_seh-1.dll / libwinpthread-1.dll，版本对应
#     Qt 自带的旧 gcc）。它排在 ucrt64 前面时，**cc1plus 启动加载的是 Qt 的
#     libstdc++** ⇒ 与 gcc 16.2 的 ABI 不匹配 ⇒ 进程静默死掉：退出码 1、
#     stderr 一个字都没有（和"被 Defender 杀"表现一模一样，但重试 8 次全败）。
#     分辨方法：C 文件能编过、C++ 文件全灭 —— cc1 不需要 libstdc++，
#     cc1plus 需要，只有它踩坑。
#     实证：PATH=MSYS+usr+sys32+win ⇒ C/C++ 都过；PATH=QT+MSYS+… ⇒ C++ 全灭。
#   Qt 的 DLL 只在**运行** skedaddle.exe 时才需要，编译期一律不进 PATH。
MSYS_PATH="/d/msys64/ucrt64/bin:/usr/bin:/c/Windows/system32:/c/Windows"

for ((i = 1; i <= MAX_TRY; i++)); do
    if [ "$i" -gt 1 ]; then
        # 递增延时：让 Defender 的扫描窗口过去
        sleep "0.$((i * 3))"
    fi
    : > "$ERR_FILE"
    env PATH="$MSYS_PATH" "$@" 2>"$ERR_FILE"
    rc=$?
    if [ "$rc" = 0 ]; then
        # 成功：把 stderr 原样放回去（告警不能丢）
        cat "$ERR_FILE" >&2
        exit 0
    fi
    if [ -s "$ERR_FILE" ]; then
        # 真编译错误：立刻失败，别用重试掩盖
        cat "$ERR_FILE" >&2
        exit "$rc"
    fi
    # stderr 为空 ⇒ 进程被杀（Defender），重试
done

echo "cc_retry.sh: 编译重试 $MAX_TRY 次仍失败（stderr 为空，疑似 Defender 持续拦截）" >&2
exit 1
