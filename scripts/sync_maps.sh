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
