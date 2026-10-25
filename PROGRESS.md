# 项目进度

> 本文件是**面向人的当前状态总览**：已完成什么、现在什么水平、接下来做什么。>   
> 详细施工过程见 [`PLAN.md`](PLAN.md)（§0–§52.8，按章节累积）；模块化路线图与阶段判据见>   
> [`ARCH_ASSESSMENT.md`](ARCH_ASSESSMENT.md)；协作约定与"别再试"清单见>   
> [`.workbuddy/memory/MEMORY.md`](.workbuddy/memory/MEMORY.md)；每日工作日志见>   
> `.workbuddy/memory/YYYY-MM-DD.md`。

**最后更新：2026-10-24**

---

## 一、总体状态

| 维度            | 状态                                                                                                           |
| ------------- | ------------------------------------------------------------------------------------------------------------ |
| **验收门禁**      | ✅ **54 用例 / 基线 52 通过 + 2 项环境 glitch** + **编译零警告**（详见第十一节）                                                    |
| **代码规模**      | **121 个源文件 / 31 460 行**（src 83 文件 22 646 行 + tools 28 文件 6 730 行 + ui_qt 10 文件 2 084 行）                      |
| **模块化**       | ✅ **12 个 STATIC 库**（4 叶子 + pbf + 5 模块 + sdl + 聚合层），链接器强制边界；`route.c` 1856→**928 行**拆 6 子模块（详见第四节）            |
| **数据规模**      | 8 省 / **5 228 万节点** / **4 072 万边** / 去重后 4 560 万唯一坐标                                                         |
| **渲染**        | ✅ 面/路/线 + 中文标注 + 跨省去重 + 按需加载                                                                                 |
| **路由**        | ✅ 双向加权 A* + ALT 地标下界 + 三类转向限制 + 跨省缝合                                                                         |
| **搜索/导航**     | ✅ 地名/POI 检索、偏航重算、ETA、转向指令                                                                                    |
| **跨平台**       | ✅ Linux / macOS / Windows：平台差异统一收进 `src/utils/plat.{h,c}`，业务代码零 `#ifdef`                                     |
| **核心去 SDL 化** | ✅ `navimap_core` 不链 SDL；SDL 仅留 `navmap_sdl`（window_sdl.c）+ `map_shot` 两个适配层；字体改走 `stb_truetype`（去掉 SDL2_ttf） |
| **未做**        | GPX、收藏、3D/地形                                                                                              |

> 渲染与算路的**实测数字**见第二节，全部来自 2026-10-22 的回归与基准测试（模块化改动未触及算法，数字沿用）。

---

## 二、当前性能水平（实测）

### 渲染（`map_shot` 第二帧，1280×800，8 省数据）

| 缩放  | 帧时间        |
| --- | ---------- |
| z9  | **116 ms** |
| z11 | **56 ms**  |
| z13 | **42 ms**  |
| z15 | **31 ms**  |

（2026-10-22 实测，每档 4 轮取最小值；各轮波动 ±1 ms）

对照起点：2026-09-22 时 z12/z14/z16 分别是 248 / 331 / 269 ms。现在靠**分阶段遍历 + 查表化 + 自写像素光栅化**解决，数据格式未动。已开 LTO（`-O2 -flto`）；`-O3`、`-march=native` 实测无额外收益。

### 算路（50 条城市对，30 地标 + w=1.25）

| 指标        | 当前                 | 无表基线               |
| --------- | ------------------ | ------------------ |
| 中位        | **123 ms**         | 196 ms             |
| p90       | **753 ms**         | 3131 ms            |
| 最慢        | **1503 ms**        | 8913 ms            |
| > 3 s 的条数 | **0**              | —                  |
| 展开数 / 里程  | 10.43 M / 21 503 分 | 23.88 M / 22 217 分 |

⇒ **快 1.6×、p90 快 4.2×，质量好 3.21%**。逐条配对实测中，30 地标 @ w=1.25 是**唯一"速度与质量同时更优"的档位**（好 25 / 差 10）。

---

## 三、已完成的功能（按模块）

| 模块         | 内容                                     | 关键实据                                                               |
| ---------- | -------------------------------------- | ------------------------------------------------------------------ |
| **PBF 解码** | 自写 zlib blob / DenseNodes / delta 编码解码 | `src/pbf/`（2 文件 ~350 行，单独成库 `navmap_pbf`）                          |
| **路网构建**   | OSM → CSR 邻接 + 转向限制 + 瓦片空间索引           | `src/map/graph_build.c`（1 260 行，模块库 `navmap_map`）                  |
| **图集管理**   | 多省加载、**按视口按需 mmap**、副本省检测              | `src/map/mapset.c`                                                 |
| **路由引擎**   | 双向加权 A*、ALT 地标下界、跨省缝合、接缝补查             | `src/route/route.c`（**928 行**，原 1856；抽 6 子模块入 `libnavmap_route.a`） |
| **转向限制**   | 允许集 `only_*` / 必经 `via=way` / 单点禁转     | 全量 3 387 条                                                         |
| **反向索引**   | 落盘 `rev.bin`（8 省 355 MB），坏了自动重建        | `src/route/revidx.c`                                               |
| **地标表**    | 30 地标下界表，460×767 格，21.2 MB             | `src/route/lmtable.c` + `lmbuild_cli`                              |
| **渲染管线**   | 面 → 路 → 线 → 标注分阶段；自写像素光栅               | `src/render/paint.c`（931 行）+ `raster.{h,c}`，模块库 `navmap_render`    |
| **中文标注**   | 字形缓存（stb_truetype）、字体可选、字号运行期可调        | `src/render/font.c` + `labels.c`                                   |
| **搜索**     | 地名/POI 分类加权 + bigram 匹配                | `src/search/search.c`                                              |
| **导航**     | 点选起终点、模拟导航、偏航重算、转向指令                   | `src/ui/window.c` + `maneuver.c`（模块库 `navmap_ui`）                  |
| **地名库**    | PBF → SQLite `places.db`，POI 已入库       | `tools/places_build.c`                                             |

---

## 四、模块化进度（阶段 1 / 2 / 3，2026-10-23→24）

> 详细分阶段判据、commit 对照、门禁列表见 [`ARCH_ASSESSMENT.md`](ARCH_ASSESSMENT.md) §4。本>   
> 节为面向人的结论版。目标：把"整车复用"的单体库拆成**链接器强制边界**的多个库，消除>   
> "要一块得拖整库 / 一个类型牵动全库重编"的耦合。

### 库结构（12 个 STATIC 库，全部经 `--start-group` 链接）

```
叶子层（零内部依赖，可独立取用）
  navmap_plat    src/utils/plat.c          ← 平台抽象唯一宿主
  navmap_geo     src/utils/geo.c
  navmap_strpool src/utils/strpool.c
  navmap_mem     src/utils/mem.c
  navmap_pbf     src/pbf/*                  ← 零内部依赖

模块层（DAG，render↔ui 有意为之的环由 start-group 消解）
  navmap_map     src/map/*
  navmap_search  src/search/*
  navmap_route   src/route/*（含抽出的 6 子模块）
  navmap_render  src/render/*
  navmap_ui      src/ui/*

适配层
  navmap_sdl     src/ui/window_sdl.c        ← SDL 仅在此 + map_shot 出现

聚合层（对外门面，方案2 架构边界）
  navimap_core   src/core_anchor.c（仅锚点符号）
     → PUBLIC 链入 上述全部模块库；24 个 exe / navmap_sdl / navmap_qt 只链它
```

### 阶段判据与落地

| 阶段       | 内容                                                                                                             | 结果                     |
| -------- | -------------------------------------------------------------------------------------------------------------- | ---------------------- |
| **阶段 1** | 提 4 叶子库（geo/strpool/plat/mem）+ pbf 为独立 STATIC；`navimap_core` 退为聚合层                                             | ✅ 零破环，consumer 全部只链聚合层 |
| **阶段 2** | 加 map/search/route/render/ui 5 模块库；`navmap_core` 仅留 `core_anchor.c`，用 `--start-group` 消解 render↔ui 环           | ✅ 链接器强制边界              |
| **阶段 3** | `route.c` 1856→**928 行**，抽出 `st/restr/cost/sew/lmdist/result` 6 子模块入 `libnavmap_route.a`；私有类型收进 `route_priv.h` | ✅ 大文件瘦身                |

### 配套门禁（防止回归）

- `core_no_sdl`：断言 `navmap_core` 不传递 SDL 依赖（负向对照已实证能 FAIL）。
- `blend_equiv` / `clip_equiv`：自写光栅与 SDL 软渲染器逐像素等价（3 个真 bug 已据此修出）。
- `font_cjk` / `font_cjk_neg`：判"字体能否画中文"查字体文件 cmap，而非 Qt 度量（offscreen 下度量全假）。
- `qt_ui_test`：Qt 前端控件交互自检（搜索按钮展开/收起、坐标显示随鼠标更新、无顶栏工具条等）。

### 余留项（有意不拆）

- `paint.c`（931 行）未拆：无重入/并行渲染需求，拆只增接口面、无收益。
- render↔ui 语义环：**保留**（有意为之 + start-group 已消解），不强行引入中性派发头。
- 不上多包构建（vcpkg/conan）：与 `ARCH_ASSESSMENT.md` §5 一致，单仓 + 链接器边界已够。

---

## 五、工具链（27 个可执行 / tools/ 28 源文件）

**验收/自检类**（把 GUI 无法无头验证的逻辑做成可断言的 CLI）：

- `map_shot` 渲染截图 + 像素级断言 ｜ `route_cli` 算路基准 ｜ `navsim_cli` 导航模拟
- `restr_ab` 转向限制**双向判定 + 负向对照** ｜ `heap_selftest` 四叉堆对拍（含 `--expect-fail`）
- `tile_selftest` 瓦片索引**全图 5 200 万节点逐点**对拍 ｜ `tile_xcheck` 跨省坐标一致性
- `graph_check` / `graph_extract` 图结构校验 ｜ `revbuild_cli` 反向索引落盘校验
- `coverage_cli` 覆盖度 ｜ `search_cli` 搜索 ｜ `places_build --selftest` POI 分类自检
- `plat_selftest` **平台抽象层契约**正/负双向验收
- `core_no_sdl` **架构边界门禁**：`navmap_core` 不得传递 SDL
- `blend_equiv` / `clip_equiv` 自写光栅 ↔ SDL 逐像素等价 ｜ `font_cjk_check` CJK 字形能力
- `qt_ui_test` Qt 前端 UI 交互自检（16 项）
- `tts_selftest` **离线语音播报**自检（TTS-OK）｜ `alts_selftest` **多方案对比**自检（ALTS-OK）｜ `lane_selftest` **车道指示**数据链路自检（LANE-OK）

**构建类**：`places_build` 地名库 ｜ `lmbuild_cli` 地标表 ｜ `pbf_info` ｜ `tag_stats` ｜ `restr_probe`  
**样板去重**：8 个 CLI 的公共参数解析/图加载收敛到 `tools/tool_common.{h,c}`（`a8f3441`）。

---

## 六、显示层按需解码（PBF 模式，2026-10-22 新增）

`--pbf-display=<sources目录>[:LRU上限]` 开启：显示层（面/线要素）改为**从原始 PBF 运行时解码**，  
不再从 nmbin 打开 `lines/areas/polyrefs`。这三个文件因此**可以整个不落盘**。

```bash
./build/navimap --graph=products/graph --pbf-display=sources:3
./build/map_shot --graph=products/graph/tianjin --pbf-display=sources --lat=39.08 --lon=117.20 --zoom=13
```

**设计要点**

- **省粒度而非瓦片粒度**：实测 PBF 的 data blob 平均覆盖全省 43% 的纬度跨度（北京 621 个 blob，    
  单个跨 0.99°/1.33°，z11 视口仍命中 53%）⇒ 按 blob 跳读收益低于索引成本，**不可行**。    
  改为"整省解一次 + LRU 缓存"。
- **不需要扫第二遍 PBF 取坐标**：`polyrefs` 存的是节点内部下标，而这个下标空间**就是** nmbin    
  `nodes.bin` 的下标空间（都按 osm id 升序、都去掉了文件里不存在的节点）⇒ 拿 `nodes.bin`    
  的 osm_id 数组做二分查表即可，省掉整遍 PBF 扫描与两份坐标数组（河北 105 MB）。
- **等价性**：解码结果与落盘文件**8 省逐字节相同**（8 省全 IDENTICAL）。渲染端零改动。
- **实测代价**：解码耗时 天津 0.6 s / 北京 1.6 s / 河北 4.4 s；峰值 RSS 104 / 275 / 714 MB。    
  ⚠️ 解码在**后台线程异步**进行（`dmap_worker`），主线程不阻塞；解码期间该省
  has_features=0、渲染自然空白，界面画"省名 解码中… NN%"占位（`paint_dmap_overlay`），
  并实时回报 `pbfmap_decode` 的字节级进度回调。
- **收益**：磁盘省 **195 MB**（8 省 lines+areas+polyrefs 合计）。    
  ⚠️ 但 nmbin 的 4.2 G 里 **94% 是路由图**（nodes/edges/edgelist/rev），    
  那部分**省不掉**（A* 需要全图），仍走 mmap。

**两种模式现已逐像素完全一致（12 组实测 0 差异）**

原先有一处 12.357% 的文字像素差异，根因不是数据而是**一个悬垂纹理指针**，已修复：

`app_render_to_surface` 每帧新建/销毁一个软件渲染器，但销毁前没有作废字形缓存。  
`glyph_get` 靠 `tex_owner != r` 判断"renderer 换了没有"，而判据是**指针相等** ——  
中间的 `SDL_DestroyRenderer` 把那块内存还回分配器，下一帧 `SDL_CreateSoftwareRenderer`  
很可能拿回**同一个地址**（实测 8 省图集上就是如此）。于是判据失效，缓存里的 `g->tex`  
成了悬垂指针，下一帧 `RenderCopy` 画出错的字。

修复就是在销毁 renderer 前无条件 `font_drop_textures()`（只在字体后端为 stb_truetype 时  
调，且**不能每帧调**——会毁掉 CJK 缓存；SDL 软渲染器后端路径已落实此约定）。

**教训**：SDL 对象销毁/重建的成对约定，必须在**每条**销毁路径上落实，尤其当判据是  
**指针相等**时，它对"地址被复用"完全失效。

---

## 七、跨平台与硬编码清理（2026-10-23）

**动机**：代码里散落一批"只在本机成立"的写法——编译期全绿，换平台才炸，且报错位置  
离写的地方很远。一次性收进平台抽象层，并配正反双向门禁。

**`src/utils/plat.{h,c}`（平台抽象层，唯一允许出现平台差异的地方）**

| 收编的对象                                     | 现在                                                                                                                |
| ----------------------------------------- | ----------------------------------------------------------------------------------------------------------------- |
| `/usr/share/fonts/...` 字体链                | `nm_font_candidates`：Linux 原链 + Windows `%WINDIR%\Fonts` + macOS `Songti/STHeiti/PingFang/Arial Unicode`          |
| `/tmp/navimap_shot.bmp`                   | `nm_temp_dir()`（TMPDIR/TEMP/TMP → /tmp → .）                                                                       |
| `mmap`/`munmap`/`madvise`、`mallopt`       | `nm_map_file`/`nm_unmap_file`（Windows: CreateFileMapping + MapViewOfFile，映射句柄按基址登记）；`mallopt` 加宏守卫，不满足整段消失（零警告门禁） |
| 目录枚举/创建/删除/stat                           | `nm_dir_*` / `nm_mkdir_p` / `nm_rmtree` / `nm_unlink` / `nm_stat_file`                                            |
| `/proc/self/exe`、`fork`+`execv`、`kill`、时钟 | `nm_exe_path` / `nm_spawn_wait` / `nm_pid_alive` / `nm_now_ms` / `nm_peak_rss_mb`                                 |
| `strcasecmp`、`strdup`、`rename` 覆盖语义       | `nm_strcasecmp` / `nm_strdup` / `nm_rename`                                                                       |
| **字体后端**                                  | 渲染文字改走 **`stb_truetype`**（`src/render/font.c`），**不再依赖 SDL2_ttf**；CJK 字形按字体文件 cmap 探测                              |

**CMake 跨平台化**：`m` 库仅类 Unix 链接；Windows 链 `psapi` 并定义 `SDL_MAIN_HANDLED` +  
`_CRT_SECURE_NO_WARNINGS`；protobuf-c 查找去掉 `NO_DEFAULT_PATH` 改为 HINTS 优先 + 默认路径兜底。  
**平台差异只许出现在 `plat.{h,c}`**（业务代码零 `#ifdef _WIN32`），由 `plat_selftest` 正反  
双向门禁钉死（负向对照已实测能 FAIL）。

---

## 八、界面元素对齐 nav-desktop（2026-10-23）

按 osmand-app/nav-desktop 的界面元素清单，11 类 chrome 全部落到 navimap（纯 C/SDL2 自绘，  
新增 `src/render/chrome.{h,c}`，绘制/命中测试/动作派发共用同一份几何）：

| 元素              | 实现                                                    |
| --------------- | ----------------------------------------------------- |
| WinCtl 自绘标题栏    | `SDL_WINDOW_BORDERLESS` + 自绘标题与三色窗口按钮；拖拽移动窗口          |
| TopBar 顶栏       | 菜单按钮（☰）+ 搜索框 + 浏览/导航 模式页签                             |
| RightCol 右栏     | 放大/缩小/定位/昼夜 四钮                                        |
| NavTop 导航顶栏     | 返回 + 目的地 + 退出                                         |
| NavBottom       | 偏航重算指示                                                |
| StepsPanel 步骤面板 | 左侧转向指令列表（maneuver 逐条）                                 |
| MainMenu 主菜单    | ☰ 下拉浮层                                                |
| NoMapCard       | 图集为空时居中提示卡片                                           |
| DayNightCtl 昼夜  | 右栏昼夜钮 + `d` 键；夜间半透明蒙版                                 |
| ScaleBar 比例尺    | 左下角，按纬度换算 m/px，1/2/5 整数档自动选                           |
| Compass 指北针     | `MapView` 新增 `rot`（投影双向旋转，`rot=0` 与旧投影逐位等价）；Ctrl+拖动旋转 |

验证：编译零警告；regress 关键数值不变；浏览/导航两态截图核对。  
已知限制：无边框窗口拖拽在 Wayland 下可能受限；夜间是蒙版近似。

---

## 九、Qt 前端 navmap_qt（2026-10-23）

SDL 自绘 chrome 在 Wayland 下呈现异常，界面层迁移 Qt：**Qt 只管窗口/部件/交互，地图渲染核心一行不动**。

- `src/ui_qt/`：`qt_main.cpp` + `main_window.cpp`（菜单栏/搜索框/页签/右栏/步骤面板/状态栏，真控件）
  - `map_canvas.cpp`（地图画布：`app_render_to_surface` 画进 `SDL_Surface` → `QImage::Format_RGB32`      
    → paintEvent blit；拖拽/滚轮/旋转/点选全部经 C 核心 API）。
- `App.qt_ui=1`：`paint_frame` 跳过 SDL 自绘 chrome/搜索框/面板/指令条（Qt 控件承担），地图、结果图钉、夜间蒙版保留。
- CMake 自动探测 **Qt Widgets（Qt6）**，找到才编 `navmap_qt`，没装 Qt 的环境不受影响。
- 验证：零警告编译；offscreen `--shot` 截图核对（菜单栏/搜索框/页签/右栏四钮/比例尺全齐，地图颜色正确）；    
  SIGTERM 退出验证通过（入口先 `SDL_SetHint(NO_SIGNAL_HANDLERS)` + `SDL_INIT_NOPARACHUTE`）。
- 右栏补齐：**坐标网按钮**（▦，纯 Qt 叠加层，C 核心零改动）+ **菜单按钮**（☰，弹出同一份 QMenu）。    
  `--grid` 可开局打开坐标网。

---

## 十、近期工作


| 时间            | 工作                | 结果                                                                     |
| ------------- | ----------------- | ---------------------------------------------------------------------- |
| 2026-10-22    | **图集按需加载**        | 运行时只 mmap 视口所在省份，滚出即卸载。北京 z11 实测：加载 3 省 / 卸载 5 省                       |
| 2026-10-22    | **PBF vs OBF 评估** | PBF 446 M vs nmbin 4.2 G，显示相关仅 ~19 M/省、94% 是路由图 ⇒ **保留 nmbin，不引入 OBF** |
| 2026-10-22    | **全面代码审查**        | 通读 24 运行时源 + 18 工具 + 45/45 回归 + 50 路 ASan/UBSan ⇒ **无待修 bug**          |
| 2026-10-22    | **建立版本管理**        | 首次纳入 git，推送到 GitHub `a-long-long/navimap`                              |
| 2026-10-23    | **跨平台清理**         | 平台差异全收 `plat.{h,c}`；`plat_selftest` 正反门禁（见第七节）                         |
| 2026-10-23    | **界面 + Qt 前端**    | 11 类 chrome 对齐 nav-desktop；`navmap_qt` 落地（见第八/九节）                      |
| 2026-10-23→24 | **模块化阶段 1/2/3**   | 12 库 + `route.c` 1856→928（见第四节），配套 5 道架构门禁                             |

---

## 十·五、导航功能补齐（2026-10-24→25）

原"未做"三项一次性补齐，均已**实测验证**：

| 功能        | 实现要点                                                                                              | 关键实据                                                              |
| --------- | ------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------- |
| **语音播报**  | 离线 TTS（跨平台、行进中按 maneuver 触发）；不依赖网络，纯本地合成                                                | `src/audio/tts.*` + `tts_selftest`（TTS-OK）                              |
| **多方案对比** | `RouteResultSet`（K=4：最快/最短/避收费/避非铺装），复用 `router_plan` + 临时切换 `r->opt`；四方案共用同一对连通端点保证可比 | `src/route/route.c` `router_route_alts` + chrome 对比面板 + `alts_selftest`（ALTS-OK） |
| **车道指示**  | 附加 side-file `lanes.bin`（每条边 u32 车道串 id，CSR 序）+ `lane_names.bin`（字符串池）；`turn:lanes` 解析区分正/反向；nav banner 上方画车道格并高亮可走车道 | `src/map/graph_build.c` `write_lanes` + `mapset_edge_lanes` + `src/render/navdraw.c` `paint_lane_arrows` + `lane_selftest`（LANE-OK） |

**车道指示设计约束**（避免破坏其余 7 省旧图）：
- 不动 `NM_EDGE_BYTES`（18）；车道数据走**附加 side-file**，旧省无该文件时 `has_lanes=0` 自动降级（只重建 beijing）。
- `turn:lanes` 正向用 `turn:lanes:forward`/generic，反向只用 `turn:lanes:backward`（不能把 generic 抄给反向，会左右颠倒）。
- 渲染：nav banner 上方一排车道格，按 `turn_dir`（0=直行 / -1=左转含掉头 / +1=右转）高亮可走车道，画左/右/直/掉头箭头。

**实测**（beijing 抽取，41 MB 裁剪 PBF）：
- 含 `turn:lanes` 的 way **245** 条 → 去重车道串 **72** 条 → 落盘 **807** 条边带车道串（`lane_selftest` 全合法、坏串 0）。
- 截图验证（`map_shot --sim --sim-sec=200`）：靠左转向时底部指令条上方出现 6 格车道行，前 2 格（left）高亮蓝、其余灰，箭头清晰。

---

## 十一、验收门禁（regress.sh）

- **54 个用例**；本机基线 **52 通过 / 2 失败**。两项失败是**环境 glitch**，非代码回归：
  1. `plat_selftest` 字体偏好（本机无期望的中文字体链）；
  2. `core_no_sdl_sdl` 找不到 `build/navimap` 给 `objdump` 提取依赖。       
     ⇒ **见到 52/54 即视为无回归**，与历史记录一致。
- **编译零警告**为硬门禁（`-Wall -Wextra` + 特定 `-Wno-*` 白名单，零警告 FAIL）。
- 布尔判据用**确定性量**（展开数 / 里程 / 像素差），不用墙钟时间；A/B 至少 4 轮；探针量完撤干净。

```bash
source env.sh                  # 修正 PKG_CONFIG_PATH / LD_LIBRARY_PATH
./scripts/regress.sh --build   # 构建 + 54 项回归 + 零警告门禁
./scripts/sync_maps.sh         # 重建 products/ 产物
```

---

## 十二、接下来的候选（按性价比排序）

> ✅ **PBF 后台解码优化（已完成，2026-10-25）**：解码本就在后台线程异步进行
> （`dmap_worker`，`3f6fb3a`），本次补齐 **进度提示**——`pbfmap_decode` 加字节级进度
> 回调，渲染端在解码中的省份画"省名 解码中… NN%"占位药丸（`paint_dmap_overlay`），
> 后台线程预热（`mapset_enable_pbf_display` 即起 worker）。`dmap_selftest` 验证
> set_view 不阻塞主线程（max 0.0 ms）、进度爬升 0→100%、解码完成；
> `pbfmap_check` 仍 IDENTICAL。
> 另：`--dmap-demo=<省>:<pct>` 可强制某省进入"解码中"占位态，用于截图验证。

1. **补 POI 图标与更多显示要素**（建筑 / 等高线 / 土地利用着色）——从 PBF 侧补，不换数据源。
2. **GPX 轨迹导入导出 / 收藏夹**——现有导航缺落地的轨迹与收藏管理。

> ⚠️ 改全局行为（权重 w、地标数、启发式）前必读 `.workbuddy/memory/MEMORY.md` 的>   
> "方法论"与"别再试"清单：手挑样本下结论已翻车 4 次，改地标数必须连带重定 w。
