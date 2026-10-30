# 第 13 课 · 架构纪律与验收门禁

> 模块/脚本：`scripts/check_layers.sh`、`scripts/regress.sh`、`CMakeLists.txt`、`tools/core_no_sdl.c`、`tools/blend_equiv.c`、`tools/clip_equiv.c`
> 前置：第 1–12 课全部内容（本课是“为什么敢改”的门禁总览）。

## 一、教学目的

理解“为什么这个工程敢改”——因为有一整套防回归门禁：分层门禁、像素等价对照、确定性量判据、负向对照。学完后学生应能：

- 独立跑全套门禁；
- 解释“为什么判据要用确定性量而非时间”；
- 说明“为什么 `-Wl,--start-group` 被移除”；
- 讲清纯 DAG 依赖 `utils ← pbf ← map ← route ← render ← ui ← ui_qt` 是怎么被两层防线（链接器 + `check_layers.sh`）共同钉死的。

## 二、功能构成

| 工具/脚本 | 职责 | 路径与行号区间 |
|---|---|---|
| `scripts/check_layers.sh` | 解析真实 `#include`（剥块注释与行注释）算模块依赖图，与方向表比对，违规打印文件行号并 `exit 1` | `scripts/check_layers.sh:1-179`（完整体见第三节） |
| `scripts/regress.sh` | 全量回归；`ASSERT-OK/ASSERT-FAIL` 双判据（退出码 + 标记）；覆盖渲染/路由/语音/平台/门禁等几十个用例 | `scripts/regress.sh:1-802`（关键判据函数见第三节；完整源码已内嵌于 3.6 节，全文 802 行） |
| `CMakeLists.txt`（分层段） | 分层链接：模块库按 DAG 顺序定义，`navimap_core` 聚合；已移除 `-Wl,--start-group`，链接器单趟解析即第二道防线 | `CMakeLists.txt:142-365` |
| `tools/core_no_sdl.c` | 架构边界门禁：核心与 Qt 前端零 SDL 符号；对照体必须含 SDL（负向对照，证明检查器非空壳） | `tools/core_no_sdl.c:1-230`（完整体见第三节） |
| `tools/blend_equiv.c` | alpha 混色与 SDL 软渲染器逐像素等价对照（去 SDL 化合法链 SDL2） | `tools/blend_equiv.c:1-160`（完整体见第三节） |
| `tools/clip_equiv.c` | 线段裁剪与 `SDL_IntersectRectAndLine` 等价对照（20 万随机线段） | `tools/clip_equiv.c:1-77`（完整体见第三节） |

分层方向表（来自 `check_layers.sh:65-73`，即第三节脚本中的 `NM_ALLOWED_*` 环境变量）：

```
utils    叶子，零内部依赖
pbf      叶子，零内部依赖
map      → pbf, utils
route    → map, utils
search   → utils
settings → utils
render   → map, route, search, settings, utils   （纯光栅层，零 ui 依赖）
ui       → map, render, route, search, settings, utils
ui_qt    → map, render, settings, ui, utils
```

刻意**不**允许的（就是本门禁要守的）：`map→render/route/ui/ui_qt`、`render→ui/ui_qt`、`route→render/ui/ui_qt`、`search→map/render/route/ui/ui_qt`。

## 三、完整实现代码

### 3.1 分层方向门禁：`scripts/check_layers.sh`（完整体）

解析真实 include（剥注释，`check_layers.sh:92-95`），逐文件逐行算出模块级依赖边，与允许方向表比对；违规逐条报出文件行号并 `exit 1`。结论行 `say_result` 始终打印，供回归门禁的“标记 + 退出码”双判据使用。

```sh
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
```

### 3.2 分层链接：`CMakeLists.txt`（关键段，完整体）

`CMakeLists.txt:142-365` 是分层链接的核心。要点：模块库按 DAG 顺序定义；`render` 因 `chrome.c/audit.c` 已移到 `ui/`，退化为纯光栅层（零 ui 依赖），符号级环消除；因此**不再需要 `-Wl,--start-group`**，链接器单趟即可按拓扑序解析全部符号（实测去掉后零 `undefined reference`，出图逐字节一致）。保留 `start-group` 反而会让链接器多轮扫描**悄悄补上**新引入的环——边界形同虚设。

```cmake
# 阶段 2：把核心剩余模块提升为独立 STATIC 库（ARCH_ASSESSMENT.md §4 DAG）。
#   map → pbf, utils        route → map, utils
#   search → utils          render → map, route, search, settings, utils
#   ui → map, render, route, search, settings, utils
# ★ 2026-10-28 R2：原先 render/chrome.c + render/audit.c 住在 render 库里，因要读
#   App 全量定义而反向引用 ui/window.h ⇒ render↔ui 是真实的符号级环。现在这两个
#   文件已移到 ui/（git mv，历史保留）：render 退化为纯光栅层（零 ui 依赖），
#   ui → render 仍是既有正确方向 ⇒ 符号级环消除（见 PLAN.md §53）。
#   ⇒ 下面所有链接都**不再需要** -Wl,--start-group：模块依赖已是纯 DAG，
#     链接器单趟就能按拓扑序解析全部符号（实测：去掉 start-group 后全量
#     链接零 undefined reference，且出图逐字节一致）。
#     保留 start-group 反而会**掩盖**将来新引入的环——链接器多轮扫描会
#     悄悄补上，边界形同虚设。故已移除；R3 的分层门禁负责防回流。
set(MAP_SRCS
    src/map/road_tags.c
    src/map/binfmt.c
    src/map/graph_build.c
    src/map/pbfmap.c
    src/map/mapset.c
    src/map/namepool.c
    src/map/autobuild.c
    src/map/coverage.c
    src/map/map_view.c
    src/map/tileidx.c
    src/map/gpx.c)
set(ROUTE_SRCS
    src/route/st.c
    src/route/sew.c
    src/route/restr.c
    src/route/cost.c
    src/route/lmdist.c
    src/route/result.c
    src/route/route.c
    src/route/lmtable.c
    src/route/revidx.c
    src/route/maneuver.c
    src/route/navsim.c
    src/route/voice.c)
set(SEARCH_SRCS
    src/search/search.c)
set(RENDER_SRCS
    src/render/paint.c
    src/render/raster.c
    src/render/navdraw.c
    src/render/font.c
    src/render/labels.c
    src/render/trackdraw.c)
set(UI_SRCS
    src/ui/window.c
    src/ui/overlay.c
    src/ui/voice_wire.c
    src/ui/offscreen_shot.c
    src/ui/chrome.c
    src/ui/audit.c)

# 阶段 2 模块库（DAG 顺序，见上方 MODULE_SRCS 注释；聚合层 navimap_core 见下方）。
# 每个模块库 PUBLIC 暴露 ${CMAKE_SOURCE_DIR}/src 含入根（消费者可用 "map/xxx.h"），
# 并链入自身的模块依赖。第三方库（z/sqlite3/protoc/m）统一由下方 navimap_core 聚合层
# 承载，模块库本身只表达"模块↔模块"依赖，便于独立复用。

# navmap_map：pbf + 全部叶子库
# gpx.c 真正调用 sqlite3_*（tracks.db 用户数据）⇒ 与 navmap_search 同样把
# sqlite3 收进本模块库，保证 gpx 系列工具只链 navmap_map 也能解析数据库符号。
add_library(navmap_map STATIC ${MAP_SRCS})
target_include_directories(navmap_map PUBLIC
    ${CMAKE_SOURCE_DIR}/src ${GEN_DIR}
    ${ZLIB_INCLUDE_DIRS} ${SQLITE3_INCLUDE_DIRS}
    ${PROTOC_INCLUDE_DIRS} ${PROTOC_REAL_INCLUDE_DIR})
target_link_libraries(navmap_map PUBLIC
    navmap_pbf navmap_geo navmap_strpool navmap_plat navmap_mem
    ${SQLITE3_LIBRARIES})

# navmap_search：仅叶子库（search→utils）。search.c 真正调用 sqlite3_*，
# 故把 sqlite3 收进本模块库，保证 search 系列工具（skedaddle_search_cli/skedaddle_places_build）
# 只链 navmap_search 也能解析数据库符号（模块可独立复用，不必拖整库）。
add_library(navmap_search STATIC ${SEARCH_SRCS})
target_include_directories(navmap_search PUBLIC
    ${CMAKE_SOURCE_DIR}/src ${GEN_DIR}
    ${ZLIB_INCLUDE_DIRS} ${SQLITE3_INCLUDE_DIRS}
    ${PROTOC_INCLUDE_DIRS} ${PROTOC_REAL_INCLUDE_DIR})
target_link_libraries(navmap_search PUBLIC
    navmap_geo navmap_strpool navmap_plat navmap_mem
    ${SQLITE3_LIBRARIES})

# navmap_route：map + 叶子库
add_library(navmap_route STATIC ${ROUTE_SRCS})
target_include_directories(navmap_route PUBLIC
    ${CMAKE_SOURCE_DIR}/src ${GEN_DIR}
    ${ZLIB_INCLUDE_DIRS} ${SQLITE3_INCLUDE_DIRS}
    ${PROTOC_INCLUDE_DIRS} ${PROTOC_REAL_INCLUDE_DIR})
target_link_libraries(navmap_route PUBLIC
    navmap_map navmap_geo navmap_strpool navmap_plat navmap_mem)

# navmap_render：map + route + search + settings + 叶子库
#   ★ 2026-10-28 R2 起 render 零 ui 依赖（chrome/audit 已移到 ui），
#     符号级环消除 ⇒ 无需再靠 start-group 消解，也不再隐式依赖 ui。
add_library(navmap_render STATIC ${RENDER_SRCS})
target_include_directories(navmap_render PUBLIC
    ${CMAKE_SOURCE_DIR}/src ${GEN_DIR}
    ${ZLIB_INCLUDE_DIRS} ${SQLITE3_INCLUDE_DIRS}
    ${PROTOC_INCLUDE_DIRS} ${PROTOC_REAL_INCLUDE_DIR})
target_link_libraries(navmap_render PUBLIC
    navmap_map navmap_route navmap_search navmap_settings
    navmap_geo navmap_strpool navmap_plat navmap_mem)

# navmap_ui：render + route + search + map + 叶子库
add_library(navmap_ui STATIC ${UI_SRCS})
target_include_directories(navmap_ui PUBLIC
    ${CMAKE_SOURCE_DIR}/src ${GEN_DIR}
    ${ZLIB_INCLUDE_DIRS} ${SQLITE3_INCLUDE_DIRS}
    ${PROTOC_INCLUDE_DIRS} ${PROTOC_REAL_INCLUDE_DIR})
target_link_libraries(navmap_ui PUBLIC
    navmap_render navmap_route navmap_search navmap_map navmap_settings
    navmap_geo navmap_strpool navmap_plat navmap_mem)

# navmap_settings：完整设置系统（极简 INI 存读 + 设置清单），依赖仅 plat。
add_library(navmap_settings STATIC src/settings/settings.c)
target_include_directories(navmap_settings PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(navmap_settings PUBLIC navmap_plat)

# navimap_core：聚合层（方案2 架构边界的对外门面）。
#   ★ 仅含一个无副作用的锚点符号（src/core_anchor.c），不含业务 .c；
#     把 5 个模块库 + 5 个叶子库 + 第三方库聚合成一个链接面，
#     使 24 个 exe / skedaddle 等无需改动即可继续链接 navimap_core。
#   ★ 核心零 SDL：聚合层连 SDL 的 include 都不给；SDL 仅 skedaddle_blend_equiv / skedaddle_clip_equiv 合法链。
add_library(navimap_core STATIC src/core_anchor.c)

# ★ 核心不含 SDL：连 include 路径都不给。理由见上面"依赖"一节的架构约束。
#   谁需要 SDL（去 SDL 化门禁对照体）自己去直接链 SDL2。
target_include_directories(navimap_core PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${GEN_DIR}
    ${ZLIB_INCLUDE_DIRS}
    ${SQLITE3_INCLUDE_DIRS}
    ${PROTOC_INCLUDE_DIRS}
    ${PROTOC_REAL_INCLUDE_DIR})

# 平台相关链接：libm 只有类 Unix 有；Windows 的 GetProcessMemoryInfo 在 psapi.dll。
# 注意：必须并入同一次 target_link_libraries 调用。本机 CMake 3.22.1 对"同一目标的
# 第二次 target_link_libraries（且包在 if() 内）"会误报 target not built by this project，
# 故用配置期变量在这里一次性决定平台库，再在下方单次调用里展开。
if(UNIX)
    set(NAVIMAP_CORE_SYS_LIBS m)
elseif(WIN32)
    set(NAVIMAP_CORE_SYS_LIBS psapi)
else()
    set(NAVIMAP_CORE_SYS_LIBS "")
endif()

# ★ 2026-10-28 R2：模块依赖已是纯 DAG（render 零 ui 依赖，符号级环消除），
#   链接器单趟即可按拓扑序解析全部符号 ⇒ 不再需要 -Wl,--start-group。
#   ★ 刻意保留"单趟链接"这项约束：start-group 会让链接器多轮扫描，把将来
#     新引入的符号级环**悄悄补上** ⇒ 边界形同虚设。R3 的分层门禁负责防回流。
target_link_libraries(navimap_core PUBLIC
    navmap_map navmap_route navmap_search navmap_render navmap_ui navmap_settings
    navmap_pbf navmap_geo navmap_strpool navmap_plat navmap_mem
    ${ZLIB_LIBRARIES} ${SQLITE3_LIBRARIES} ${PROTOC_LIB} ${NAVIMAP_CORE_SYS_LIBS})
```

`skedaddle` 只链 `navimap_core + Qt Widgets`，离屏出图走 `src/ui/offscreen_shot.c`（零 SDL），见 `CMakeLists.txt:496-512` 与 `CMakeLists.txt:529-531`（门禁工具 `skedaddle_core_no_sdl` 只链 `nm_link_tool`，不碰 SDL）。

### 3.3 架构边界门禁：`tools/core_no_sdl.c`（完整体）

证明 C 核心零 SDL 依赖。关键设计：用 `nm -u`（静态库 / ELF 有效）与 `objdump -p`（Windows PE 导入表）**两个提取器取并集**（`skedaddle_core_no_sdl.c:138-155`），且用**退出码**判定提取成功、绝不靠行数（避免 stderr 污染造成的假绿，`skedaddle_core_no_sdl.c:192-205`）。`--expect-sdl` 是负向对照：对照体（如 `skedaddle_blend_equiv`）必须含 SDL 符号，否则证明检查器是空壳。

```c
/* skedaddle_core_no_sdl —— 断言某个目标里**没有** SDL 符号（方案2 的架构门禁）。
 *
 * 为什么要有它：
 *   Qt 前端（skedaddle）必须**一行 SDL 都不链**。但"Qt 不链 SDL"很容易在
 *   某次改动里悄悄失效：core 里多加一个 #include <SDL.h> 或一个 SDL 调用，
 *   编译照样过、Qt 照样能跑（只是间接多依赖了 SDL2.dll），直到某天有人在没装
 *   SDL 的机器上构建 Qt 前端才炸。静态链接下更隐蔽：core.a 里引了 SDL 符号，
 *   最终由 navimap_qt 链接进去，依赖就落到 Qt 前端头上了。
 *
 * 判据（查符号表，不是"看起来没有"）：
 *   正向：目标里不得出现任何 SDL_ 未定义符号；
 *   反向：SDL 适配层（navmap.exe）**必须**含 SDL 符号 —— 证明这个检查器
 *         不是"查不出东西的空壳"。
 *
 * ★ 为什么必须**两个提取器取并集**（踩过的坑）：
 *   - `nm -u <PE exe>`：Windows 的 PE 可执行文件里，导入的 DLL 符号**不进**
 *     未定义符号表（实测 navimap.exe 只有 3 行，全是 CRT 的 __tls_*）。
 *     真正的 SDL_CreateWindow 之类在 **objdump -p 的导入表**里。
 *   - `objdump -p <PE exe>`：只对 PE 有用；Linux 的 ELF 走这条只能拿到
 *     .dynamic 段，静态库的导入符号看不到。
 *   - `nm -u <静态库 .a>`：有效（实测 465 行未定义符号）。
 *   所以：nm 和 objdump 两个都跑，取并集。只跑一个会在另一个平台上
 *   静默漏检 —— 这正是"永远绿的门禁"最常见的成因。
 *
 * 用法：
 *   skedaddle_core_no_sdl <文件...>              期望：这些目标里没有 SDL 符号
 *   skedaddle_core_no_sdl --expect-sdl <文件...> 期望：这些目标里**必须有** SDL 符号
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "utils/plat.h"

#if defined(_WIN32)
#  define NM_EXE      "nm.exe"
#  define OBJDUMP_EXE "objdump.exe"
#else
#  define NM_EXE      "nm"
#  define OBJDUMP_EXE "objdump"
#endif

/* stderr 的落点：必须与 stdout 分开，否则错误行会被当成符号行（假绿）。
 * 放在系统临时目录并带进程号，避免污染源码树、也避免并发冲突。 */
static char g_stderr_path[1200];

static const char *stderr_path(void)
{
    if (!g_stderr_path[0]) {
        const char *dir = nm_temp_dir();
        snprintf(g_stderr_path, sizeof(g_stderr_path), "%s%snm_no_sdl_%ld.err",
                 dir ? dir : ".", (dir && dir[0] && dir[strlen(dir) - 1] == '/') ? "" : "/",
                 (long)nm_get_pid());
    }
    return g_stderr_path;
}

/* 定位工具链里的 nm / objdump。
 *
 * ★ 为什么必须自己找（踩过的坑）：Windows 上 popen 走的是**原生** CreateProcess，
 *   它继承的是当前进程的 PATH；而 MSYS2 的 PATH（/d/msys64/ucrt64/bin）在
 *   原生进程里未必被翻译过去 —— 实测 popen("nm.exe ...") 报"系统找不到指定的
 *   文件"，可同一个 shell 里 nm.exe 明明能跑。用 plat 的 nm_find_in_path
 *   （它自己按 Windows 的 ';' 规则切 PATH、并且会补 .exe）就稳了。
 *
 * 找不到就返回 NULL ⇒ 调用方报"无法判定"，绝不静默当成"通过"。 */
static const char *find_tool(const char *name)
{
    static char buf[2][1024];
    static int  slot = 0;
    char path[1024];
    if (nm_find_in_path(name, path, sizeof(path)) == 0) {
        char *b = buf[slot];
        slot = (slot + 1) % 2;
        snprintf(b, sizeof(buf[0]), "%s", path);
        return b;
    }
    return NULL;
}

typedef struct {
    int  total;      /* 扫到的**符号**行数（只算 stdout，不含错误输出） */
    int  sdl;        /* 含 "SDL" 的行数 */
    int  ok;         /* 至少一个提取器成功执行（退出码 0） */
    char first[256]; /* 首个 SDL 命中行 */
} Scan;

/* 跑一条命令，把**stdout** 逐行喂给回调。
 *
 * ★ cmd 兼容 + 不被 stderr 污染（踩过的坑，坑得很隐蔽）：
 *   MinGW 的 popen 走 **cmd.exe**，不是 POSIX shell。两条硬规则：
 *     ① 带引号的可执行文件路径在 cmd 里必须用 `call` 前缀，否则 cmd 把引号
 *        当成普通字符 ⇒ 报"…不是内部或外部命令"；
 *     ② `2>/dev/null` 在 cmd 里是**无效重定向**（/dev/null 不是 Windows 路径），
 *        会让整条命令失败。
 *   ★★ 但 `2>&1` 同样有害：stderr 的错误行会混进 stdout，被当成"符号行"计数。
 *      后果是**假绿**——命令根本没跑起来（"文件不存在"之类），错误行也凑出了
 *      total>0，于是判成"提取成功且无 SDL"。实测就踩到了：把 SDL 版
 *      navimap.exe 传给"期望无 SDL"的判据，它报了 OK。
 *      正确做法：**stderr 单独重定向到临时文件**，stdout 走管道；
 *      再用**退出码**判定这次提取到底成功没有。
 *
 *   Linux 上 /bin/sh 没有 ① ② 两条限制，但同样要把 stderr 分离（语义一致）。 */
static int run_scan(const char *cmd, Scan *sc)
{
    FILE *fp = popen(cmd, "r");
    if (!fp) return 0;
    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        sc->total++;
        if (strstr(line, "SDL")) {
            if (sc->sdl == 0) snprintf(sc->first, sizeof(sc->first), "%s", line);
            sc->sdl++;
        }
    }
    int rc = pclose(fp);
    /* pclose 返回的是命令的退出状态（Unix 语义）。0 = 成功。 */
    if (rc == 0) sc->ok = 1;
    return 1;
}

/* 拼一条"扫某个文件的符号表"的命令。
 * stderr 单独重定向到临时文件（见 run_scan 的说明：混进 stdout 会造成假绿）。 */
static void make_cmd(char *out, size_t cap, const char *tool,
                     const char *args, const char *file)
{
#if defined(_WIN32)
    /* cmd.exe：2> 到 nul 是合法的（Windows 空设备），但错误信息就看不见了；
     * 落到临时文件里，好在失败时把原因报出来。 */
    snprintf(out, cap, "call \"%s\" %s \"%s\" 2>\"%s\"",
             tool, args, file, stderr_path());
#else
    snprintf(out, cap, "\"%s\" %s \"%s\" 2>\"%s\"",
             tool, args, file, stderr_path());
#endif
}

static void scan_file(const char *file, Scan *sc)
{
    char cmd[2600];

    /* ① 未定义符号表：静态库与 Linux ELF 有效 */
    const char *nm = find_tool(NM_EXE);
    if (nm) {
        make_cmd(cmd, sizeof(cmd), nm, "-u", file);
        run_scan(cmd, sc);
    }

    /* ② 导入表：Windows PE 的 DLL 导入符号只在这里 */
    const char *od = find_tool(OBJDUMP_EXE);
    if (od) {
        make_cmd(cmd, sizeof(cmd), od, "-p", file);
        run_scan(cmd, sc);
    }
}

/* 读出 stderr 临时文件的第一行（非空才读），用于失败时说明原因。 */
static void read_stderr(char *out, size_t cap)
{
    if (!cap) return;
    out[0] = '\0';
    FILE *fp = fopen(stderr_path(), "r");
    if (!fp) return;
    while (fgets(out, (int)cap, fp)) {
        size_t n = strlen(out);
        while (n && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = '\0';
        if (n) break;
    }
    fclose(fp);
}

int main(int argc, char **argv)
{
    int expect_sdl = 0, first_arg = 1;
    if (argc > 1 && strcmp(argv[1], "--expect-sdl") == 0) {
        expect_sdl = 1;
        first_arg = 2;
    }
    if (argc <= first_arg) {
        fprintf(stderr,
                "用法: skedaddle_core_no_sdl [--expect-sdl] <文件...>\n"
                "  默认断言这些目标里没有 SDL 符号；--expect-sdl 断言必须有。\n");
        return 2;
    }

    int bad = 0;
    for (int i = first_arg; i < argc; i++) {
        Scan sc;
        memset(&sc, 0, sizeof(sc));
        scan_file(argv[i], &sc);

        /* ★ 判定用"提取器是否成功"（退出码），不是"扫到几行"。
         *   用行数判定会假绿：命令失败时 stderr 混进 stdout 也凑得出行数。
         *   实测踩到过——把 SDL 版 navimap.exe 传给"期望无 SDL"的判据，
         *   因为 nm 在 PE 上不列 DLL 导入符号（只有 3 行 CRT 符号）、
         *   objdump 的输出又被 stderr 噪声污染，结果报了 OK。 */
        if (!sc.ok) {
            char err[512];
            read_stderr(err, sizeof(err));
            fprintf(stderr, "CORE-NO-SDL-FAIL  %s: 提取器都没成功执行"
                            "（文件不存在？或 nm/objdump 不可用？）%s%s\n",
                    argv[i], err[0] ? "  原因: " : "", err);
            bad = 1;
            continue;
        }
        if (expect_sdl) {
            if (sc.sdl > 0) {
                printf("OK  %s  有 %d 个 SDL 符号（首个: %s）\n",
                       argv[i], sc.sdl, sc.first);
            } else {
                fprintf(stderr, "CORE-NO-SDL-FAIL  %s: 期望有 SDL 符号，实际 0 个"
                                "（检查器失效？或该目标已彻底去 SDL？）\n", argv[i]);
                bad = 1;
            }
        } else {
            if (sc.sdl == 0) {
                printf("OK  %s  扫过 %d 行符号，无 SDL\n", argv[i], sc.total);
            } else {
                fprintf(stderr, "CORE-NO-SDL-FAIL  %s: %d/%d 行含 SDL（首个: %s）\n",
                        argv[i], sc.sdl, sc.total, sc.first);
                bad = 1;
            }
        }
    }
    remove(stderr_path());          /* 清掉 stderr 临时文件 */
    if (bad) return 1;
    printf("CORE-NO-SDL-OK\n");
    return 0;
}
```

### 3.4 去 SDL 化门禁对照体：`tools/blend_equiv.c`（完整体）

自写光栅的 alpha 混色与 SDL 软渲染器逐像素等价。覆盖四种图元（填充 / 描边矩形 / 线段 / 单点），差异必须为 0。这是 SDL 在“合法链”上唯一允许出现的地方之一。

```c
/* skedaddle_blend_equiv — 验证自写光栅的 alpha 混色与 SDL 软渲染器逐像素等价。
 *
 * 为什么要它：raster.h 写着"与 SDL 软渲染器逐像素等价是硬要求"，但之前只
 * 验证了线段裁剪（4000 随机线段 + 1500 随机折线 0 差异），**没验证 alpha
 * 混色**。去 SDL 化之后 chrome 的半透明面板/圆角按钮全走 raster_px_rgba，
 * 若混色公式与 SDL 差 1 级，画面上就是"同一块面板颜色深浅略有不同"——
 * 肉眼看不出，但逐像素比对会报差异，必须查清是"公式不等价"还是"要素丢了"。
 *
 * 做法：同一组 (前景色, 背景色, alpha) 两条路各画一遍，逐像素比。
 *   SDL 路：SDL_CreateSoftwareRenderer + SDL_SetRenderDrawColor + FillRect
 *   raster 路：raster_px_rgba
 * 判据：差异必须是 0；若不为 0，报出最大的差值与出现次数（说明公式不等价）。
 *
 * ★ 覆盖四种图元（填充 / 描边矩形 / 线段 / 点），因为 chrome.c 里的
 *   rfill / rstroke / rline / raster_point 各走一条不同的 SDL 调用，
 *   语义差别各不相同（尤其 SDL_RenderDrawPoint 会做 alpha 混色，
 *   而 raster_point 是直接覆盖 —— 两者不等价，见下面的对照）。
 */
#include <SDL.h>
#include "render/raster.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 四种图元各测一遍。返回 0 = 该图元 0 差异。 */
static int test_shape(int shape)
{
    const int W = 64, H = 64;

    typedef struct { int r, g, b, a; } FgCol;
    typedef struct { int r, g, b; }     BgCol;
    static const FgCol FG[] = {
        {255,255,255,255}, {255,255,255,128}, {255,255,255,64},
        {  0,  0,  0,255}, {  0,  0,  0,128}, { 30,110,235,255},
        {255,186, 64,200}, {244,241,234, 90}, { 12, 18, 24,255},
    };
    static const BgCol BG[] = {
        {244,241,234}, {255,255,255}, { 40, 44, 52}, {200,210,220},
    };

    int total_cmp = 0, total_diff = 0, max_delta = 0;

    for (size_t fi = 0; fi < sizeof(FG)/sizeof(FG[0]); fi++) {
        for (size_t bi = 0; bi < sizeof(BG)/sizeof(BG[0]); bi++) {
            const FgCol fg = FG[fi];
            const BgCol bg = BG[bi];

            SDL_Surface *s = SDL_CreateRGBSurface(0, W, H, 32, 0, 0, 0, 0);
            if (!s) { fprintf(stderr, "surface 失败\n"); return 2; }
            SDL_Renderer *r = SDL_CreateSoftwareRenderer(s);
            if (!r) { fprintf(stderr, "renderer 失败\n"); return 2; }
            SDL_SetRenderDrawColor(r, (Uint8)bg.r, (Uint8)bg.g, (Uint8)bg.b, 255);
            SDL_RenderClear(r);

            uint32_t *px = (uint32_t *)malloc((size_t)W * H * sizeof(uint32_t));
            if (!px) { fprintf(stderr, "malloc 失败\n"); return 2; }
            Raster rs;
            raster_bind_mem(&rs, px, W, H, (size_t)W,
                            s->format->Rmask, s->format->Gmask,
                            s->format->Bmask, s->format->Amask);
            raster_fill_rect(&rs, 0, 0, W, H, (uint8_t)bg.r, (uint8_t)bg.g,
                             (uint8_t)bg.b, 255);

            /* --- 画图元：SDL 路与 raster 路各画一遍 --- */
            switch (shape) {
            case 0:   /* 填充矩形 */
                SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(r, (Uint8)fg.r, (Uint8)fg.g, (Uint8)fg.b, (Uint8)fg.a);
                { SDL_Rect q = { 8, 8, 40, 30 }; SDL_RenderFillRect(r, &q); }
                raster_fill_rect(&rs, 8, 8, 40, 30, (uint8_t)fg.r, (uint8_t)fg.g,
                                 (uint8_t)fg.b, (uint8_t)fg.a);
                break;
            case 1:   /* 描边矩形（1px 框） */
                SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(r, (Uint8)fg.r, (Uint8)fg.g, (Uint8)fg.b, (Uint8)fg.a);
                { SDL_Rect q = { 8, 8, 40, 30 }; SDL_RenderDrawRect(r, &q); }
                raster_rect(&rs, 8, 8, 40, 30, (uint8_t)fg.r, (uint8_t)fg.g,
                            (uint8_t)fg.b, (uint8_t)fg.a);
                break;
            case 2:   /* 线段（斜线，最能暴露光栅化差异） */
                SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(r, (Uint8)fg.r, (Uint8)fg.g, (Uint8)fg.b, (Uint8)fg.a);
                SDL_RenderDrawLine(r, 3, 5, 55, 47);
                raster_line_rgba(&rs, 3, 5, 55, 47, (uint8_t)fg.r, (uint8_t)fg.g,
                                 (uint8_t)fg.b, (uint8_t)fg.a);
                break;
            case 3:   /* 单点：chrome.c 的画点处 blend 模式是 NONE、alpha 恒 255
                       （rfill/rstroke 都会把 blend 模式复位成 NONE），
                       所以这里按**应用实际用法**测：直接覆盖，不混色。 */
                SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
                SDL_SetRenderDrawColor(r, (Uint8)fg.r, (Uint8)fg.g, (Uint8)fg.b, 255);
                SDL_RenderDrawPoint(r, 32, 32);
                raster_point(&rs, 32, 32, raster_rgb(&rs, (uint8_t)fg.r, (uint8_t)fg.g,
                                                     (uint8_t)fg.b));
                break;
            }
            SDL_RenderPresent(r);

            const uint32_t *sp = (const uint32_t *)s->pixels;
            int shown = 0;
            for (int i = 0; i < W * H; i++) {
                total_cmp++;
                uint32_t a = sp[i] & 0x00FFFFFFu;
                uint32_t b = px[i] & 0x00FFFFFFu;
                if (a != b) {
                    total_diff++;
                    int dr = (int)((a >> 16) & 0xFF) - (int)((b >> 16) & 0xFF);
                    int dg = (int)((a >>  8) & 0xFF) - (int)((b >>  8) & 0xFF);
                    int db = (int)(a & 0xFF) - (int)(b & 0xFF);
                    int d = dr < 0 ? -dr : dr;
                    int t = dg < 0 ? -dg : dg; if (t > d) d = t;
                    t = db < 0 ? -db : db;       if (t > d) d = t;
                    if (d > max_delta) max_delta = d;
                    if (shown < 6) {
                        printf("      x=%2d y=%2d SDL(%3d,%3d,%3d) raster(%3d,%3d,%3d)\n",
                               i % W, i / W,
                               (int)((a >> 16) & 0xFF), (int)((a >> 8) & 0xFF), (int)(a & 0xFF),
                               (int)((b >> 16) & 0xFF), (int)((b >> 8) & 0xFF), (int)(b & 0xFF));
                        shown++;
                    }
                }
            }
            free(px);
            SDL_DestroyRenderer(r);
            SDL_FreeSurface(s);
        }
    }
    printf("  %-14s compare %6d  diff %6d  max_delta %d\n",
           shape == 0 ? "填充矩形" : shape == 1 ? "描边矩形" :
           shape == 2 ? "线段" : "单点", total_cmp, total_diff, max_delta);
    return total_diff;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 2;
    }

    static const char *SHAPE[] = { "填充矩形", "描边矩形", "线段", "单点" };
    int bad = 0;
    for (int sh = 0; sh < 4; sh++) {
        if (test_shape(sh) != 0) {
            printf("  >>> %s 与 SDL 不等价\n", SHAPE[sh]);
            bad++;
        }
    }
    if (bad) {
        printf("BLEND-EQUIV-FAIL：%d 种图元与 SDL 不等价\n", bad);
        SDL_Quit();
        return 1;
    }
    printf("BLEND-EQUIV-OK（填充/描边/线段/单点 均与 SDL 软渲染器逐像素一致）\n");
    SDL_Quit();
    return 0;
}
```

### 3.5 去 SDL 化门禁对照体：`tools/clip_equiv.c`（完整体）

自写 `raster_clip_line`（Cohen-Sutherland）与 `SDL_IntersectRectAndLine` 逐像素等价。20 万随机线段，端点必须逐个完全相同（差异集中在画布最外几圈）。

```c
/* skedaddle_clip_equiv — 验证自写光栅的线段裁剪与 SDL_IntersectRectAndLine 逐像素等价。
 *
 * 为什么要它：raster.h 写着"与 SDL 软渲染器逐像素等价是硬要求"。去 SDL 化
 * 时把 SDL_IntersectRectAndLine 换成了自写的 raster_clip_line（ Cohen-
 * Sutherland ），**但两者不是同一个算法**：SDL 那份是参数化裁剪，交点位置
 * 的取整方式不同 ⇒ 线段跨过画布边界时被裁到**不同像素**。
 *
 * 症状：整幅图只差 0.07%，且差异**全部集中在画布最外几圈**（x=0..2 /
 * x=w-3..w-1 / y=0..5 / y=h-6..h-1），地图内部 0 差异 —— 正是"只在裁剪
 * 发生时才有差别"的特征。
 *
 * 做法：随机线段（大量跨越边界），两条裁剪各跑一遍，比裁剪后的端点。
 * 判据：端点必须逐个完全相同。
 */
#include <SDL.h>
#include "render/raster.h"

#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 2;
    }

    const int W = 800, H = 600;
    Raster rs;
    raster_bind_mem(&rs, NULL, 1, 1, 1, 0, 0, 0, 0);  /* 只借 clip 字段 */
    SDL_Rect clip = { 0, 0, W, H };

    unsigned seed = 12345;
    #define RND(n) (seed = seed * 1103515245u + 12345u, (int)((seed >> 16) % (unsigned)(n)))

    int total = 0, mismatch = 0;
    int shown = 0;
    for (int i = 0; i < 200000; i++) {
        int x1 = RND(W + 200) - 100;
        int y1 = RND(H + 200) - 100;
        int x2 = RND(W + 200) - 100;
        int y2 = RND(H + 200) - 100;

        /* SDL 版 */
        int ax = x1, ay = y1, bx = x2, by = y2;
        int sdl_in = SDL_IntersectRectAndLine(&clip, &ax, &ay, &bx, &by);

        /* 自写版 */
        rs.clip.x = 0; rs.clip.y = 0; rs.clip.w = W; rs.clip.h = H;
        int cx = x1, cy = y1, dx = x2, dy = y2;
        int our_in = raster_clip_line(&rs, &cx, &cy, &dx, &dy);

        total++;
        if (sdl_in != our_in || ax != cx || ay != cy || bx != dx || by != dy) {
            mismatch++;
            if (shown < 8) {
                printf("  线段 (%d,%d)-(%d,%d): SDL(in=%d)(%d,%d)-(%d,%d)"
                       "  ours(in=%d)(%d,%d)-(%d,%d)\n",
                       x1, y1, x2, y2, sdl_in, ax, ay, bx, by,
                       our_in, cx, cy, dx, dy);
                shown++;
            }
        }
    }
    printf("compare        %d\n", total);
    printf("mismatch       %d  (%.4f%%)\n", mismatch,
           100.0 * mismatch / (double)(total ? total : 1));
    if (mismatch == 0) {
        printf("CLIP-EQUIV-OK（自写裁剪与 SDL_IntersectRectAndLine 完全一致）\n");
        SDL_Quit();
        return 0;
    }
    printf("CLIP-EQUIV-FAIL：裁剪结果与 SDL 不一致\n");
    SDL_Quit();
    return 1;
}
```

### 3.6 回归门禁：`scripts/regress.sh`（关键判据函数，完整体；完整源码内嵌于本节末尾，全文 802 行）

`regress.sh` 共 802 行，逐用例串起全量回归。其核心是**双判据**：命令必须出现期望标记且不得出现 `ASSERT-FAIL` 且退出码为 0。下列是承载判据逻辑的完整函数与关键调用点。注意 `layer_dirs`（`regress.sh:316`）与 `skedaddle_core_no_sdl`/`core_no_sdl_sdl`（`regress.sh:300-306`）正是把分层门禁与 SDL 边界门禁接进回归；`area_lod_tier` 用**确定性量** `areas_dotted` 而非时间。

```sh
# run <名字> <必须出现的标记> <命令...>
pass=0; fail=0
run() {
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

# shot <名字> <省目录|graph> <lat> <lon> <zoom>
shot() {
    local name=$1 g=$2 la=$3 lo=$4 z=$5
    run "$name" "ASSERT-OK" "$BUILD/skedaddle" --shot-offscreen --graph="$g" \
        --lat="$la" --lon="$lo" --zoom="$z" --w=1280 --h=800 \
        --out="$SHOTS/$name.bmp"
}

# 面要素 LOD 档位（AREA_DOT_PX）。主判据用确定性量 areas_dotted，不用时间。
area_lod_tier() {
    local name=$1 z=$2 lo=$3 hi=$4 want_drawn=$5
    local log="$SHOTS/$name.log"
    "$BUILD/skedaddle" --shot-offscreen --graph="$GRAPH" \
        --lat=39.90 --lon=116.40 --zoom="$z" --w=1280 --h=800 \
        --out="$SHOTS/$name.bmp" >"$log" 2>&1
    local rc=$?
    local dotted drawn
    dotted=$(awk '/^areas_dotted/{print $2}' "$log")
    drawn=$(awk '/^areas_drawn/{print $2}' "$log")
    if [ "$rc" != 0 ] || [ -z "$dotted" ] || [ -z "$drawn" ]; then
        fail=$((fail + 1)); printf '%-28s FAIL rc=%s 期望 areas_dotted/areas_drawn 输出\n' "$name" "$rc"
        tail -n 6 "$log" | sed 's/^/      | /'; return
    fi
    # 面一个都不能少：简化只改画法，不改要素数（实测恒为 113933 @ z9）
    if [ "$drawn" != "$want_drawn" ]; then
        fail=$((fail + 1)); printf '%-28s FAIL areas_drawn=%s（应为 %s：简化不该改变要素数）\n' \
            "$name" "$drawn" "$want_drawn"; return
    fi
    if [ "$dotted" -ge "$lo" ] && [ "$dotted" -le "$hi" ]; then
        pass=$((pass + 1)); printf '%-28s OK   (dotted=%s 在 [%s,%s])\n' "$name" "$dotted" "$lo" "$hi"
    else
        fail=$((fail + 1)); printf '%-28s FAIL dotted=%s 不在 [%s,%s]（AREA_DOT_PX 档位被改动？）\n' \
            "$name" "$dotted" "$lo" "$hi"
    fi
}

# run_neg <名字> <期望出现的失败标记> <命令...>：负向对照，命令必须失败且出现标记。
run_neg() {
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
```

回归脚本把分层门禁与 SDL 边界门禁接进来的关键调用（来自 `regress.sh:300-317`，完整体）：

```sh
# ③ 架构边界：核心与 Qt 前端零 SDL 符号；对照体**必须**链 SDL（负向对照，
#    证明检查器不是"永远绿"的空壳）。
if [ -x "$BUILD/skedaddle_core_no_sdl" ]; then
    exefile() { if [ -x "$1.exe" ]; then echo "$1.exe"; else echo "$1"; fi; }
    run skedaddle_core_no_sdl "CORE-NO-SDL-OK" "$BUILD/skedaddle_core_no_sdl" \
        "$(exefile "$BUILD/skedaddle")" "$BUILD/libnavimap_core.a"
    run core_no_sdl_sdl "CORE-NO-SDL-OK" "$BUILD/skedaddle_core_no_sdl" --expect-sdl \
        "$(exefile "$BUILD/skedaddle_blend_equiv")"
fi

# ④ 分层方向门禁（PLAN.md §53 R3）：模块依赖方向必须保持 DAG。
if [ -x "$ROOT/scripts/check_layers.sh" ]; then
    run layer_dirs "分层方向门禁 PASS" "$ROOT/scripts/check_layers.sh" --quiet
fi
```

> 完整源码 · `scripts/regress.sh` (全文 802 行)

```bash
> 完整源码 · `scripts/regress.sh`（逐字内嵌，全文 811 行）
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
    # #87：离屏判据从 map_shot 迁到 skedaddle --shot-offscreen（与 map_shot 同源、
    # 审计逐字节一致的单可信源）。--shot-offscreen 在 Qt 初始化前就走纯 C 离屏通道，
    # 不建窗口、不链 SDL、不依赖显示环境（QT_QPA_PLATFORM / SDL_VIDEODRIVER 都不用）。
    run "$name" "ASSERT-OK" "$BUILD/skedaddle" --shot-offscreen --graph="$g" \
        --lat="$la" --lon="$lo" --zoom="$z" --w=1280 --h=800 \
        --out="$SHOTS/$name.bmp"
}

# ---- 面要素 LOD 档位（AREA_DOT_PX）的主判据 ----
# 2026-10-29 决策：AREA_DOT_PX 定档 4（z9 −21%，净损失 3.5%）。
#
# ★ 主判据用**确定性量**「走小面路径的面数」(areas_dotted)，**不用时间**：
#   时间会被机器负载放大一个数量级（同一条路线手跑三次 502/503/522 ms，
#   而在 load average 10 + swap 满的机器上会跑出 3885 ms）⇒ 时间型判据必然 flaky，
#   而 flaky 判据比没有判据更糟（久了大家会习惯性忽略 FAIL）。
#   areas_dotted 是"多少个面被简化成单个像素"，只取决于数据与档位常量，
#   与机器负载完全无关。
#
# 判据形式（skill §61：已知缺陷写成**上界**，不写成"必须等于"）：
#   areas_dotted 落在 [LO, HI] 区间内 ⇒ 通过
#   ⇒ 将来档位调整（无论是产品决策还是 bug 修复）只要落在区间外就会报出来，
#     而区间本身留了余量，不会因为数据微调就假 FAIL。
area_lod_tier() {             # area_lod_tier <名字> <zoom> <dotted_lo> <dotted_hi> <drawn>
    local name=$1 z=$2 lo=$3 hi=$4 want_drawn=$5
    local log="$SHOTS/$name.log"
    "$BUILD/skedaddle" --shot-offscreen --graph="$GRAPH" \
        --lat=39.90 --lon=116.40 --zoom="$z" --w=1280 --h=800 \
        --out="$SHOTS/$name.bmp" >"$log" 2>&1
    local rc=$?
    local dotted drawn
    dotted=$(awk '/^areas_dotted/{print $2}' "$log")
    drawn=$(awk '/^areas_drawn/{print $2}' "$log")
    if [ "$rc" != 0 ] || [ -z "$dotted" ] || [ -z "$drawn" ]; then
        fail=$((fail + 1))
        printf '%-28s FAIL rc=%s 期望 areas_dotted/areas_drawn 输出\n' "$name" "$rc"
        tail -n 6 "$log" | sed 's/^/      | /'
        return
    fi
    # 面一个都不能少：简化只改画法，不改要素数（实测恒为 113933 @ z9）
    if [ "$drawn" != "$want_drawn" ]; then
        fail=$((fail + 1))
        printf '%-28s FAIL areas_drawn=%s（应为 %s：简化不该改变要素数）\n' \
            "$name" "$drawn" "$want_drawn"
        return
    fi
    if [ "$dotted" -ge "$lo" ] && [ "$dotted" -le "$hi" ]; then
        pass=$((pass + 1))
        printf '%-28s OK   (dotted=%s 在 [%s,%s])\n' "$name" "$dotted" "$lo" "$hi"
    else
        fail=$((fail + 1))
        printf '%-28s FAIL dotted=%s 不在 [%s,%s]（AREA_DOT_PX 档位被改动？）\n' \
            "$name" "$dotted" "$lo" "$hi"
    fi
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

echo "=== 面要素 LOD 档位（AREA_DOT_PX=4，2026-10-29 定档）"
# 主判据是确定性量 areas_dotted（走小面路径的面数），不是时间。
# 实测基线（AREA_DOT_PX=4，8 省图 / 北京中心 / 1280×800）：
#   z9  dotted=104379（档位 2 时是 76007）  drawn=113933
#   z11 dotted=20938                         drawn=36568
#   z13 dotted=22471                         drawn=37661
# 区间留了余量（z9 用 ±8%），数据微调不会假 FAIL；而档位被改动就会报出来。
# 区间要能区分相邻档位（负向对照实测：档位 2 ⇒ dotted=76007、
# 档位 4 ⇒ 104379、档位 8 ⇒ 111916）—— 上限卡在 108000，档位 8 就被抓到。
area_lod_tier area_lod_z9  9  98000 108000 113933
area_lod_tier area_lod_z11 11 19500  22500  36568
area_lod_tier area_lod_z13 13 21000  24000  37661

echo "=== 路网 / 导航 / 限制 / 索引"
run route_cross "ROUTE-OK"  "$BUILD/skedaddle_route_cli"  --graph="$GRAPH" --from=39.90,116.40 --to=34.75,113.62
run navsim_bj   "NAVSIM-OK" "$BUILD/skedaddle_navsim_cli" --graph="$GRAPH" --from=39.90,116.40 --to=39.95,116.45
run navsim_tj   "NAVSIM-OK" "$BUILD/skedaddle_navsim_cli" --graph="$GRAPH" --from=39.13,117.20 --to=39.18,117.26
run navsim_xa   "NAVSIM-OK" "$BUILD/skedaddle_navsim_cli" --graph="$GRAPH" --from=34.34,108.94 --to=34.38,109.02
# 转向限制的定向 A/B 对照。必须用全图集（$GRAPH）：
#   1. only_*（381 条）在 2026-09-23 之前根本没接进路由，光"路径自检通过"
#      证明不了它在拦东西——判据要双向：关限制时能走出被禁转向，开限制时必须走不出来。
#   2. 副本省（beijing/tianjin ⊂ hebei）的限制在运行时是不展开的死数据，
#      skedaddle_restr_ab 会跳过它们并打印条数；已核对过 hebei 的限制集合在坐标层面完整
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

run skedaddle_restr_ab    "RESTR-AB-OK" "$BUILD/skedaddle_restr_ab" --graph="$GRAPH" --probe=40 --only=60
# ↑ 的负向对照：只把"会合点处的限制检查"关掉，其余限制全开。
#   这一条**必须失败**——它是整套限制判据有效性的前提。实测：
#   关掉后 via=way 20 条里 19 条、only_* 30 组里 30 组会重新走出被禁转向。
run_neg restr_ab_noseam "RESTR-AB-FAIL" "$BUILD/skedaddle_restr_ab" --graph="$GRAPH" \
        --probe=20 --only=30 --no-seam-restr
# ---- 反向索引落盘 rev.bin：三条判据（PLAN.md §35）----
# 1) 逐元素一致：落盘结果 == 内存构建。写盘 / 读盘 / 内存构建三条路共用
#    同一份 rev_build_mem()，所以这条断言能同时抓住写坏和读坏
#    （各写一份构建代码的话，比的是两份 bug，谁错都一样）。
run rev_verify "REV-VERIFY-OK" "$BUILD/skedaddle_revbuild_cli" --graph="$GRAPH" --verify

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
run rev_cached "ROUTE-OK" "$BUILD/skedaddle_route_cli" --graph="$REVT/ok"   --from=$LN_FROM --to=$LN_TO
run rev_norev  "ROUTE-OK" "$BUILD/skedaddle_route_cli" --graph="$REVT/none" --from=$LN_FROM --to=$LN_TO
run rev_bad_strict "建反向索引" env NAVIMAP_REV_STRICT=1 "$BUILD/skedaddle_route_cli" \
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
run skedaddle_plat_selftest     "ASSERT-OK" "$BUILD/skedaddle_plat_selftest"
run plat_selftest_neg "ASSERT-OK" "$BUILD/skedaddle_plat_selftest" --negative

# ---- 设置持久化（完整设置系统）----
run skedaddle_settings_selftest "SETTINGS-OK" "$BUILD/skedaddle_settings_selftest"

# ---- 语音播报（离线 TTS）----
run skedaddle_tts_selftest "TTS-OK" "$BUILD/skedaddle_tts_selftest"

# ---- 语音播报策略（抄写 OsmAnd 官方：分级提前量 / 中文话术 / 节奏 / 沿路告警）----
# 纯逻辑、无头可跑；期望值全部来自官方源码（AnnounceTimeDistances / nav-desktop
# VoiceRouter / Alarm / AlarmType / Units / Alarms）。
run skedaddle_voice_selftest "VOICE-OK" "$BUILD/skedaddle_voice_selftest"

# ---- 语音播报**端到端**（走生产接线 nav_voice_frame，验证"窗口真的播了"）----
# 用 NAVIMAP_TTS_DRYRUN 截下 nm_tts_speak 交出去的话来断言；单函数自测抓不到接线 bug。
run skedaddle_voice_e2e "VOICE-E2E-OK" "$BUILD/skedaddle_voice_e2e"

# ---- 多方案对比（阶段 12）----
run skedaddle_alts_selftest "ALTS-OK" "$BUILD/skedaddle_alts_selftest"

# ---- 车道指示（turn:lanes）----
# 数据链路：beijing 图含 lanes.bin，mapset 能按 (省,边) 取出合法车道串。
run skedaddle_lane_selftest "LANE-OK" "$BUILD/skedaddle_lane_selftest" --graph="$GRAPH/beijing"

# ---- PBF 显示层后台异步解码（不阻塞主线程 + 进度回调 + 解码完成）----
run skedaddle_dmap_selftest "DMAP-OK" "$BUILD/skedaddle_dmap_selftest" "$GRAPH" "$ROOT/sources"

# ---- 显示层分 tile（抽取期切好，运行时按需加载；覆盖性：整省要素无丢失）----
run skedaddle_disp_tile_selftest "DISP-TILE-OK" "$BUILD/skedaddle_disp_tile_selftest" --graph="$GRAPH/beijing"
run skedaddle_disp_runtime_selftest "DISP-RUNTIME-OK" "$BUILD/skedaddle_disp_runtime_selftest" --graph="$GRAPH/beijing"
run skedaddle_gpx_selftest "GPX-OK" "$BUILD/skedaddle_gpx_selftest"

# ---- 坐标系（大地基准）：WGS84/CGCS2000/北京54/西安80 枚举/恒等/往返/偏移自测 ----
run skedaddle_datum_selftest "DATUM-OK" "$BUILD/skedaddle_datum_selftest"

# ---- 去 SDL 化的三条门禁（方案2）----
#
# 背景：navimap_core 已做到"零 SDL"，Qt 前端（skedaddle）一行 SDL 都不链，
# SDL 只存在于两个**去 SDL 化对照体**（skedaddle_blend_equiv / skedaddle_clip_equiv，它们必须链
# SDL 才能跟 SDL 软渲染器逐像素对拍）。这条边界一旦被悄悄放宽，
# 编译照样过、运行照样正常，只是 Qt 前端又背上了 SDL2.dll 依赖 —— 直到某天
# 在没装 SDL 的机器上构建才炸。所以必须把边界**钉成可断言的判据**。
#
# ① 光栅 alpha 混色与 SDL 软渲染器逐像素等价：
#    自写 raster_px_rgba 与 SDL 的混色公式差 1 级（SDL 是两项分别除 255 再相加，
#    我们原先是一次除），13.9% 的色值对不上 ⇒ 画面上"同一块面板深浅略不同"。
#    肉眼看不出，但逐像素比对会报差异 —— 这正是它该被门禁住的原因。
run skedaddle_blend_equiv "BLEND-EQUIV-OK" "$BUILD/skedaddle_blend_equiv"

# ② 线段裁剪与 SDL_IntersectRectAndLine 等价：
#    手写 Cohen-Sutherland 与 SDL 的实现在 34.3% 的越界线段上差 1 像素，
#    症状是"画布边缘的路多/少一像素"。已按 SDL 的 SDL_rect_impl.h 逐字重写。
run skedaddle_clip_equiv "CLIP-EQUIV-OK" "$BUILD/skedaddle_clip_equiv"

# ③ 架构边界：核心与 Qt 前端零 SDL 符号；对照体**必须**链 SDL（负向对照，
#    证明检查器不是"永远绿"的空壳）。
#   产物名要补后缀：Windows 上是 skedaddle.exe，Linux 上是 skedaddle。
if [ -x "$BUILD/skedaddle_core_no_sdl" ]; then
    exefile() { if [ -x "$1.exe" ]; then echo "$1.exe"; else echo "$1"; fi; }
    run skedaddle_core_no_sdl "CORE-NO-SDL-OK" "$BUILD/skedaddle_core_no_sdl" \
        "$(exefile "$BUILD/skedaddle")" "$BUILD/libnavimap_core.a"
    run core_no_sdl_sdl "CORE-NO-SDL-OK" "$BUILD/skedaddle_core_no_sdl" --expect-sdl \
        "$(exefile "$BUILD/skedaddle_blend_equiv")"
fi

# ④ 分层方向门禁（PLAN.md §53 R3）：模块依赖方向必须保持 DAG。
#   为什么单列一条：R1/R2 消掉了 map→render 与 render→ui 两处倒置，但
#   **倒置是会被写回来的**。原先没有任何机器检查，全靠人自觉。
#   更糟的是符号级环曾迫使 CMake 用 -Wl,--start-group 包住全部模块库，
#   链接器多轮扫描会悄悄补上反向依赖 ⇒ 边界形同虚设（R2 已移除该包裹）。
#   本脚本自带负向对照能力：人为插入 map→render 的 include 时它必须 FAIL
#   并指出文件行号（已实测），否则就是假门禁。
if [ -x "$ROOT/scripts/check_layers.sh" ]; then
    run layer_dirs "分层方向门禁 PASS" "$ROOT/scripts/check_layers.sh" --quiet
fi

# ---- docs/teaching/ 讲义编号一致性门禁 ----
#
# docs/teaching/ 与 tutorial/ 是同一套 00–14 编号的"概念 vs 动手"两份资料，
# 每课讲义的首个 H1 标题必须声明与文件名前缀一致的「第 N 课」，防止再次漂移。
# 详见 scripts/check_doc_numbers.sh（自带负向对照：改错位 H1 必 FAIL）。
if [ -x "$ROOT/scripts/check_doc_numbers.sh" ]; then
    run doc_numbers "DOC-NUMBERS-OK" "$ROOT/scripts/check_doc_numbers.sh"
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
run font_cjk "FONT-CJK-OK" "$BUILD/skedaddle_font_cjk_check"

# ★ 负向对照：--negative 断言**检测逻辑本身**正反两侧都对 ——
#   无中文字体必须被判成"无中文"，有中文字体必须被判成"有中文"。
#   （不要用"把字体藏起来"那种对照：藏起自带字体后系统字体还在，
#     判据照样通过 ⇒ 那种对照是假的，测不出检查器有没有在干活。）
run font_cjk_neg "FONT-CJK-OK" "$BUILD/skedaddle_font_cjk_check" --negative

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
#
# ★ Windows(MSYS2) 上跑 skedaddle.exe 需要 Qt 的 bin 进 PATH（否则加载 DLL 失败，
#   表现成"命令不存在"）。**编译期**反过来：PATH 里不能有 Qt bin（cc1plus 会加载
#   到不匹配的 libstdc++ 静默死掉，见 scripts/cc_retry.sh）——所以这里只在
#   **运行** Qt 前端时临时加，构建照旧走 build_win.sh。
QT_BIN="${QT_BIN:-/d/Qt/6.11.2/mingw_64/bin}"
qt_run() {
    if [ -d "$QT_BIN" ]; then
        env QT_QPA_PLATFORM=offscreen PATH="$QT_BIN:$PATH" "$@"
    else
        env QT_QPA_PLATFORM=offscreen "$@"
    fi
}
if [ -x "$BUILD/skedaddle" ] || [ -x "$BUILD/skedaddle.exe" ]; then
    exefile2() { if [ -x "$1.exe" ]; then echo "$1.exe"; else echo "$1"; fi; }
    # ★ ui-test 必须**用独立配置目录**跑：它断言"浮层默认透明度 75%"，
    #   读的是真实设置文件。而设置文件会被程序退出时写回（app_persist_settings），
    #   ⇒ 任何一次带 --ovl-alpha=...=0 的实验、或用户自己把某个浮层调成 0，
    #   都会污染真实配置、让这条用例假红（实测踩过：负向对照实验写回
    #   alpha_search=0 → ui-test 报 6 项不符，其中 4 项是搜索浮层连锁失败）。
    #   隔离后这条用例只受代码影响，不受本机状态影响。
    CFG_UIT="$SHOTS/ui_test_cfg"
    /bin/rm -rf "$CFG_UIT" 2>/dev/null
    mkdir -p "$CFG_UIT/navimap"
    printf 'day_night=0\nshow_panel=1\nshow_poi=1\nshow_labels=1\nvoice_on=1\nmode=0\nfont_size=14\nmax_class=4\nview_lat=39.94\nview_lon=116.405\nview_zoom=13\ndatum=0\ngrid_type=1\nalpha_search=75\nalpha_rcol=75\n' > "$CFG_UIT/navimap/settings.ini"
    run qt_ui_test "QT-UI-OK" qt_run \
        env APPDATA="$CFG_UIT/appdata" XDG_CONFIG_HOME="$CFG_UIT/xdg" \
        "$(exefile2 "$BUILD/skedaddle")" --ui-test --w=900 --h=600
fi

# ---- 优先队列：每次 pop 必须是当前最小值（PLAN.md §37）----
# 堆坏了不会崩，只会让 A* 悄悄弹出非最小节点、路线变长。所以要有专门判据：
# 20 万次随机操作，每次 pop 与**独立参考实现**对拍 + 多重集比对 + 堆不变式。
run heap_order "HEAP-SELFTEST-OK" "$BUILD/skedaddle_heap_selftest"

# 双向判据的另一半：故意写坏的实现（不上浮 / 取最后一个元素）**必须**被抓出来。
# 分辨力验过：把"下沉漏掉最后一个子节点"或"父下标算错"注入 pqueue.h，
# 都会被这条判据报出 4~6 万处违规 —— 也就是改叉数这类改动是有人盯着的。
run heap_guard "HEAP-SELFTEST-DIFF-OK" "$BUILD/skedaddle_heap_selftest" --expect-fail

# 堆的**稳态微基准**（PLAN.md §43）：把 pq_pop 放大到 100% 才测得准 —— 它在
# 全链路里只占 6.8%（京→郑 72 ms），改它的收益会被 ±50 ms 的运行间噪声吃掉。
# 判据是**双实现在同一进程里对照**（逐层交换 vs 空穴法，两者跑同一份初始堆、
#    同一串伪随机数），外加 FNV-1a 摘要：换实现后两者必须**逐位相同** ——
#    这能排除"变快了是因为语义改错了"。
run heap_bench "HEAP-BENCH" "$BUILD/skedaddle_heap_selftest" --bench=6019,2000000,3

# ---- 瓦片下标：查表版 vs 精确版 逐点等价（PLAN.md §36）----
# ① 全图 5228 万个真实节点逐点比，外加定向构造的 49 万个"正好落在瓦片边界上"
#    的点（按瓦片单位扰动）。nm_tile_of 取错瓦片 ⇒ 缝合成员查错 ⇒ 路线错，
#    而误差只有万分之几个点会触发，**抽样测不出来**。
run tile_eq "TILE-SELFTEST-OK" "$BUILD/skedaddle_tile_selftest" --graph="$GRAPH"

# ② 双向判据的另一半：不带边界回退的 raw 版**必须**有差异，否则说明那条回退
#    分支是死代码（或者查表精度被改了）。两条互斥，任一条变了都会有一边报警。
#    实测：raw 版在北京就有 1167 个真实节点取错瓦片 ⇒ 回退不是摆设。
run tile_guard_needed "TILE-SELFTEST-DIFF-OK" "$BUILD/skedaddle_tile_selftest" --graph="$GRAPH" --expect-diff

run skedaddle_tile_xcheck "不一致的坐标 : 0" "$BUILD/skedaddle_tile_xcheck"
run coverage    "覆盖=有路网"  "$BUILD/skedaddle_coverage_cli"

# 无图区提示的"反查 + 一键下载"整链（2026-10-27）：
# 把"这里没有地图数据"改成"这是哪儿（国家/省/市/区）+ 该下哪一块 + 预选到那一项"，
# 其中两处会**静默失效**：反查结果没传到弹窗、selectByLeaf 找不到叶子。
# 两者都不报错、界面照样打开，只是按钮点下去停在树根 ⇒ 必须做成退出码。
# 用 (30,110)（重庆方向、未下载）当样本：它不在 8 省任何 bbox 内，必然走反查分支。
if [ -x "$BUILD/skedaddle" ] || [ -x "$BUILD/skedaddle.exe" ]; then
    run nomap_prompt "nomap_select" qt_run \
        "$(exefile2 "$BUILD/skedaddle")" --graph="$GRAPH" \
        --lat=30.0 --lon=110.0 --zoom=10 --w=1280 --h=800 \
        --shot=/tmp/navimap_regress/nomap.png --shot-nomap
fi
run search      "查询 中关村"  "$BUILD/skedaddle_search_cli"
# 地名库自检：每条 place 都得能反查出 bigram，kind 不许超出 PLK_ROAD。
# 以前缺这条判据的代价是实打实踩到的：skedaddle_places_build 刚加上路名那一刻，库里
# 5.7 万条被自检判为非法，而当时 21 项里没有一项会报警（地图照画、路照通）。
run places_selftest "PLACES-OK" "$BUILD/skedaddle_places_build" --db="$ROOT/products/places_auto.db" --selftest

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
run route_hard "ROUTE-OK" "$BUILD/skedaddle_route_cli" --graph="$GRAPH" \
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
run route_far_dalian "ROUTE-OK" "$BUILD/skedaddle_route_cli" --graph="$GRAPH" \
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
    NAVIMAP_LM="$1" "$BUILD/skedaddle_route_cli" --graph="$GRAPH" \
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
#   之前 navsim_* 用的是独立工具 skedaddle_navsim_cli，**根本没走到 window.c**；
#   这里用 skedaddle --shot-offscreen --from/--to 走 `app_nav_plan_sync`
#   （离屏通道设 a.qt_ui=0，主程序算路/渲染一条龙与旧 map_shot 逐字节一致），
#   一次盖住：① 吸附 → ② 算路 → ③ 转向指令 → ④ 路线真的画到画面上（route_pixels）。
#   ★ 起终点特意选 **保定→石家庄**：修复前它吸附到 hebei #6204583（悬在与主干
#     断开的小连通块里）⇒ 双向都"不连通"，是 §51.1 那个 bug 的原生样本。
#     判据"里程在 120~160 km 内"能同时挡住"算不出来"和"吸附错了点"。
run app_route "ASSERT-OK" "$BUILD/skedaddle" --shot-offscreen --graph="$GRAPH" \
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

# ---- GPX 轨迹 / 收藏：skedaddle --shot-offscreen --gpx（离屏像素判据）----
# 与 Qt 前端那条 UI 断言分工：Qt 侧钉"导入 → 面板条目跟着 store 变"，
# 这里钉"轨迹/收藏**真的画到画面上**"（像素判据），走共享审计模块、不依赖 Qt/offscreen。
# ★ 负向对照已实证：把 paint_tracks / paint_favorites 改成直接 return 0 ⇒
#   分别报「没有轨迹像素」「没有金色像素」两条 ASSERT-FAIL（各实测过一次）。
# 视口中心取北京二环一带，GPX 内联生成（不往仓库里塞夹具）。
GPX_FILE="$SHOTS/gpx_case.gpx"
cat > "$GPX_FILE" <<'GEOF'
<?xml version="1.0" encoding="UTF-8"?>
<gpx version="1.1" creator="navimap-regress">
  <wpt lat="39.9520" lon="116.3960"><name>故宫东门</name></wpt>
  <wpt lat="39.9480" lon="116.4040"><name>王府井</name></wpt>
  <trk><name>回归测试线</name><trkseg>
    <trkpt lat="39.9450" lon="116.3900"/>
    <trkpt lat="39.9500" lon="116.4000"/>
    <trkpt lat="39.9550" lon="116.4100"/>
    <trkpt lat="39.9600" lon="116.4200"/>
    <trkpt lat="39.9500" lon="116.4300"/>
    <trkpt lat="39.9400" lon="116.4200"/>
  </trkseg></trk>
</gpx>
GEOF
# 离屏通道不建窗口、不链 SDL，任何环境都能跑（无需 QT_QPA_PLATFORM / SDL_VIDEODRIVER）。
run gpx_render "ASSERT-OK" "$BUILD/skedaddle" --shot-offscreen --graph="$GRAPH" \
    --gpx="$GPX_FILE" \
    --lat=39.95 --lon=116.40 --zoom=13 --w=1280 --h=800 \
    --out="$SHOTS/gpx_case.bmp"
if [ -f "$SHOTS/gpx_render.log" ]; then
    tpx=$(sed -n 's/^gpx_trk_px  *\([0-9]*\).*/\1/p' "$SHOTS/gpx_render.log" | head -1)
    fpx=$(sed -n 's/^gpx_fav_px  *\([0-9]*\).*/\1/p' "$SHOTS/gpx_render.log" | head -1)
    ok=$(awk -v p="${tpx:-0}" 'BEGIN{print (p>200) ? 1 : 0}')
    if [ "$ok" = 1 ]; then
        pass=$((pass + 1)); printf '%-28s OK   (%s 个轨迹像素 > 200)\n' "gpx_trk_px" "$tpx"
    else
        fail=$((fail + 1)); printf '%-28s FAIL %s 像素（轨迹没画上去？）\n' "gpx_trk_px" "${tpx:-0}"
    fi
    ok=$(awk -v p="${fpx:-0}" 'BEGIN{print (p>20) ? 1 : 0}')
    if [ "$ok" = 1 ]; then
        pass=$((pass + 1)); printf '%-28s OK   (%s 个收藏金色像素 > 20)\n' "gpx_fav_px" "$fpx"
    else
        fail=$((fail + 1)); printf '%-28s FAIL %s 像素（收藏没画上去？）\n' "gpx_fav_px" "${fpx:-0}"
    fi
fi

# ---- GPX 贴合道路：skedaddle_route_cli --gpx-snap（#114）----
# 把同一条 GPX 轨迹（GPX_FILE）抽稀成途经点、吸附到路网沿道路串联，
# 输出一条沿道路的折线并跑 route_check 自检。判据只认 ROUTE-OK（"能出结果 + 路径自检过"），
# 不卡绕行系数（道路绕远是预期行为）。该 GPX 在 7 省图（GRAPH）下必能贴合。
# Qt 侧那条"轨迹行含「贴合」按钮 + 调用优雅返回"断言分工：这里钉**命令行端真的贴合成功**。
run gpx_snap "ROUTE-OK" "$BUILD/skedaddle_route_cli" --graph="$GRAPH" --gpx-snap="$GPX_FILE"

# ---- 浮层透明度：默认 75%，且**每个浮层各自独立** ----
# 判据不是"两张图不一样"（那样设成 20% 也会过），而是**与 alpha 成正比的精确
# 关系**：|M−H| / |O−H| == pct/100（M=该 pct、O=100%、H=0%=纯底图）。
#   这个比值与元素底色的固有 alpha 无关（右栏按钮底色是 245 而非 255，
#   按固定配色算就会假红）。判据实现在 scripts/ovl_px.py。
# ★★ 规则是"**只淡底板，文字/图标保持清晰**"（J 先生 2026-10-25 要求），所以
#   判据是两条：
#     ① 底板像素（矩形内的**中位数**）比值 == pct/100
#        —— 用中位数不用单点取样：底板像素占多数，单点很容易采到文字上；
#     ② 必须有一批比值 == 1.0 的像素（文字/图标一点没淡）
#        —— 前景不参与线性淡出 ⇒ 它在 100% 与 75% 两张图上逐像素相同。
#   两条合起来才把"只淡底板"钉死：少了 ① 会把"整体不淡"放过去，
#   少了 ② 会把"文字被一起淡掉"放过去。
# 同时验"逐浮层独立"：改一个浮层的透明度，矩形外一个像素都不许跟着变。
# ★ 负向对照已实证（三条，各自打到不同的一条判据上）：
#   ① 把 chrome.c 的 ovl_begin() 改成 g_ovl_k = 1.0（压根不按浮层缩放）
#      ⇒ 100% 与 0% 两张图完全相同 ⇒ "矩形内找不到明显不同的像素" FAIL；
#   ② 把 draw_titlebar 的 ovl_begin(a, OVL_TITLE) 改成 OVL_RCOL（串浮层：
#      改右栏的透明度连标题栏一起变）⇒ "矩形外有 24000 个像素跟着变了" FAIL
#      （24000 = 800×30，正好是标题栏面积）；
#   ③ 把 chrome.c 的 rtext 改回乘系数（文字跟着一起淡）
#      ⇒ "前景（文字/图标）没有保持清晰" FAIL。
ovl_shot() {   # $1=key $2=pct $3=输出
    # ★ skedaddle --shot-offscreen 的输出由 run() 自己重定向进 $log；这里**不要**
    #   再对 run 调用挂重定向 —— 那会把 run() 的 OK/FAIL 结果行一起吞掉。
    # ★ 离屏通道设 a.qt_ui=0，chrome 由 C 核心画进缓冲（与旧 map_shot 逐字节一致），
    #   所以浮层矩形坐标仍是 C 侧布局：标题栏 0,0,800,30；右栏四按钮 750,88,42,184。
    run "ovl_$1_$2" "ASSERT-OK" "$BUILD/skedaddle" --shot-offscreen --graph="$GRAPH" \
        --lat=39.94 --lon=116.405 --zoom=13 \
        --w=800 --h=600 --ovl-alpha="$1=$2" --out="$3"
}
# 标题栏矩形 0,0,800,30（CHROME_TITLE_H=30）；右栏四按钮 750,88,42,184
# ★ 离屏通道设 a.qt_ui=0，chrome 由 C 核心画进缓冲；标题栏**只有底板没有文字**
#   （离屏通道不加载字体，"NavMap" 那行字不画）⇒ 这条只验底板比值 + 逐浮层独立，
#   前景判据 --fg=0 跳过。前景（文字/图标）保持清晰由下面两条验：
#   C 右栏（图标，本段）与 Qt 搜索按钮（**文字**，--shot 视觉验收段）。
ovl_shot alpha_title 100 "$SHOTS/ovl_title_100.bmp"
ovl_shot alpha_title 75  "$SHOTS/ovl_title_075.bmp"
ovl_shot alpha_title 0   "$SHOTS/ovl_title_000.bmp"
if python3 "$ROOT/scripts/ovl_px.py" "$SHOTS/ovl_title_100.bmp" \
        "$SHOTS/ovl_title_075.bmp" "$SHOTS/ovl_title_000.bmp" \
        --rect=0,0,800,30 --pct=75 --fg=0 > "$SHOTS/ovl_title.log" 2>&1; then
    pass=$((pass + 1)); printf '%-28s OK   (标题栏 75%%：底板混合比=75%%，矩形外零影响)\n' "ovl_alpha"
else
    fail=$((fail + 1)); printf '%-28s FAIL 标题栏 75%% 不成立（见 ovl_title.log）\n' "ovl_alpha"
fi
ovl_shot alpha_rcol 100 "$SHOTS/ovl_rcol_100.bmp"
ovl_shot alpha_rcol 75  "$SHOTS/ovl_rcol_075.bmp"
ovl_shot alpha_rcol 0   "$SHOTS/ovl_rcol_000.bmp"
if python3 "$ROOT/scripts/ovl_px.py" "$SHOTS/ovl_rcol_100.bmp" \
        "$SHOTS/ovl_rcol_075.bmp" "$SHOTS/ovl_rcol_000.bmp" \
        --rect=750,88,42,184 --pct=75 > "$SHOTS/ovl_rcol.log" 2>&1; then
    pass=$((pass + 1)); printf '%-28s OK   (右栏 75%%：底板 75%% + 图标保持清晰)\n' "ovl_alpha_rcol"
else
    fail=$((fail + 1)); printf '%-28s FAIL 右栏 75%% 不成立（见 ovl_rcol.log）\n' "ovl_alpha_rcol"
fi

# ---- 浮层透明度（Qt 前端）：底板真的合成进截图 ----
# Qt 侧是子控件浮层：setWindowOpacity 对它们无效（只对顶层窗口有效），
# QGraphicsOpacityEffect 有效但会把图标和文字**一起**淡掉 ⇒ 现在改成
# IconButton 自绘底板时按系数算 alpha（qt_icon_button.hpp），
# 面板/输入框用 QSS 的 rgba 背景（main_window.cpp 的 trackPanelStyle）。
# 判据与 C 侧同一份脚本、同一个数学关系 ⇒ 两个前端口径一致。
# offscreen 下这条也要成立（实测混合比 0.7500）。
if [ -x "$BUILD/skedaddle.exe" ] || [ -x "$BUILD/skedaddle" ]; then
    QEXE=$(if [ -x "$BUILD/skedaddle.exe" ]; then echo "$BUILD/skedaddle.exe"; else echo "$BUILD/skedaddle"; fi)
    CFGQ="$SHOTS/ovl_qt_cfg"
    /bin/rm -rf "$CFGQ" 2>/dev/null
    mkdir -p "$CFGQ/navimap"
    printf 'day_night=0\nshow_panel=1\nshow_poi=1\nshow_labels=1\nvoice_on=1\nmode=0\nfont_size=14\nmax_class=4\nview_lat=39.94\nview_lon=116.405\nview_zoom=13\ndatum=0\ngrid_type=1\n' > "$CFGQ/navimap/settings.ini"
    for pv in 100 75 0; do
        qt_run env APPDATA="$CFGQ" XDG_CONFIG_HOME="$CFGQ/xdg" \
            "$QEXE" --graph="$GRAPH" --w=800 --h=600 \
            --ovl-alpha=alpha_rcol=$pv --shot="$SHOTS/ovl_qt_$pv.bmp" \
            > "$SHOTS/ovl_qt_$pv.log" 2>&1
    done
    if python3 "$ROOT/scripts/ovl_px.py" "$SHOTS/ovl_qt_100.bmp" \
            "$SHOTS/ovl_qt_75.bmp" "$SHOTS/ovl_qt_0.bmp" \
            --rect=740,0,60,420 --pct=75 > "$SHOTS/ovl_qt.log" 2>&1; then
        pass=$((pass + 1)); printf '%-28s OK   (Qt 右栏 75%%：底板 75%% + 图标清晰)\n' "qt_ovl_alpha"
    else
        fail=$((fail + 1)); printf '%-28s FAIL Qt 右栏 75%% 不成立（见 ovl_qt.log）\n' "qt_ovl_alpha"
    fi

    # ---- 浮层透明度（Qt 搜索按钮）：**文字**必须保持清晰 ----
    # 这条直接验 J 先生 2026-10-25 提的核心诉求："只淡底板、文字保持清晰"。
    # 搜索按钮是 IconButton（自绘底板 + 自绘图标 + 「搜索」二字），Qt 侧有
    # 字体 ⇒ 文字真的画出来了（已删的旧 map_shot 不加载字体，C 侧截图里没有文字）。
    # 判据：底板混合比 = 75%，且「搜索」二字的像素在 100% 与 75% 两张图上
    # **没有被一起淡掉**。
    # ★ --fgrect 单独框出文字区：图标在左、文字在右，不框的话图标会替文字
    #   顶缸（口径 a 在整块按钮上只数到 3 个文字像素，抓不到）。
    # ★ 14px 中文字形抗锯齿后没有 coverage=1 的纯色核 ⇒ 口径 a 数不到，
    #   要靠口径 b（相对底图的偏离量没按 75% 缩小）—— 它只在框准前景时才有
    #   判别力（文字区：正向 111 个 / 文字被淡后 39 个）。
    # ★ 负向对照：把 IconButton 里画文字的 pen 颜色也乘上系数 ⇒ 文字被一起淡
    #   ⇒ "前景（文字/图标）没有保持清晰" FAIL（实测过一次）。
    for pv in 100 75 0; do
        qt_run env APPDATA="$CFGQ" XDG_CONFIG_HOME="$CFGQ/xdg" \
            "$QEXE" --graph="$GRAPH" --w=800 --h=600 \
            --ovl-alpha=alpha_search=$pv --shot="$SHOTS/ovl_qts_$pv.bmp" \
            > "$SHOTS/ovl_qts_$pv.log" 2>&1
    done
    # ★ --rect 必须框住**整个按钮**：判据 3（逐浮层独立）要求矩形外零变化。
    #   ★ 2026-10-29 坐标随 UI 改版更新：远端取消顶栏菜单栏后，搜索浮层从
    #     顶栏下沿移到**画布左上角**，位置由代码固定（main_window.cpp 的
    #     margin=10 ⇒ move(10,10)），不再随顶栏高度浮动 ⇒ 跨平台位置稳定了，
    #     只有按钮**宽度**仍随平台字体度量浮动（图标+「搜索」二字）。
    #     Linux 实测：按钮整体 x 10..93 / y 10..45（84x36），文字区 x 49..80。
    #     矩形取 6,6,96,44（比按钮各留 4px 余量，够盖住 Windows 侧宽度浮动），
    #     fgrect 取 45,18,40,20（框住文字区，两侧各留 4~5px 余量）。
    #     真误伤（串浮层）是标题栏那种上万 px 的整块变化，不会被这点余量放过去。
    #   ★ 负向对照随坐标更新重验过：注入"文字 pen 也乘系数"后，
    #     前景判据 20/800 ⇒ FAIL（阈值 60），判据仍有分辨力。
    if python3 "$ROOT/scripts/ovl_px.py" "$SHOTS/ovl_qts_100.bmp" \
            "$SHOTS/ovl_qts_75.bmp" "$SHOTS/ovl_qts_0.bmp" \
            --rect=6,6,96,44 --fgrect=45,18,40,20 --fg-min=60 --pct=75 \
            > "$SHOTS/ovl_qts.log" 2>&1; then
        pass=$((pass + 1)); printf '%-28s OK   (Qt 搜索按钮：底板 75%% + 「搜索」字清晰)\n' "qt_ovl_text"
    else
        fail=$((fail + 1)); printf '%-28s FAIL Qt 搜索按钮文字不清晰（见 ovl_qts.log）\n' "qt_ovl_text"
    fi
fi

# ---- GPX 持久化：配置目录不存在时必须自己建出来 ----
# 失效模式很阴：sqlite3_open **不建父目录**，打不开只返回 NULL ⇒ 导入照常
# 打印"1 条轨迹"（内存侧成功了），但一条也不落库、**没有任何提示**，
# 重启后收藏全丢。全新账户 / 刚清过 %APPDATA% 必中。
# 判据：把配置目录指到一个**尚不存在**的路径，跑一次导入，断言 tracks.db 真被建出来。
# ★ 负向对照：把 gpx_db_default_path 里的 nm_mkdir_p 那行注掉 ⇒ 目录不建、
#   sqlite 打不开 ⇒ 这条 FAIL（实测过一次）。
# 跨平台：Windows 看 APPDATA，其余看 XDG_CONFIG_HOME；两个都设，哪个生效都能验。
if [ -x "$BUILD/skedaddle.exe" ] || [ -x "$BUILD/skedaddle" ]; then
    CFG="$SHOTS/gpx_cfg_fresh"
    /bin/rm -rf "$CFG" 2>/dev/null
    QEXE=$(if [ -x "$BUILD/skedaddle.exe" ]; then echo "$BUILD/skedaddle.exe"; else echo "$BUILD/skedaddle"; fi)
    qt_run env APPDATA="$CFG/appdata" XDG_CONFIG_HOME="$CFG/xdg" \
        "$QEXE" --graph="$GRAPH/beijing" --gpx="$GPX_FILE" \
        --shot="$SHOTS/gpx_cfg.bmp" > "$SHOTS/gpx_persist.log" 2>&1
    # 两种平台口径都找一遍，任一处出现 tracks.db 即算建出来了
    found=0
    for cand in "$CFG/appdata/navimap/tracks.db" "$CFG/xdg/navimap/tracks.db"; do
        [ -s "$cand" ] && found=1
    done
    if [ "$found" = 1 ]; then
        pass=$((pass + 1)); printf '%-28s OK   (配置目录不存在时自建 tracks.db)\n' "gpx_persist"
    else
        fail=$((fail + 1)); printf '%-28s FAIL 全新配置目录下没建出 tracks.db（收藏会静默丢失）\n' "gpx_persist"
    fi
fi

# ★ 地标距离（一对多 Dijkstra）的验收（PLAN.md §48.5）：
#   判据是**数值区间**，不是"能跑完"。北京地标到郑州的代价实测 23136.8 秒
#   （= 385.6 分），与 skedaddle_route_cli 的最优解（386 分，打印取整到整分）一致。
#   区间取 22500~23800 秒（±3%）：
#     - 上界挡住"少做了转向限制 / 没做缝合"以外的**变大**类退化；
#     - 下界挡住"跨省缝合把距离算短了"这类**变小**的错误。
#   ★★ 负向对照已做：把跨省缝合那段临时改成不执行，5 个跨省查询**全部
#      UNREACHABLE** ⇒ 这条判据真能失败，缝合代码确实是承重的。
run lmdist "LM-DIST-OK" "$BUILD/skedaddle_lmdist_cli" --graph="$GRAPH" \
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
```

## 四、与其他功能之间的联系

- **守卫范围**：门禁覆盖第 1–12 课全部模块——`check_layers.sh` 锁死 DAG 方向（纯 `utils ← pbf ← map ← route ← render ← ui ← ui_qt`），`skedaddle_core_no_sdl` 锁死第 12 课 Qt 不链 SDL，`skedaddle_blend_equiv`/`skedaddle_clip_equiv` 锁死第 7/8 课渲染像素等价，`regress.sh` 把全部用例串成回归门禁。
- **依赖 L10–L12**：`route_cross/navsim_*/skedaddle_restr_ab/rev_*` 验收第 10 课路由；`app_route/gpx_render/ovl_shot` 经 `skedaddle --shot-offscreen` 验收第 8/11/12 课；`layer_dirs` 验收含第 12 课 `ui_qt` 的整张依赖图；`skedaddle_core_no_sdl` 验收第 12 课边界。
- **构建约束（双重防线）**：`CMakeLists.txt` 移除 `--start-group` 是本课硬前提——单趟链接器把“任何反向依赖”变成即时 `undefined reference`（第一道防线），与 `check_layers.sh`（第二道防线）形成双重防线。
- **纯 DAG 总结**：`utils`/`pbf` 为叶子；`map` 只许依赖 `pbf,utils`；`route` 只许依赖 `map,utils`；`render` 纯光栅、零 ui 依赖；`ui` 依赖 `render` 但不许被 `render/map/route` 反向依赖；`ui_qt` 只依赖 `map,render,settings,ui,utils`。任何新引入的反向依赖都会同时被链接器与 `check_layers.sh` 抓出。

## 五、演示、练习与验收

- **演示**：
  - `./scripts/check_layers.sh`（故意在一个 `.c` 加 `#include "ui/window.h"` 到 `map` 模块，看它 FAIL 并指出行号）；
  - `./scripts/regress.sh`（看基线全绿）。
- **练习**：
  - 把 `src/render/paint.h` 的 `AREA_DOT_PX` 改到 8，跑 `regress.sh` 的 `area_lod_tier`，确认被抓出（z9 上限 108000，档位 8 ⇒ 111916 必 FAIL）；改回 4；
  - 把 `links` 方向表中 `NM_ALLOWED_map` 偷偷加 `render`，看 `check_layers.sh` 从 FAIL 变 PASS 的反向实验，理解“门禁必须有分辨力”。
- **验收**：能独立跑全套门禁，并解释“为什么判据要用确定性量而非时间、为什么 start-group 被移除、为什么 skedaddle_core_no_sdl 需要 `--expect-sdl` 负向对照”。
