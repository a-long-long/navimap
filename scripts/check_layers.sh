#!/usr/bin/env bash
#
# 分层方向门禁：机器检查模块依赖方向，防止倒置回流。
#
#   用法: ./scripts/check_layers.sh              检查 src/ 全量（打印依赖图）
#         ./scripts/check_layers.sh --quiet      只在失败时输出
#
# 为什么要有它：
#   PLAN.md §53 的 R1/R2 把两处模块级倒置（map→render、render→ui）消掉了，
#   但**倒置是会被手痒写回来的**。原先没有任何机器检查，全靠人自觉 ——
#   而 ARCH_ASSESSMENT.md §4 早就画出了这两条边，只标注"语义上合理、无需破坏"，
#   结果它们一待就是几个月。
#   更糟的是：符号级环曾迫使 CMake 用 -Wl,--start-group 包住全部模块库，
#   链接器多轮扫描会**悄悄补上**新引入的反向依赖 ⇒ 边界形同虚设。
#   （R2 已移除 start-group，现在新引入的反向依赖会立刻 undefined-reference
#     —— 链接器这道防线回来了，但只有"方向真的一直对"才有效。）
#
# 判据：解析每个源文件的**真实** #include（剥掉块注释与行注释），
#       算出模块级依赖图，与允许的方向表比对。有违规则退出码非 0。
#
# ★ 负向对照（必须有分辨力，否则就是假门禁）：
#   本脚本曾被人为插入一条 map→render 的 include 验证过：
#   它确实 FAIL 并指出具体文件行号；撤掉后恢复 PASS。
#   见 PLAN.md §53.5。

set -u
cd "$(dirname "$0")/.."
ROOT=$PWD

QUIET=0
for a in "$@"; do
    case "$a" in
        --quiet) QUIET=1 ;;
        *) echo "未知参数: $a" >&2; exit 2 ;;
    esac
done

say() { [ "$QUIET" = 1 ] || printf '%s\n' "$*"; }

# ★ 结论行**始终**打印（不受 --quiet 影响）：回归门禁的 run() 靠
#   "必须出现的标记 + 退出码"双判据判定，标记缺失会被判成假 FAIL。
say_result() { printf '%s\n' "$*"; }

# ---------------------------------------------------------------------------
# 允许的依赖方向（from 模块 → 允许依赖的模块集合，空格分隔）
#
# 层级（自下而上）：
#   utils    叶子（平台抽象 / 几何 / 内存 / 字符串池），零内部依赖
#   pbf      叶子（PBF 解析），零内部依赖
#   map      → pbf, utils
#   route    → map, utils
#   search   → utils
#   settings → utils
#   render   → map, route, search, settings, utils   （纯光栅层，零 ui 依赖）
#   ui       → map, render, route, search, settings, utils
#   ui_qt    → map, render, settings, ui, utils
#
# ★ 同层依赖允许（render→render、ui→ui）：那是模块内部拆分，不是层级倒置。
# ★ 刻意**不**允许的（就是本门禁要守的）：
#     map  → render / route / ui / ui_qt
#     render → ui / ui_qt
#     route → render / ui / ui_qt
#     search→ map / render / route / ui / ui_qt
# ---------------------------------------------------------------------------
export NM_ALLOWED_map="map pbf utils"
export NM_ALLOWED_pbf="pbf"
export NM_ALLOWED_route="route map utils"
export NM_ALLOWED_search="search utils"
export NM_ALLOWED_settings="settings utils"
export NM_ALLOWED_render="render map route search settings utils"
export NM_ALLOWED_ui="ui map render route search settings utils"
export NM_ALLOWED_ui_qt="ui_qt map render settings ui utils"
export NM_ALLOWED_utils="utils"

PY=$(command -v python3 || command -v python)
if [ -z "$PY" ]; then
    echo "需要 python3 才能解析 include" >&2
    exit 2
fi

OUT=$("$PY" - "$ROOT" <<'PYEOF'
import os, re, sys, glob

root = sys.argv[1]
src = os.path.join(root, "src")
if not os.path.isdir(src):
    print("ERR: no src dir", file=sys.stderr)
    sys.exit(2)

MODS = ["utils", "pbf", "map", "route", "search", "settings", "render", "ui", "ui_qt"]

def strip_comments(s):
    s = re.sub(r'/\*.*?\*/', '', s, flags=re.S)
    s = re.sub(r'//[^\n]*', '', s)
    return s

inc_re = re.compile(r'#\s*include\s*"([^"]+)"')

# 收集真实 include 边：逐文件、逐行剥注释后匹配
edges = []   # (from_mod, to_mod, relfile, lineno, target)
for mod in MODS:
    files = []
    for ext in ("c", "h", "cpp", "hpp", "cc"):
        files += glob.glob(os.path.join(src, mod, "**", "*." + ext), recursive=True)
    for f in sorted(set(files)):
        try:
            raw = open(f, encoding="utf-8", errors="replace").read()
        except OSError:
            continue
        rel = os.path.relpath(f, root)
        for ln, line in enumerate(raw.splitlines(), 1):
            l = strip_comments(line)
            for m in inc_re.finditer(l):
                tgt = m.group(1)
                if "/" not in tgt:
                    continue
                tmod = tgt.split("/", 1)[0]
                if tmod not in MODS:
                    continue
                edges.append((mod, tmod, rel, ln, tgt))

# 允许方向表
allowed = {}
for mod in MODS:
    env_key = "NM_ALLOWED_" + mod
    allowed[mod] = set(os.environ.get(env_key, "").split())

# 汇总模块级依赖（去重）
dep = {}
for fm, tm, rel, ln, tgt in edges:
    dep.setdefault(fm, set()).add(tm)

# 违规边（逐条 include 报出来，便于定位到行号）
viol = []
for fm, tm, rel, ln, tgt in edges:
    if tm not in allowed.get(fm, set()):
        viol.append((fm, tm, rel, ln, tgt))

# 输出格式：
#   DEP|<mod>|<dep1> <dep2> ...
#   VIOL|<from>|<to>|<file>|<line>|<target>
for mod in MODS:
    d = sorted(dep.get(mod, set()))
    print("DEP|%s|%s" % (mod, " ".join(d)))
for fm, tm, rel, ln, tgt in sorted(set(viol)):
    print("VIOL|%s|%s|%s|%d|%s" % (fm, tm, rel, ln, tgt))

print("COUNT|%d" % len(set(viol)))
PYEOF
)
rc=$?
if [ "$rc" != 0 ]; then
    echo "分层检查脚本自身执行失败（python 退出码 $rc）" >&2
    exit 2
fi

viol_count=$(printf '%s\n' "$OUT" | grep '^COUNT|' | cut -d'|' -f2)
viol_count=${viol_count:-0}

say "=== 模块依赖图（真实 include，剥注释后）==="
printf '%s\n' "$OUT" | grep '^DEP|' | while IFS='|' read -r _ mod deps; do
    say "$(printf '  %-9s -> %s' "$mod" "${deps:-（无）}")"
done

if [ "$viol_count" != 0 ]; then
    say ""
    say "=== 分层违规 ${viol_count} 处 ==="
    printf '%s\n' "$OUT" | grep '^VIOL|' | while IFS='|' read -r _ fm tm rel ln tgt; do
        printf '  ❌ %s → %s  (%s:%s  #include "%s")\n' "$fm" "$tm" "$rel" "$ln" "$tgt"
    done
    say ""
    say_result "分层方向门禁 FAIL：模块依赖方向必须保持 DAG，不得反向依赖。"
    exit 1
fi

say ""
say_result "分层方向门禁 PASS：0 违规"
exit 0
