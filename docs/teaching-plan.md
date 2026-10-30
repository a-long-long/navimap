# NavMap 项目教案（循序渐进版）

> 适用对象：有一定 C 语言基础、想通过**一个真实工程**理解"离线地图导航"全栈实现的学习者。
> 教学主线：先让程序**跑起来** → 再读懂**每一层在做什么** → 最后能**改得动、测得准**。
> 全部内容都锚定在本仓库当前真实代码（`src/`、`tools/`、`scripts/`），不是泛泛而谈。

---

## 〇、教学总览

### 0.1 这个工程是什么

一个**纯 C（C11）+ Qt5/Qt6 前端**的离线地图导航程序：

- 输入：OSM 的 `.osm.pbf` 原始数据（8 省约 456 MB）。
- 产出：可缩放平移的地图渲染、跨省路线规划、地名搜索、模拟导航与语音播报。
- 特点：**自写软件光栅**（不依赖 SDL 软渲染）、**坐标键接边**（跨省无需映射表）、**Landmark/ALT 加速**的双向加权 A*（不是 CH）、**纯 DAG 模块分层**、**全量可无头自测**。

### 0.2 教学原则（本项目最重要的 4 条，先讲给学生听）

1. **能实测就不要估算**——性能瓶颈、算法正确性一律靠 `scripts/regress.sh`、像素判据、确定性量说话，不靠"我觉得"。
2. **核心与窗口系统解耦**——C 渲染核心一行不动，Qt 只把像素缓冲包成 `QImage`。这就是为什么能"无头出图 + GUI 两套前端共用同一渲染入口"。
3. **依赖是纯 DAG**——`utils ← pbf ← map ← route ← render ← ui ← ui_qt`，反向依赖会立刻 `undefined-reference`。`scripts/check_layers.sh` 是守护门禁。
4. **核心零 SDL**——`navimap_core` 连 SDL 的 include 都不给；SDL 仅残留在两个去 SDL 化对照体（`blend_equiv`/`clip_equiv`）里做"逐像素等价"验证。

### 0.3 课程表（建议 15 篇：第 0 课总览 → 第 14 课综合项目，编号与 `tutorial/00–14` 一一对应）

> 自本版起课程统一编号为 **00–14**：第 0 课是总览（读源码讲义 `docs/teaching/00_overview.md`，与可运行小程序 `tutorial/00_minimal` 配套）；
> 01–13 为主题课；14 为综合项目。每课都有两份**同号**资料——「读真实源码讲义 `docs/teaching/NN_*.md`」与「可独立编译运行的小程序 `tutorial/NN_*/`」。

| 课次 | 主题 | 对应模块 | 关键产出 | 配套资料 |
|---|---|---|---|---|
| 0 | 课程总览与程序运行流程 | 全局 | 程序运行流程图 | `docs/teaching/00_overview.md` + `tutorial/00_minimal` |
| 1 | 世界观与坐标系 | `utils/geo`, `utils/datum` | 经纬度量化、投影、测距 | `docs/teaching/01_coords.md` + `tutorial/01_coords` |
| 2 | 平台抽象层 | `utils/{plat,mem,strpool}` | 跨平台不写 `#ifdef` | `docs/teaching/02_platform.md` + `tutorial/02_platform` |
| 3 | PBF 流式解析 | `pbf/*` | 解出 node/way/relation | `docs/teaching/03_pbf.md` + `tutorial/03_pbf` |
| 4 | 自描述二进制与图集 | `map/{binfmt,mapset,namepool}` | 读 `*.nmbin` | `docs/teaching/04_binary.md` + `tutorial/04_binary` |
| 5 | 建图：PBF→路网 | `map/{graph_build,road_tags}` | 生成 nodes/edges | `docs/teaching/05_graph.md` + `tutorial/05_graph` |
| 6 | 视口、投影与瓦片键 | `map/{map_view,tileidx}` | 经纬度↔屏幕 | `docs/teaching/06_viewport.md` + `tutorial/06_viewport` |
| 7 | 软件光栅与性能旋钮 | `render/{raster,paint,navdraw}` | `AREA_DOT_PX` 权衡 | `docs/teaching/07_raster.md` + `tutorial/07_raster` |
| 8 | 渲染管线编排 | `ui/{window,chrome,offscreen_shot,audit}` | `paint_frame` 全貌 | `docs/teaching/08_pipeline.md` + `tutorial/08_pipeline` |
| 9 | 地名搜索 | `search/*` | bigram 倒排 | `docs/teaching/09_search.md` + `tutorial/09_search` |
| 10 | 路由算法 | `route/*` | 双向加权 A* | `docs/teaching/10_routing.md` + `tutorial/10_routing` |
| 11 | 导航层与语音 | `route/{maneuver,navsim,voice}`, `ui/voice_wire` | 转向指令+播报 | `docs/teaching/11_navvoice.md` + `tutorial/11_navvoice` |
| 12 | Qt 前端与边界 | `ui_qt/*` | 渐进式 Qt 化 | `docs/teaching/12_qt.md` + `tutorial/12_qt` |
| 13 | 架构纪律与验收门禁 | `scripts/*` | 防回归 | `docs/teaching/13_gates.md` + `tutorial/13_gates` |
| 14 | 综合项目 | 全程 | 改一个点 + 加一条判据 | `docs/teaching/14_capstone.md` + `tutorial/14_capstone` |

---

## 一、环境搭建（第 0 课前准备）

### 1.1 必做：固化本机路径

```bash
source env.sh        # 固化 PKG_CONFIG_PATH / LD_LIBRARY_PATH / PATH，指向 ~/.local 下的 protoc-c 等
```

> 为什么必须 source：本机 `libprotobuf-c` 头在 `~/.local/usr/include`、库在 `~/.local/usr/lib/<arch>`，`pkg-config` 给的 `Cflags` 是错的（见 `CMakeLists.txt:51-72`）。不 source 会 FATAL。

### 1.2 构建（本机 Linux / aarch64 / Qt5 已装）

```bash
source env.sh
cmake -S . -B build_rel -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH=$HOME/.local/qt5-dev/usr \
      -DQt5_DIR=$HOME/.local/qt5-dev/usr/lib/aarch64-linux-gnu/cmake/Qt5
cmake --build build_rel -j$(nproc)
```

> 注意：`scripts/build_release.sh` 是 **Windows/MSYS2 专用**，本机不要跑（见工作记忆）。本机 Release 走上面这条原生 CMake 命令。

### 1.3 跑起来（GUI 与无头两条路）

```bash
# GUI（Wayland 会话里直接跑）
./build_rel/navmap_qt --graph=products/graph/beijing

# 无头出图（CI/自测用，零 SDL，纯内存缓冲 + 内建 24 位 BMP）
./build_rel/navmap_qt --shot-offscreen --graph=products/graph/beijing \
    --lat=39.9 --lon=116.4 --zoom=11 --w=1280 --h=800 --out=shot.bmp

# 算一条路线（CLI，可断言）
./build_rel/route_cli --graph=products/graph/beijing \
    --from=39.90,116.40 --to=39.99,116.30 --geojson=route.json
```

**课堂演示**：让学生各自跑出一张 `shot.bmp`，用 `scripts/bmp2png.py` 转成 png 看一眼，确认"地图真的画出来了"。

### 1.4 验收门禁（先把这套跑通，后面每课都用）

```bash
./scripts/regress.sh        # 全量回归；基线 82 通过 / 3 失败（3 项为环境缺失非回归）
./scripts/check_layers.sh   # 分层门禁：任何反向 #include 都会打印文件行号并退出非 0
```

---

## 二、分课教案

---

### 第 1 课 · 世界观与坐标系

**课时目标**：理解"地球坐标 → 屏幕像素"这条主线的两端，以及本项目为什么坚持 WGS84 内部坐标。

**前置知识**：经纬度、墨卡托投影、大地基准（datum）的直觉。

**必读代码**
- `src/utils/geo.h` / `geo.c`：`nm_lat_to_i32`/`nm_i32_to_deg`（坐标量化成 1e-7 度 int32）、`nm_haversine_m`（真实测距）、`nm_mercator`/`nm_mercator_y_fast`（每帧几十万次的投影，**快版故意不逐位等于精确版**，换 5~6× 速度）。
- `src/utils/datum.h` / `datum.c`：`DatumId`（WGS84/CGCS2000/BJ54/XA80）、`datum_wgs84_to`、`datum_gk_forward/inverse`（高斯方里格）、`DatumSevenParam`（Bursa-Wolf 七参数）。

**核心讲解**
1. **内部几何永远是 WGS84**：坐标系只影响"显示读数"（经纬网、右下角坐标、方里格），**不重投影底图**。这是坐标系教学的关键不变量。
2. **测距一律 haversine**，Web Mercator 只用于显示，不能拿来做距离（会随纬度失真）。
3. **精确版 vs 快版投影**：`nm_mercator_y_fast` 用查表+插值，与 `nm_mercator_inv` 不互逆——讲"精度 vs 性能"取舍的绝佳案例。

**课堂演示**
```bash
./build_rel/datum_selftest        # 坐标系枚举/往返/位移合理性 + 负向对照
```

**练习**：用 `nm_haversine_m` 和 `nm_fast_dist_m` 分别算北京→上海距离，对比误差与速度。

**验收**：能说清"为什么内部用 WGS84、为什么投影要分快慢两版"。

---

### 第 2 课 · 平台抽象层（utils）

**课时目标**：理解"平台差异 100% 收口到 `plat.c`"这件事为什么重要。

**必读代码**
- `src/utils/plat.h` / `plat.c`：`NmMutex`/`NmCond`/`NmProc`/`NmDir`、`nm_thread_spawn`、`nm_now_ms/us`、`nm_tts_speak`、`nm_font_candidates`（中文字体候选链）。
- `src/utils/mem.h` / `mem.c`：`nm_mmap_file`（钉死 glibc `mmap_threshold` 避免主堆崩溃）。
- `src/utils/strpool.h`：`StrPool` + `sp_intern/sp_get`（路名去重池，id 0 = "无名"）。

**核心讲解**
1. **业务代码零 `#ifdef _WIN32`**：所有"换个平台就不成立"的调用都只在 `plat.c` 里出现。这是**可移植性 / 依赖倒置**的范本。
2. **链接纪律**：`plat.c` 的线程用 pthread 实现，故 `-pthread` 由 `navmap_plat` 库统一承载（CMakeLists.txt:213-222）。漏挂会在链接 `tts_selftest` 时报 `pthread_create` 未定义。

**课堂演示**
```bash
./build_rel/plat_selftest        # 平台层全接口自验收（正反双向）
```

**练习**：在 `plat.c` 里加一个 `nm_sleep_ms`（用 `usleep`/`Sleep` 适配），并在一个 selftest 里调用它。

**验收**：能在不碰任何业务 `.c` 的前提下，新增一个跨平台小工具函数。

---

### 第 3 课 · PBF 流式解析

**课时目标**：读懂 OSM PBF 的块结构，并避开最经典的坑——stringtable 不是 NUL 结尾。

**必读代码**
- `src/pbf/pbf_stream.h` / `pbf_stream.c`：`pbf_stream_open/next/close`；`pbf_stream_next` 返回 `type`（`"OSMHeader"`/`"OSMData"`）、解压缓冲、`PBF_DONE/PBF_ERR`，上限 `PBF_MAX_BLOB_BYTES=64MB`。
- `src/pbf/pbf_block.h` / `pbf_block.c`：`pbf_unpack_block` + `PbfBlock`；`pbf_str(sid)` 返回 NUL 结尾 C 串。

**核心讲解**
1. **块结构**：4 字节大端长度 + `BlobHeader` + `Blob`（内部 zlib 压缩，少数 lz4）→ `HeaderBlock` / `PrimitiveBlock`。
2. **⚠️ stringtable 坑**：protobuf-c 的 `ProtobufCBinaryData` **不保证 `\0`**。直接拿 `data` 当 C 串，`strcmp` 会把 `"path"` 当作 `"path\x7f"`，highway 分类整类落空。`pbf_unpack_block` 因此把所有 stringtable 拷成一份 NUL 结尾副本——这是"序列化数据 ≠ C 字符串"的反面教材。
3. **delta 编码**：`DenseNodes` 的 id/lat/lon 都是 delta 编码，必须累加还原。

**课堂演示**
```bash
./build_rel/pbf_info sources/beijing.osm.pbf     # 看 block 结构、way 数、node 数
```

**练习**：用 `pbf_info` 对比 `bbbike.pbf`（截断文件）与正常文件，确认截断文件**报错退出而非崩溃**。

**验收**：能解释为什么 `pbf_str` 不能直接返回 protobuf-c 的原始 `data` 指针。

---

### 第 4 课 · 自描述二进制与图集

**课时目标**：理解"为什么不用 8 字节头+定长记录，而要做自描述格式"，以及多省图集如何按需加载。

**必读代码**
- `src/map/binfmt.h` / `binfmt.c`：`NMBinFile`（`data/hdr/mapped`）、`NMBinHeader`/`NMBinField`——**字段名/偏移/类型/宽度写进文件头**；落盘统一小端、定长整型（避免 aarch64 跨编译器布局漂移）。`NMBIN_MAGIC="NAVMBIN\x01"`，`NMBIN_VERSION=3`。
- `src/map/mapset.h` / `mapset.c`：`MapSrc`/`MapSet`，每省一组 `nodes/edges/restrictions/lines/areas/polyrefs/tiles/...`；含 `bbox`、`dup_of`（被完全包含的副本省，如北京⊂河北 100%）、后台解码线程（`dmap_mtx`/`job_prov`/`job_done`）。
- `src/map/namepool.h` / `namepool.c`：每省 `names.bin` 解析结果。

**核心讲解**
1. **self-describing 的设计哲学**：上一版"8 字节头+定长记录"的字段语义随源码丢失，换新机器/新编译器就读不出来。自描述头部让"丢掉源码也能读"。
2. **副本省与去重**：`dup_of` 在路由与渲染都跳过；跨省重复靠 `DedupSet`（64 位指纹 + 世代）判定。
3. **模块归属**：`namepool`/`coordkey` 已下沉到 map 层（原在 render），消除 `map→render` 倒置。

**课堂演示**
```bash
./build_rel/dmap_selftest            # 后台异步解码：set_view 不阻塞 + 进度回调
./build_rel/disp_tile_selftest
```

**练习**：用 `xxd` 看一个 `.nmbin` 文件头的 magic 与字段表，手动解析出第一个字段的偏移。

**验收**：能说清"自描述二进制 vs 定长记录"各自的取舍，以及为什么本项目选前者。

---

### 第 5 课 · 建图：PBF → 路网

**课时目标**：理解从原始 OSM 要素到可导航图（CSR 边 + 限制）的抽取过程。

**必读代码**
- `src/map/road_tags.h` / `road_tags.c`：`RoadTags`（`road_class`/`drivable`/`oneway`/`speed_kmh`/`lanes`/`turn_lanes_*`）+ 边标志位 `EF_REVERSE/EF_ROUNDABOUT/EF_TOLL/EF_FERRY/EF_NO_ACCESS…`）；`road_tags_parse` 解析 way 的 keys/vals。
- `src/map/graph_build.h` / `graph_build.c`：**两遍扫描**——挑可通行 way → 节点下标映射 → 建 CSR 边 + 限制。

**核心讲解**
1. **最小导航集**：只抽 `highway=*`、`railway`、`waterway/natural`（底图）、`boundary`、`name`、通行标志等。
2. **CSR 存储**：邻接表用压缩稀疏行（offset + edge list），适合 A* 的高效遍历。
3. **限制（restriction）**：`relation` 里的 `type=restriction` 在建图阶段就抽成 `restrictions.bin`。

**课堂演示**
```bash
./build_rel/graph_extract --help      # 看抽取参数（被 autobuild 调）
./build_rel/graph_check               # 校验抽取结果
./build_rel/tag_stats sources/beijing.osm.pbf   # highway 取值统计
```

**练习**：挑一个 `highway=*` 取值（如 `tertiary`），在 `road_tags.c` 里追它如何变成 `road_class`。

**验收**：能描述"给一个 way，建图后它变成了图里的什么结构"。

---

### 第 6 课 · 视口、投影与瓦片键

**课时目标**：打通"地理坐标 ↔ 屏幕像素"的转换，理解视口驱动按需加载与瓦片空间索引。

**必读代码**
- `src/map/map_view.h` / `map_view.c`：`MapView`（`lat/lon/zoom/w/h/rot`）+ **投影缓存** `cx,cy,scale` 与惰性失效键 `c_lat/c_lon/c_zoom/c_rot/c_w/c_h`）；`mv_project`/`mv_unproj`/`mv_fit_bbox`、`mv_proj_begin`/`mv_proj`（批量投影，与逐点等价但省缓存键比对）。
- `src/map/tileidx.h` / `tileidx.c`：瓦片键三版本——`nm_tile_of`（查表+边界回退）/`nm_tile_of_raw`（纯查表，仅供自检证"边界回退必要"）/`nm_tile_of_exact`（sin+log 参考）。

**核心讲解**
1. **投影缓存的惰性失效**：只有视口参数真正变了才重算 `cx,cy,scale`，否则直接复用——避免每帧重投影。
2. **瓦片键三版本**：`lround` 会掩盖 ULP 差异，所以 `tile_selftest` 对 5228 万真实节点**逐点**比对而非抽样。

**课堂演示**
```bash
./build_rel/tile_selftest       # 查表版 vs 精确版 逐点等价
./build_rel/tile_xcheck         # 跨省接边验证
```

**练习**：给定两个经纬度，手算 Z14 瓦片坐标，再用 `nm_tile_of` 验证。

**验收**：能写出"经纬度 → 屏幕像素"的调用链，并说明投影缓存为什么能省时间。

---

### 第 7 课 · 软件光栅与性能旋钮

**课时目标**：理解自写光栅的动机，以及"画质 ↔ 帧时间"如何用确定性量来权衡。

**必读代码**
- `src/render/raster.h` / `raster.c`：`Raster`（`px/w/h/stride/clip`）；`raster_bind_mem`、`raster_line`（逐字移植 `SDL_RenderDrawLine` + Cohen-Sutherland `raster_clip_line`，64 位中间量防溢出）、`raster_fill_rect`/`raster_px_rgba`（带 alpha）。
- `src/render/paint.h`：`#define AREA_DOT_PX 4`（面 LOD 旋钮）；`PaintStats`（`ms_bg/ms_areas/ms_roads/...` + `areas_drawn/areas_dotted/...`）。
- `src/render/paint.c`：`paint_areas`/`paint_roads`/`paint_lines`；`paint_thick_line`。
- `src/render/navdraw.c`：`paint_route`/`paint_nav_car`/`paint_nav_banner`（`NAV_ROUTE_*` 蓝芯白边配色）。

**核心讲解**
1. **为什么自写光栅**：SDL 软渲染每次 `DrawLine` 约 65ns 固定税，z9 一帧 91 万次 ≈ 60ms（占 39%）；自写后这笔税消失，且与 SDL **逐像素等价**是硬要求（`raster_clip_line` 逐字移植，`tools/clip_equiv.c` 用 20 万随机线段门禁）。
2. **★ `AREA_DOT_PX` 旋钮（定档 4）**：面外接框小于阈值走"只点一个像素"快速路径。档位 4 时 `z9 −21%`、净损失面积 3.46%——这是"画质↔帧时间"权衡的**实证范例**。
3. **线要素 bbox 缓存**：线无瓦片索引，每帧扫 10 万条线、每条最多 4096 点全投影，占 z14 一帧 75%；`line_bbox` 懒填缓存消除它。

**课堂演示**
```bash
# 改 LOD 档位看效果（改 src/render/paint.h 的 AREA_DOT_PX 后重编）
./build_rel/navmap_qt --shot-offscreen --graph=products/graph/beijing \
    --lat=39.9 --lon=116.4 --zoom=9 --w=1280 --h=800 --out=z9_a.bmp
python3 scripts/bmpdiff.py z9_a.bmp z9_b.bmp    # 看丢了多少要素（要素色→背景色=丢要素）
```

**练习**：把 `AREA_DOT_PX` 分别改成 2 / 4 / 8，各出一张图，用 `scripts/bmpdiff.py` 量化"净损失面积"（变背景色像素 − 从背景色变出像素，正数=丢要素）。

**验收**：能用**确定性量**（`areas_dotted`）而非时间来解释 LOD 档位的影响——因为时间会被机器负载放大一个数量级。

---

### 第 8 课 · 渲染管线编排

**课时目标**：看懂一帧是怎么从"图集+视口"变成"像素缓冲"的，以及 Qt 与离屏两套前端如何共用同一入口。

**必读代码**
- `src/ui/window.h` / `window.c`：`App` 状态机、`paint_frame`（`window.c:1169`，渲染编排）、`app_render_to_buf`（`window.c:1435`，绑定 `Raster` 后调 `paint_frame`）。
- `src/ui/offscreen_shot.c` / `offscreen_shot.h`：`offscreen_shot_main`（`navmap_qt --shot-offscreen` 单一可信源；纯内存缓冲 `0x00RRGGBB` + 内建 24 位 BMP 写出，零 SDL）。
- `src/ui/chrome.c` / `chrome.h`：`ChromeID`/`paint_chrome`/`chrome_hit`/`chrome_act`（自绘标题栏/顶栏/右栏/菜单/面板）。
- `src/ui/audit.c` / `audit.h`：`app_offscreen_audit`（像素计数 + 断言共享模块）。

**核心讲解**
1. **`paint_frame` 的 9 步**：背景 → 视口 bbox → `mapset_set_view`（进省加载/离省卸载）→ 分阶段跨省统一层级（先所有省的面、再所有省的路）→ 标注 → 叠加层（GPX/路线/车头）→ 昼夜蒙版 → 面板/chrome。
2. **★ `app_render_to_buf` 是唯一渲染入口**：Qt 把缓冲包成 `QImage::Format_RGB32`，离屏通道额外 `write_bmp24` + `app_offscreen_audit`。C 核心一行不动。
3. **分阶段计时**：`TICK(ms_bg)` 等是定位帧时间瓶颈的唯一手段——没有它就会去优化"投影贵"这种猜错的方向。

**课堂演示**
```bash
./build_rel/navmap_qt --shot-offscreen --graph=products/graph/beijing \
    --lat=39.9 --lon=116.4 --zoom=11 --w=1280 --h=800 --out=frame.bmp
# 看 stdout 里 ms_bg/ms_areas/ms_roads/ms_lines/ms_labels 的分阶段耗时
```

**练习**：在 `paint_frame` 里新加一个 `TICK(ms_mystage)`，包住某段代码，看耗时出现在哪。

**验收**：能口述一帧的渲染顺序，并说明"为什么 Qt 前端和离屏共用 `app_render_to_buf`"。

---

### 第 9 课 · 地名搜索

**课时目标**：理解用 bigram 倒排索引替代 `LIKE '%x%'` 全表扫描的经典索引设计。

**必读代码**
- `src/search/search.h` / `search.c`：`SearchHit`（`name/lat/lon/kind/weight/dist_m/score`）；`search_open`/`search_query`/`search_bigrams`。

**核心讲解**
1. **bigram 切分**：按**字符**（不是字节）切成 `中关`/`关村` → 查 `gram` 表取交集 `place_id` → 逐条取详情 → **再用子串复核**（bigram 全中不等于连续）。
2. **precision/recall 权衡**：子串复核这一步是必须的，否则会召回"中"和"关村"恰好相邻但非连续串的误命中。
3. **数据来源**：直接吃现成 `places_auto.db`（SQLite），用 bigram 倒排索引。

**课堂演示**
```bash
./build_rel/search_cli --db=products/places_auto.db --q=中关村
./build_rel/places_build --help      # 看地名库怎么生成
```

**练习**：把"中关村"换成单字查询，观察它如何退化到 `gram` 前缀 + 全表 `LIKE`。

**验收**：能解释"为什么 bigram + 子串复核"比纯 `LIKE` 快且准。

---

### 第 10 课 · 路由算法（核心）

**课时目标**：读懂跨省双向加权 A* 的全过程，以及坐标键接边、Landmark 下界、转向限制的配合。

**必读代码**
- `src/route/route.h` / `route.c`：`RouteResult`、`RouteOpts`（**必须指名初始化**——位置初始化插字段会静默错位）、`router_route_ll`/`router_plan`/`plan_search`（**双向加权 A\* 主循环**）。
- `src/route/st.h` / `st.c`：`StEnt`/`StateTab`——开地址哈希，**状态主键 = 坐标 `coord_key_e7`**（不是 (省,下标)），前驱存 `prev_idx`（ents 下标，省一次冷哈希）。
- `src/route/revidx.h` / `revidx.c`：`RevIdx`（每省反向入边 CSR）。
- `src/route/lmtable.h` / `lmtable.c`：`LmTable`（`LM_MAGIC="LMAP"`，含图指纹 nprov/total_nodes）；`lm_dijkstra` 填表。
- `src/route/restr.h` / `restr.c`：`restr_blocked`/`restr_blocked2`（kind 0/1/2 三类，含 via=way 两段）。
- `src/route/sew.h` / `sew.c`：`sew_get`/`sew_build`（瓦片缝合缓存）。
- `src/route/cost.h` / `cost.c`：`edge_cost`（缺速用 `fallback_speed`）。

**核心讲解**
1. **★ 坐标即主键 `coord_key_e7`**：学 OsmAnd `id=(x31<<31)|y31`，跨省接边不需要"本省→全局"映射表。初版用 (省,节点) 当主键让北京→天津展开 111 万、抖 560 次，换键后消失。这是**状态空间设计**的范例。
2. **加权 A\* 的 w 是旋钮**：默认 `ROUTE_HEUR_W_DEFAULT=1.4`（京→郑 1076→88ms，代价 +2.8%）；有 lm.bin 时兜底 `1.25`（h 变紧，有效贪心度变了）；自适应两遍：第 1 遍基准 w 撞 `adapt_try=200000` 才换更重的 `1.8` 重跑。**按概率分布调参，而非手挑路线**。
3. **没有 CH，而是 Landmark（ALT）加速**：`lm.bin` 提供下界——**但它不是严格下界**（50 条里 2 条略次优），所以开 `--exact` 不再保证最优；验收对真值用 `NAVIMAP_LM=0`。纠正"CH/contraction"的常见误解。
4. **终止条件**：`lb = exact ? max(ff,fb) : (ff+fb)`，≥`best` 即停。
5. **关键数据文件**：路由图 = `nodes.bin` + `edges.bin`（均 NMBIN 自描述）；加速文件 = `rev.bin`（每省反向入边）+ `lm.bin`（整图一份，带图指纹）。

**课堂演示**
```bash
./build_rel/route_cli --graph=products/graph/beijing \
    --from=39.90,116.40 --to=39.99,116.30 --geojson=route.json
./build_rel/heap_selftest --expect-fail     # 优先队列负向对照
./build_rel/revbuild_cli --verify           # rev.bin 逐元素自验收
./build_rel/lmbuild_cli --help
./build_rel/restr_ab                         # via=way 两段限制 A/B 对照
```

**练习**：把 `ROUTE_HEUR_W_DEFAULT` 改成 1.0（纯 Dijkstra）和 2.0，对比展开数与耗时（用 `route_cli` 的 `expanded`/`elapsed_ms` 输出）。

**验收**：能画出"双向 A* 一次展开"的数据流（取堆顶 → `members_of` → 松弛出边 → 查转向限制 → 相遇判定）。

---

### 第 11 课 · 导航层与语音

**课时目标**：理解从"一条路线"到"可播报的导航"的最后一公里。

**必读代码**
- `src/route/maneuver.h` / `maneuver.c`：`Maneuver`/`ManeuverList`——把 `RouteResult` 转成"左转/右转/到达"指令序列。
- `src/route/navsim.h` / `navsim.c`：`NavSim`——模拟定位 / 偏航 / 剩余 ETA，`navsim_start/advance/locate`。
- `src/route/voice.h` / `voice.c`：`VoiceRouter`/`VoiceFrame`——**只决定"说什么"**（纯逻辑）。
- `src/ui/voice_wire.h` / `voice_wire.c`：`nav_voice_frame`——**接线层**（取状态→编排→`nm_tts_speak`）。生产与 `tools/voice_e2e.c` 共用，是唯一裁决点。

**核心讲解**
1. **分层**：`voice.c` 只定"说什么"，发声在 `plat.c` 的 `nm_tts_speak`；接线**只有一份**在 `voice_wire.c`。接线若长在 `App` 里，无头自测够不着只能复刻 ⇒ 判据必假绿。
2. **顺序耦合**：到达那一刻既要置 `sim_paused=1` 又要播收尾语 ⇒ ①先播报后置位；②收尾语单独判、不受 paused 挡（否则永远播不出来，实测真 bug）。
3. **冷却**：官方 `say()` 是**所有**播报唯一出口，15 秒同文本冷却对限速/到达/告警/转向一视同仁。

**课堂演示**
```bash
./build_rel/navsim_cli --help              # 模拟定位/偏航/ETA
./build_rel/voice_selftest                 # 播报策略（话术/提前量/节奏）
NAVIMAP_TTS_DRYRUN=/tmp/tts.txt ./build_rel/voice_e2e   # 端到端：直接断言真实发声文本
./scripts/voice_neg.sh                     # 负向对照
```

**练习**：在 `voice.c` 里加一条新播报规则（如"前方 200 米有收费站"），然后用 `voice_e2e` + `NAVIMAP_TTS_DRYRUN` 验证它真的被说出。

**验收**：能解释"为什么 `voice_wire.c` 是唯一接线点、为什么收尾语要单独判"。

---

### 第 12 课 · Qt 前端与边界原则

**课时目标**：理解"遗留 C 系统如何渐进式 Qt 化"——核心一行不动，只换缓冲归属与叠加层。

**必读代码**
- `src/ui_qt/qt_main.cpp`：**唯一的 GUI 进程入口**（`int main` 在 `qt_main.cpp:68`）。
- `src/ui_qt/map_canvas.h` / `map_canvas.cpp`：`MapCanvas`（`tick()`/`renderNow()`/`paintEvent`）；`m_px` 自持 `uint32_t` 缓冲，`app_render_to_buf` 画进它再包成 `QImage::Format_RGB32`（**踩过坑：RGBA8888→黑屏、RGBX8888→红蓝互换，用 RGB32 最稳**）；`drawScaleBar/drawCompass/drawGraticule/cursorCoordsText`（坐标网叠加层）。
- `src/ui_qt/main_window.cpp` / `download_dialog.cpp`：主窗口、下载对话框。
- `src/ui_qt/qt_icons.cpp` / `qt_icon_button.hpp`：图标自绘（字体字符 `inFont()` 全 false ⇒ 自绘路径）、`IconButton`（自绘底板按系数算 alpha）。

**核心讲解**
1. **边界原则**：C 渲染核心一行不动，只换"缓冲归属 + 叠加层由 QPainter 画"。`qt_ui=1` 时 `paint_frame` 跳过 C 自绘 chrome/搜索框/面板（由 Qt 真控件承担），但地图渲染保留。
2. **浮层透明度语义**：只淡底板、文字/图标保持清晰（`IconButton` 自绘底板按 pct 算 alpha；`QGraphicsOpacityEffect` 已否——它会把文字一起淡掉）。

**课堂演示**
```bash
./build_rel/navmap_qt --graph=products/graph/beijing
# 在 GUI 里缩放/平移/搜索/起终点导航，肉眼确认交互
```

**练习**：在 `map_canvas.cpp` 的 `paintEvent` 里加一行 `QPainter` 叠加（如在角落画个 FPS 文本），确认不影响 C 核心。

**验收**：能说清"Qt 前端改了什么、C 核心为什么完全没动"。

---

### 第 13 课 · 架构纪律与验收门禁

**课时目标**：理解"为什么这个工程敢改"——因为有一整套防回归门禁。

**必读代码 / 脚本**
- `scripts/check_layers.sh`：解析**真实** `#include`（剥块注释与行注释——本项目注释里常出现 include 路径文本），比对允许方向表，违规打印**文件行号**并退出非 0。跨模块改动前先跑它。
- `scripts/regress.sh`：用 `ASSERT-OK/ASSERT-FAIL` 标记双判据；含 `layer_dirs`（分层）、`area_lod_tier`（LOD 确定性量）、`shot/app_route/gpx_render/ovl_shot`（全走 `--shot-offscreen`）。
- `tools/core_no_sdl.c`：核心零 SDL 正反双向断言。
- `tools/blend_equiv.c` / `tools/clip_equiv.c`：自写光栅 vs SDL 软渲染逐像素等价对照体（SDL 在这里合法）。
- `scripts/bmpdiff.py`：逐字节比对两张 BMP（要素色→背景色=丢要素）。
- `scripts/ovl_px.py`：半透明比率判定（`|M−H| / |O−H| == pct/100`）。

**核心讲解**
1. **分层门禁是第二道防线**：链接器单趟解析（不用 `--start-group`）是第一道，`-Wl,--start-group` 已移除——它会让链接器多轮扫描悄悄补上反向依赖，边界形同虚设。
2. **确定性量 > 时间**：时间判据 flaky（机器负载放大一个数量级），所以用 `areas_dotted`、像素计数这类确定性量守 LOD。
3. **负向对照必须能失败**：任何判据都要注入一个"必失败"的改动来验证它真的在起作用（如 `heap_selftest --expect-fail`、`voice_neg.sh`）。

**课堂演示**
```bash
./scripts/check_layers.sh     # 故意在一个 .c 里加 #include "ui/window.h" 到 map 模块，看它 FAIL
./scripts/regress.sh          # 全量回归，看基线
```

**练习**：在 `src/render/paint.h` 里把 `AREA_DOT_PX` 改到 8，跑 `regress.sh` 的 `area_lod_tier`，确认它被抓出来；再改回 4。

**验收**：能独立跑全套门禁，并解释"为什么判据要用确定性量而非时间"。

---

## 三、综合项目（第 14 课 / Capstone）

> 逐课展开版见 [`docs/teaching/14_capstone.md`](docs/teaching/14_capstone.md)，可运行小程序见 `tutorial/14_capstone/`。
> 两者同号，配套完成"改一个点 + 加一条判据"的闭环。

**目标**：独立完成"改一个点 + 加一条判据"，体验完整的"改 → 测 → 守"闭环。

**可选课题（按难度）**
1. **【易】新增一个 CLI 工具**：比如 `route_cli` 加 `--via` 途经点（接 `router_route_via`），并加一条 `alts_selftest` 式断言。
2. **【中】调一个 LOD 旋钮**：把 `AREA_DOT_PX` 从 4 调到 6，用 `bmpdiff.py` 量化净损失，更新 `docs/PERF-AREA-LOD-TIERS.md` 档位表，收紧 `regress.sh` 的 `area_lod_tier` 区间。
3. **【难】加一个新要素类型**：在 `paint.c` 加一种渲染要素（如 `paint_contours` 已存在，可复制其骨架做"等高线标注"），并在 `offscreen_shot` 输出里加一个计数，补一条 regress 判据。
4. **【难】路由新策略**：在 `cost.c` 的 `edge_cost` 里加一个 `avoid_unpaved` 的惩罚项，用 `route_cli` 对比开关前后的 `total_m`/`expanded`。

**交付要求**
- 改动走 `scripts/check_layers.sh` 零违规。
- 新增/改动有对应 `regress.sh` 判据（确定性量优先）。
- 提交前 `./scripts/regress.sh` 全绿（或仅剩已知环境缺失项）。

---

## 四、附录

### A. CLI 工具速查（按"演示什么"分类）

| 类别 | 工具 |
|---|---|
| PBF/建图 | `pbf_info` `graph_extract` `graph_check` `tag_stats` `pbfmap_check` `gen_contours` |
| 路网/路由 | `route_cli` `revbuild_cli` `lmbuild_cli` `lmdist_cli` `restr_ab` `restr_probe` `tile_xcheck` `tile_selftest` `heap_selftest` `alts_selftest` `lane_selftest` |
| 导航层 | `navsim_cli` `voice_selftest` `voice_e2e` |
| 搜索/地名 | `search_cli` `places_build` |
| 覆盖/坐标 | `coverage_cli` `datum_selftest` |
| 字体/TTS | `font_selftest` `font_probe` `font_cjk_check` `tts_selftest` |
| GPX | `gpx_selftest` |
| 架构门禁 | `core_no_sdl` `blend_equiv` `clip_equiv` `plat_selftest` `dmap_selftest` `disp_tile_selftest` `disp_runtime_selftest` |
| 前端 | `navmap_qt`（含 `--shot-offscreen`） |

### B. 关键不变量（贯穿全课）

1. 内部几何永远是 **WGS84（1e-7 度 int32）**；基准只动显示读数。
2. 测距一律 `haversine`，Web Mercator 只用于显示。
3. **PBF stringtable 不是 NUL 结尾**，必须走 `pbf_str`。
4. `AREA_DOT_PX`（paint.h，定档 4）是面 LOD 旋钮，回归用确定性量 `areas_dotted`。
5. `coord_key_e7` 是路由状态主键，跨省接边零映射表；`REDGE_SEAM` 标记接缝段。
6. 核心零 SDL；`app_render_to_buf` 是 Qt 与离屏共用的唯一渲染入口。
7. `.cpp` 包含 C 头必须 `extern "C"`（`datum.h`/`settings.h`/`plat.h`/`gpx.h` 等）。
8. 优先队列 `PQueue`（`pqueue.h`）是 4 叉最小堆，载荷存 **StEnt 下标**（非 key）。

### C. 常见坑（给学生提前打预防针）

- `nm_mercator_y_fast` ≠ `nm_mercator`（故意近似）。
- `RouteOpts` 必须**指名初始化**，位置初始化会静默错位。
- 改 `.hpp` 后 ninja 可能不重编对应 `.cpp` ⇒ 注入的负向对照没进 exe ⇒ 假通过，需 `touch` 对应 `.cpp`。
- 实验参数（`--ovl-alpha` 等）跑实验必须隔离配置（`XDG_CONFIG_HOME=/tmp/xxx`），否则污染真实 `settings.ini`。
- 删文件/删 include 前必须 `grep` 它提供的符号在别处的声明（`window.h` 的 `app_run` 成悬空声明就是教训）。
