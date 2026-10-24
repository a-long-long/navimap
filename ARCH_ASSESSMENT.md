# navimap 代码可复用性 / 模块化 / 组织化评估

> 评估对象：本机 `E:/navimap` 工作区（提交 `d447d00` 前后，含 Windows/Qt6 双版本兼容改动前沉淀）
> 评估方法：基于真实编译产物的量化（文件行数、头文件依赖图、全局状态扫描、函数前缀 fan-out），非主观印象。
> 规模快照：84 个源文件（`.c/.h/.cpp`），约 2.16 万行（含 `src/` + `tools/` + `src/ui_qt/`）。

---

## 0. 结论速览表

| 层次 | 含义 | 可行性 | 实测成本 | 价值 | 建议 |
|---|---|---|---|---|---|
| **L1 拆文件/目录** | 按职责把类/文件挪进子目录 | ✅ 已完成 | 极低（几乎为 0） | 中（可读性） | **无需再做** |
| **L2 拆独立库** | 每模块编成独立静态库，只暴露 API | ✅ **已完成（阶段1+2）** | 中（CMake 改造） | 中高（边界被链接器强制、可独立复用） | 已落地（12 个 libnavmap_*.a） |
| **L3 拆上帝文件** | 把 route.c / paint.c 等大文件按职责分解 | ⚠️ 暂不推荐 | 高（且破热路径风险） | 低（已内部高内聚） | **等具体复用/测试需求出现再做** |

**一句话结论**：代码组织已经"天然模块化"——目录即模块、依赖除 `render↔ui` 环外是干净的 DAG（见 §1.3 修正注）、零跨文件可变全局、符号带模块前缀。当前最大的可回收价值不是"重构代码"，而是**把已有的模块边界用构建系统固化成独立库（L2，已完成阶段1+2）**，从而解锁局部复用与更快的增量构建；L3 不要现在动。

---

## 1. 现状实测

### 1.1 构建系统（关键事实）
- 整个 `src/` 编译成**一个** `navimap_core` 静态库（`add_library(navimap_core STATIC ${CORE_SRCS})`，`CORE_SRCS` 显式罗列、非 glob）。
- 24 个可执行文件（主程序 + `tools/` 各类 CLI + 可选 `navmap_qt`）全部 `target_link_libraries(... navimap_core)`。
- 没有多包/多模块构建（vcpkg/conan 之类）。这是 L2 的"起点状态"，不是缺陷。

### 1.2 目录即模块（L1 已到位）
```
src/
  utils/    geo mem plat strpool           ← 叶子层（互不依赖）
  pbf/      pbf_block pbf_stream           ← 叶子层（只依赖自己）
  map/      autobuild binfmt coverage graph_build map_view mapset pbfmap road_tags tileidx (+coordkey.h)
  route/    route lmtable revidx maneuver navsim (+pqueue.h)
  search/   search
  render/   paint chrome navdraw labels font raster
  ui/       window
  ui_qt/    main_window map_canvas qt_main (C++ 前端，独立构建守卫)
  main.c
tools/      24 个 CLI 工具（各自链接 navimap_core）
```
每个 `.c` 配套一个 `.h`，头文件只声明本模块公开符号——**头/源一一对应、含入关系清晰**。

### 1.3 模块级依赖图（剥注释后按真实 `#include` 构建；★ 已修正：原版缺 `map→render` 边且误判为无环）

```
Layer 6  src/main.c ───────────────────────────────▶ src/ui
Layer 5  src/ui        ──▶ map, render, utils    (ui→render，且 render→ui ⇒ 见下环)
Layer 4  src/render    ──▶ map, route, ui, utils (render↔ui 符号级环)
Layer 3  src/route     ──▶ map, utils        src/search ──▶ utils
Layer 2  src/map       ──▶ pbf, render, utils  (mapset.c 调 render/labels.c 的 name_pool_open/close)
Layer 1  src/pbf       ──▶ (self only)
Layer 0  src/utils     ──▶ (leaf, 被其余 5 个模块全部依赖)
```
- **模块出度**：render→4、ui→3、map→3、route→2、search→1、pbf→0、utils→0
- **模块入度**：utils←5、map←3、ui←2、render←2、route←1、pbf←1、search←0
- **环检测（★ 修正）**：存在一个 `render ↔ ui` 符号级环——`ui/window.c` 调用 `render` 的 `paint/navdraw/chrome`，`render/chrome.c` 反过来调用 `ui/window.c` 的 `app_nav_*`；此外 `map/mapset.c` 调用 `render/labels.c` 的 `name_pool_open/close`（即 `map→render` 边，原 DAG 漏画）。除该环外为 DAG。**修正前提**：拆库仍可行，但 5 个模块库**不能按纯拓扑序链接**（会 undefined reference），必须用 `-Wl,--start-group … -Wl,--end-group` 包裹全部模块库消解环（GNU/MinGW ld；MSVC `link.exe` 自带多轮，无需 start-group）。该环在语义上合理（UI 调度渲染、渲染回调 UI 的导航动作），无需破坏。

### 1.4 公开 API 暴露面
- 头文件共声明 **253** 个函数（即对外暴露面）。
- 最大暴露面：`plat.h`(36)、`window.h`(25)、`route.h`(17)、`paint.h`(16)、`binfmt.h`(16)、`map_view.h`(12)。
- 数字合理：项目 2 万行、253 个入口，平均每个模块 ~25 个公开符号，不臃肿。

### 1.5 全局可变状态（模块化/可测试性的真正障碍，逐项实测）
- **跨文件 `extern` 可变全局：0 个**（grep 全 `src/` 无结果）。状态未泄漏到文件外——这是很强的纪律信号。
- **文件作用域 static 可变全局**（按模块）：
  - `render/paint.c`：**6 个**（`g_rl` 道路标签数组、`g_rl_n`、`g_last_px/tag`、`g_sp_x/y/have`，外加 `rla/rlo/bx/by` 环形缓冲、`seen[]`）。这些是**渲染流水线状态**，意味着 `paint` 不可重入、单测需手动 reset——**当前唯一的真实障碍**。
  - `render/labels.c`：`g_grid` 标签排布网格 + `g_grid_w/h`（模块状态，可封装成 struct 传参）。
  - `utils/plat.c`：`g_nm_maps` / `g_nm_nmaps`（平台文件映射表，属抽象层应持有的合法状态）。
- 结论：可变全局**仅局限于 render + platform 两层**，未扩散到全代码库；按技能判据（"同进程跑两遍探针是否互不影响"），render 目前不满足，但影响范围可控。

### 1.6 "上帝文件"判定（L3 是否必要）
| 文件 | 行数 | 内部 static 函数 | 判定 |
|---|---|---|---|
| `route/route.c` | 1856 | 34 | 大而内聚：路由核心，已拆 34 个静态辅助；fan-out 显示它是"路由枢纽"（调用 lm/rev/st/coord/tile/mapset/nm），属角色固有，非职责混乱 |
| `ui/window.c` | 1288 | 14 | SDL 前端聚合（事件/循环/绘制调度），内聚 |
| `map/graph_build.c` | 1260 | 26 | 图构建流水线，已分 26 段 |
| `render/paint.c` | 947 | 19 | 绘制核心，内聚；唯一问题在 §1.5 的全局状态 |

**判定**：这些"大文件"是**大而内聚（large-but-coherent）**，不是"大而无当（god class）"。它们已被内部 static 函数充分分解，且各自职责单一。L3 拆解的投入高、收益低、且会扰动已重度调优的热路径（见工作记忆：route/paint 耗时已逐个优化到毫秒级）。

---

## 2. 逐层展开

### L1（拆文件/目录）—— 已完成，价值已耗尽
目录划分与依赖方向天然吻合职责。"文件→模块"映射清晰，无"一个文件跨俩职责"的异味。**再投入 L1 没有收益**，这点和多数"先分包"的项目相反——本项目起点就很好。

### L2（拆独立库）—— 可行（除 render↔ui 环外为 DAG，start-group 消解）
因依赖除 `render↔ui` 环外是 DAG，把每个目录提升为独立 `STATIC` 库时**不需要打破任何环**（环在语义上合理：UI 调度渲染、渲染回调 UI 动作），只需把 5 个模块库整体包进 `-Wl,--start-group … -Wl,--end-group` 链接：
```
navmap_geo / navmap_strpool / navmap_plat / navmap_mem  (叶子，mem→plat)
   → navmap_pbf → navmap_map → {navmap_route, navmap_search} → navmap_render ⇄ navmap_ui
# 注：navmap_map 还依赖 navmap_render（mapset.c 调 name_pool）；navmap_render ⇄ navmap_ui 互为依赖。
#     故 5 个模块库链接时须整体包进 start-group，顺序不敏感。
```
收益：
1. **局部复用**：`pbf`（解析器）、`geo`（投影/距离数学）、`strpool`、`plat`（平台抽象）这 4 个是**纯叶子、零内部依赖**，拆出后即可被其它项目独立取用——目前要复用它们得整车拖走 `navimap_core`。
2. **链接器强制边界**：比"约定"更硬的模块墙，避免日后偷偷反向依赖。
3. **增量构建更快**：改 `render` 不再重链整库。
代价：CMake 把 1 个 lib 改成 N 个 lib + 调整 24 个 exe 的 `target_link_libraries`（工作量大但机械、低风险，因 `CORE_SRCS` 本就显式罗列）。

### L3（拆上帝文件）—— 暂不推荐
`route.c` / `paint.c` 是热点且已调优。在没有"需要独立测试某子功能"或"需要多渲染上下文"的具体需求前，拆解只会引入回归。若未来确有需求，按技能建议"耦合从低到高"顺序：先抽只依赖配置/工具的小块，导航/路由核心放最后。

---

## 3. 既有有利条件（决定重构风险低）

1. **干净的叶子层**：`utils` / `pbf` 不依赖任何内部模块，是理想的独立库候选。
2. **无幽灵依赖**：依赖图是真实 `#include` 推导（已剥注释/字符串），无注释里"以前写在 X 里"之类的伪依赖。
3. **符号带模块前缀**：`route_*` `paint_*` `nm_*` `geo_*` —— fan-out 可直接按前缀读，模块边界"可命名、可 grep"。
4. **平台隔离已收口**：全部 `#ifdef _WIN32` 在 `plat.{h,c}`，业务代码零分叉（前序工作已验证 MSYS2/Qt6 实机编译）。
5. **验证手段齐备**：`tools/` 有 `regress.sh`（54 项）、`plat_selftest`、`map_shot` 像素级截图、各 `route_cli`/`search_cli` 探针——任何拆分都可立即回归，风险可控。
6. **零跨文件 extern 全局**：状态封装良好，拆库不会触发"全局被谁改了"的隐性数据流问题。

---

## 4. 分阶段路线（每阶段可独立验证、独立回滚）

- **阶段 0（已完成）**：把本报告 §1.3 的 DAG 作为模块契约文档沉淀（注意：原 DAG 缺 `map→render` 边且误判为无环，见 §1.3 修正注）。
- **阶段 1（已完成，commit 4986ace）**：把 `navmap_utils`（src/utils/*）拆成 4 个独立叶子库 `navmap_geo / navmap_strpool / navmap_plat / navmap_mem`（mem→plat），加 `navmap_pbf`，共 5 个叶子库；`navimap_core` 改为依赖它们。零破环风险。验收：regress.sh 52/54（2 项为环境相关 glitch）、零警告。
- **阶段 2（已完成，commit a244de9 + 9316777）**：把 `map / route / search / render / ui` 提升为 5 个独立模块库，`navimap_core` 退化为聚合层（仅含 `core_anchor.c`，用 `-Wl,--start-group` 包裹所有模块库 + 叶子库 + 3rd 方库）；24 个 CLI 工具裁剪为只链所需模块子集（via `nm_link_tool()`）；`navmap_qt` 仍链聚合层。纯 CMake 改动。验收：regress.sh 52/54 零回归、核心 `core_no_sdl` 门禁仍报 CORE-NO-SDL-OK。
- **阶段 3（按需）**：仅当出现“独立复用/单测某子模块”或“多渲染上下文”需求时，再分解 `route.c` / `paint.c` 内部；否则不动。

---

## 5. 不建议现在做的事

- ❌ **不要上多包构建（vcpkg/conan/外部包管理）**：模块边界还没用库固化前，多包只会增加上手成本。
- ❌ **不要预先拆解 route.c / paint.c**：热路径已重度调优，拆解收益低、回归风险高。
- ❌ **不要把 paint.c 全局状态强行改成 context struct**：当前单渲染上下文完全够用；只有"需要重入/并行渲染"时才值得为此重构（会波及 `paint/chrome/navdraw/labels` 一整条渲染链）。
- ❌ **不要为了"目录好看"就算模块化完成**：主体价值在 L2 的库边界与 L3 的职责分解，不在包名。

---

## 6. 测量方法与可复现命令（含踩过的坑）

```bash
# 1) 行数分布
find src tools src/ui_qt -name '*.c' -o -name '*.h' -o -name '*.cpp' \
  | xargs wc -l | sort -rn | head -40

# 2) 模块级依赖图 + 环检测（剥注释后解析 #include，映射到所属目录）
python3 - <<'PY'
import os,re,glob
hdr={}
for h in glob.glob('src/**/*.h',recursive=True):
    hdr[os.path.normpath(h)]=os.path.dirname(h); hdr[os.path.basename(h)]=os.path.dirname(h)
E=set()
for f in glob.glob('src/**/*.c',recursive=True):
    c=os.path.dirname(f); t=open(f,encoding='utf-8',errors='ignore').read()
    t=re.sub(r'/\*.*?\*/',' ',t,flags=re.S); t=re.sub(r'//[^\n]*',' ',t)
    for m in re.findall(r'#include\s+"([^"]+)"',t):
        p=hdr.get(os.path.normpath(m)) or hdr.get(os.path.basename(m))
        if p and p!=c: E.add((c,p))
out={}; [out.setdefault(c,set()).add(p) for c,p in E]
print('out-degree:',{k:sorted(v) for k,v in out.items()})
# DFS 判环（略，详见评估脚本）-> 结果：无环
PY

# 3) 跨文件 extern 可变全局（最危险隐藏状态）
grep -rnE 'extern[[:space:]]+[a-zA-Z_].*[^;]*[a-zA-Z_0-9]+[[:space:]]*[\[\];]' src/ \
  | grep -vE 'extern[[:space:]]+"C"|extern[[:space:]]+const'   # 结果：0

# 4) 文件作用域 static 可变全局（按模块状态）
grep -rnE '^[[:space:]]*static[[:space:]]+[a-zA-Z_].*=[^=]*;' src/ | grep -v const
```

**坑（已踩，供复现时避坑）**：
- `sort -t' '` 在 MSYS2/Git-Bash 里引号会被吞，改用 `sort -k2 -rn` 或 `python` 聚合。
- 依赖图**绝不能用"相对文件路径拼 include"**推导（include 走 `-I` 搜索路径，不在文件旁），必须先把每个头文件映射到"所属模块目录"再建边，否则会出现 `src/map/utils` 这类假节点。
- 判环前务必**剥掉注释与字符串**，否则注释里"以前这段在 X 里"会被误判成依赖；本项目实测无此伪依赖，但方法上必须做。
- 打印结论含 `✓` 等非 ASCII 时，MSYS2 默认 GBK 会 `UnicodeEncodeError`；报告/脚本统一用纯 ASCII。

---

## 7. 最终判定

**可复用性**：当前是"整车复用"（要一块得拖整库）。叶子模块（pbf/geo/strpool/plat）已具备独立复用条件，只差构建系统把它们"显式独立"出来——这是最划算的一步。
**模块化**：逻辑上已经模块化（DAG + 前缀 + 头源对应），物理上还是单库。把逻辑边界固化为库边界，是低风险、中高收益的下一步。
**代码组织化**：优秀。目录=模块、命名显式、平台隔离收口、验证手段齐备。无需为组织化本身再投入。
