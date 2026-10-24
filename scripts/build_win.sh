#!/usr/bin/env bash
#
# Windows(MSYS2) 下的构建脚本：绕开"PATH 里有多套 MSYS2 工具链 ⇒ DLL 冲突"
# 这个坑（详见下面"为什么"）。
#
# 用法:
#   ./scripts/build_win.sh            # 构建全部
#   ./scripts/build_win.sh -j4        # 指定并发
#   ./scripts/build_win.sh --clean    # 先删 build/ 再配置
#
# 为什么不能直接 `cmake --build build`：
#
#   ★ 这台机器的 PATH 里有 **不止一套 MSYS2 工具链**：
#       D:\msys64\ucrt64\bin   ← 我们要用的（gcc 16.2 / Qt6 匹配）
#       D:\mingw64\bin         ← 另一套（32 位或旧版本）
#     两套里的 DLL 同名（libgmp-10 / libmpfr-6 / libwinpthread-1 / zlib1 …）。
#     Windows 的 DLL 搜索按 PATH 顺序，**先找到的赢**。
#     当 ucrt64 排在前面时一切正常；一旦顺序颠倒（或 PATH 被别的工具改写），
#     cc1plus.exe 就会加载到**不兼容的 DLL**，于是：
#         编译器进程**静默死掉**：退出码 1，且 stderr 一个字都没有。
#     这跟"代码有语法错误"完全不同 —— 报错时 gcc 一定会说话。
#
#   怎么快速分辨：
#       手动跑同一条命令成功、ninja 里失败 ⇒ 不是代码问题，是环境（DLL 冲突）。
#       修法就是给构建进程一个**只含 ucrt64 的干净 PATH**。
#
#   本脚本还顺手做两件事：
#     ① Qt6 的 bin 目录要进 PATH（否则运行 navmap_qt 报缺 DLL）；
#     ② 并发降到 -j4：MSYS2 下 gcc 偶发启动失败（Defender 实时扫描新起的
#        cc1plus 进程时偶发），低并发能显著降低重试成本。
#
# ★ 为什么不能直接 `cmake --build build`：见下面 CLEAN_PATH 的注释 —— 本机
#   的 PATH 形式不对会让 gcc **静默失败**，而 MSYS 形式/Windows 形式的差别
#   只有实测才知道（详见 CLEAN_PATH 处）。
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD="$ROOT/build"

# ---- 受控 PATH ----------------------------------------------------------
# ★★ 必须用 **`env PATH=...`** 启动构建，**不能**用 `export PATH=...`。
#
#   原因（本机踩的坑，很隐蔽）：MSYS 的 runtime 在把环境传给**原生 Windows
#   程序**时，会对 PATH 做路径转换（`/d/msys64/ucrt64/bin` → `D:\msys64\ucrt64\bin`）。
#   而 ninja 在 Windows 上是通过 cmd.exe 执行编译命令的 ⇒ 编译器拿到的就是
#   转换后的 PATH ⇒ **gcc/g++ 静默失败**：退出码 1，stderr 一个字都没有。
#
#   证据链（都实测过）：
#     · cc1plus.exe **直接跑**成功（rc=0）；
#     · `env PATH=<MSYS 形式>` 跑 gcc：**12/12 成功**；
#     · `export PATH=<MSYS 形式>` 跑 gcc：**全部失败**（被转换成 Windows 形式）；
#     · 项目里**原有**的 main_window.cpp（没被本次改动碰过）同样编不过
#       ⇒ 排除"代码写错了"这个解释。
#
#   `env` 直接把字符串塞进子进程环境、**绕过**了 MSYS 的转换层，
#   于是整条链（cmake → ninja → cmd.exe → gcc → cc1plus）都在 MSYS 环境里跑。
#
#   注意：PATH 里不能有 /d/mingw64\bin（另一套 MSYS2 工具链，DLL 同名会抢）。
MSYS_BIN=/d/msys64/ucrt64/bin
QT_BIN=/d/Qt/6.11.2/mingw_64/bin
if [ ! -d "$MSYS_BIN" ]; then
    echo "找不到 $MSYS_BIN（MSYS2 UCRT64 工具链）" >&2
    exit 1
fi
CLEAN_PATH="$QT_BIN:$MSYS_BIN:/usr/bin:/c/Windows/system32:/c/Windows"

JOBS=4
CLEAN=0
for a in "$@"; do
    case "$a" in
        -j*) JOBS=${a#-j} ;;
        --clean) CLEAN=1 ;;
        *) echo "未知参数: $a" >&2; exit 2 ;;
    esac
done

if [ "$CLEAN" = 1 ]; then
    echo "=== 清理 $BUILD ==="
    rm -rf "$BUILD"
fi

if [ ! -f "$BUILD/CMakeCache.txt" ]; then
    echo "=== 配置（首次）==="
    # ★ 用 env 而不是 export：见 CLEAN_PATH 处的注释（PATH 转换会让 gcc 静默失败）。
    env PATH="$CLEAN_PATH" \
        /d/msys64/ucrt64/bin/cmake.exe -S "$ROOT" -B "$BUILD" \
        -DCMAKE_PREFIX_PATH=D:/Qt/6.11.2/mingw_64
fi

echo "=== 构建（-j$JOBS，受控 PATH）==="
# ★ 关键：env PATH=... 让 cmake/ninja/gcc 全程用**未转换**的 MSYS 形式 PATH。
#   ninja 在 Windows 上是通过 cmd.exe 执行编译命令的，cmd 继承的就是这个 PATH。
env PATH="$CLEAN_PATH" \
    /d/msys64/ucrt64/bin/cmake.exe --build "$BUILD" -j "$JOBS"

echo
echo "=== 构建完成 ==="
ls -la "$BUILD"/navmap.exe "$BUILD"/navmap_qt.exe "$BUILD"/map_shot.exe 2>/dev/null || true
