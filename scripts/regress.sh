#!/usr/bin/env bash
#
# 一键回归：改完渲染 / 路网 / 图集相关代码后跑一遍，确认没有悄悄退化。
#
#   用法: ./scripts/regress.sh              用现有 build/
#         ./scripts/regress.sh --build      先编译再跑
#         ./scripts/regress.sh --keep       保留这一轮产出的 BMP（默认放 /tmp）
#
# 为什么要有它：这些验收以前散在会话里，靠人拼命令。渲染改动有很强的
# "长得还是那样、但要素其实少了"的失效模式（geometry 命令被静默丢弃、
# 跨省去重误判、LOD 表改错），必须每次跑同一套。
#
# 判据：每个用例必须 出现期望标记 且 不出现 ASSERT-FAIL / 非 0 返回值。

set -u
cd "$(dirname "$0")/.."
ROOT=$PWD
BUILD=$ROOT/build
GRAPH=$ROOT/products/graph

DO_BUILD=0
for a in "$@"; do
    case "$a" in
        --build) DO_BUILD=1 ;;
        --keep)  KEEP=1; SHOTS=$ROOT/docs/shots/regress ;;
        *) echo "未知参数: $a" >&2; exit 2 ;;
    esac
done
SHOTS=${SHOTS:-${TMPDIR:-/tmp}/navimap_regress}
mkdir -p "$SHOTS"

if [ "$DO_BUILD" = 1 ]; then
    source "$ROOT/env.sh"
    if ! cmake --build "$BUILD" -j 4; then echo "编译失败"; exit 1; fi
    # 本项目保持零警告：新警告一律当失败，不然警告会堆到没人看的程度
    touch src/utils/*.c src/pbf/*.c src/map/*.c src/route/*.c src/render/*.c \
          src/search/*.c src/ui/*.c tools/*.c
    # lto-wrapper 的 "using serial compilation of N LTRANS jobs" 是 LTO 并行
    # 调度的提示性 note、非代码警告，且非本项目可控，过滤掉以免假 FAIL。
    if cmake --build "$BUILD" -j 4 2>&1 | grep -iE "warning" \
            | grep -v "lto-wrapper.exe: warning: using serial compilation"; then
        echo "警告检查   FAIL（上面有编译警告）"
        exit 1
    fi
    echo "警告检查   OK（零警告）"
fi

pass=0; fail=0
run() {                       # run <名字> <必须出现的标记> <命令...>
    local name=$1 want=$2; shift 2
    local log="$SHOTS/$name.log"
    "$@" >"$log" 2>&1
    local rc=$?
    local bad
    bad=$(grep -cE "ASSERT-FAIL" "$log")
    if [ "$rc" = 0 ] && grep -q -- "$want" "$log" && [ "$bad" = 0 ]; then
        pass=$((pass + 1)); printf '%-28s OK   (%s)\n' "$name" "$want"
    else
        fail=$((fail + 1)); printf '%-28s FAIL rc=%s 期望[%s] FAIL行数=%s\n' \
            "$name" "$rc" "$want" "$bad"
        tail -n 6 "$log" | sed 's/^/      | /'
    fi
}

shot() {                      # shot <名字> <省目录|graph> <lat> <lon> <zoom>
    local name=$1 g=$2 la=$3 lo=$4 z=$5
    run "$name" "ASSERT-OK" "$BUILD/map_shot" --graph="$g" \
        --lat="$la" --lon="$lo" --zoom="$z" --w=1280 --h=800 \
        --out="$SHOTS/$name.bmp"
}

echo "=== 渲染 / 覆盖（每省 z13 + 跨省 z9/z11/z13 + 无图区域）"
# 各省取省会附近，保证落在省内而不是省界外面
shot r_beijing  "$GRAPH/beijing"  39.90 116.40 13
shot r_tianjin  "$GRAPH/tianjin"  39.13 117.20 13
shot r_hebei    "$GRAPH/hebei"    38.04 114.51 13
shot r_shanxi   "$GRAPH/shanxi"   37.87 112.55 13
shot r_shandong "$GRAPH/shandong" 36.67 117.00 13
shot r_henan    "$GRAPH/henan"    34.75 113.62 13
shot r_shaanxi  "$GRAPH/shaanxi"  34.34 108.94 13
shot r_liaoning "$GRAPH/liaoning" 41.80 123.43 13
shot r_all_z9   "$GRAPH"          39.90 116.40  9
shot r_all_z11  "$GRAPH"          39.90 116.40 11
shot r_all_z13  "$GRAPH"          39.90 116.40 13
shot r_nomap    "$GRAPH"          30.00 110.00 11   # 图集外：必须出下载提示横幅

echo "=== 路网 / 导航 / 限制 / 索引"
run route_cross "ROUTE-OK"  "$BUILD/route_cli"  --graph="$GRAPH" --from=39.90,116.40 --to=34.75,113.62
run navsim_bj   "NAVSIM-OK" "$BUILD/navsim_cli" --graph="$GRAPH" --from=39.90,116.40 --to=39.95,116.45
run navsim_tj   "NAVSIM-OK" "$BUILD/navsim_cli" --graph="$GRAPH" --from=39.13,117.20 --to=39.18,117.26
run navsim_xa   "NAVSIM-OK" "$BUILD/navsim_cli" --graph="$GRAPH" --from=34.34,108.94 --to=34.38,109.02
# 转向限制的定向 A/B 对照。必须用全图集（$GRAPH）：
#   1. only_*（381 条）在 2026-09-23 之前根本没接进路由，光"路径自检通过"
#      证明不了它在拦东西——判据要双向：关限制时能走出被禁转向，开限制时必须走不出来。
#   2. 副本省（beijing/tianjin ⊂ hebei）的限制在运行时是不展开的死数据，
#      restr_ab 会跳过它们并打印条数；已核对过 hebei 的限制集合在坐标层面完整
#      覆盖了这两省的所有限制路口，所以跳过不等于漏检。
run_neg() {                   # run_neg <名字> <期望出现的失败标记> <命令...>
    # 与 run 相反：**命令必须失败**（返回非 0 且出现标记）。
    # 用来放"负向对照"——证明正向那条判据真的有判别力，而不是永远绿。
    # 本项目有过教训：缺这类用例时，整片"通过了"可能只是因为判据从来没生效过。
    local name=$1 want=$2; shift 2
    local log="$SHOTS/$name.log"
    "$@" >"$log" 2>&1
    local rc=$?
    if [ "$rc" != 0 ] && grep -q -- "$want" "$log"; then
        pass=$((pass + 1)); printf '%-28s OK   (%s)\n' "$name" "$want"
    else
        fail=$((fail + 1)); printf '%-28s FAIL rc=%s 期望非零且出现[%s]\n' \
            "$name" "$rc" "$want"
        tail -n 6 "$log" | sed 's/^/      | /'
    fi
}

run restr_ab    "RESTR-AB-OK" "$BUILD/restr_ab" --graph="$GRAPH" --probe=40 --only=60
# ↑ 的负向对照：只把"会合点处的限制检查"关掉，其余限制全开。
#   这一条**必须失败**——它是整套限制判据有效性的前提。实测：
#   关掉后 via=way 20 条里 19 条、only_* 30 组里 30 组会重新走出被禁转向。
run_neg restr_ab_noseam "RESTR-AB-FAIL" "$BUILD/restr_ab" --graph="$GRAPH" \
        --probe=20 --only=30 --no-seam-restr
# ---- 反向索引落盘 rev.bin：三条判据（PLAN.md §35）----
# 1) 逐元素一致：落盘结果 == 内存构建。写盘 / 读盘 / 内存构建三条路共用
#    同一份 rev_build_mem()，所以这条断言能同时抓住写坏和读坏
#    （各写一份构建代码的话，比的是两份 bug，谁错都一样）。
run rev_verify "REV-VERIFY-OK" "$BUILD/revbuild_cli" --graph="$GRAPH" --verify

# 2) 回退路径必须活着：临时图集只链数据、不链 rev.bin（符号链接，零复制），
#    路由必须照常算出**同样**的里程。"没有缓存就自己建"这条自愈路径平时
#    不走，不走就不会被发现坏掉——所以必须有一条用例专门走它。
# 3) 改坏的 rev.bin 在严格模式（NAVIMAP_REV_STRICT=1）下必须被拒绝并回退：
#    结构校验（magic/规模/尾哨兵）挡不住正文单字节损坏，这是刻意的取舍，
#    但严格模式必须能挡住，否则那条取舍就没有兜底。
REVT=$SHOTS/revtest
rm -rf "$REVT"
# revtest 是辽宁省图的三份拷贝（ok/none/bad，各约 3.9G，共约 12G），
# 且每轮开头就重建——历史数据没有任何保留价值。本机 /tmp 在 40G 系统盘、
# 项目数据在 127G 数据盘，不清理的话每轮回归都在小盘上堆 12G：
# 2026-10-24 实踩，7 轮回归 + 一份 A/B 快照把系统盘顶到 100%（剩 34M），
# 连编译都写不了临时文件。非 --keep 模式退出/被杀/中断一律清掉；
# --keep 是给人工排查用的，那份自己管。
if [ "${KEEP:-0}" != 1 ]; then
    trap 'rm -rf "$REVT"' EXIT INT TERM
fi
for d in ok none bad; do
    mkdir -p "$REVT/$d/liaoning"
    ln -s "$GRAPH"/liaoning/*.bin "$REVT/$d/liaoning/"
    cp "$GRAPH/liaoning/manifest.txt" "$REVT/$d/liaoning/"
done
rm -f "$REVT/none/liaoning/rev.bin"
rm -f "$REVT/bad/liaoning/rev.bin"
cp "$GRAPH/liaoning/rev.bin" "$REVT/bad/liaoning/rev.bin"
# conv=notrunc 保留原文件其余字节；status=none 是 GNU dd 专有（BSD/macOS 的
# dd 没有这个选项），改用 2>/dev/null 两侧兼容。
printf '\xff' | dd of="$REVT/bad/liaoning/rev.bin" bs=1 seek=100 conv=notrunc 2>/dev/null

LN_FROM=41.8057,123.4315
LN_TO=38.9140,121.6147
run rev_cached "ROUTE-OK" "$BUILD/route_cli" --graph="$REVT/ok"   --from=$LN_FROM --to=$LN_TO
run rev_norev  "ROUTE-OK" "$BUILD/route_cli" --graph="$REVT/none" --from=$LN_FROM --to=$LN_TO
run rev_bad_strict "建反向索引" env NAVIMAP_REV_STRICT=1 "$BUILD/route_cli" \
        --graph="$REVT/bad" --from=$LN_FROM --to=$LN_TO

# 有缓存 / 没缓存 必须给出**同一条**路线（里程逐位相同）
km_a=$(sed -n 's/^总长 *\([0-9.]*\) km.*/\1/p' "$SHOTS/rev_cached.log" | head -1)
km_b=$(sed -n 's/^总长 *\([0-9.]*\) km.*/\1/p' "$SHOTS/rev_norev.log"  | head -1)
if [ -n "$km_a" ] && [ "$km_a" = "$km_b" ]; then
    pass=$((pass + 1)); printf '%-28s OK   (%s km，与无缓存一致)\n' "rev_same_km" "$km_a"
else
    fail=$((fail + 1)); printf '%-28s FAIL 有缓存 %s / 无缓存 %s\n' "rev_same_km" "${km_a:-空}" "${km_b:-空}"
fi

# ---- 平台抽象层（utils/plat.h）：跨平台契约的正反双向验收 ----
# 平台层专收"只在某平台成立"的调用（路径分隔符、临时目录、字体链、目录枚举、
# 进程派生、时钟、mmap）。它的失效模式全是编译期全绿、换平台才炸，所以契约
# 必须在本机就断言住：
#   ① 正向：该成功的必须成功（含 spawn 自己并原样带回退出码）；
#   ② 负向：该失败的必须失败（不存在路径 / 空 PATH / 起不来的进程）。
# 两侧坏掉任何一侧都会红——不是"永远绿"的门禁。
run plat_selftest     "ASSERT-OK" "$BUILD/plat_selftest"
run plat_selftest_neg "ASSERT-OK" "$BUILD/plat_selftest" --negative

# ---- 设置持久化（完整设置系统）----
run settings_selftest "SETTINGS-OK" "$BUILD/settings_selftest"

# ---- 去 SDL 化的三条门禁（方案2）----
#
# 背景：navimap_core 已做到"零 SDL"，Qt 前端（navmap_qt）一行 SDL 都不链，
# SDL 只存在于两个适配层（navimap_sdl / map_shot）。这条边界一旦被悄悄放宽，
# 编译照样过、运行照样正常，只是 Qt 前端又背上了 SDL2.dll 依赖 —— 直到某天
# 在没装 SDL 的机器上构建才炸。所以必须把边界**钉成可断言的判据**。
#
# ① 光栅 alpha 混色与 SDL 软渲染器逐像素等价：
#    自写 raster_px_rgba 与 SDL 的混色公式差 1 级（SDL 是两项分别除 255 再相加，
#    我们原先是一次除），13.9% 的色值对不上 ⇒ 画面上"同一块面板深浅略不同"。
#    肉眼看不出，但逐像素比对会报差异 —— 这正是它该被门禁住的原因。
run blend_equiv "BLEND-EQUIV-OK" "$BUILD/blend_equiv"

# ② 线段裁剪与 SDL_IntersectRectAndLine 等价：
#    手写 Cohen-Sutherland 与 SDL 的实现在 34.3% 的越界线段上差 1 像素，
#    症状是"画布边缘的路多/少一像素"。已按 SDL 的 SDL_rect_impl.h 逐字重写。
run clip_equiv "CLIP-EQUIV-OK" "$BUILD/clip_equiv"

# ③ 架构边界：核心与 Qt 前端零 SDL 符号；SDL 适配层**必须**有（负向对照，
#    证明检查器不是"永远绿"的空壳）。
#   产物名要补后缀：Windows 上是 navimap.exe，Linux 上是 navimap。
if [ -x "$BUILD/core_no_sdl" ]; then
    exefile() { if [ -x "$1.exe" ]; then echo "$1.exe"; else echo "$1"; fi; }
    run core_no_sdl "CORE-NO-SDL-OK" "$BUILD/core_no_sdl" \
        "$(exefile "$BUILD/navmap_qt")" "$BUILD/libnavimap_core.a"
    run core_no_sdl_sdl "CORE-NO-SDL-OK" "$BUILD/core_no_sdl" --expect-sdl \
        "$(exefile "$BUILD/navimap")"
fi

# ---- 字体链的中文覆盖（中文变方框的根因门禁）----
#
# 地图上的文字（C 侧 stb_truetype）和 Qt 控件上的文字（Qt 自己的字体系统）
# **共用同一条候选链**（plat.c 的 nm_font_candidates）。链里没有能画中文的
# 字体 ⇒ 两边都变方框（tofu）。
#
# 为什么单列一条：字体链的失效是**静默**的 —— 编译过、运行正常、地图照画，
# 只是字变成了方框，很容易被当成"渲染 bug"去查错方向。
#
# 判据是查字体文件**本身**的 cmap（stb_truetype 的 FindGlyphIndex）
# 且要求光栅化后非零像素 > 0 —— 只看 index 会漏掉"有字形但画不出"的情况。
run font_cjk "FONT-CJK-OK" "$BUILD/font_cjk_check"

# ★ 负向对照：--negative 断言**检测逻辑本身**正反两侧都对 ——
#   无中文字体必须被判成"无中文"，有中文字体必须被判成"有中文"。
#   （不要用"把字体藏起来"那种对照：藏起自带字体后系统字体还在，
#     判据照样通过 ⇒ 那种对照是假的，测不出检查器有没有在干活。）
run font_cjk_neg "FONT-CJK-OK" "$BUILD/font_cjk_check" --negative

# ---- Qt 前端的 UI 交互自检 ----
#
# "搜索栏改成按钮、点击才出现" 这类交互，**截图只能证明静态样子对**，
# 证明不了"点了真的会出现、Esc 真的会收起"。这类"注释写了 ≠ 路径落实"的洞，
# 本项目踩过好几次（窗口最小化/最大化按钮、font_drop_textures）。
#
# --ui-test 断言：初始按钮可见/输入框隐藏 → 点按钮输入框出现并聚焦 →
# 输入触发查询 → Esc 收起输入框 → 所有自有按钮都有悬停提示 →
# 右栏按钮浮在画布上。
#
# ★ 负向对照已实测：把 eventFilter 里的 hideSearch() 去掉 ⇒ 立刻报 2 项 FAIL。
if [ -x "$BUILD/navmap_qt" ] || [ -x "$BUILD/navmap_qt.exe" ]; then
    exefile2() { if [ -x "$1.exe" ]; then echo "$1.exe"; else echo "$1"; fi; }
    run qt_ui_test "QT-UI-OK" env QT_QPA_PLATFORM=offscreen \
        "$(exefile2 "$BUILD/navmap_qt")" --ui-test --w=900 --h=600
fi

# ---- 优先队列：每次 pop 必须是当前最小值（PLAN.md §37）----
# 堆坏了不会崩，只会让 A* 悄悄弹出非最小节点、路线变长。所以要有专门判据：
# 20 万次随机操作，每次 pop 与**独立参考实现**对拍 + 多重集比对 + 堆不变式。
run heap_order "HEAP-SELFTEST-OK" "$BUILD/heap_selftest"

# 双向判据的另一半：故意写坏的实现（不上浮 / 取最后一个元素）**必须**被抓出来。
# 分辨力验过：把"下沉漏掉最后一个子节点"或"父下标算错"注入 pqueue.h，
# 都会被这条判据报出 4~6 万处违规 —— 也就是改叉数这类改动是有人盯着的。
run heap_guard "HEAP-SELFTEST-DIFF-OK" "$BUILD/heap_selftest" --expect-fail

# 堆的**稳态微基准**（PLAN.md §43）：把 pq_pop 放大到 100% 才测得准 —— 它在
# 全链路里只占 6.8%（京→郑 72 ms），改它的收益会被 ±50 ms 的运行间噪声吃掉。
# 判据是**双实现在同一进程里对照**（逐层交换 vs 空穴法，两者跑同一份初始堆、
#    同一串伪随机数），外加 FNV-1a 摘要：换实现后两者必须**逐位相同** ——
#    这能排除"变快了是因为语义改错了"。
run heap_bench "HEAP-BENCH" "$BUILD/heap_selftest" --bench=6019,2000000,3

# ---- 瓦片下标：查表版 vs 精确版 逐点等价（PLAN.md §36）----
# ① 全图 5228 万个真实节点逐点比，外加定向构造的 49 万个"正好落在瓦片边界上"
#    的点（按瓦片单位扰动）。nm_tile_of 取错瓦片 ⇒ 缝合成员查错 ⇒ 路线错，
#    而误差只有万分之几个点会触发，**抽样测不出来**。
run tile_eq "TILE-SELFTEST-OK" "$BUILD/tile_selftest" --graph="$GRAPH"

# ② 双向判据的另一半：不带边界回退的 raw 版**必须**有差异，否则说明那条回退
#    分支是死代码（或者查表精度被改了）。两条互斥，任一条变了都会有一边报警。
#    实测：raw 版在北京就有 1167 个真实节点取错瓦片 ⇒ 回退不是摆设。
run tile_guard_needed "TILE-SELFTEST-DIFF-OK" "$BUILD/tile_selftest" --graph="$GRAPH" --expect-diff

run tile_xcheck "不一致的坐标 : 0" "$BUILD/tile_xcheck"
run coverage    "覆盖=有路网"  "$BUILD/coverage_cli"
run search      "查询 中关村"  "$BUILD/search_cli"
# 地名库自检：每条 place 都得能反查出 bigram，kind 不许超出 PLK_ROAD。
# 以前缺这条判据的代价是实打实踩到的：places_build 刚加上路名那一刻，库里
# 5.7 万条被自检判为非法，而当时 21 项里没有一项会报警（地图照画、路照通）。
run places_selftest "PLACES-OK" "$BUILD/places_build" --db="$ROOT/products/places_auto.db" --selftest

# 跨省总里程做区间自检：路由语义改坏（比如双向 A* 提前终止）会让它明显跑偏，
# 但只判 OK 标记看不出来——之前双向 A* 的最优性问题就是靠里程差发现的。
if [ -f "$SHOTS/route_cross.log" ]; then
    km=$(sed -n 's/^总长 *\([0-9.]*\) km.*/\1/p' "$SHOTS/route_cross.log" | head -1)
    ok=$(awk -v k="${km:-0}" 'BEGIN{print (k>650 && k<750) ? 1 : 0}')
    if [ "$ok" = 1 ]; then
        pass=$((pass + 1)); printf '%-28s OK   (%.2f km 在 650~750 内)\n' "route_cross_km" "$km"
    else
        fail=$((fail + 1)); printf '%-28s FAIL %.2f km 不在 650~750 内\n' "route_cross_km" "$km"
    fi
fi

# ★ 自适应 w 的专项判据（PLAN.md §45.8 / §47.3）：
#   这条 2037 km 路线在纯 w=1.4 下是 **5997746 展开 / ~11 s**，靠"第 1 遍撞顶 → 换
#   w=1.8 重跑"才降到 **439655 展开 / ~0.5 s**。**只判 ROUTE-OK 看不出自适应坏没坏**
#   —— 撞顶逻辑一旦失效，它照样出路线，只是慢 16 倍。
#
#   ★★ 主判据改成**展开数**而不是耗时（§47.3）：耗时会被机器负载放大 7 倍以上
#   （实测同一条路线在 load≈10 + zram swap 满时跑出 3885 ms，安静时 507 ms），
#   于是"卡耗时"会变成 flaky 项；而**展开数是确定的**，与负载无关。
#   600 万 vs 44 万 ⇒ 阈值取 100 万，两边各有 6 倍余量。
#   耗时仍保留一项，但改成 **repeat=3 取最小** + 6000 ms 阈值，只用来兜住
#   "每展开的常数被改坏"这种退化，不是主判据。
#
# ★★ 阈值与语义都随地标表变过三次，改表/改 w 时必须重定基线：
#   - 无表（1.4 撞顶升 1.8）= 599.8 万   ← 必须能被判 FAIL
#   - 10 地标 w=1.3 = 263.9 万（§51.4，当时阈值被迫抬到 350 万）
#   - **30 地标 w=1.25 = 53.2 万**（§52 现在）⇒ 阈值收回 **100 万**
#   ★ 收回到 100 万是故意的：这样"地标数被改小"（10 地标 263.9 万）也会 FAIL，
#     与下面 lm_nlm 那一项互为交叉验证 —— 单项可能被人一起改掉，两项一起改难。
#   ★ 它**挡不住"表没加载"**：表掉了会退回无表 1.4→1.8 = 44 万，照样过。
#     所以下面另有 lm_loaded / lm_nlm 两项专门盯这个。
run route_hard "ROUTE-OK" "$BUILD/route_cli" --graph="$GRAPH" \
    --from=42.03,122.38 --to=35.12,106.68 --repeat=3
if [ -f "$SHOTS/route_hard.log" ]; then
    exp=$(sed -n 's/^搜索 *展开 \([0-9]*\) 节点，耗时 [0-9]* ms/\1/p' \
          "$SHOTS/route_hard.log" | head -1)
    ok=$(awk -v e="${exp:-99999999}" 'BEGIN{print (e<1000000) ? 1 : 0}')
    if [ "$ok" = 1 ]; then
        pass=$((pass + 1)); printf '%-28s OK   (%s 展开 < 100 万，30 地标 w=1.25 正常)\n' "route_hard_expand" "$exp"
    else
        fail=$((fail + 1)); printf '%-28s FAIL %s 展开（退化到无表量级？无表固定 1.4 是 5997746）\n' "route_hard_expand" "$exp"
    fi
    # ★ 地标表必须真的加载上。表是**可选**的（没有就退回直线启发式，不报错），
    #   所以"表丢了"的表现是算路悄悄变慢而不是崩 —— 必须有专门一项盯住，
    #   上面那个展开数判据反而会放行（无表反而只要 44 万）。
    if grep -q '^地标表 *已加载' "$SHOTS/route_hard.log"; then
        pass=$((pass + 1)); printf '%-28s OK   (%s)\n' "lm_loaded" \
            "$(sed -n 's/^地标表 *//p' "$SHOTS/route_hard.log" | head -1)"
    else
        fail=$((fail + 1)); printf '%-28s FAIL 没打印"地标表 已加载"——lm.bin 没生效？\n' "lm_loaded"
    fi
    # ★ 地标**个数**也要钉住（PLAN.md §52）：sync_maps.sh 的指纹若漏了地标集，
    #   改了源码里的地标列表后重建会被 SKIP，旧表继续被加载 —— 既不报错也不崩，
    #   只是算路悄悄慢 1.7 倍、质量悄悄差 0.4%。LM_WANT_NLM 必须跟着源码改。
    LM_WANT_NLM=30
    nlm=$(sed -n 's/^地标表 *已加载（\([0-9]*\) 个地标.*/\1/p' "$SHOTS/route_hard.log" | head -1)
    if [ "${nlm:-0}" = "$LM_WANT_NLM" ]; then
        pass=$((pass + 1)); printf '%-28s OK   (%s 个地标，期望 %s)\n' "lm_nlm" "$nlm" "$LM_WANT_NLM"
    else
        fail=$((fail + 1)); printf '%-28s FAIL %s 个地标（期望 %s）——lm.bin 是用旧地标集建的？\n' \
            "lm_nlm" "${nlm:-<没读到>}" "$LM_WANT_NLM"
    fi
    ms=$(sed -n 's/^\[repeat [0-9]*\] 展开 [0-9]* 节点，耗时 \([0-9]*\) ms/\1/p' \
         "$SHOTS/route_hard.log" | sort -n | head -1)
    ok=$(awk -v m="${ms:-99999}" 'BEGIN{print (m<6000) ? 1 : 0}')
    if [ "$ok" = 1 ]; then
        pass=$((pass + 1)); printf '%-28s OK   (%s ms < 6000，repeat=3 取最小)\n' "route_hard_ms" "$ms"
    else
        fail=$((fail + 1)); printf '%-28s FAIL %s ms（每展开的常数被改坏？正常 ~510）\n' "route_hard_ms" "$ms"
    fi
fi

# ★ 展开上限的专项判据（PLAN.md §48.3）：
#   郑州→大连 要 **11334703 展开**（绕行系数 1.80，半岛端点 + 启发式失效），
#   以前撞在 DEFAULT_MAX_EXPAND=800 万 上 ⇒ 直接 ROUTE-FAIL「超出规模上限」。
#   同类失败的还有西安/南阳/太原 → 大连。**这些是覆盖范围内的真实 O-D，不是数据问题。**
#   只判 ROUTE-OK 就够（判据要的是"能出结果"），**不要卡耗时**——这条本来就 15 s，
#   而且 §47.4 已经证明耗时会被机器负载放大一个数量级。
run route_far_dalian "ROUTE-OK" "$BUILD/route_cli" --graph="$GRAPH" \
    --from=34.75,113.62 --to=38.91,121.61

# ★★ 地标下界的"不许恶化"判据（PLAN.md §52.8）
#
#   背景：地标下界**不是严格下界**（实测 50 条里 2 条开表比真值差）。已拍板接受，
#   但它不能再变差 —— 变差意味着又多让几条路线悄悄次优，而且极难归因。
#
#   判据形式特意写成**上界**而不是"相等"：
#       开表 w=1.0 的分钟  <=  无表 w=1.0 的分钟 + LM_ADMISSIBLE_SLACK(1)
#   ⇒ 现在（129 vs 128）通过；如果哪天**修好了**（128 vs 128）也通过；
#     只有**变得更差**（130 vs 128）才 FAIL。判据不该在修好那天反而报错。
#   （真值那一侧是现算的，不是写死的 ⇒ 换数据/换图集也不会误判。）
#
#   ★ 为什么选太原→石家庄：它是两条已知越界里**最便宜**的一条（54.6 万 / 66.5 万
#     展开，几秒跑完），进得了回归；另一条南阳→天津要 900~1100 万展开，
#     留给 scripts/lmadmissible.py 全量跑（50 条约 25 分钟，太慢不适合进这里）。
#   ★ --heur-w 和 --heur-w-hi **必须一起给**：只给前者的话，第一遍封顶 20 万后会
#     换默认的 hi 重跑 ⇒ 根本不是 w=1.0（§49"兜底只能有一个入口"的延伸）。
LM_ADMISSIBLE_SLACK=1
_lm_exact() {  # $1 = 1 开表 / 0 无表；stdout = 预计分钟
    NAVIMAP_LM="$1" "$BUILD/route_cli" --graph="$GRAPH" \
        --from=37.87,112.55 --to=38.04,114.51 --repeat=1 \
        --exact --heur-w=1.0 --heur-w-hi=1.0 2>/dev/null |
    sed -n 's/^预计 *\([0-9]*\) 分.*/\1/p' | head -1
}
m_on=$(_lm_exact 1)
m_off=$(_lm_exact 0)
if [ -n "$m_on" ] && [ -n "$m_off" ] &&
   [ "$m_on" -le $((m_off + LM_ADMISSIBLE_SLACK)) ]; then
    pass=$((pass + 1))
    printf '%-28s OK   (开表 %s 分 <= 真值 %s + %s；已知越界 %+d 分，未恶化)\n' \
        "lm_admissible" "$m_on" "$m_off" "$LM_ADMISSIBLE_SLACK" "$((m_on - m_off))"
else
    fail=$((fail + 1))
    printf '%-28s FAIL 开表 %s 分，真值 %s 分（差 %s 分 > %s ⇒ 下界越界变严重了）\n' \
        "lm_admissible" "${m_on:-<失败>}" "${m_off:-<失败>}" \
        "$((m_on - m_off))" "$LM_ADMISSIBLE_SLACK"
fi

# ★★ 主程序（window.c）自己的算路路径（PLAN.md §51.9）
#   之前 navsim_* 用的是独立工具 navsim_cli，**根本没走到 window.c**；
#   map_shot 不带 --from/--to 也不算路 ⇒ 主程序的 `nav_worker` /
#   `app_nav_plan_sync` 长期没有任何自动化覆盖，而它们才是对外行为。
#   这里用 map_shot --from/--to 走 `app_nav_plan_sync`，一次盖住：
#     ① 吸附 → ② 算路 → ③ 转向指令 → ④ 路线真的画到画面上（route_pixels）。
#   ★ 起终点特意选 **保定→石家庄**：修复前它吸附到 hebei #6204583（悬在与主干
#     断开的小连通块里）⇒ 双向都"不连通"，是 §51.1 那个 bug 的原生样本。
#     判据"里程在 120~160 km 内"能同时挡住"算不出来"和"吸附错了点"。
run app_route "ASSERT-OK" "$BUILD/map_shot" --graph="$GRAPH" \
    --from=38.87,115.46 --to=38.04,114.51 --fit-route --w=800 --h=600 \
    --out="$SHOTS/app_route.bmp"
if [ -f "$SHOTS/app_route.log" ]; then
    km=$(sed -n 's/^route  *\([0-9.]*\) km.*/\1/p' "$SHOTS/app_route.log" | head -1)
    px=$(sed -n 's/^route_pixels  *\([0-9]*\)/\1/p' "$SHOTS/app_route.log" | head -1)
    # 区间取 110~180（实测 139.04，留 ±30%）：只挡"吸附到了错的节点"
    # （那会让里程明显跑偏）和"算不出来"，**不挡**调 w 带来的正常小幅变化。
    ok=$(awk -v k="${km:-0}" 'BEGIN{print (k>110 && k<180) ? 1 : 0}')
    if [ "$ok" = 1 ]; then
        pass=$((pass + 1)); printf '%-28s OK   (%s km 在 110~180 内)\n' "app_route_km" "$km"
    else
        fail=$((fail + 1)); printf '%-28s FAIL %s km（保定→石家庄应 ~139）\n' "app_route_km" "${km:-无}"
    fi
    ok=$(awk -v p="${px:-0}" 'BEGIN{print (p>200) ? 1 : 0}')
    if [ "$ok" = 1 ]; then
        pass=$((pass + 1)); printf '%-28s OK   (%s 个路线像素 > 200)\n' "app_route_px" "$px"
    else
        fail=$((fail + 1)); printf '%-28s FAIL %s 像素（路线没画上去？）\n' "app_route_px" "${px:-0}"
    fi
fi

# ★ 地标距离（一对多 Dijkstra）的验收（PLAN.md §48.5）：
#   判据是**数值区间**，不是"能跑完"。北京地标到郑州的代价实测 23136.8 秒
#   （= 385.6 分），与 route_cli 的最优解（386 分，打印取整到整分）一致。
#   区间取 22500~23800 秒（±3%）：
#     - 上界挡住"少做了转向限制 / 没做缝合"以外的**变大**类退化；
#     - 下界挡住"跨省缝合把距离算短了"这类**变小**的错误。
#   ★★ 负向对照已做：把跨省缝合那段临时改成不执行，5 个跨省查询**全部
#      UNREACHABLE** ⇒ 这条判据真能失败，缝合代码确实是承重的。
run lmdist "LM-DIST-OK" "$BUILD/lmdist_cli" --graph="$GRAPH" \
    --from=39.90,116.40 --q=34.75,113.62 --q=36.07,120.38 --q=38.91,121.61
if [ -f "$SHOTS/lmdist.log" ]; then
    v=$(sed -n 's/^Q 34\.75000,113\.62000 .* \([0-9.]*\) 秒/\1/p' \
         "$SHOTS/lmdist.log" | head -1)
    ok=$(awk -v x="${v:-0}" 'BEGIN{print (x>22500 && x<23800) ? 1 : 0}')
    if [ "$ok" = 1 ]; then
        pass=$((pass + 1)); printf '%-28s OK   (%s 秒 在 22500~23800 内)\n' "lmdist_km" "$v"
    else
        fail=$((fail + 1)); printf '%-28s FAIL %s 秒 不在 22500~23800 内（正常 23136.8）\n' "lmdist_km" "$v"
    fi
fi

echo
echo "=== 结果: $pass 通过 / $fail 失败   日志与截图: $SHOTS"
[ "$fail" = 0 ]
