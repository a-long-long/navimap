# 第 14 课 · 架构纪律与验收门禁

> 模块/脚本：`scripts/check_layers.sh`、`scripts/regress.sh`、`CMakeLists.txt`、`tools/core_no_sdl.c`、`tools/blend_equiv.c`、`tools/clip_equiv.c`
> 前置：第 2–13 课全部内容（本课是“为什么敢改”的门禁总览）。

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
| `scripts/regress.sh` | 全量回归；`ASSERT-OK/ASSERT-FAIL` 双判据（退出码 + 标记）；覆盖渲染/路由/语音/平台/门禁等几十个用例 | `scripts/regress.sh:1-802`（关键判据函数见第三节，全文为控制篇幅省略，可在 `scripts/regress.sh:1` 查阅） |
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
# 故把 sqlite3 收进本模块库，保证 search 系列工具（search_cli/places_build）
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
#     使 24 个 exe / navmap_qt 等无需改动即可继续链接 navimap_core。
#   ★ 核心零 SDL：聚合层连 SDL 的 include 都不给；SDL 仅 blend_equiv / clip_equiv 合法链。
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

`navmap_qt` 只链 `navimap_core + Qt Widgets`，离屏出图走 `src/ui/offscreen_shot.c`（零 SDL），见 `CMakeLists.txt:496-512` 与 `CMakeLists.txt:529-531`（门禁工具 `core_no_sdl` 只链 `nm_link_tool`，不碰 SDL）。

### 3.3 架构边界门禁：`tools/core_no_sdl.c`（完整体）

证明 C 核心零 SDL 依赖。关键设计：用 `nm -u`（静态库 / ELF 有效）与 `objdump -p`（Windows PE 导入表）**两个提取器取并集**（`core_no_sdl.c:138-155`），且用**退出码**判定提取成功、绝不靠行数（避免 stderr 污染造成的假绿，`core_no_sdl.c:192-205`）。`--expect-sdl` 是负向对照：对照体（如 `blend_equiv`）必须含 SDL 符号，否则证明检查器是空壳。

```c
/* core_no_sdl —— 断言某个目标里**没有** SDL 符号（方案2 的架构门禁）。
 *
 * 为什么要有它：
 *   Qt 前端（navmap_qt）必须**一行 SDL 都不链**。但"Qt 不链 SDL"很容易在
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
 *   core_no_sdl <文件...>              期望：这些目标里没有 SDL 符号
 *   core_no_sdl --expect-sdl <文件...> 期望：这些目标里**必须有** SDL 符号
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
                "用法: core_no_sdl [--expect-sdl] <文件...>\n"
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
/* blend_equiv — 验证自写光栅的 alpha 混色与 SDL 软渲染器逐像素等价。
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
/* clip_equiv — 验证自写光栅的线段裁剪与 SDL_IntersectRectAndLine 逐像素等价。
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

### 3.6 回归门禁：`scripts/regress.sh`（关键判据函数，完整体；全文为控制篇幅省略，可在 `scripts/regress.sh:1` 查阅）

`regress.sh` 共 802 行，逐用例串起全量回归。其核心是**双判据**：命令必须出现期望标记且不得出现 `ASSERT-FAIL` 且退出码为 0。下列是承载判据逻辑的完整函数与关键调用点。注意 `layer_dirs`（`regress.sh:316`）与 `core_no_sdl`/`core_no_sdl_sdl`（`regress.sh:300-306`）正是把分层门禁与 SDL 边界门禁接进回归；`area_lod_tier` 用**确定性量** `areas_dotted` 而非时间。

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
    run "$name" "ASSERT-OK" "$BUILD/navmap_qt" --shot-offscreen --graph="$g" \
        --lat="$la" --lon="$lo" --zoom="$z" --w=1280 --h=800 \
        --out="$SHOTS/$name.bmp"
}

# 面要素 LOD 档位（AREA_DOT_PX）。主判据用确定性量 areas_dotted，不用时间。
area_lod_tier() {
    local name=$1 z=$2 lo=$3 hi=$4 want_drawn=$5
    local log="$SHOTS/$name.log"
    "$BUILD/navmap_qt" --shot-offscreen --graph="$GRAPH" \
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
if [ -x "$BUILD/core_no_sdl" ]; then
    exefile() { if [ -x "$1.exe" ]; then echo "$1.exe"; else echo "$1"; fi; }
    run core_no_sdl "CORE-NO-SDL-OK" "$BUILD/core_no_sdl" \
        "$(exefile "$BUILD/navmap_qt")" "$BUILD/libnavimap_core.a"
    run core_no_sdl_sdl "CORE-NO-SDL-OK" "$BUILD/core_no_sdl" --expect-sdl \
        "$(exefile "$BUILD/blend_equiv")"
fi

# ④ 分层方向门禁（PLAN.md §53 R3）：模块依赖方向必须保持 DAG。
if [ -x "$ROOT/scripts/check_layers.sh" ]; then
    run layer_dirs "分层方向门禁 PASS" "$ROOT/scripts/check_layers.sh" --quiet
fi
```

## 四、与其他功能之间的联系

- **守卫范围**：门禁覆盖第 2–13 课全部模块——`check_layers.sh` 锁死 DAG 方向（纯 `utils ← pbf ← map ← route ← render ← ui ← ui_qt`），`core_no_sdl` 锁死第 13 课 Qt 不链 SDL，`blend_equiv`/`clip_equiv` 锁死第 8/9 课渲染像素等价，`regress.sh` 把全部用例串成回归门禁。
- **依赖 L11–L13**：`route_cross/navsim_*/restr_ab/rev_*` 验收第 11 课路由；`app_route/gpx_render/ovl_shot` 经 `navmap_qt --shot-offscreen` 验收第 9/12/13 课；`layer_dirs` 验收含第 13 课 `ui_qt` 的整张依赖图；`core_no_sdl` 验收第 13 课边界。
- **构建约束（双重防线）**：`CMakeLists.txt` 移除 `--start-group` 是本课硬前提——单趟链接器把“任何反向依赖”变成即时 `undefined reference`（第一道防线），与 `check_layers.sh`（第二道防线）形成双重防线。
- **纯 DAG 总结**：`utils`/`pbf` 为叶子；`map` 只许依赖 `pbf,utils`；`route` 只许依赖 `map,utils`；`render` 纯光栅、零 ui 依赖；`ui` 依赖 `render` 但不许被 `render/map/route` 反向依赖；`ui_qt` 只依赖 `map,render,settings,ui,utils`。任何新引入的反向依赖都会同时被链接器与 `check_layers.sh` 抓出。

## 五、演示、练习与验收

- **演示**：
  - `./scripts/check_layers.sh`（故意在一个 `.c` 加 `#include "ui/window.h"` 到 `map` 模块，看它 FAIL 并指出行号）；
  - `./scripts/regress.sh`（看基线全绿）。
- **练习**：
  - 把 `src/render/paint.h` 的 `AREA_DOT_PX` 改到 8，跑 `regress.sh` 的 `area_lod_tier`，确认被抓出（z9 上限 108000，档位 8 ⇒ 111916 必 FAIL）；改回 4；
  - 把 `links` 方向表中 `NM_ALLOWED_map` 偷偷加 `render`，看 `check_layers.sh` 从 FAIL 变 PASS 的反向实验，理解“门禁必须有分辨力”。
- **验收**：能独立跑全套门禁，并解释“为什么判据要用确定性量而非时间、为什么 start-group 被移除、为什么 core_no_sdl 需要 `--expect-sdl` 负向对照”。
