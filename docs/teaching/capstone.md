# ★ 综合项目（Capstone）

> 目标：独立完成“改一个点 + 加一条判据”，体验完整的“改 → 测 → 守”闭环；并通过一个端到端示例，把第 2–14 课串成一条完整的程序主干。
> 前置：第 2–14 课全部。
> 纪律：改动走 `scripts/check_layers.sh` 零违规；新增/改动有对应 `regress.sh` 判据（确定性量优先）；提交前 `./scripts/regress.sh` 全绿（或仅剩已知环境缺失项）。

## 一、教学目的

- 把整门课的程序主干（数据生产 → 每帧渲染 → 用户交互 → Qt 外壳 → 门禁）串成一条可追踪的链路；
- 通过一个“从一份 PBF 到屏幕上一条导航路线”的端到端示例，亲眼看到 L2–L14 各自的代码是在主干的哪一环被调用的；
- 掌握“改 → 测 → 守”闭环：任何改动都要落到 `regress.sh` 的一条判据上，且判据能负向失败；
- 学会复用已验证路径，不重走被否分支（见第五节“别再试”清单）。

## 二、功能构成

| 阶段 | 对应课次 | 关键文件 / 函数 | 路径与行号区间 |
|---|---|---|---|
| 数据生产（PBF→二进制→建图） | L4 / L5 / L6 | `scripts/sync_maps.sh`、`tools/graph_extract.c`（`graph_build`、`nmbin_open`、`rev_write`） | `scripts/sync_maps.sh:1-432`（完整体见第三节）；`tools/graph_extract.c:1-172`（完整体见第三节） |
| 每帧渲染（视口投影→软件光栅→管线） | L7 / L8 / L9 | `src/ui/offscreen_shot.c`（`offscreen_shot_main`、`render_buffer`、`write_bmp24`、`app_offscreen_audit`）、`map/map_view.c`（`mv_project`、`mv_visible_bbox`） | `src/ui/offscreen_shot.c:194-647`（`offscreen_shot_main` 完整体）；关键段见第三节 |
| 用户交互（搜索→路由→导航语音） | L10 / L11 / L12 | `tools/route_cli.c`（`router_route_ll`、`maneuver_build`）、`src/ui/window.c`（`app_nav_plan_sync`）、`route/maneuver.c`、`route/voice.c`、`ui/voice_wire.c` | `tools/route_cli.c:46-338`（main 完整体）；`--maneuvers` 块见第三节 |
| Qt 外壳 | L13 | `src/ui_qt/qt_main.cpp`、`map_canvas.cpp`、`main_window.cpp` | `CMakeLists.txt:496-512`（`navmap_qt` 目标） |
| 门禁 | L14 | `scripts/regress.sh`、`scripts/check_layers.sh`、`tools/core_no_sdl.c` | `scripts/regress.sh:1-802`（全文为控制篇幅省略，可在 `scripts/regress.sh:1` 查阅） |

纯 DAG 底座：`utils ← pbf ← map ← route ← render ← ui ← ui_qt`（L3 平台抽象、L2 坐标系为全程底座）。

## 三、完整实现代码

下面用“从一份 PBF 到屏幕上一条导航路线”的端到端示例，把各课代码按运行时顺序串联起来。被展示的代码均为完整体（不截断）。

### 3.1 数据生产：`scripts/sync_maps.sh`（完整体）

把 `sources/` 下新下载的 PBF 自动变成可加载的路网，并顺手重建地名库 `places_auto.db` 与地标表 `lm.bin`。它内部调用 `graph_extract` + `graph_check` + `places_build` + `lmbuild_cli`，并用 manifest 指纹判断过期（`sync_maps.sh:120-146`）。

```sh
#!/bin/bash
# sync_maps.sh —— 让 sources/ 下新下载的 PBF 自动变成可加载的路网
#
# 存在理由：主程序不会自己扫描目录，也不会检查产物是不是过期。以前加一张新地图
# 要手工敲 graph_extract、手工记目录名、手工传给 --graph，而且换了 PBF 忘了重转时
# 程序一声不吭继续画旧图（app_init 只比对 magic，连版本号都不校验）。
# 本脚本把"发现新地图 → 转换 → 校验"这条链自动掉，指纹记在每份产物的 manifest.txt 里。
#
# 用法：
#   ./scripts/sync_maps.sh                # 同步 sources/ 下所有 PBF（已最新的直接跳过）
#   ./scripts/sync_maps.sh --dry-run      # 只报告要做什么，不动手
#   ./scripts/sync_maps.sh --only=hebei   # 只处理一个
#   ./scripts/sync_maps.sh --force        # 无视指纹，全部重转
#   ./scripts/sync_maps.sh --no-probe     # 跳过 PBF 完整性预检（快一点，坏文件会等到转换时才炸）
#   ./scripts/sync_maps.sh --no-check     # 转换后不跑 graph_check
#
# 退出码：0 = 全部就绪；1 = 至少一个源文件转换失败（需要人工介入）。

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT/build"
SRC_DIR="$ROOT/sources"
OUT_ROOT="$ROOT/products/graph"

# 输出根可能还不存在（全新克隆、或被人手工删过）——graph_extract 不会自己建父目录
mkdir -p "$OUT_ROOT" || { printf '[FAIL ] 无法创建输出目录 %s\n' "$OUT_ROOT"; exit 1; }

DRY=0
FORCE=0
DO_CHECK=1
DO_PROBE=1
ONLY=""

for arg in "$@"; do
    case "$arg" in
        --dry-run)   DRY=1 ;;
        --force)     FORCE=1 ;;
        --no-check)  DO_CHECK=0 ;;
        --no-probe)  DO_PROBE=0 ;;
        --only=*)    ONLY="${arg#--only=}" ;;
        -h|--help)   sed -n '2,20p' "${BASH_SOURCE[0]}"; exit 0 ;;
        *) printf '未知参数: %s（试试 --help）\n' "$arg"; exit 2 ;;
    esac
done

# 二进制链接了 ~/.local 里的 protobuf-c / SDL2，不 source 就跑不起来
NAVMAP_ENV_QUIET=1 . "$ROOT/env.sh"

# ---- 跨平台小工具（GNU 与 BSD/macOS 双兼容）--------------------------------
# stat：GNU 用 -c %s/%Y，BSD/macOS 用 -f %z/%m（macOS 的 stat 没有 -c）。
# 先试 GNU，失败再试 BSD，两侧输出同为一个裸数字。
fsize()  { stat -c %s "$1" 2>/dev/null || stat -f %z "$1"; }
fmtime() { stat -c %Y "$1" 2>/dev/null || stat -f %m "$1"; }
# 核数：nproc（GNU coreutils）→ getconf（POSIX）→ sysctl（macOS）→ 兜底 4
NPROC=$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null \
        || sysctl -n hw.ncpu 2>/dev/null || echo 4)
# md5：GNU 是 md5sum（输出 "hash 文件名"），BSD/macOS 是 md5 -q（裸 hash）。
# 统一从 stdin 读、只往 stdout 写裸 hash。
md5of() {
    if command -v md5sum >/dev/null 2>&1; then md5sum | cut -d' ' -f1
    else md5 -q; fi
}

for b in graph_extract graph_check pbf_info places_build lmbuild_cli; do
    if [ ! -x "$BIN/$b" ]; then
        printf '缺少 %s —— 先构建：\n  source env.sh && cmake --build build -j"$NPROC"\n' "$BIN/$b"
        exit 1
    fi
done

# 当前期望的二进制格式版本。产物 manifest 里记的版本对不上就重转。
WANT_VER="$("$BIN/graph_extract" --fmt-version)"

# 从 manifest.txt 取一个键的值（键后面跟的是空白，值到行尾）
mani_get() { sed -n "s/^$1[[:space:]]\+//p" "$2" 2>/dev/null | head -1; }

# 删掉脚本自己造出来的临时目录。删不掉必须喊出来——2026-09-22 那批 1.45 GB
# 残留就是这么来的：清理静默失败，后面的 mv 把新产物塞进了旧目录里。
# 注意：某些环境里 rm 是被包过的"安全删除"（只搬回收站/原地改名），返回值
# 为 0 但东西还在，所以除了看返回值，还要回头看一眼路径是不是真没了。
drop_tmp() {
    local t="$1"
    [ -e "$t" ] || return 0
    rm -rf "$t"
    if [ -e "$t" ]; then
        printf '        注意：%s 删不掉，请手工清理：\n' "$t"
        printf '        /bin/rm -rf %s\n' "$t"
        return 1
    fi
    return 0
}

avail_kb=$(df -Pk "$ROOT" | awk 'NR==2{print $4}')
if [ "${avail_kb:-0}" -lt 2097152 ]; then
    printf '警告: %s 所在分区只剩 %s MB，转换一个省大约要几百 MB 到 1 GB\n' \
        "$ROOT" "$(( avail_kb / 1024 ))"
fi

n_skip=0; n_build=0; n_fail=0; n_warn=0
ready=()

while IFS= read -r f; do
    [ -n "$f" ] || continue
    base=$(basename "$f")
    name="${base%.osm.pbf}"
    name="${name%.pbf}"
    [ -n "$name" ] || continue
    if [ -n "$ONLY" ] && [ "$name" != "$ONLY" ]; then continue; fi

    dir="$OUT_ROOT/$name"
    mani="$dir/manifest.txt"

    # 守卫：只在 OUT_ROOT 之下动手，绝不接受拼歪的路径
    case "$dir" in
        "$OUT_ROOT"/*) ;;
        *) printf '[FAIL] %s: 路径 %s 不在 %s 下，拒绝操作\n' "$name" "$dir" "$OUT_ROOT"; n_fail=$((n_fail+1)); continue ;;
    esac

    # ---- 判断要不要转换 ----
    reason=""
    if [ "$FORCE" = 1 ]; then
        reason="--force 指定重转"
    elif [ ! -f "$mani" ]; then
        if [ -d "$dir" ]; then
            reason="上次转换没走完（无 manifest.txt，产物不可用）"
        else
            reason="新地图，尚未转换"
        fi
    else
        for need in nodes.bin edges.bin osmid.bin; do
            if [ ! -f "$dir/$need" ]; then reason="缺少 $need（上次转换被中断）"; break; fi
        done
    fi

    if [ -z "$reason" ]; then
        mb=$(mani_get src_bytes "$mani")
        mm=$(mani_get src_mtime "$mani")
        mv=$(mani_get binfmt_version "$mani")
        sb=$(fsize "$f")
        sm=$(fmtime "$f")
        if   [ "$mb" != "$sb" ]; then reason="源文件大小变了（$mb → $sb 字节）"
        elif [ "$mm" != "$sm" ]; then reason="源文件被换过了（mtime 不同）"
        elif [ "$mv" != "$WANT_VER" ]; then reason="二进制格式升级 v$mv → v$WANT_VER"
        fi
    fi

    if [ -z "$reason" ]; then
        printf '[SKIP ] %-12s 已是最新（节点 %s / 边 %s）\n' \
            "$name" "$(mani_get nodes "$mani")" "$(mani_get edges "$mani")"
        n_skip=$((n_skip+1))
        ready+=("$name")
        continue
    fi

    printf '[BUILD] %-12s %s\n' "$name" "$reason"
    if [ "$DRY" = 1 ]; then n_build=$((n_build+1)); continue; fi

    # ---- 完整性预检：截断/损坏的 PBF 在这里就被拦下，而不是转出一张残缺的图 ----
    if [ "$DO_PROBE" = 1 ]; then
        if ! "$BIN/pbf_info" "$f" >/dev/null 2>&1; then
            printf '[FAIL ] %-12s PBF 不完整或已损坏（pbf_info 读不通）。\n' "$name"
            printf '        建议挪走：mv %s %s/_bad/\n' "$f" "$SRC_DIR"
            n_fail=$((n_fail+1))
            continue
        fi
    fi

    # 先转到临时目录，成功才替换：避免"旧的删了、新的失败"两头空
    tmp="$dir.tmp.$$"
    if ! drop_tmp "$tmp"; then
        printf '[FAIL ] %-12s 上次残留的 %s 清不掉，跳过（先手工删掉再跑）\n' "$name" "$tmp"
        n_fail=$((n_fail+1))
        continue
    fi
    t0=$(date +%s)
    if ! "$BIN/graph_extract" --out="$tmp" "$f" > "$tmp.log" 2>&1; then
        printf '[FAIL ] %-12s graph_extract 闸门未过（%s 秒）\n' "$name" "$(( $(date +%s) - t0 ))"
        mv "$tmp.log" "$OUT_ROOT/failed-$name.log"
        grep -h 'ASSERT-FAIL' "$OUT_ROOT/failed-$name.log" | head -5 | sed 's/^/        /'
        printf '        多半是新地图出现了 road_tags.c 表里没有的 highway 取值，需要人工补表。\n'
        printf '        完整日志：%s/failed-%s.log\n' "$OUT_ROOT" "$name"
        drop_tmp "$tmp"
        drop_tmp "$tmp.log"
        n_fail=$((n_fail+1))
        continue
    fi

    if [ "$DO_CHECK" = 1 ]; then
        if ! "$BIN/graph_check" "$tmp" > "$tmp.check.log" 2>&1; then
            printf '[FAIL ] %-12s graph_check 不通过，已丢弃本次产物\n' "$name"
            mv "$tmp.check.log" "$OUT_ROOT/failed-$name.log"
        grep -h 'ASSERT-FAIL' "$OUT_ROOT/failed-$name.log" | head -5 | sed 's/^/        /'
        drop_tmp "$tmp"
        n_fail=$((n_fail+1))
        continue
    fi
    rm -f "$tmp.check.log" || printf '        注意：%s 删不掉，请手工清理\n' "$tmp.check.log"
    fi

    # ---- 切换：先给旧产物让位，再把新产物搬到正式位置 ----
    # 顺序很关键，绝不能写成 `rm -rf "$dir"; mv "$tmp" "$dir"`：
    # 一旦删除被外部安全策略拦下（目录还在），mv 会把新产物塞进旧目录里，
    # 变成 $dir/$name.tmp.PID 这种嵌套垃圾，而脚本还以为成功了 —— 2026-09-22
    # 那批 1.45 GB 残留就是这么来的。每一步都必须看返回值。
    if ! mv "$tmp.log" "$tmp/extract.log"; then
        printf '[FAIL ] %-12s extract.log 归档失败，丢弃本次产物\n' "$name"
        drop_tmp "$tmp"
        drop_tmp "$tmp.log"
        n_fail=$((n_fail+1))
        continue
    fi

    old=""
    if [ -e "$dir" ]; then
        old="$dir.old.$$"
        if ! mv "$dir" "$old"; then
            printf '[FAIL ] %-12s 旧产物 %s 挪不动，保留原状不动\n' "$name" "$dir"
            drop_tmp "$tmp"
            n_fail=$((n_fail+1))
            continue
        fi
    fi

    if ! mv "$tmp" "$dir"; then
        printf '[FAIL ] %-12s 新产物无法就位到 %s，已把旧产物换回去\n' "$name" "$dir"
        [ -n "$old" ] && mv "$old" "$dir"
        drop_tmp "$tmp"
        n_fail=$((n_fail+1))
        continue
    fi

    # 到这里新产物已经就位，旧目录删不掉也只是留一个 .old.PID，不影响加载，
    # 所以只警告、不算转换失败——但必须说出来，不能让它悄悄变成磁盘垃圾。
    if [ -n "$old" ]; then
        if ! drop_tmp "$old"; then
            n_warn=$((n_warn+1))
        fi
    fi

    size=$(du -sh "$dir" 2>/dev/null | cut -f1)
    printf '[ OK  ] %-12s %s 秒，产物 %s → %s\n' \
        "$name" "$(( $(date +%s) - t0 ))" "${size:-?}" "$dir"
    n_build=$((n_build+1))
    ready+=("$name")

done < <(find "$SRC_DIR" -maxdepth 1 -type f \( -name '*.osm.pbf' -o -name '*.pbf' \) | sort)

# ---------------------------------------------------------------------------
# 地名 / POI / 街道名库（两份 Provinces 共用一个库，所以放在各省循环之外只做一次）
#
# 为什么必须在这里而不是手工跑：地名挂在 PBF 的**节点**上，graph_extract 一个节点
# 都不留（只抽 ways），所以"加了新地图却没重建这里"的表现极难发现 —— 地图照画、
# 路照通，只有搜索安静地变差、地图上安静地少了地名。以前就是这么丢掉生成器的。
#
# 指纹判据和上面各省那套一样：sources 的字节数 / mtime 全对上才跳过。
# 注意它的 meta 键是带下标的（source[0] / src_bytes[0]），跟 manifest.txt 不同。
# ---------------------------------------------------------------------------
PLACES_DB="$ROOT/products/places_auto.db"
PLACES_META="$PLACES_DB.meta"

pmeta_idx() {   # pmeta_idx <下标> <键名>
    # meta 的写法是 "键[下标]    = 值"，键值之间是 "= "，跟各省 manifest.txt
    # 的纯空格分隔不一样；这里把 "= " 一起剥掉，否则取回来的是 "= sources/x.pbf"。
    grep -m1 "^$2\[$1\][[:space:]]" "$PLACES_META" 2>/dev/null |
        sed 's/^[^=]*=[[:space:]]*//'
}

p_reason=""
if [ "$FORCE" = 1 ]; then
    p_reason="--force 指定重建"
elif [ ! -f "$PLACES_DB" ]; then
    p_reason="还没有地名库"
elif [ ! -f "$PLACES_META" ]; then
    p_reason="$PLACES_DB 没有 meta，无法判断是否过期"
else
    pi=0
    # ★ 这里**不套 ONLY 过滤**：地名库是 8 个省共用一个，下标必须是它在全量
    # sources 里的位置。套了过滤就会出现"用 Tianjin 去比 meta[0]"这种错位，
    # 结果是每次跑 --only 都误判要重建。
    while IFS= read -r f; do
        [ -n "$f" ] || continue
        # meta 里记的是相对 ROOT 的路径（直接跑 places_build 时传什么就是什么），
        # 而 find 给的是绝对路径 —— 不剥前缀就永远对不上，每次都会重建一次。
        frel="${f#$ROOT/}"
        mrel="$(pmeta_idx "$pi" source)"
        mrel="${mrel#$ROOT/}"     # meta 里可能记绝对路径（本脚本传的就是绝对路径）
        if [ "$mrel" != "$frel" ] ||
           [ "$(pmeta_idx "$pi" src_bytes)" != "$(fsize "$f")" ] ||
           [ "$(pmeta_idx "$pi" src_mtime)" != "$(fmtime "$f")" ]; then
            p_reason="源文件清单/指纹对不上（第一处出现在下标 $pi）"
            break
        fi
        pi=$((pi+1))
    done < <(find "$SRC_DIR" -maxdepth 1 -type f \( -name '*.osm.pbf' -o -name '*.pbf' \) | sort)
    if [ -z "$p_reason" ] && [ "$pi" -ne "$(grep -c '^source\[' "$PLACES_META")" ]; then
        p_reason="sources 数量变了（meta $(grep -c '^source\[' "$PLACES_META") vs 实际 $pi）"
    fi
fi

if [ -z "$p_reason" ]; then
    printf '[SKIP ] %-12s 已是最新（place %s 条）\n' "places" \
        "$(sqlite3 "$PLACES_DB" 'select count(*) from place' 2>/dev/null || echo '?')"
else
    printf '[BUILD] %-12s %s\n' "places" "$p_reason"
    if [ "$DRY" = 1 ]; then
        :
    else
        t0=$(date +%s)
        # 先建到临时文件，自检过了才 mv 就位：地名库是运行时由 sqlite3 直接打开的，
        # 绝不能让它在半途留下一个写了一半、还能被打开的库。
        ptmp="$PLACES_DB.tmp.$$"
        pmtmp="$PLACES_META.tmp.$$"
        if "$BIN/places_build" --sources="$SRC_DIR" --db="$ptmp" --meta="$pmtmp" > "$ptmp.log" 2>&1 &&
           "$BIN/places_build" --db="$ptmp" --selftest >> "$ptmp.log" 2>&1; then
            sed -e "s#^db .*#db           = ${PLACES_DB#$ROOT/}#" "$pmtmp" > "$pmtmp.fix" &&
                mv -f "$pmtmp.fix" "$pmtmp"
            if mv -f "$ptmp" "$PLACES_DB" && mv -f "$pmtmp" "$PLACES_META"; then
                printf '[ OK  ] %-12s %s 秒，place %s 条 → %s\n' "places" \
                    "$(( $(date +%s) - t0 ))" \
                    "$(sqlite3 "$PLACES_DB" 'select count(*) from place' 2>/dev/null)" "$PLACES_DB"
            else
                printf '[FAIL ] %-12s 新库无法就位\n' "places"; n_fail=$((n_fail+1))
            fi
            rm -f "$ptmp.log" "$pmtmp"
        else
            printf '[FAIL ] %-12s places_build 未通过（自检失败）\n' "places"
            tail -3 "$ptmp.log" | sed 's/^/        /'
            mv "$ptmp.log" "$ROOT/products/failed-places.log"
            rm -f "$ptmp" "$pmtmp"
            n_fail=$((n_fail+1))
        fi
    fi
fi

# ---------------------------------------------------------------------------
# 地标下界表 lm.bin（A* 启发式的加料，PLAN.md §50）
#
# 它是**可选**的：没有它程序退回纯直线启发式，照常算路、只是慢（长路线的展开数
# 多一个数量级）。所以它失败时只警告、不计入 n_fail —— 但必须说出来，否则表现是
# "新检出算路很慢"这种极难归因的问题。
#
# 指纹 = 各省 manifest 的 (nodes, edges, binfmt_version) + 表格式版本
#      + 地标个数与地标集指纹。
# ★ 别只看文件在不在：省数据重抽之后表就静默过期了，那时它不崩、不报错，
#   只是悄悄给出偏大的下界（路线次优甚至错）。
# ★ 地标那两项同理：改了 tools/lmbuild_cli.c 里的地标列表，路网指纹不变，
#   没有这两项就会 SKIP 掉重建，旧表继续被用。
# ---------------------------------------------------------------------------
LM_BIN="$OUT_ROOT/lm.bin"
LM_META="$LM_BIN.meta"
LM_WANT_VER=2          # 改 lmtable.h 的 LM_VERSION 时必须同步改这里
LM_NLM=30              # 用内置地标列表的前 N 个（列表在 tools/lmbuild_cli.c 里）

if [ "${#ready[@]}" -eq 0 ]; then
    printf '[SKIP ] %-12s 没有可用路网，跳过\n' "lm"
else
    # ★ 地标集指纹：lm.bin 的 header 里**不存**地标坐标，所以光看文件在不在、
    #   路网指纹对不对，都发现不了"源码里的地标列表改了"。不加这一项的话，
    #   改列表后重建会被 SKIP，旧表静默留在原地（与 §51.5 同源）。
    #   --print-lmfp 不加载路网，开销可忽略。
    lm_fp=$("$BIN/lmbuild_cli" --print-lmfp --nlm="$LM_NLM" 2>/dev/null |
            sed -n 's/^lmfp[[:space:]]\+//p' | head -1)

    want_fp=$(
        {
            for n in "${ready[@]}"; do
                m="$OUT_ROOT/$n/manifest.txt"
                printf '%s %s %s %s\n' "$n" "$(mani_get nodes "$m")" \
                    "$(mani_get edges "$m")" "$(mani_get binfmt_version "$m")"
            done
            printf 'lmver %s\n' "$LM_WANT_VER"
            printf 'nlm %s\nlmfp %s\n' "$LM_NLM" "$lm_fp"
        } | md5of
    )
    have_fp=$(sed -n 's/^fingerprint[[:space:]]\+//p' "$LM_META" 2>/dev/null | head -1)

    if [ -f "$LM_BIN" ] && [ -n "$have_fp" ] && [ "$have_fp" = "$want_fp" ]; then
        printf '[SKIP ] %-12s 已是最新（%s 字节）\n' "lm" "$(stat -c %s "$LM_BIN")"
    else
        if [ -n "$have_fp" ]; then lm_reason="路网指纹变了"
        else lm_reason="还没有地标表"; fi
        printf '[BUILD] %-12s %s\n' "lm" "$lm_reason"
        if [ "$DRY" = 1 ]; then
            :
        else
            ltmp="$LM_BIN.tmp.$$"
            t0=$(date +%s)
            # 直接写临时文件，成功才 mv：万一构建中途 OOM，不能把一份半截的表
            # 留在正式路径上（半截表 magic 是对的，会被当成有效表加载）。
            if "$BIN/lmbuild_cli" --graph="$OUT_ROOT" --out="$ltmp" \
                    --cell=3000 --nlm="$LM_NLM" > "$ltmp.log" 2>&1 &&
               [ -s "$ltmp" ]; then
                printf 'fingerprint %s\nlmver %s\ncell_m 3000\nnlm %s\nlmfp %s\n' \
                    "$want_fp" "$LM_WANT_VER" "$LM_NLM" "$lm_fp" > "$LM_META.tmp.$$"
                if mv -f "$ltmp" "$LM_BIN" && mv -f "$LM_META.tmp.$$" "$LM_META"; then
                    printf '[ OK  ] %-12s %s 秒，%s 字节 → %s\n' "lm" \
                        "$(( $(date +%s) - t0 ))" "$(stat -c %s "$LM_BIN")" "$LM_BIN"
                else
                    printf '[WARN ] %-12s 新表无法就位，算路会退回纯直线启发式\n' "lm"
                    n_warn=$((n_warn+1))
                fi
            else
                printf '[WARN ] %-12s 构建失败（%s 秒），算路退回纯直线启发式\n' \
                    "lm" "$(( $(date +%s) - t0 ))"
                tail -3 "$ltmp.log" | sed 's/^/        /'
                mv "$ltmp.log" "$ROOT/products/failed-lm.log"
                n_warn=$((n_warn+1))
            fi
            rm -f "$ltmp" "$ltmp.log" "$LM_META.tmp.$$"
        fi
    fi
fi

printf '\n---- 汇总 ----\n'
printf '跳过(已最新) %d   转换 %d   失败 %d' "$n_skip" "$n_build" "$n_fail"
if [ "$n_warn" -gt 0 ]; then printf '   残留待清理 %d' "$n_warn"; fi
printf '\n'

if [ "${#ready[@]}" -gt 0 ]; then
    printf '\n可用路网（%d 个）：\n' "${#ready[@]}"
    for n in "${ready[@]}"; do
        m="$OUT_ROOT/$n/manifest.txt"
        printf '  %-12s 节点 %-10s 边 %-10s  %s\n' \
            "$n" "$(mani_get nodes "$m")" "$(mani_get edges "$m")" "$OUT_ROOT/$n"
    done
    printf '\n启动（一次只能加载一个，跨省索引还没做）：\n'
    printf '  ./build/navimap --graph=%s/%s\n' "$OUT_ROOT" "${ready[0]}"
fi

[ "$n_fail" -eq 0 ] || exit 1
```

### 3.2 建图核心：`tools/graph_extract.c`（完整体）

PBF → 自描述路网二进制（`nodes.bin`/`edges.bin`/`osmid.bin`/`names.bin`/`restrictions.bin`/`manifest.txt`），并顺手写派生索引 `rev.bin`。**闸门没过就不写 manifest**（`graph_extract.c:89-107`），让“有 manifest”严格等价于“产物可用”。

```c
/* graph_extract —— PBF → 自描述路网二进制
 *
 * 用法：
 *   ./graph_extract --out=products/graph/beijing sources/beijing.osm.pbf
 *   ./graph_extract --fmt-version          # 只打印当前 NMBIN 版本号后退出
 *
 * 产出（全部带 NAVMBIN 自描述头，字段名/偏移/类型写在文件里）：
 *   nodes.bin  edges.bin  osmid.bin  names.bin  restrictions.bin  manifest.txt
 *
 * 退出码非 0 = 有断言失败（闸门见下）。
 *
 * manifest.txt 是"这份产物由哪个源文件、多大的文件、什么时候生成的"指纹，
 * scripts/sync_maps.sh 靠它判断要不要重转；并且★只在全部闸门通过后才写，
 * 所以"有 manifest"等价于"这份产物可用"。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>

#include "map/graph_build.h"
#include "route/revidx.h"
#include "map/binfmt.h"
#include "pbf/pbf_stream.h"
#include "utils/plat.h"

int main(int argc, char **argv)
{
    /* 给同步脚本用的探针：打印当前二进制格式版本。格式一升级，
       所有旧产物的 manifest 版本号就对不上，必须重转——否则加载端
       （app_init 只比对 magic，不校验版本）会静默画错图。 */
    if (argc == 2 && strcmp(argv[1], "--fmt-version") == 0) {
        printf("%d\n", NMBIN_VERSION);
        return 0;
    }

    const char *out_dir = NULL, *src = NULL;
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--out=", 6) == 0) out_dir = argv[i] + 6;
        else if (argv[i][0] == '-') { fprintf(stderr, "未知参数 %s\n", argv[i]); return 2; }
        else src = argv[i];
    }
    if (!src || !out_dir) {
        fprintf(stderr, "用法: %s --out=<目录> <file.osm.pbf>\n", argv[0]);
        return 2;
    }
    nm_mkdir_p(out_dir);

    /* 源文件指纹：同步脚本靠 size+mtime 判断产物是否过期 */
    uint64_t src_bytes = 0;
    int64_t  src_mtime = 0;
    nm_stat_file(src, &src_bytes, &src_mtime);

    GraphBuildStats st;
    memset(&st, 0, sizeof(st));
    if (graph_build(src, out_dir, &st) != 0) {
        fprintf(stderr, "graph_build 失败\n");
        return 1;
    }

    printf("source         %s\n", src);
    printf("ways_total     %" PRIu64 "\n", st.ways_total);
    printf("ways_kept      %" PRIu64 "\n", st.ways_kept);
    printf("ways_skip      %" PRIu64 "  (no highway %" PRIu64 ", too short %" PRIu64 ")\n",
           st.ways_skip, st.ways_no_hw, st.ways_too_short);
    printf("nodes          %" PRIu64 "\n", st.nodes);
    printf("edges          %" PRIu64 "\n", st.edges);
    printf("lines          %" PRIu64 "  (步行道/铁路/水系线，不进路由)\n", st.lines_kept);
    printf("areas          %" PRIu64 "  (不闭合跳过 %" PRIu64 ", 节点缺失丢弃 %" PRIu64 ")\n",
           st.areas_kept, st.areas_skip_open, st.linearea_dropped_missing);
    printf("dropped_missing_node %" PRIu64 "  (无坐标节点剔除 %" PRIu64 ")\n",
           st.dropped_missing_node, st.dead_nodes);
    printf("names          %" PRIu64 "\n", st.names);
    printf("lanes_ways     %" PRIu64 "  (含 turn:lanes 的 way，写入车道指示)\n", st.ways_with_lanes);
    printf("lane_strings   %" PRIu64 "  (去重后的车道转向串)\n", st.lane_strings);
    printf("rt_unknown_ways %" PRIu64 "  (白名单内 %" PRIu64 ", 闸门 rt_unknown_drivable=%" PRIu64 ")\n",
           st.rt_unknown_ways, st.rt_unknown_nondrivable, st.rt_unknown_drivable);
    printf("restrictions   written %" PRIu64 " / scanned %" PRIu64
           "  (via=way 展开 %" PRIu64 ", via=way 落空 %" PRIu64
           ", 成员不全 %" PRIu64 ", 定位失败 %" PRIu64 ")\n",
           st.restr_written, st.restr_scanned, st.restr_via_way_written,
           st.restr_skip_via_way, st.restr_skip_incomplete, st.restr_skip_unresolved);
    printf("bbox_e7        %d %d %d %d\n", st.bbox[0], st.bbox[1], st.bbox[2], st.bbox[3]);
    printf("tiles          %" PRIu64 "\n", st.tiles_written);
    printf("disp_tiles     %" PRIu64 "  (显示层分 tile，运行时只加载视口内)\n", st.disp_tiles);
    printf("elapsed_sec    %.1f\n", st.elapsed_sec);

    /* ---- 闸门 ---- */
    int fail = 0;
    if (st.rt_unknown_drivable != 0) {
        printf("ASSERT-FAIL    rt_unknown_drivable=%" PRIu64
               "（有可通行道路的 highway 取值不在表里，必须补表）\n", st.rt_unknown_drivable);
        fail = 1;
    }
    if (st.nodes == 0 || st.edges == 0) { printf("ASSERT-FAIL    空图\n"); fail = 1; }
    if (st.ways_kept + st.ways_skip + st.lines_kept + st.areas_kept +
        st.areas_skip_open != st.ways_total) {
        printf("ASSERT-FAIL    ways_kept+skip+lines+areas != total\n"); fail = 1;
    }

    /* ★ 闸门没过就不写 manifest：让"有 manifest"严格等价于"产物可用"。
       以前是先写 manifest 再判 fail，结果同步脚本会把失败产物当成已转换。 */
    if (fail) {
        fprintf(stderr, "ASSERT-FAIL 详见上面的 ASSERT-FAIL 行（未写 manifest，产物不可用）\n");
        return 1;
    }

    /* ---- manifest ---- */
    char mp[4096];
    snprintf(mp, sizeof(mp), "%s/manifest.txt", out_dir);
    FILE *f = fopen(mp, "wb");
    if (f) {
        fprintf(f, "# NavMap 路网抽取结果（自描述二进制 NAVMBIN v%d）\n", NMBIN_VERSION);
        fprintf(f, "source         %s\n", src);
        fprintf(f, "src_bytes      %" PRIu64 "\n", src_bytes);
        fprintf(f, "src_mtime      %" PRId64 "\n", src_mtime);
        fprintf(f, "generated_at   %" PRId64 "\n", (int64_t)time(NULL));
        fprintf(f, "binfmt_version %d\n", NMBIN_VERSION);
        fprintf(f, "ways_total     %" PRIu64 "\n", st.ways_total);
        fprintf(f, "ways_kept      %" PRIu64 "\n", st.ways_kept);
        fprintf(f, "ways_skip      %" PRIu64 "\n", st.ways_skip);
        fprintf(f, "nodes          %" PRIu64 "\n", st.nodes);
        fprintf(f, "edges          %" PRIu64 "\n", st.edges);
        fprintf(f, "lines          %" PRIu64 "\n", st.lines_kept);
        fprintf(f, "areas          %" PRIu64 "   # 不闭合 %" PRIu64 " 缺失丢弃 %" PRIu64 "\n",
                st.areas_kept, st.areas_skip_open, st.linearea_dropped_missing);
        fprintf(f, "dropped_missing_node %" PRIu64 "\n", st.dropped_missing_node);
        fprintf(f, "dead_nodes       %" PRIu64 "   # 无坐标被剔除\n", st.dead_nodes);
        fprintf(f, "names          %" PRIu64 "\n", st.names);
        fprintf(f, "rt_unknown_ways %" PRIu64 "\n", st.rt_unknown_ways);
        fprintf(f, "rt_unknown_nondrivable %" PRIu64 "\n", st.rt_unknown_nondrivable);
        fprintf(f, "rt_unknown_drivable %" PRIu64 "   # ★ 闸门：必须为 0\n", st.rt_unknown_drivable);
        fprintf(f, "restrictions   %" PRIu64 "   # 扫描 %" PRIu64
                "（via=way 展开 %" PRIu64 " / via=way 落空 %" PRIu64
                " / 成员不全 %" PRIu64 " / 定位失败 %" PRIu64 "）\n",
                st.restr_written, st.restr_scanned, st.restr_via_way_written,
                st.restr_skip_via_way, st.restr_skip_incomplete,
                st.restr_skip_unresolved);
        fprintf(f, "bbox_e7        %d %d %d %d\n", st.bbox[0], st.bbox[1], st.bbox[2], st.bbox[3]);
        fprintf(f, "disp_tiles     %" PRIu64 "\n", st.disp_tiles);
        fprintf(f, "elapsed_sec    %.1f\n", st.elapsed_sec);
        fclose(f);
    } else {
        fprintf(stderr, "警告: manifest.txt 写不出来（%s），同步脚本会认为这里没转换过\n", mp);
    }

    /* ---- 派生索引 rev.bin（反向入边）----
       它 100% 由 nodes/edges 决定，抽取时顺手写一份，运行时就不用每次算路
       现建（hebei 实测 323 ms，占短途端到端 401 ms 的 80%）。
       ★ 写失败不算闸门失败：运行时读到没有/坏的 rev.bin 会回退到现场构建，
         结果一定正确，只是慢。不能让"派生文件没写成"毁掉一整次抽取。 */
    {
        NMBinFile nf, ef;
        char p[4096];
        int ok = 0;
        snprintf(p, sizeof p, "%s/nodes.bin", out_dir);
        if (nmbin_open(p, &nf) == 0) {
            snprintf(p, sizeof p, "%s/edges.bin", out_dir);
            if (nmbin_open(p, &ef) == 0) {
                ok = (rev_write(out_dir, &nf, &ef) == 0);
                nmbin_close(&ef);
            }
            nmbin_close(&nf);
        }
        printf("rev_bin        %s\n", ok ? "written" : "skipped（运行时会现场构建）");
    }

    printf("ASSERT-OK\n");
    return 0;
}
```

### 3.3 每帧渲染（离屏通道）：`src/ui/offscreen_shot.c` 的 `offscreen_shot_main` 关键段（完整体位于 `offscreen_shot.c:194-647`）

`navmap_qt --shot-offscreen` 调 `offscreen_shot_main()`，是离屏像素审计的**单一可信源**（regress 的全部离屏判据都走它）。它内部用 `app_init` 加载图集、`app_render_to_buf` 渲进内存缓冲、`write_bmp24` 写 24 位 BMP，**零 SDL**。下面展示“进程内算路”这一段（L11 路由结果被直接叠进画面），以及末尾的确定性量审计锚点（L8/L9 的 `areas_dotted`）。

```c
    App a;
    if (app_init(&a, graph, w, h, font, font_size, &ab) != 0) return 1;
    a.qt_ui = 0;   /* 离屏审计通道：让 C 核心把 chrome（无图横幅/搜索框/标题栏）
                      画进缓冲 —— 审计的计数源必须是 C 核心缓冲，不能换成
                      Qt 控件，否则像素判据失去意义。 */
    if (pbf_display) {
        if (mapset_enable_pbf_display(&a.ms, pbf_display, pbf_lru) != 0) {
            fprintf(stderr, "提示: PBF 显示层不可用（%s），改用 nmbin 产物\n", pbf_display);
        } else {
            printf("显示层: PBF 运行时解码（%s, LRU %d）\n", pbf_display, pbf_lru);
            /* 离屏截图：没有"逐帧渐进"可言，直接同步解码各省。 */
            for (int i = 0; i < a.ms.n; i++)
                mapset_ensure_display(&a.ms, i);
        }
    }
    if (have_center == 3) { a.view.lat = lat; a.view.lon = lon; }
    a.view.zoom = zoom;
    a.view.w = w; a.view.h = h;
    a.max_class = max_class;
    if (settings_ovl) { a.settings_open = 1; a.settings_ovl = 1; }
    if (open_tf) { a.tf_open = 1; }
    if (open_menu) { a.menu_open = 1; }
    /* --ovl-alpha：默认 75% 已在 app_init 里读好，这里再覆盖 */
    for (int i = 0; i < n_ovl; i++)
        app_ovl_alpha_set(&a, ovl_set[i].id, ovl_set[i].pct);

    /* 进程内算路（阶段 9）：起终点 → 路线 + 转向指令，随后由 paint_frame
       直接叠到画面上，不用先导出 GeoJSON。 */
    int have_route = 0;
    if (from_s || to_s) {
        if (!from_s || !to_s) {
            printf("ASSERT-FAIL    --from 与 --to 必须成对给\n");
            return 1;
        }
        double alat, alon, blat, blon;
        if (parse_ll(from_s, &alat, &alon) != 0 ||
            parse_ll(to_s, &blat, &blon) != 0) {
            printf("ASSERT-FAIL    起终点格式应为 lat,lon\n");
            return 1;
        }
        if (app_nav_plan_sync(&a, alat, alon, blat, blon) != 0) {
            printf("ASSERT-FAIL    算路失败：%s\n",
                   a.nav_msg[0] ? a.nav_msg : "(未知)");
            return 1;
        }
        have_route = 1;
        printf("route          %.2f km  约 %.0f 分  %u 段  接缝 %u  展开 %llu  %llu ms\n",
               a.rr.total_m / 1000.0, a.rr.total_s / 60.0, a.rr.n - 1,
               a.rr.n_seams, (unsigned long long)a.rr.expanded,
               (unsigned long long)a.rr.elapsed_ms);
        printf("snap           起点 %.1f m  终点 %.1f m\n",
               a.pick_a.snap_m, a.pick_b.snap_m);
        printf("maneuvers      %d 条（里程和 %.1f m / 总里程 %.1f m）\n",
               a.man.n, a.man.sum_m, a.rr.total_m);
        if (a.have_alts) {
            printf("alts           %d 方案\n", a.ralts.n);
            for (int k = 0; k < a.ralts.n; k++) {
                if (a.ralts.err[k] == 0)
                    printf("  [%d] %-8s %.2f km  约 %.0f 分  收费 %.2f km  非铺装 %.2f km\n",
                           k, a.ralts.tag[k], a.ralts.total_m[k] / 1000.0,
                           a.ralts.total_s[k] / 60.0, a.ralts.toll_m[k] / 1000.0,
                           a.ralts.unpaved_m[k] / 1000.0);
                else
                    printf("  [%d] %-8s (不可行 err=%d)\n", k, a.ralts.tag[k], a.ralts.err[k]);
            }
        }
        if (have_center != 3 || fit_route) {
            app_nav_fit(&a);
            printf("fit-route      zoom %.2f 中心 %.5f,%.5f\n",
                   a.view.zoom, a.view.lat, a.view.lon);
        }
    }
```

末尾的审计锚点（确定性量 `areas_dotted`，与 `regress.sh` 的 `area_lod_tier` 同口径）：

```c
    printf("areas_drawn    %llu\n", (unsigned long long)st.areas_drawn);
    /* ★ 面要素 LOD 档位的判据锚点（确定性量，不用时间）：
     *   走"小面画点"快速路径的面数。AREA_DOT_PX 定档 4 时这个数会变，
     *   它直接反映"多少面被简化成单个像素"，与机器负载无关。 */
    printf("areas_dotted   %llu\n", (unsigned long long)st.areas_dotted);
    printf("lines_drawn    %llu\n", (unsigned long long)st.lines_drawn);
    printf("dedup_skipped  %llu\n", (unsigned long long)st.dedup_skipped);
    printf("place_labels   %llu\n", (unsigned long long)st.labels_drawn);
    printf("poi_icons      %llu\n", (unsigned long long)st.poi_icons);
    printf("road_labels    %llu\n", (unsigned long long)st.road_labels_drawn);

    /* 像素计数 + 断言 + 日志：交给共享审计模块（render/audit.c）。
       navmap_qt --shot-offscreen 也走同一份逻辑，保证两前端判据逐位一致。 */
    AuditCtx actx;
    actx.have_search = have_search;
    actx.have_route  = have_route;
    actx.have_sim    = have_sim;
    actx.deviate_m   = deviate_m;
    actx.n_gpx_in    = n_gpx_in;
    int rc = app_offscreen_audit(&a, buf, w, h, w * 4, &st, &actx);
    free(buf);
    return rc;
```

### 3.4 用户交互（路由 + 转向指令）：`tools/route_cli.c` 的 `--maneuvers` 块（完整体位于 `route_cli.c:280-300`）

`route_cli` 是路由的命令行入口（L11）。`--maneuvers` 调 `maneuver_build` 生成转向指令，并断言“指令里程和 == 总里程”（不变量）。GUI 面板看到的转向指令就是同一份。

```c
    if (want_man) {
        ManeuverList ml;
        int nm = maneuver_build(&rr, &ms, &ml);
        printf("转向指令      %d 条\n", nm);
        char t[160];
        for (int i = 0; i < nm; i++) {
            maneuver_text(&ml.v[i], t, sizeof(t));
            printf("  %2d  %8.0f m  %-12s %s\n", i + 1, ml.v[i].dist_m,
                   maneuver_kind_text(&ml.v[i]), t);
        }
        /* 不变量：指令里程之和必须等于总里程。不等就说明有段被漏掉或重复计。 */
        double tol = rr.total_m * 0.001 + 1.0;
        if (nm < 2 || fabs(ml.sum_m - rr.total_m) > tol) {
            printf("ASSERT-FAIL 指令里程和 %.1f m != 总里程 %.1f m\n",
                   ml.sum_m, rr.total_m);
            fail = 1;
        }
        printf("指令里程和    %.1f m（总里程 %.1f m，差 %.2f m）\n",
               ml.sum_m, rr.total_m, ml.sum_m - rr.total_m);
        maneuver_free(&ml);
    }
```

### 3.5 端到端命令序列（从一份 PBF 到屏幕上一条导航路线）

把上面各段按运行时顺序串起来，得到一条可复现的链路（命令均为真实可用的接口）：

```sh
# ① 数据生产（L4 解析 / L5 二进制 / L6 建图）：把 PBF 变成可加载路网
cp  ~/Downloads/beijing.osm.pbf  sources/beijing.osm.pbf
./scripts/sync_maps.sh                       # 内部调 graph_extract + graph_check + places_build + lmbuild_cli
#   产物：products/graph/beijing/{nodes,edges,osmid,names,restrictions}.bin + manifest.txt + rev.bin
#         products/places_auto.db（L10 搜索库）、products/graph/lm.bin（L11 地标表）

# ② 算一条路 + 转向指令（L11 路由 / L12 导航层）
./build/route_cli --graph=products/graph --from=39.90,116.40 --to=34.75,113.62 --maneuvers
#   输出：起终点吸附、总长、绕行系数、预计分钟、转向指令 N 条（里程和==总里程）

# ③ 离屏出一张带路线的图（L7 视口投影 / L8 软件光栅 / L9 渲染管线 / L13 Qt 外壳）
./build/navmap_qt --shot-offscreen --graph=products/graph/beijing \
    --from=39.90,116.40 --to=34.75,113.62 --fit-route \
    --lat=39.90 --lon=116.40 --zoom=12 --w=1280 --h=800 --out=/tmp/route.bmp
#   等价于 GUI 里选起终点后看到的画面；offscreen_shot_main 直接调 app_nav_plan_sync 算路并叠画。

# ④ 搜索验收（L10）：离屏也能查 + 画
./build/navmap_qt --shot-offscreen --graph=products/graph/beijing --q=中关村 --w=1280 --h=800 --out=/tmp/search.bmp

# ⑤ 门禁（L14）：改完跑全套
./scripts/regress.sh            # 或 ./scripts/regress.sh --build 先编译
./scripts/check_layers.sh       # 纯 DAG 依赖方向 0 违规
```

这一条链路恰好覆盖：**数据生产**（PBF→二进制→建图）→ **每帧渲染**（视口投影→软件光栅→管线）→ **用户交互**（搜索→路由→导航语音）→ **Qt 外壳** → **门禁**。

## 四、与其他功能之间的联系

- **L2 坐标系 / L3 平台抽象**：全程底座。`utils/geo`（墨卡托投影、`nm_haversine_m`）与 `utils/plat`（线程 / mmap / 临时目录）被 L4–L13 所有模块依赖，是纯 DAG 的最底端叶子，反向绝不被依赖。
- **L4 PBF 解析 → L5 自描述二进制与图集 → L6 建图**：`graph_extract`（L6）消费 `pbf/pbf_stream`（L4），产出 `nmbin` 自描述二进制（L5），再由 `map/mapset`（L5）在运行时按视口加载。这是 `mapset_set_view`（L7）在视野变化时加载的内容，也是路由（L11）的输入。
- **L7 视口投影 → L8 软件光栅 → L9 渲染管线**：`mv_visible_bbox`/`mv_project`（L7）决定可见范围，喂给 `paint_areas/roads/lines`（L8），由 `paint_frame`（L9，`offscreen_shot.c` 的 `render_buffer` → `app_render_to_buf` → `paint_frame`）按 9 步编排成一帧。
- **L10 搜索 → L11 路由 → L12 导航语音**：UI 上触发先后出现；`route_cli`/`app_nav_plan_sync`（L11）出 `RouteResult`，交给 `maneuver`（L12）生成指令，再经 `navsim`/`voice`/`ui/voice_wire`（L12）播报。
- **L13 Qt 前端**：只是外壳——`qt_main.cpp` 启动事件循环，每帧 `MapCanvas::tick()` 驱动 `paint_frame`；它只链 `navimap_core + Qt Widgets`，不链 SDL。
- **L14 架构门禁**：守护前面所有课——`check_layers.sh` 把纯 DAG 锁死，`core_no_sdl` 把“L13 不链 SDL”锁死，`regress.sh` 把 L8/L9/L11/L12/L13 的几十个用例串成回归门禁。
- **纯 DAG 总结**：`utils ← pbf ← map ← route ← render ← ui ← ui_qt`。`map` 只许依赖 `pbf,utils`；`route` 只许依赖 `map,utils`；`render` 纯光栅、零 ui 依赖；`ui` 依赖 `render` 但绝不被 `render/map/route` 反向依赖；`ui_qt` 只依赖 `map,render,settings,ui,utils`。任何反向依赖会被链接器（第一道）与 `check_layers.sh`（第二道）同时抓出。

## 五、演示、练习与验收

- **端到端演示（从 PBF 到屏幕路线）**：
  1. 放一份 `beijing.osm.pbf` 到 `sources/`，跑 `./scripts/sync_maps.sh`，确认 `products/graph/beijing` 出现 `manifest.txt` 与各类 `.bin`；
  2. 跑 `./build/route_cli --graph=products/graph --from=39.90,116.40 --to=34.75,113.62 --maneuvers`，确认打印里程与转向指令、且 `ASSERT-OK`；
  3. 跑 `./build/navmap_qt --shot-offscreen --graph=products/graph/beijing --from=39.90,116.40 --to=34.75,113.62 --fit-route --out=/tmp/route.bmp`，打开 `/tmp/route.bmp` 应能看到一条红色路线；
  4. 跑 `./scripts/regress.sh`，确认全绿（或仅剩已知环境缺失项，如 beijing 缺 `lanes.bin`）。
- **练习（改 → 测 → 守 闭环）**：任选 `capstone.md` 原稿四类选题之一（如【中】`AREA_DOT_PX` 4→6）：改 `src/render/paint.h` → 用 `scripts/bmpdiff.py` 量化净损失面积 → 更新档位表并收紧 `regress.sh` 的 `area_lod_tier` 区间 → 跑回归确认被钉住。
- **验收 checklist**：
  - [ ] `./scripts/check_layers.sh` 零违规（无反向 `#include`）；
  - [ ] 改动有对应 `regress.sh` 判据（确定性量优先于时间）；
  - [ ] 负向对照：故意把改动改坏，确认判据 FAIL；改回后 PASS；
  - [ ] `./scripts/regress.sh` 全绿（或仅剩已知环境缺失项，如 beijing 缺 `lanes.bin`）；
  - [ ] 文档：在对应 `docs/teaching/lesson-*.md` 或 `docs/*.md` 补一句“本项目已实践”。

### 参考：本项目已踩过的“别再试”清单（来自工作记忆）

- 渲染简化几何、去描边、面 tile 索引、顶点内联、软件预取、缓存上一条目、合并折线、NMBIN v3——均已被否或回退。
- 瓦片缓存/条带复用已试做并全量回退（条带当独立小视口漏画大要素；只设 raster clip 是负优化）。
- 算路：rev.bin v2 / PQ 叉数≠4 / 状态表改布局——均三连败。
- 地标：>30 无收益、加大 penalty 无效；w 随地标紧度下走，改地标数须连带重定 w。
- `nmbin_open` 全部 disp tile 会耗尽 Windows 映射句柄 ⇒ 逐 tile 开→拼→关。
- 浮层透明度：`QGraphicsOpacityEffect` 已否；“整块元素一起淡”已否（J 先生否决）。Qt 唯一可行解 = IconButton 自绘底板按系数算 alpha + QSS `rgba()` + viewport 透明。

> 这些不是理论禁忌，而是用回归判据实测淘汰的结论。做综合项目时优先复用已验证路径，不要重走被否分支。
