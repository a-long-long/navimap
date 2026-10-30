#!/usr/bin/env bash
#
# 编号一致性门禁：docs/teaching/ 下每个 NN_xxx.md 的 H1 标题必须声明
# 与文件名前缀一致的「第 N 课」，防止讲义与 tutorial/（00–14 同号）再次漂移。
#
# 为什么单列一条：docs/teaching/ 与 tutorial/ 是同一套 00–14 编号的"概念 vs
# 动手"两份资料，过去靠手工对齐，易漂。本脚本把"文件名 05 ↔ H1 第 5 课"
# 这条不变量固化成机器判据。
#
# 规则：遍历 docs/teaching/[0-9][0-9]_*.md（不含 index.md），取文件首个 H1
# 行（首个 ^# 行），断言其中包含「第 N 课」，N = 文件名前两位去前导零。
# 例外：capstone 的 H1 为「第 14 课 · ★ 综合项目」，★ 只是强调它综合性质，
# 编号仍须是 14，所以走同一条规则、不开特例。
#
# 判据（与 regress.sh 的 run() 双判据对齐）：
#   - 全通过 → 打印 DOC-NUMBERS-OK 且退出码 0
#   - 任一不一致 → 打印 ASSERT-FAIL:<文件> 并 DOC-NUMBERS-FAIL，退出码 1
#
# ★ 负向对照：把某课 H1 的「第 N 课」改成别的数字后本脚本必须 FAIL（已实测）；
#   否则就是假门禁，测不出有人把讲义编号改错位。

set -u
cd "$(dirname "$0")/.."
ROOT=$PWD
DIR="$ROOT/docs/teaching"

rc=0
fails=0
checked=0

for f in "$DIR"/[0-9][0-9]_*.md; do
    [ -e "$f" ] || continue
    base=$(basename "$f")
    nn=${base:0:2}
    n=$((10#$nn))                       # 去前导零：00->0, 05->5, 14->14
    # 取文件首个 H1 行（首个以「# 」开头的行）
    h1=$(awk '/^#[[:space:]]/{print; exit}' "$f")
    if [ -z "$h1" ]; then
        echo "ASSERT-FAIL: $base 找不到 H1 标题（首个 ^# 行）"
        rc=1; fails=$((fails+1)); continue
    fi
    need="第 $n 课"
    if [[ "$h1" == *"$need"* ]]; then
        echo "OK: $base H1 含「$need」"
    else
        echo "ASSERT-FAIL: $base H1 应为「$need」，实际: $h1"
        rc=1; fails=$((fails+1))
    fi
    checked=$((checked+1))
done

if [ "$rc" = 0 ]; then
    echo "DOC-NUMBERS-OK  (已核对 $checked 个讲义，H1 编号与文件名一致)"
else
    echo "DOC-NUMBERS-FAIL  ($fails 处编号不一致)"
fi
exit $rc
