#!/usr/bin/env bash
#
# shot_bench.sh —— 固定一组视口，各渲一帧，记录"第 2 帧"耗时并把 BMP 存下来。
#
# 存在理由：帧时间优化最容易自欺——只量自己改的那个缩放级别、只量快那一帧。
# 这个脚本把"哪些视口、第几帧"钉死，改前改后跑同一条命令，输出同格式的表，
# 顺带把 BMP 落盘给 bmpdiff.py 做像素验收。
#
# 用法: ./scripts/shot_bench.sh <输出目录>        例: ./scripts/shot_bench.sh /tmp/v1
# 输出: 每行 "<用例>  第2帧 <ms>  路 <ms>  面 <ms>  线 <ms>"，末尾打各用例 BMP 路径。

set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:?用法: shot_bench.sh <输出目录>}"
mkdir -p "$OUT"
cd "$ROOT"

NAVMAP_ENV_QUIET=1 . "$ROOT/env.sh" 2>/dev/null || true

SHOT="$ROOT/build/map_shot"
[ -x "$SHOT" ] || { echo "缺 $SHOT，先构建"; exit 1; }

ALL="$ROOT/products/graph"

# 用例表：<名字>|<--graph 参数>|<lat>|<lon>|<zoom>
CASES="
beijing-z9|$ROOT/products/graph/beijing|39.90|116.40|9
beijing-z11|$ROOT/products/graph/beijing|39.90|116.40|11
beijing-z13|$ROOT/products/graph/beijing|39.90|116.40|13
hebei-z9|$ROOT/products/graph/hebei|39.90|116.40|9
hebei-z11|$ROOT/products/graph/hebei|39.90|116.40|11
all-z9|$ALL|39.90|116.40|9
all-z11|$ALL|39.90|116.40|11
all-z13|$ALL|39.90|116.40|13
"

printf '%-14s %8s %8s %8s %8s %10s %10s\n' 用例 第2帧 路 面 线 画出边 去重跳过
echo "$CASES" | while IFS='|' read -r name graph lat lon zoom; do
    [ -n "$name" ] || continue
    log="$OUT/$name.log"
    bmp="$OUT/$name.bmp"
    "$SHOT" --graph="$graph" --lat="$lat" --lon="$lon" --zoom="$zoom" \
            --w=1280 --h=800 --out="$bmp" >"$log" 2>&1
    rc=$?
    if [ $rc -ne 0 ] || grep -q "ASSERT-FAIL" "$log"; then
        printf '%-14s  FAILED rc=%s\n' "$name" "$rc"
        tail -n 4 "$log" | sed 's/^/      | /'
        continue
    fi
    f2=$(sed -n 's/.*render_ms *[0-9]* *(第2帧 \([0-9]*\)).*/\1/p' "$log" | head -1)
    br=$(sed -n 's/.*分解(第2帧) *背景 [0-9]* *面 \([0-9]*\) *路 \([0-9]*\) *线 \([0-9]*\).*/\1 \2 \3/p' "$log" | head -1)
    areas=$(echo "$br" | awk '{print $1}')
    roads=$(echo "$br" | awk '{print $2}')
    lines=$(echo "$br" | awk '{print $3}')
    drawn=$(sed -n 's/^edges_drawn *\([0-9]*\).*/\1/p' "$log" | head -1)
    dd=$(sed -n 's/^dedup_skipped *\([0-9]*\).*/\1/p' "$log" | head -1)
    printf '%-14s %8s %8s %8s %8s %10s %10s\n' \
        "$name" "${f2:-?}" "${roads:-?}" "${areas:-?}" "${lines:-?}" \
        "${drawn:-?}" "${dd:-?}"
done
echo "BMP 目录: $OUT"
