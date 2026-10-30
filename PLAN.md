# NavMap 实现计划与风险清单

> 依据：`arch.md` + 2026-09-21 本机实测（不是推测，每条都跑过命令验证）  
> 目标：UOS Desktop 20 / aarch64 / Wayland，C 语言从零做离线地图导航，PBF 为主格式，自动识别并加载新下载地图。

---

## 0. 现状盘点（先看清手里有什么）

### 0.1 代码：已不在工作区，需要重建

`/home/along/navimap/` 现在**只有** `arch.md` 和 `data/`，`src/` 和 `build/` 已被删除。  
从 `/tmp/navimap_*.log` 可以还原出上一版工程的真实结构，**建议原样沿用这些路径**：

```
src/app.c  src/main.c
src/map/map_view.c  src/map/osm_map.c
src/ui/window.c  src/pbf/pbf_reader.c
generated/osm.pb-c.c        # 由 protoc-c 从 osmformat.proto 生成
CMakeLists.txt              # target: navimap
```

上一版的状态（来自 gdb 日志，很关键）：

```
Program received signal SIGSEGV, Segmentation fault.
#0  append_block_to_map ()      #1  map_data_load_pbf ()
#2  window_create ()            #3  app_create ()      #4  main ()
```

⇒ 上一版**死在把 PBF 的 PrimitiveBlock 追加进内存结构这一步**，连窗口都没起来。这个崩溃点和第 3 节 A-2 的 glibc 坑高度吻合，重建时务必先处理。

### 0.2 数据：完好，且已经跑通过大规模抽取

| 资产                         | 内容                                                                                                | 规模         |
| -------------------------- | ------------------------------------------------------------------------------------------------- | ---------- |
| `data/*.osm.pbf`           | 北京/河北/河南/辽宁/陕西/山东/山西/天津 8 省 + `bbbike.pbf`(5.5M 小样本)                                              | 约 456 MB   |
| `data/graph_auto/`         | 分片路网：`shard_NNNN.{nodes,edges,occ,osmid}.bin` + `graphset.txt` + `names.bin` + `restrictions.bin` | **2.3 GB** |
| `data/graph_auto/meta.txt` | 8 省抽取统计：5439.1 万节点 / 2215.2 万边 / 281.3 万 way / 2979 条转向限制，耗时 104 秒                                | —          |
| `data/places_auto.db`      | 地名库：`place(id,name,lat,lon,kind,weight)` + `gram(gram,place_id)` 二元文法索引                           | 32 MB      |
| `data/_bad/source.osm.pbf` | 被标记为"坏"的样本，保留做**负向对照**                                                                            | 40 MB      |

也就是说：**PBF → 路网 → 地名库这条离线管线是被验证过能跑通的（8 省 104 秒）**，现在缺的是消费这些数据的一端（渲染 / 搜索 / 路由 / GUI）。

### 0.3 二进制格式：能反推出尺寸，但字段语义已随源码丢失

按"文件字节数 / 记录数"反推，两个分片都严丝合缝（天津分片只有 62 个节点，正好用来验证头部长度）：

| 文件                  | 推算                                   | 验证                  |
| ------------------- | ------------------------------------ | ------------------- |
| `shard_*.nodes.bin` | 8 B 头 + **28 B/节点**                  | 62×28+8 = 1744 ✓    |
| `shard_*.edges.bin` | 8 B 头 + **18 B/边**                   | 2×18+8 = 44 ✓       |
| `shard_*.osmid.bin` | 无头，**8 B/节点**                        | 62×8 = 496 ✓        |
| `restrictions.bin`  | 8 B 头 + **16 B/条**                   | 2979×16+8 = 47672 ✓ |
| `shard_*.occ.bin`   | 固定 810016 B = 16 B 头 + 900×900 × 1 B | 疑为全局 900×900 占用网格   |

字段内部布局（哪个偏移是 lat、哪个是边权重、通行标志在位几）**没有文档**。见第 3 节 A-4。

### 0.4 "天津分片只有 62 个节点"——已查清，不是 bug（2026-09-21 实测）

`graphset.txt` 里 `shard 7 tianjin.osm.pbf 62 2` 看着像抽取失败，用阶段 2 新写的 `skedaddle_pbf_info` 实测后排除了：

| 文件                | dense 节点   | ways      | 观测 bbox                           |
| ----------------- | ---------- | --------- | --------------------------------- |
| `tianjin.osm.pbf` | 1,835,076  | 248,304   | 36.89~~44.24 N / 116.13~~126.60 E |
| `hebei.osm.pbf`   | 14,970,674 | 1,564,478 | 33.12~~44.24 N / 110.65~~126.60 E |

河北的 bbox **完全覆盖**天津的范围，天津的路网在 `hebei.osm.pbf` 里已经存在一整份；  
合图时按"(起止节点 + 通行标志)"全局去重，天津这张图就只新增 62 个节点、2 条边——  
这正是 `meta.txt` 里 `dup_edges_dropped = 4,045,818` 的来源。**数据是对的，天津是冗余文件。**

⇒ 结论：不要加"单文件节点数不得过小"的断言（会把合法冗余判成失败）。  
改为断言**去重率**：某源文件被去重掉的比例异常高（>95%）时**提示**确认是否下重了区域，而不是报错。

顺带：`data/bbbike.pbf` 是**截断文件**（最后一个 blob 声明 32913 字节但文件到头了，  
差约 28 KB），不能当作基准样本 —— 正好留着做解析器的负向对照（必须报错退出，不能崩）。

---

## 1. 落地原则

1. **先跑起来 → 先看见地图 → 再做搜索/导航 → 最后优化**，严格按 arch.md 的顺序。
2. **每一阶段的验收必须是可在命令行断言的**。本机没有 `xvfb-run`（上次跑 `xvfb-run` 直接报"未找到命令"）、没有 `valgrind`，GUI 只能由你本人在 Wayland 会话里肉眼确认；所以逻辑层一律做成 CLI 工具，输出可断言（有 `gdb`、`perf`）。
3. **先用北京单省（40 MB）跑通全流程**，通过后再扩到 8 省。天津那个异常分片单独复查。

---

## 2. 实现步骤

### 阶段 0：环境固化与工程骨架（半天）

- 写一个 `env.sh`，把本机这些**必须显式设置**的路径固化下来（第 3 节 A-1 解释了为什么）：

```bash
export PKG_CONFIG_PATH="$HOME/.local/lib/pkgconfig:$HOME/.local/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/local/lib/pkgconfig:$PKG_CONFIG_PATH"
export LD_LIBRARY_PATH="$HOME/.local/lib:/usr/local/lib:$LD_LIBRARY_PATH"
export PATH="$HOME/.local/usr/bin:$PATH"      # protoc-c / protoc-gen-c 在这里，不在默认 PATH
```

- CMake 工程：沿用 `src/{app,main,map/,ui/,pbf/,db/,render/,utils/}` + `generated/`，`pkg_check_modules` 找 sdl2 / SDL2_ttf / zlib / sqlite3 / libprotobuf-c。
- 用 `protoc-c`（`~/.local/usr/bin/protoc-c`，版本 1.3.1）生成 `osm.pb-c.c/h`。
- **验收**：`cmake` 输出里必须出现 `SDL2_ttf 2.0.15 found`；如果还是 `not found`，说明 `env.sh` 没生效，不许往下走。



### 阶段 1：SDL2 窗口 + 主循环 + 事件（0.5~1 天）

- `SDL_Init` → 建窗口 → 渲染器 → 主循环 → 处理 `SDL_QUIT` / 鼠标按下-拖动-抬起 / `SDL_MOUSEWHEEL` / 键盘。
- 不画地图，先画纯色背景 + 一个跟随鼠标的十字，确认 Wayland 下窗口正常、事件不丢。
- **验收**：你能看到窗口；拖动能改变十字位置；滚轮数字变化；关闭窗口进程干净退出（无段错误）。

### 阶段 2：PBF 流式解析（2~3 天，最关键的一层）

- 块结构：`BlobHeader(len,type,datasize)` → `Blob`（zlib 压缩，少数用 lz4）→ `HeaderBlock` / `PrimitiveBlock`。
- `PrimitiveBlock` 要点：`stringtable`、`granularity`（默认 100）、`lat_offset/lon_offset`，`DenseNodes` 的 **id/lat/lon 都是 delta 编码**。
- 只要导航最小集：`highway=*`（含 `motorway→residential` 全等级）、`railway`、`waterway/natural` 作底图、`boundary`、`name`/`name:zh`、`oneway`、`maxspeed`、`access`、`turn:lanes` 等通行标志；`relation` 先只处理 `type=restriction`。
- **验收（CLI 断言，别靠眼睛）**：
  - `bbbike.pbf`（5.5 MB，小样本）解析出的 way 数、node 数与独立脚本（或 osmium）一致；
  - 北京 PBF：way_kept + way_skip = way 总数，且 `rt_unknown_drivable == 0`（沿用 meta.txt 里那条闸门）；
  - 边界：空文件、截断文件、`data/_bad/source.osm.pbf` 都必须**报错退出**而不是崩。

### 阶段 3：路网抽取与落盘（2~3 天）

- 输出改为**自描述二进制**：文件头 `magic(4) + version(4) + record_size(4) + count(8) + 字段名表`，宁可多几十字节，也不要再出现"格式随源码一起丢"的情况。
- 顺带判断：旧 `graph_auto` 是沿用还是重新生成（见第 5 节待拍板 Q1）。建议重新生成，旧的留着做对照。
- 保留 arch.md 里那几条质量闸门：`rt_unknown_drivable == 0`、跨省去重计数、`dup_edges_kept_diff`（同段但通行标志冲突要保留两条边）。
- **验收**：`nodes.bin` 字节数 == 8 + 28×节点数（照 0.3 的公式逐文件校验）；每个源文件的节点数不为极小值（防天津那种空分片）；8 省总耗时在 2~3 分钟内。

### 阶段 4：空间索引与按需加载（1~2 天）

- 沿用"分片 + block"的思路：`graphset.txt` 已有 `shard → block → (node 起止, bbox)` 两级索引，这是对的，继续用。
- 打开方式一律 **mmap + 按需触碰**，禁止 `fread` 全量（理由见 A-3）。
- 实现 `IMapSource` 抽象：`Open/GetBounds/QueryPlaces/QueryWays/Render`，把文件格式和渲染解耦，将来加 `.mbtiles` / `.map` 只写适配器。
- **验收**：只加载北京 shard 时，进程峰值 RSS 明显低于 shard 文件大小；跨省查询只触碰命中的 shard。

### 阶段 5：渲染（2~3 天）

- 坐标链固定为三组函数，别在代码里到处混用：  
  `lonlat → 世界坐标(Web Mercator, 米)` → `屏幕像素`，以及逆变换。
- 先软件渲染（`SDL_Surface`/`SDL_Renderer` 的软件后端）画出：背景 → 水域/绿地 → 道路（按 `highway` 等级分 0~3 层 LOD）→ 路名 → 定位点。
- 视口裁剪 + 等级过滤 + 抽稀（Douglas-Peucker）这三件事一开始就要有，否则一放大就卡。
- **验收**：北京四环内可流畅拖动缩放；缩到全省时帧时间可接受（用 `perf` 或直接打点计时）。

### 阶段 6：中文文本与标注（1 天）

- `SDL2_ttf` 2.0.15 + Freetype 22.1.16 都在，字体直接用本机已有的（见 B-4 的坑）：  
  `/usr/share/fonts/fonts-gb/GB_ST_GB18030.ttf`（单面孔，最省事）、`/usr/share/fonts/truetype/yozo/CESI_HT_GB18030.TTF`；  
  用 `NotoSansCJK-*.ttc` 时必须 `TTF_OpenFontIndex()` 指定 SC 面孔，否则可能拿到 JP 字形。
- CJK 字体 20 MB+，加载慢，必须做**字形缓存（glyph atlas）**，否则每帧 `TTF_RenderUTF8` 会拖死帧率。
- **验收**：路名、搜索结果中文不乱码、不缺字（GB18030 覆盖生僻地名）；连续渲染 1000 个标注的耗时可接受。

### 阶段 7：搜索（1~2 天）

- 直接复用 `places_auto.db`：`place` 表按 `kind` 过滤，`gram` 表是二元文法（bigram）索引，适合中文，照着查即可；新地图增量写入。
- arch.md 的建议要对：SQLite 只做**索引**，别把路网塞进去当主存储。
- **验收**：北京范围内搜"中关村"/"西二旗"能在百毫秒级返回，且按距离排序合理；空串、纯英文、生僻字不崩。

### 阶段 8：路线计算（3~5 天，可以后置但要留接口）

- 22 M 边、54 M 节点，裸 Dijkstra 全图不可用：先双向 Dijkstra / &#x41;*（A* 用经纬度距离做启发），跑通后做 CH（Contraction Hierarchies）预处理。
- 权重 = 长度 / 道路等级 / 限速；`restrictions.bin` 里的 2979 条转向限制要接入（否则会规划出禁止左转的路线）。
- **验收**：选两个跨城点（北京→天津）能出结果且里程与常识相符；同一对起终点跑 100 次结果稳定；故意选禁止转向的路口，验证不会给出违法转向。

### 阶段 9：GUI 与导航体验（持续）

- 抄 `/home/along/osmand-app/android/OsmAnd/res/layout` 的**布局与交互**（顶栏地图选择/搜索/路线、侧栏 POI 与搜索结果、右下缩放控件、底部状态栏），但那是 Java/Kotlin 代码，**只能抄交互，不能抄实现**。
- 导航层：定位（先做模拟定位点）、偏航检测、转向指令（直行/左转/右转/靠左/环岛第 N 出口）、剩余距离与 ETA。

---

## 3. 需要注意的部分


### A 级 —— 不处理就会卡住

**A-1. SDL2_ttf 默认查不到，CMake 会静默关掉文字渲染（已实测）**

```
$ pkg-config --modversion SDL2_ttf      →  not found        # 默认 PKG_CONFIG_PATH
$ PKG_CONFIG_PATH=$HOME/.local/lib/pkgconfig pkg-config --modversion SDL2_ttf
                                        →  2.0.15           # 加上就好了
```

库在 `~/.local/lib/libSDL2_ttf-2.0.so.0.14.1`，头文件在 `~/.local/include/SDL2/SDL_ttf.h`，`protoc-c` 在 `~/.local/usr/bin`。  
上一版 CMake 日志里那句 `SDL2_ttf not found; text rendering is disabled for now` 就是这么来的 —— **它是静默降级，不会报错**，等你做到路名那一步才发现没有文字。先写 `env.sh`，构建和运行时都要 source。

**A-2. 本机 glibc 的动态 mmap 阈值会把大数组写坏（本机实测，跨项目通用）**  
症状：某个函数里出现"不可能的取值"然后 SIGSEGV。触发条件是**在那之前 malloc 并 free 过任意 ≥1 MB 的内存** —— glibc 会把 `mmap_threshold` 动态抬上去，之后十几 MB 的数组不再走 mmap，改从 brk 主堆分配，而这条路径在本机不可靠。  
上一版崩在 `append_block_to_map`（正好是"边解析边往大数组里追加"），和这个坑完全对得上。修法，进程启动时钉死分配器：

```c
#include <malloc.h>
__attribute__((constructor(101))) static void fix_allocator(void) {
    mallopt(M_MMAP_THRESHOLD, 128 * 1024);
    mallopt(M_TRIM_THRESHOLD, -1);
}
```

配套：所有计数数组 / 位图 / 索引数组一律 `calloc` 显式清零 —— 大块走 mmap 时是零页、走主堆时是脏内存，靠"没 memset 却一直是对的"的代码会在数据量变小时突然崩。

**A-3. 内存：可用约 3 GB，而路网数据 2.3 GB（已实测）**  
`free -m`：总 7750 MB、可用 3036 MB、8 核。所以：

- 路网一律 **mmap 按需**，不要 `fread` 全量到堆；
- 路由用的数组也 mmap，别为了"快"复制一份到内存；
- 注意**口径**：`ru_maxrss`（峰值 RSS）看不出 mmap 省了多少（顺序扫一遍页就全驻留了），要看系统可用内存的低点（`/proc/meminfo` 的 `MemAvailable` 采样）。
- 8 省全量一次性建图会顶到内存上限，用分片 + 流式落盘。

**A-4. 旧二进制的字段语义已经失传**  
只反推出记录尺寸（28/18/8/16 B），字段布局不知道。两条路：

- 重新生成，并且新格式带 `magic + version + record_size + 字段名表` 的自描述头（推荐，顺手把天津空分片的问题一起修了）；
- 或者先写一个"格式反推校验器"：用已知经纬度范围、边数、`osmid.bin` 与 OSM id 的对应关系去验证猜测，命中后再写加载器。  
  别在数据规模上来之后才发现字段猜错了。

**A-5. GUI 无法无头验证（已实测：`xvfb-run` 不存在）**  
`/tmp/navimap_run.log`：`/bin/bash: xvfb-run：未找到命令`。没有 `valgrind`（有 `gdb`、`perf`）。  
⇒ 所有逻辑必须能在命令行断言，GUI 只做很薄的一层；每个阶段留一个 `--selftest` 入口，把关键不变量（记录数、bbox、路径里程、索引命中）打成文本输出，用脚本比对。


### B 级 —— 容易踩的坑

**B-1. Wayland**：当前会话 `XDG_SESSION_TYPE=wayland`、`WAYLAND_DISPLAY=wayland-0`。不依赖 X11 API、不依赖 GTK/Qt；必要时 `SDL_VIDEODRIVER=wayland` 显式指定。窗口缩放 / HiDPI 要自己处理 `SDL_GetRendererOutputSize` 与逻辑尺寸的差异。

**B-2. PBF 解析细节**：blob 的 datasize 与 zlib 解压后长度都要校验；`DenseNodes` 的 delta 是**累加**；`lat/lon` 需 `(delta_accum * granularity + lat_offset) / 1e9`；`stringtable` 是索引引用；`PrimitiveBlock` 里的 way 只给 node id 引用，需要 id→坐标 的映射（内存大头就在这一步，建议两遍扫描或外部排序）。

**B-3. protobuf-c 路线**：1.3.1 可用，`protoc-c` 需 `PATH` 含 `~/.local/usr/bin`。`/tmp` 里堆了 30 多个手写 PBF 解析调试程序，说明此前在这上面耗过——建议直接走 protobuf-c，另写一个**独立的最小校验器**（只解 header + 第一个 block）做交叉验证，而不是重写一遍解析器。

**B-4. 中文字体**：`NotoSansCJK-*.ttc` 是**字体集合**，`TTF_OpenFont()` 只取 face 0（可能是日文面孔），要么用 `TTF_OpenFontIndex()`，要么直接用单面孔的 `GB_ST_GB18030.ttf` / `CESI_HT_GB18030.TTF`。CJK 字体文件 20 MB+，加载慢，必须做字形缓存。

**B-5. 坐标与精度**：经纬度用 `double` 存、用 `int32` 落盘量化；Web Mercator 在高纬度变形大，别拿它算距离（距离用 haversine）。投影只用于显示。

**B-6. aarch64**：落盘结构显式定长（`uint32/uint64` + 固定字节序，统一小端），不要用 `int`/`long`/结构体直接写盘；OSM id 是 64 位；避免任何 x86 intrinsics（SIMD 一律先不做）。

**B-7. 跨省合并**：上次丢了 404 万条重复边（`dup_edges_dropped`），同时保留了 607 条"同段但通行标志冲突"的双边（`dup_edges_kept_diff`）—— 去重必须按 **(起止节点对 + 通行标志)** 做键，只按节点对去重会丢掉单向/禁行差异。

### C 级 —— 性能

- 渲染：视口裁剪 + 按 `highway` 等级 LOD + 抽稀，缩小时跳过小路，别一次性画全部 22 M 边。
- 路由：双向 Dijkstra/A* 起步，上量后做 CH 预处理；边权重落盘成整数（米 / 十分之一秒），避免搜索时反复浮点计算。
- 搜索：复用 `gram` 二元索引；结果按距离与 `weight` 排序；SQLite 开 WAL 与合适的 `PRAGMA`。
- 磁盘：`.bin` 文件用 mmap 读，随机访问模式别指望 `madvise`（本机实测四种策略差异 <1.5%，属于噪声）。

---

## 4. 建议的模块划分

```
src/pbf/     pbf_reader.c        流式的 Blob/PrimitiveBlock 解码
src/map/     osm_map.c           路网抽取（way → edge）、落盘
             map_view.c          视口、缩放、坐标变换
src/db/      places.c            SQLite 地名索引读写（place / gram）
src/route/   router.c            双向 Dijkstra / A* / CH，转向限制
src/render/  renderer.c          LOD 绘制、线宽、颜色表
             text.c              SDL2_ttf + 字形缓存 + 标注避让
src/ui/      window.c  widget.c  顶栏/侧栏/缩放控件/状态栏
src/utils/   geo.c  mem.c(mallopt/mmap)  log.c
```

统一抽象（arch.md 的思路，照做）：

```c
typedef struct {
    int (*open)(MapSource*, const char* path);
    int (*get_bounds)(MapSource*, BBox*);
    int (*query_ways)(MapSource*, const BBox*, int zoom, WayIter*);
    int (*query_places)(MapSource*, const char* q, PlaceIter*);
} IMapSourceVtbl;
```

---

## 5. 需要你拍板的 4 件事

1. **旧 `graph_auto` 二进制沿用还是重新生成？** 我建议重新生成 + 自描述头（旧的留作对照），因为字段语义已失传，且天津分片是空的。
2. **PBF 解析用 protobuf-c 还是手写？** 建议 protobuf-c（已验证 1.3.1 可用），另配一个最小校验器做交叉验证。
3. **先单省（北京）跑通再扩 8 省？** 建议是，避免一上来就在 450 MB 数据上调试。
4. **渲染先软件渲染还是直接 OpenGL？** 建议先用软件渲染跑通（Wayland 下最稳），性能不够再换后端。

---

## 6. 下一步（拍板后即可开工）

按顺序交付这三样，每一样都能独立验证：

1. `env.sh` + CMake 骨架，确认 `SDL2_ttf 2.0.15` 被找到；
2. `pbf_dump` CLI：解析 `data/bbbike.pbf`，打印 node/way 计数与 bbox，带 `--selftest` 断言；
3. SDL2 最小窗口（Wayland 下由你确认能看见、能拖动）。

---


## 7. 进度（2026-09-21 实施完成，2026-09-22 全阶段复核更新）

四个决策全部落地。**PLAN 定义的 9 个阶段已全部完成**（2026-09-22 复核：编译零警告、  
5 个无头验收工具全绿、8 省图数据 ASSERT-OK）。近期补充见 §12（GUI 导航）与 §13（搜索）。

| 阶段        | 状态 | 说明                                                                                                                                                            |
| --------- | -- | ------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 0 骨架      | ✅  | `env.sh` + CMake（SDL2_ttf 改为 REQUIRED，缺失即失败）+ protoc-c 代码生成                                                                                                   |
| 2 PBF 解析  | ✅  | `skedaddle_pbf_info`：北京 530,968 ways 与旧版 meta.txt 完全对上；`bbbike.pbf` 实为截断文件（当负向样本）                                                                                       |
| 3 路网落盘    | ✅  | `skedaddle_graph_extract` + `skedaddle_graph_check`：北京 1,451,478 节点 / 2,843,824 边 / 363 转向限制，3.3 秒                                                                                |
| 1+5 渲染    | ✅  | `navimap`（Wayland 窗口实测跑通）+ `map_shot`（离屏渲染，三档 zoom 人工核验）                                                                                                      |
| 4 空间索引    | ✅  | 单省 Z14 tile（北京 10,593 tile）；**多省图集**：`--graph=products/graph`（父目录）自动扫描加载全部 8 省、按视口 bbox 整省裁剪、分阶段渲染（面→路→线→标注）；**跨省渲染去重**；**数据包含检测**（北京/天津 ⊂ 河北，加载时自动识别，路由跳过副本） |
| 6 中文标注    | ✅  | 字形缓存（glyph cache，按码点哈希）+ 字体可选（`--font=`）、字号运行期可调（`[` `]`，8~32pt）                                                                                              |
| 7 搜索      | ✅  | 2026-09-22 完成，见 §13：`skedaddle_search_cli` 均值 1.0 ms，边界输入不崩；GUI 顶栏搜索框 + 结果钉子                                                                                            |
| 8 路线计算    | ✅  | 跨省坐标键 A* + 双向 A\*；京→郑 693.65 km，5.4 s / 展开 231 万；`route_check` 自检（含 Σseg_ds == total_s）三条路线全过                                                                 |
| 9 GUI 与导航 | ✅  | 2026-09-22 完成，见 §12：点选起终点（后台线程算路）+ 路线叠加 + 指令面板 + 模拟导航（跟随/偏航重算/剩余 ETA）                                                                                         |

> ⚠️ 帧时间仍是最大短板：z12/z14/z16 第二帧分别 248/331/269 ms（软件渲染，  
> 见 §9「剩余性能待办」）。交互式流畅度未达标，这是下一阶段（ALT/CH 之前）最值得做的。


### 跨省去重与坐标键地基（2026-09-21 补，学自 OsmAnd）

调研结论：OsmAnd 的跨区路由不给节点编全局号，而是把**整数坐标本身当主键**  
（`RoutePlannerContext.cpp:132`：`id = (x31<<31)|y31`），按 tile 查所有数据源、  
同一坐标的分段自然合并——跨省接边零映射表。渲染层按 OSM id 去重  
（`ObfMapObjectsProvider_P.cpp:701-808`），zoom≤14 保留边界两侧被裁剪的互补片段。

据此落地两件事：

1. **跨省渲染去重**（`src/render/paint.{h,c}` 的 `DedupSet`）：
   - 身份键：边 = 两端 OSM 节点 id 的**有序对**（★ 不能排序，否则双向路的回程边  
     被误判重复、整条路消失）；线/面 = 自带的 OSM way id（LF_OSM_ID/AF_OSM_ID）。
   - 128 位键开放寻址 + 世代标记，换帧只 ++gen 不清表。
   - **只在 ≥2 个数据源同时盖住视口时启用**，单省走 NULL 快路径（md5 回归逐像素一致）。
   - 效果：各省 pbf 按多边形裁切，实测北京 ⊂ 河北 100%、天津 ⊂ 河北 100%，  
     此前同框全画两遍（z16 北京 edges/areas/lines 精确 2.0x，帧 269→681ms）。  
     去重后全国 z9 跳过 14.3 万条重复，京津冀 z12 拦下 6.97 万条（帧 230→191ms）。
   - 顺带修了长边跨 tile 重复登记导致的同省重画（z16 北京 1890→1818 唯一边）。
2. **坐标键地基**：`src/map/coordkey.h`（`coord_key_e7` 打包）+  
   `mapset_query_tile()`（按 tile 遍历所有省的空间索引，OsmAnd loadTileHeader 同款）+  
   `tools/tile_xcheck`（验证工具：报跨省共享坐标数、断言同坐标 OSM id 一致）。  
   实测冀津交界 tile 7 个缝合点，hebei:#9768464 与 tianjin:#901054 是同一个  
   OSM 节点——本地下标不同、坐标与 OSM id 一致，坐标键方案的前提成立。

### 新地图自动识别（2026-09-21 补）

`scripts/sync_maps.sh`：扫描 `sources/*.pbf`，与 `products/graph/<省>/manifest.txt` 里的指纹  
（`src_bytes` / `src_mtime` / `binfmt_version`，由 skedaddle_graph_extract 写入）比对，  
缺失或过期就自动 `skedaddle_pbf_info` 预检（拦截断文件）→ `skedaddle_graph_extract`（转临时目录，  
成功才替换）→ `skedaddle_graph_check`。闸门失败（新省出现表外 highway 取值）会明确报  
"需人工补表"，不会静默产出残图。注意：

- **"有 manifest.txt" 严格等价于 "产物可用"**（闸门没过不写 manifest，此前是先写后判，失败产物会被当成已转换）。
- 加载端仍未校验：`app_init` 不读 manifest、`nmbin_open` 只比对 magic 不校验版本——  
  换了 PBF 不跑本脚本，程序会静默画旧图。启动校验待做（加载端读 manifest 比对 + 调用 nmbin_check）。
- 地名库 `places_auto.db` 的**生成器已随旧源码丢失**，新省只有路名标注（names.bin），  
  没有地名/POI 标注（places.db），需要重写 skedaddle_places_build。


### 阶段 8：跨省路线计算（2026-09-21 完成）

核心代码 `src/route/route.{h,c}` + CLI `tools/route_cli.c`（`map_shot --route=` 可把  
算出的路径叠画成红线做视觉验收）。

- **状态主键 = 坐标**（`coord_key_e7`），不是 (省,节点)。这是整个实现的胜负手：
  - 初版用 (省,节点) 当主键、接缝按"同坐标成员一起 settle"合并，结果北京→天津  
    展开 111 万状态、跑 97 秒，且路径在京/冀副本间抖动 560 次（同一路口两个副本  
    各 settle 一次，回溯时前驱在两省间乱跳）；
  - 改坐标键后：一个物理路口只有一个状态，副本重复自动消除，展开 57.5 万、  
    路径抖动归零。
- **数据包含检测**（`mapset.c: detect_duplicates`）：加载时对 bbox 包含且节点数  
  更少的省对做抽样（从 edgelist 取 1000 个点，≥99% 在对方空间索引里找到同坐标  
  节点即判包含），识别出 北京/天津 ⊂ 河北。路由展开时跳过副本省 →  
  北京→天津从 46.8s 降到 **0.62s**（75 倍）。★ 采样必须从 edgelist 取：  
  按节点表采样会把"只有入边"的节点也算进去，命中率掉到 60% 导致漏判。
- **A***：启发式 = 大圆距离 ÷ 130km/h（用 `nm_haversine_m`，不能用等距近似的  
  `nm_fast_dist_m`——跨纬度大时会高估、破坏 admissible）。
- 转向限制：每省懒建 `(via_node,from_edge) → to_edge` 有序表 + 二分；只处理  
  "禁止"型。已知近似：跨省接缝那一跳不查限制（入边属于另一个省）。
- 瓦片缝合缓存：256 槽哈希 + 线性探测；先只读瓦片目录统计**去重后的有效省数**，  
  ≤1 就完全不建索引（省内部瓦片的快路径，热路径最大开销就省在这）。
- 状态表：开放寻址（key+1 编码空槽），ents 数组会 realloc ⇒ **StEnt* 严禁跨  
  st_intern 持有\*\*（全文件 intern→立刻用完→下次重新 find）。

验收（`skedaddle_route_cli` 全部 ROUTE-OK，路径逐段自检通过）：

| 路线           | 结果              | 绕行系数 | 耗时                                  |
| ------------ | --------------- | ---- | ----------------------------------- |
| 北京城内（天安门→西站） | 8.27 km         | 1.25 | 11 ms（单向）/ 13 ms（双向）                |
| 北京→天津        | 131.85 km，京→津   | 1.15 | 0.78 s / 0.70 s                     |
| 北京→郑州        | 693.65 km，京→冀→豫 | 1.11 | 17.7 s / **5.6 s**（469 万 → 231 万展开） |
| 郑州→济南        | 446.13 km       | 1.17 | 6.4 s / **2.2 s**（164 万 → 93 万展开）   |
| 西安→郑州        | 481.23 km       | 1.12 | 3.4 s / **2.4 s**                   |
| 渤海中部（负向）     | 正确报"起点无路"，不崩溃   | —    | —                                   |

**等价性验证**：单向与双向对 京→郑 输出的 GeoJSON **逐字节一致**（`cmp` 通过）；  
全部用例 `路径自检 通过`。--bidir=0 可随时退回单向做对照。

#### 双向 A*（2026-09-22 完成）

- **反向入边索引**（`RevIdx`，按省惰性构建）：边记录只有 `EF_TO` 没有  
  `EF_FROM`，反向搜索拿不到入边。按省建反向 CSR（计数排序，start[n+1] +  
  list[边数]，list 存 from 节点下标；边下标不存，拿到 from 后扫它的出边找  
  `EF_TO==nd`，出度个位数，比再花 4 字节/边划算）。只给反向搜索真正走进去的  
  省建（京→郑：henan 142ms + hebei 311ms + shanxi 85ms = 538ms，占 5.6s 的 10%）。
- **终止条件**：`min_f(正堆) + min_f(反堆) ≥ μ(已会合最优)`。启发式  
  haversine ÷ 130km/h 对两侧各自 admissible 且 consistent（边权=时间≥距离/130）。
- **反向也能查转向限制**：正向路径 u--e0-->nd--e1-->... 的限制 (via=nd,  
  from=e0, to=e1) 在反向松弛 nd→u 时两边都已知晓，与正向完全对称。
- 反向状态的 eidx 语义 = "从本点出发走向 prev 的那条边"，回溯拼装时  
  正向半段取自身、反向半段取前驱的 eidx；路径点身份一律取  
  (到达边所属省, 到达边的 EF_TO)，接缝处省份和节点下标天然对得上。
- **★ 反向 CSR 前缀和的正确写法**：  
  `for (i=0;i<n;i++) { c=start[i+1]; start[i]=acc; acc+=c; } start[n]=acc;`  
  写成 `for(i=0;i<=n;i++){c=start[i];start[i]=acc;acc+=c;}` 会漏掉  
  cnt[n-1]——最后一个节点的入边区间被截成空（本次实测踩中）。
- 代价：峰值内存 +190MB（hebei 反向索引 74MB + henan 40MB + shanxi 27MB）；  
  短途反而略慢（反向索引一次构建 137ms 摊不平，京内 13ms vs 单向 11ms），  
  长途 2.9~3.2 倍加速。

遗留：长途 5.6s / 1.5GB 仍非交互级——要进秒级得做 ALT（地标启发式，  
46M 节点 × 8 地标 ≈ 1.5GB，需压缩）或 CH 预处理；转向限制的跨省近似未验证。

### 产物

```
products/graph/beijing/          # 自描述二进制（NAVMBIN v1）
  nodes.bin   40.6 MB  28 B/节点 lat_e7/lon_e7/edge_begin/edge_count/flags/osm_id/class_mask
  edges.bin   51.2 MB  18 B/边   to/dist_cm/name_id/flags/class/speed/lanes
  osmid.bin   11.6 MB  8 B/节点
  names.bin   164 KB   变长字符串池
  restrictions.bin 363 条  from_edge/to_edge/via_node/kind/dirmask
  manifest.txt
```

### 实施中抓到的三个新坑（都已写进代码注释）

1. **protobuf-c 的 stringtable 不以 '\0' 结尾**：`pbf_str` 直接返回  
   `ProtobufCBinaryData.data` 的话，strcmp 会读到后面的垃圾——"path" 变成  
   "path\x7f"，highway 分类全落空、type=restriction 也认不出（统计数看着"差不多"  
   但就是不对，极难查）。修法：解包时把整个 stringtable 拷成 NUL 结尾副本（`pbf_block.c`）。
2. **`strncmp(s, "--w=", 5)` 永远不相等**：n 比字面量长度多 1 会把结尾 '\0' 比进去。  
   `--w=`/`--h=` 都中招。修法：n = 字面量长度。
3. **oneway=-1 的路不能打 EF_REVERSE**：它是这条路唯一的边，打了会被渲染层当成  
   "双向路的回程边"跳掉，整条路消失。

### 已知待办（2026-09-22 复核：前三条已做，后三条仍欠）

- ~~帧时间：zoom 12 一帧 ~150-330 ms。下一步做网格空间索引~~ → **Z14 空间索引已做**（§9），  
  但实测 z12/z14/z16 第二帧仍为 248/331/269 ms，**没有降到预期的一个数量级**，  
  仍是全项目最大短板。下一步：按块预投影缓存。
- ~~路名渲染（还没接字形缓存）~~ → 已做，含 glyph cache（按码点哈希，`font.c`）。
- ~~跨文件去重 + graphset 分块索引~~ → 已做（`mapset.c` + `paint.c` 的 DedupSet）。
- **via=way 的转向限制仍未处理**（182 条被跳过，占 28%）：`skedaddle_graph_extract` 只写  
  via_node 型，`route.c` 的 `restr_blocked` 也只认 via_node。会规划出禁止转向的路线。
- **`skedaddle_places_build` 地名库生成器缺失**：新导入的省没有地名标注，搜索覆盖范围不随  
  数据增长（现有 22.6 万条是旧工具产出的存量）。
- **跨省接缝处的转向限制未验证**：接缝两侧分属不同省，限制记录的 via 节点在对方省里，  
  目前未做跨省限制检查。
- **长途算路仍是 5.4 s / 231 万节点展开**：非交互级，需要 ALT 或 CH 预处理。
- **两个统计字段失真**：`PaintStats.nodes_tested` 只在无 tile 索引的退化路径自增  
  （走索引时恒 0），`edges_visited` 从未被赋值（死字段）——`map_shot` 打印的恒 0  
  会误导性能判断，要么补统计要么删字段。

---

## 8. 地图信息补全（2026-09-21 下午完成）

用户反馈"显示的地图信息不全"。诊断（skedaddle_tag_stats 统计 beijing.osm.pbf）：  
ways_total 530,968 中只有 34% 进图——建筑 137,837、landuse/natural 56,213 全丢，  
footway/path/steps/cycleway/pedestrian 87,417 因"不可通行"丢弃，且没有任何文字标注。

按决策四项全做：

| 要素       | 实现                                    | 数量(北京)             |
| -------- | ------------------------------------- | ------------------ |
| 步行道/小路   | lines.bin，RC_PATH，可显示不路由，zoom≥14      | 104,696 条线(含铁路/水系) |
| 铁路+河流线   | lines.bin，RC_RAIL/RC_WATER            | 同上                 |
| 建筑+绿地+水面 | areas.bin + polyrefs.bin（闭合 way）      | 199,709 个面         |
| 中文标注     | SDL2_ttf 字形缓存 + places.db + names.bin | 地名≤400/帧 + 路名      |

### 新文件格式

- lines.bin (kind 6)：osm_id/ref_off/ref_cnt/class(5-7)/flags/name_id，24B/条
- areas.bin (kind 7)：osm_id/ref_off/ref_cnt/kind(1建筑 2水 3绿 4其他)，20B/条
- polyrefs.bin (kind 8)：u32 节点下标序列，线/面共用
- nodes.bin 1.45M → 4.32M（含线/面专用节点，无路由边）

### 实施中抓到的两个新坑

1. **SDL 2.30 软件渲染器没有实现 SDL_RenderGeometry**——调用被静默丢弃，  
   三角形不画也不报错。靠 BMP 颜色直方图发现"填充色根本不存在"才定位。  
   改自绘扫描线 even-odd 填充（顺带支持内环/自交多边形）。
2. **names.bin 读成裸格式导致退出时 SIGSEGV**：names.bin 有 128B NMBIN 封装头，  
   直接把前 4 字节当 cnt 读出天文数字 → init 失败路径 free 后指针未置 NULL →  
   app_shutdown 里 double free。gdb 下不复现（堆布局不同），靠 coredump 定位。  
   修：用 nmbin_open 剥封装头 + 失败路径 memset 清零。

### 性能（含进程启动，纯渲染约一半）

z11 0.6s / z12 0.34s / z14 0.41s / z16 0.37s（1280x800，软件渲染）。  
z11-12 仍偏慢，待做网格空间索引（见已知待办）。

### 验证链

skedaddle_tag_stats（要素对账）→ skedaddle_graph_extract（对账式=total，rt_unknown_drivable=0）  
→ skedaddle_graph_check（lines/areas 下标越界/面闭合/kind 合法，全过）→ map_shot  
（BMP 颜色直方图断言填充色存在）→ Wayland 窗口 3 次 timeout 退出无 core dump。  
截图存档 docs/shots/{area_z11,area_z14,area_z16,label_z12,label_z16}.png

---

## 9. 空间索引（2026-09-21 晚完成）

zoom 12 全城帧时间 496ms → 210ms（软件渲染，窗口模式实测），扩 8 省前的关键前置。

### 设计

- edges.bin 是 CSR 顺序（路由/转向限制依赖边下标），**不能重排**。  
  索引独立成两个文件：
  - `edgelist.bin` (kind 10)：{node_idx, edge_idx} 对（8B），按 tile 排序。  
    存 from 节点下标是因为 CSR 边不含 from 坐标，渲染无需反查。
  - `tiles.bin` (kind 11)：Z=14 Web Mercator 瓦片（约 2.4km/格）目录，  
    每格 {edge_off, edge_cnt}；**前 2 条记录存网格元数据 {x0,y0} 和 {nx,ny}**，  
    避免 rec_size 校验被特殊布局破坏。
- 长边归属：一条边加入 from/to 两端 tile bbox 覆盖的**所有** tile  
  （跨度上限 8 格 ≈ 19km），视口只覆盖远处 tile 时高速长直段也能画出来。  
  代价：同一边在多个 tile 重复出现，重复绘制无害。
- 渲染无索引文件时自动回退全量扫描（旧目录兼容）。

### 实施中抓到的坑

1. **bbox 只统计 from 端**：死端节点只作为 to 出现，其 tile 超出  
   [min..max] → 计数排序数组越界写 → 堆损坏，退出时 munmap_chunk 报错。  
   修：bbox 统计两端 + 范围四向 clamp。
2. **edgelist 按 ne 分配**：长边进多 tile，总条数 Σcnt > ne → xl 越界写。  
   修：计数后再按总和分配。两个坑都表现为"崩在毫不相干的 free"，  
   靠 gdb backtrace 定位（第二个坑 gdb 一跑就中，无需 coredump）。

### 剩余性能待办

- lines/areas 还在每帧全量扫 bbox（z16 稳态仍有 ~250ms，主要在这）：  
  同样可以做成 tile 索引，接口已就绪。
- map_shot 的第二帧计时受"每帧新建 renderer → 字形缓存重建"干扰，  
  稳态帧时间以窗口模式标题栏为准。

---

## 10. 字体可选，默认宋体（2026-09-21 下午）

`--font=<路径|none>` 指定字体，`--font-size=N` 指定字号（默认 14）；  
不指定时走**宋体优先候选链**：CESI宋体GB18030 → CESI宋体GB2312 → 国标小标宋 →  
方正仿宋 → 国标黑体（兜底）。环境变量 NAVIMAP_FONT 仍有效，优先级：  
命令行 > 环境变量 > 默认链。`--font=none` 关闭文字标注（纯图形）。  
`.ttc/.otc` 集合自动走 TTF_OpenFontIndex（font_open_ex 供挑面孔）。

### 顺带修掉的回归（重要）

修完后对比截图发现 **map_shot 出的图完全没有文字**（昨天 tiled 版本起就如此，  
当时"视觉一致"核验恰好没注意到标注消失）。最小复现定位：

- map_shot 为测稳态帧时间每帧新建 software renderer；字形纹理挂在旧 renderer 上，  
  renderer 销毁时纹理已释放 → 换 renderer 清理逻辑对**悬空指针再 DestroyTexture**  
  → 破坏 SDL 内部状态；且清理后缓存命中的字形 tex=NULL **永不重建**（重建只在  
  首次创建路径）→ 第二帧起一个字都画不出来。
- 修：glyph_get 里 tex_owner != r 时只置 NULL 不 destroy；缓存命中时  
  tex 为空按需重建（glyph_build_tex 提为独立函数）。真实窗口单 renderer，不受影响；  
  但 map_shot 是无人值守验证工具，坏了会掩盖视觉回归——已用双帧最小复现确认修复  
  （frame2 black 3 → 267），三张不同字体模式的 BMP md5 互不相同，--font=none  
  的 md5 与修复前所有图相同（坐实此前标注全丢）。

---

## 11. 字号运行期可调（2026-09-21）

- `--font-size=N` 作初值（默认 14）；窗口内 **`[` 减小 / `]` 增大**（8~32pt，  
  +/- 已被缩放占用）。状态栏实时显示当前字号。
- 实现：字号是 TTF_Font 的属性，改它必须重建 Font——app_set_font_size 按记住的  
  字体路径（App.font_path，init 时从 font_used 剥掉" (默认)"后缀得到）重开，  
  旧字形纹理经 font_close 释放（窗口 renderer 还活着，安全）。
- 标签避让网格随字号自适应：不再固定 110x34，按单汉字宽度估算  
  （宽×8 / 宽×2.4，下限 60/22），大字不压字、小字不浪费。实测 12pt 150 个  
  地名 vs 22pt 88 个。
- 修：PaintStats 只清了部分字段，zoom<15 不画路名时 road_labels_drawn 留栈垃圾  
  （打印出 5492 亿）。改为 paint_roads 入口 memset 全清。
- 无头验证：map_shot 加 --resize-font=N（第二帧前改字号）覆盖 app_set_font_size  
  路径，断言生效且画面变化。

---

## 12. GUI 导航与模拟导航（2026-09-22 完成）

点选起终点（后台线程算路，GUI 不卡）→ 路线叠加 + 起终点标记 + 左上角指令面板，  
这部分先落地；本节补齐 PLAN 阶段 9 的**导航层**：模拟定位、偏航重算、剩余里程与 ETA。

### 交互（窗口内）

| 键/操作                 | 作用                            |
| -------------------- | ----------------------------- |
| 左键点两下                | 先起点后终点，自动算路（后台线程）             |
| `g`                  | 开始 / 停止模拟导航（自动拉近到 z15 导航视角）   |
| `t`                  | 视口跟随车头开关                      |
| `n`                  | 注入一次偏航（定位点朝车头右手拉开 300 m），自动重算 |
| `.` / `,`            | 模拟倍速 1/2/5/10/30/60/180x      |
| 右下角 +/- 按钮           | 缩放（命中判定与绘制共用同一份几何）            |
| `c` / 右键 / `f` / `p` | 清除 / 缩放到整条路线 / 面板开关（原有）       |

### 实现

- `src/route/navsim.{h,c}`：模拟导航纯计算层——沿路线推进（按段限速×倍速）、  
  点到路线投影（横向偏移 + 投影里程）、偏航判定（默认 60 m）、剩余里程/ETA、  
  当前指令定位。不碰 SDL / Router / 线程，GUI 与 CLI 共用。
- `RouteResult` 新增 `seg_ds`（每段耗时 0.1s）：剩余 ETA 必须按剩余各段自己的  
  限速求和，用总时长按里程比例摊会把"前高速后城区"算乐观。route_check 顺带  
  校验 Σ seg_ds/10 == total_s、每段与限速自洽。
- 绘制：`paint_route` 支持 done_seg/done_t 切分（已走过灰、未走蓝）；  
  `paint_nav_car` 车头三角箭头（按航向旋转，重心外扩白边）；  
  `paint_nav_banner` 底部指令条（大字转向 + 距离/路名，右侧剩余里程/ETA，  
  大字号 = 标注字号 ×2 单开一份 Font）。
- `map_shot` 新增 `--sim [--sim-sec=N] [--sim-deviate=M]`：离屏也能画导航帧。

### 验收（全部无头断言）

- `skedaddle_navsim_cli`（新工具）：推进单调性、恒等式（已走+剩余==全程）、车头恒贴线  
  （横向偏移 ≈ 0）、到达判定、模拟耗时 ≈ 预计耗时、200 m 判偏航 / 30 m 不判、  
  偏航后重算起点贴住注入点。北京城内 8.27 km 与京→郑 693.65 km（238 条指令）  
  全过，跨省投影零误差。
- `map_shot --sim`：进度 51% 时灰/蓝像素并存（4797/4727），注入 300 m 判偏航；  
  截图见 `docs/shots/nav_sim_z15_mid.png`、`nav_sim_bj_zz_z15.png`（京石高速段）、  
  `nav_sim_z15_deviate.png`。
- 回归：非模拟帧 route_pixels 8287 与改造前一致；跨省 route_check 含 seg_ds 校验通过。

### 踩坑

- **测试自己写错的"恒等式"**：CLI 里先读剩余再注入定位点，navsim 的剩余在有  
  定位点时以投影里程为准，于是拿上一帧投影比这一帧已走里程，凭空差一个步长  
  （55 m），看起来像恒等式不成立。教训：断言前先想清楚被测量的口径依赖。
- **偏航注入方向写反**：法线应是 (cos h, −sin h)（东, 北），写成 (−sin h, cos h)  
  得到的是航向本身，注入点落回路线上，200 m 注入只测出 22.6 m。这个辅助函数  
  收进 navsim（navsim_offset_point），GUI 的 n 键与 CLI 共用一份。
- **路线释放后 sim 悬空**：NavSim 存 \&a->rr 指针，nav_drop_route 里必须一并  
  关 sim（偏航重算靠 poll 里的 was_sim 恢复），否则清路线后再 tick 就是  
  use-after-free。

---

## 13. 搜索（阶段 7，2026-09-22 上午完成）

复用现成的 `places_auto.db`（22.6 万地名，覆盖已导入的 8 省）：  
`place(id,name,lat,lon,kind,weight)` + `gram(gram,place_id)` 二元文法倒排。  
SQLite 依旧只当**索引**，路网主存储不变（arch.md 的要求）。

### 实现

- `src/search/search.{h,c}`：查询管线 = 关键词按**字符**切 bigram → gram 交集  
  （`GROUP BY place_id HAVING COUNT(DISTINCT gram)=N`，等值查询走 (gram,place_id)  
  WITHOUT ROWID 主键，微秒级）→ 按 id 逐条取详情 → **子串复核**（gram 全中  
  不等于连续，"中关"+"关村" 也能召回"中…关村"）→ kind 过滤 → 排序取 top-K。
- 单字没有 bigram：先走 gram 前缀（`gram LIKE 'X%'`，仍走索引），没结果再退化  
  全表 LIKE（只在单字时发生，实测 45~150 ms）。
- 排序键 = 距离/(1+weight×0.05)，再乘长度惩罚、除匹配质量（全等×3、前缀×2）：  
  纯距离会把"XX村公厕"顶到"中关村"前面，纯权重会把远处大城拽到眼前。  
  ★ 排序分必须恒正（无中心时用 1000/(1+w)），否则后面除匹配质量会翻向。
- 排序分存在 SearchHit.score 里带回：CLI 断言排序单调不用重抄公式。
- LIKE 的 %/_ 做 ESCAPE 转义；空串直接返回 0 条；UTF-8 按**字符**追加/退格，  
  截断/退格不对齐多字节边界就会留下半个汉字。
- GUI：`/` 或 `s` 打开顶栏搜索框（SDL_StartTextInput），TEXTINPUT 上屏、  
  TEXTEDITING 只显示（拼音阶段看得见），↑↓ 选择、Enter 定位并设为起/终点  
  （第一次当起点，已有起点则当终点并算路）。搜索库与 LabelCache **各开一个  
  连接**——共用连接会互相踩游标。main/map_shot 支持 `--q=<关键词>`。
- 搜索结果在图上画钉子（选中蓝芯带名字）。

### 验收（skedaddle_search_cli + map_shot --q，全过）

- "中关村/西二旗/天安门/五道口"命中且结果名含关键词；"五道口"本体排第一  
  （匹配质量生效）；跨省同名不误判（"中关村"在山东 378 km 外有同名村，  
  按距离压后是对的——断言只要求"同名且距离 ≤ 最小距离 2 倍时排第一"）。
- 性能：均值 1.0 ms、最慢 4 ms（PLAN 要求百毫秒级，余量两个数量级）；  
  单字 45 ms；通配符/特殊字符兜底 ≤ 160 ms。
- 边界：空串/纯英文/生僻字"龘龘"/超长串/`%%`/`_`/`\` 全不崩。
- 召回抽查 10 个常见词全命中。map_shot --q：搜索框白底像素 > 0（框真的画了）、  
  结果含关键词、排序单调。截图 docs/shots/search_zjg_z13.png、search_xeq_z14.png。

### 顺带修的回归

- Σseg_ds 与 total_s 各自舍入再求和，京→郑 3939 段差出 1.9 s，route_check  
  自检炸了。修法：total_s 改用**舍入后**的段耗时累计，两边严格相等  
  （导航剩余 ETA 本来就按 seg_ds 累计，这一改口径完全统一）。
- 已知数据缺口：places_auto.db 里没有"天安门"本体（只有安检口/绿地），  
  是地名抽取阶段的遗漏，不是搜索问题；skedaddle_places_build 生成器仍缺（见遗留）。

---

## 14. via=way 两段转向限制（2026-09-22 完成）

PLAN §7 里挂着"via=way 的 182 条被跳过（占 28%）"——这是**唯一会让路线违法**的缺口  
（会规划出禁止左转/掉头的路线），所以先补它，再做帧时间。

### 先探数据，再定方案（tools/restr_probe.c，新工具）

北京 179 条 via=way：**87%（156 条）可精确定位**，且 via way **166 条只有 2 个节点**  
（就是路口中间那一小段连接路）。

★ 关键：不能把 via=way 压成单点限制。实测同一对 via/to 上挂着两条不同约束：

```
no_u_turn     from=1159434706 via=128620751 to=1089116696
no_left_turn  from=24691164   via=128620751 to=1089116696   ← 同一个 via、同一个 to
```

压成"在 B 点禁 (e_via → e_to)"会误伤从第三条路合法进入的车。必须保留 from，  
即约束的是**三边序列** e_from → e_via → e_to。

### 实现

- `binfmt.h`：记录 16 → 20 字节，新增 `RF_PREV_EDGE`（三段序列的第一条边），  
  `RF_KIND` 增加 2=via-way。**NMBIN_VERSION 1 → 2**：记录变长后按旧长度读会整表  
  错位，宁可让 sync_maps 全量重建（加载端实测正确挡住了 v1 数据："版本 1 不支持，跳过"）。
- `graph_build.c`：pass1 不再丢弃 via=way；建 way_id → 节点序列的索引；  
  `resolve_restrictions` 里求 A=from∩via、B=via∩to（唯一且 A≠B），再定位  
  e_from/e_via/e_to。via way 多段的（13 条）在这里落空被跳过——保守，不猜。  
  边定位要求唯一（`pick_edge` 不唯一就跳过），比原来"取最后一条"更保守。
- `route.c`：kind=2 单独一张索引（键同 (via,from)，多带一个 prev）。  
  正向用前驱状态的 eidx 拿到 prev（精确）；**反向只能保守**——反向从终点往起点推，  
  站在 B 时"到达 A 的那条边"还没确定，所以反向只要 (via,from,to) 命中就禁。  
  方向是保守的：不会给出被禁转向，代价是反向可能绕一点远。
- 北京：限制总数 363 → 505，其中 via=way 展开 **144 条**（35 条落空）。

### ★ 顺带挖出的真 bug：双向搜索的接缝漏检（影响既有单点限制）

验收时发现：开着限制算出来的路线**仍然**包含被禁止的序列，而且连**单点**限制  
也拦不住。根因不在限制表，而在双向 A* 的相遇逻辑：

```
正向走到坐标 k 停住（入边 e_f），反向也走到 k 停住（出边 e_b）。
两边各自展开时都看不到 "e_f → e_b" 这个完整转向：正向没走 e_b，反向没走 e_f。
直接拼接就把被禁转向放过去了。
```

修法：相遇时补查一次接缝转向（`seam_blocked`），单点 + 两段都查，不合法就不接受  
这个相遇点、继续搜。修复前后对比（同城 10 米的一个掉头）：

```
修前  3 段 / 40 m / 自检报"踩了 via=way 两段禁止序列"
修后 41 段 / 1.55 km / 自检通过（禁止掉头 → 必须绕一大圈，绕行系数 174 是对的）
```

这条 bug 与 via=way 无关，**之前的单点限制一直有这个洞**，只是没人验。

### 验收（tools/restr_ab.c，新工具）

判据是双向的，缺一边都会自欺：

1. 关掉两段限制算 rB —— 60 条里 **50 条**真的走进被禁序列（证明不是死条目）；
2. 开着限制算 rA —— **0 条**走进序列；rA 再过 route_check（含新增的"不许踩限制"）。

`route_check` 新增两条断言：路径不踩单点限制、不踩两段序列。  
（写第一版时把两段的 via 写成了 `pts[i]`，应为 `pts[i+1]`——转向点是 e1 的终点。  
写错时自检永远查不到，于是"限制没生效"看起来像自检通过。）

定向验收的必要性：随机 12 条路线、共 2000 多段，**一次都没撞上**那 144 个路口。  
必须走到限制所在的路口去测（从限制记录反解 P0/A/B/C 再构造路线）。

---

## 15. 帧时间：定位与修复（2026-09-22 完成）

上轮复核把"帧时间没达标"列为最大短板（z14 第二帧 331 ms），并怀疑是投影瓶颈。  
本轮先用**分阶段计时**定位（PaintStats 新增 ms_bg/ms_areas/ms_roads/ms_lines/  
ms_labels，map_shot 输出"分解"一行），结果**推翻了投影假设**：

```
z14 分解：背景 0 + 面 44 + 路 28 + 线 248 + 标注 11 = 331 ms
```

**线要素占 75%**。而线要素慢的原因根本不是投影算法，是**没有空间索引**：  
每帧扫全部 10.4 万条线，每条线的全部顶点（上限 4096）都要投影一遍才能判断  
可不可见。面要素早有 area_bbox 缓存做预筛，线没有。

### 修复

给 lines 加与面要素完全相同的懒填 bbox 缓存（`MapSrc.line_bbox`，nl*4 个 i32，  
e7 度，进程内只算一次）。预筛放在去重之前——bbox 测试比哈希便宜。  
预筛是保守的（外包盒相交才进入后续流程），lines_drawn 前后一致（7911）  
说明没有误杀；渲染图人工核验水面/铁路/绿地/标注都在。

### 效果（第二帧，北京城内 39.90,116.40）

| zoom | 修前     | 修后         | 倍数          |
| ---- | ------ | ---------- | ----------- |
| z16  | 269 ms | **37 ms**  | 7.3x        |
| z14  | 329 ms | **100 ms** | 3.3x        |
| z12  | 247 ms | 224 ms     | 1.1x（瓶颈换人了） |

z14 之后的新分解：面 44 + 路 28 + 线 17 + 标注 11 = 100 ms。  
z12 的瓶颈换成了面（73 ms 扫描线填充）和路（122 ms 粗线重复绘制，  
paint_thick_line 每条边画 4~6 遍 SDL_RenderDrawLine）。

### 方法论（下次别再猜）

**"投影贵"是从三角函数直觉来的，没有测量支撑。** 没有分阶段计时，  
"按块预投影缓存"就会被做在错误的地方——省下 3 ms 的投影，  
剩下的 248 ms 纹丝不动。本机 perf 装不上（perf_4.19 缺失），  
clock_gettime 分阶段打点是唯一可靠的定位手段，现在已常驻 map_shot 输出。

### 15.1 自动 LOD（同日追加，z12 的第二刀）

线要素修完后 z12 的分解是 面 73 + 路 123 + 线 19 = 224 ms。  
路 123 ms 的本质**不是单次绘制慢**（0.5 μs/边，已经很快），是**边数**：  
z12 视口覆盖全省，245096 条边连 cls=3/4 的小路都画——屏幕上密成 1 像素的  
灰线堆，纯属视觉噪音。地图常识是"越远越只看主干"。

修法：按 zoom 自动降 LOD，与用户手动的 0-4 键取交集（min）：

```
zoom < 9: cls<=0    < 11.5: cls<=1    < 12.5: cls<=2    < 14: cls<=3    其余全画
```

效果（第二帧，同一视口中心）：

| zoom | 修前     | 修后         | 边数                   |
| ---- | ------ | ---------- | -------------------- |
| z16  | 37 ms  | 37 ms      | 1890（未触发 LOD）        |
| z14  | 100 ms | 101 ms     | 24527（未触发）           |
| z13  | —      | 188 ms     | 82507                |
| z12  | 224 ms | **169 ms** | 245096 → 121419      |
| z11  | —      | 272 ms     | 142603               |
| z9   | —      | 445 ms     | 273770（高速/国道就这么多，见下） |

z12 观感反而**更清晰**（对比 §15 的修前图）：主干路网层次分明，少了一堆  
1 像素灰线。路线叠加不受影响（route_pixels 4727 前后一致）。

### 已知剩余

- **z9–z11 仍 270–445 ms**：视口覆盖全省时高速/国道本身就有 14–27 万条边  
  （约占 284 万边的 10%，不是过滤 bug）。要再快只能合并连续边成折线走  
  `SDL_RenderDrawLines` 批量调用，或者降低这个级别的交互要求——  
  全省概览时用户本来就不怎么拖动。
- **z12 面 73 ms**：16419 个面的扫描线填充。若要继续，方向是按 zoom  
  跳过小面积建筑（现在 z12 已经只画水体/绿地/大地块，建筑要 z13 起）。

## 16. 启动自愈：产物没了/坏了就自动重建（2026-09-22 完成）

此前程序是纯加载（src/ 里没有 system/popen/exec）：删掉 products/graph/tianjin  
后 map_shot 只打印一句"既不是省目录也没有 nodes.bin"就空跑，skedaddle_graph_check 直接  
ASSERT-FAIL —— 没有任何自愈。现在启动时先体检，需要就现场重建。

### 判据（与 scripts/sync_maps.sh 完全一致，避免"脚本说该转、程序说不转"）

| 触发                              | 说明                                               |
| ------------------------------- | ------------------------------------------------ |
| 产物目录不存在                         | 整个省被删了                                           |
| 缺必需文件                           | manifest.txt / nodes.bin / edges.bin / osmid.bin |
| nodes/edges 打不开或 nmbin_check 不过 | 截断、半截文件                                          |
| binfmt_version 与当前不符            | 格式升级                                             |
| src_bytes / src_mtime 与源 pbf 不符 | 源被换过了                                            |

### 实现（src/map/autobuild.{h,c}）

- `autobuild_repair()` 体检 + 重建；`mapset_load_repair()` = 先修再加载。
- 重建走 **fork + execv**（不走 shell、不经 system），只执行项目自带的  
  skedaddle_graph_extract（与本程序同目录 → PATH 依次探测），参数固定；省名过白名单  
  （禁 `/`、`.`、`..`），所以没有命令注入与路径逃逸。
- 旧产物先清掉再转：混着写会产出"新 nodes.bin 配旧 osmid.bin"这种自相矛盾的  
  图。清理用自己的 unlink/rmdir 递归（**不能**用 rm 命令——本机 rm 是只搬回收站  
  的安全壳）；删不掉就改名 `<省>.old.<pid>` 让位（mapset_load 会跳过 .old./.tmp.）。
- 并发锁：`<省>.building` 里写 pid，O_EXCL 抢占；进程死了的孤儿锁会被接管，  
  活着的就等（实测打印"等待中…"）。
- 开关：默认开，`--no-autobuild` 只诊断不动手；`--sources=` 指定源目录。  
  接入 navimap / map_shot / skedaddle_route_cli / skedaddle_navsim_cli / skedaddle_restr_ab / skedaddle_tile_xcheck。

### 验收（逐个场景实测，不是跑通正常路径就算）

| 场景              | 结果                                       |
| --------------- | ---------------------------------------- |
| 正常启动            | 0.049 s，8 省体检零输出（开销可忽略）                  |
| 删掉整个省目录         | 2.0 s 重建，恢复后 nodes.bin 的 md5 与删前**完全一致** |
| 只删 nodes.bin    | 报"产物不完整（缺 nodes.bin）"并重建                 |
| 截断 nodes.bin    | 报"nodes.bin 校验失败（声明 46276636 字节）"并重建     |
| touch 源 pbf     | 报"源文件被换过了（mtime 不同）"并重建                  |
| --no-autobuild  | 只提示 + 汇总"跳过 1"，随后按 7 省加载                 |
| 坏产物 + 源 pbf 也没了 | 报"没有原料，无法重建"（不是静默少一个省）                   |
| 单省模式            | 只检查该省，不会误判成图集把 8 省全建一遍                   |
| 孤儿锁 / 活锁        | 孤儿被接管；活锁等待（timeout 4s 确认在等）              |

★ 设计上刻意补的一条：图集模式原本只扫 sources/ 决定"该有哪些省"，于是  
"pbf 和产物都没了"会完全静默。现在反向再扫一遍 products/graph 下已有的  
省目录，坏了哪怕建不了也要说出来。

## 17. 无地图区域提示（2026-09-22 完成）

浏览或定位到没有数据的地方时，顶部弹琥珀色横幅：这片区域没图 → 去哪下载 →  
下载后放 sources/ 跑 sync_maps.sh。

### 两级判断（src/map/coverage.{h,c}）

bbox 是矩形、省界不规则，所以"落在某省 bbox 内"不等于真有路：

- `COV_NONE` — 不在任何已加载省的 bbox 内：确定没图，提示下载
- `COV_BBOX_ONLY` — 在 bbox 内但附近 3 km 查不到边：水域/未采集，语气温和些
- `COV_HAS_ROAD` — 附近有路
- `COV_UNKNOWN` — 省都没有空间索引，细判做不了

细判查 Z14 瓦片（半径按纬度折算，最多 7×7）。省名取"实际提供边的那个省"，  
而不是 bbox 先命中的那个——否则天津地界会报成北京。

### 关注点（window.c）

模拟导航开着时看**车头**，否则看**视口中心**。这个"关注点"抽成  
`app_focus_point()` 供功能与验收共用：两处各写一遍的话，"视口有图、车头无图"  
时会一边弹提示一边判失败。

### 验收（tools/coverage_cli.c + map_shot 像素断言）

- skedaddle_coverage_cli 自检：北京中心必须有路、(30,110) 必须无数据、无数据时提示里  
  必须同时含 sources/、sync_maps.sh 与下载链接、有数据时不得出现"下载"。
- map_shot 像素断言（横幅边框琥珀色 255,186,64）：  
  省外点 `nomap_banner_px 1220` ASSERT-OK；北京 ASSERT-OK 且横幅 0 px。
- 定位点分支用受控实验验证：临时把车头坐标钉在省外，视口留在北京 →  
  横幅照样弹出（1220 px），证明提示确实由定位点驱动。模拟导航会自动  
  重算回有图区，这条分支没法自然构造，只能这么验。

★ 顺带修的判据冲突：map_shot 原有一句"一帧都没画出来就 FAIL"，在无图区  
必然误报。现在按 coverage 分流——有图时要求画出路、且不许有横幅；  
无图时要求必须有横幅（否则会出现"既没地图也没提示"却 ASSERT-OK 的最差情况）。

## 18. 帧时间第三、四刀：投影缓存 + 小面画点（2026-09-22）

§15/§15.1 之后 z9–z11 仍是 270–445 ms。这轮拆出两个真瓶颈。

### ★ 第三刀：mv_project 每次都在重算常量（src/map/map_view.c）

```c
nm_mercator(lat, lon, &wx, &wy);        /* 这个点，必要 */
nm_mercator(v->lat, v->lon, &cx, &cy);  /* 视口中心 —— 一帧内是常量，却每次都算 */
double s = mv_scale(v);                 /* pow(2,zoom) —— 同样是常量 */
```

即每次投影要付 **2×(sin+log) + 1×pow**，其中一半纯属浪费；而渲染一帧要投影  
几十万到几百万次（z9 光路面就上百万次）。改成把视口中心墨卡托与缩放缓存进  
MapView，**惰性失效**：连"缓存键"一起存，逐字段比对，所以调用方直接改  
`view.lat / view.zoom` 也不会画错——window.c 里到处是直接改字段的，要求  
"改完必须调 invalidate"迟早漏一处，漏了就是画面错位，这类 bug 肉眼很难发现。

### ★ 第四刀：小面走"画点"快速路径（src/render/paint.c）

先量了面的屏幕尺寸分布（北京城内 1280×800）：

| zoom | 面数    | <2 px          | 2–4 px |
| ---- | ----- | -------------- | ------ |
| z9   | 61618 | 42952（**70%**） | 14456  |
| z11  | 36237 | 10106（28%）     | 10695  |
| z13  | 37857 | 8489（22%）      | 13982  |

这些面填充出来最多一个像素，却要走"投影全部 cnt 个顶点 → 扫描线逐行对  
n 条边求交 → 再描一遍 n 条边"这一整套。用 geo bbox 的两个角点投影估出  
屏幕尺寸（2 次投影，不必投影全部顶点），小于 `AREA_DOT_PX`(2) 就只点一个  
像素：颜色贡献保留，开销从 O(cnt×行) 降到 O(1)。

### 效果（第二帧，北京城内 39.90,116.40）

| zoom | 本轮前    | 本轮后        | 面       | 路      | 线     | 标注 |
| ---- | ------ | ---------- | ------- | ------ | ----- | -- |
| z13  | 187 ms | **138 ms** | 98→70   | 67→46  | 13→11 | 12 |
| z12  | 169 ms | **126 ms** | —       | —      | —     | —  |
| z11  | 273 ms | **196 ms** | 159→111 | 78→55  | 31→22 | 9  |
| z9   | 445 ms | **251 ms** | 249→112 | 145→98 | 48→33 | 9  |

其中"小面画点"单独贡献（A/B 对照，AREA_DOT_PX=0 vs 2 的面耗时）：  
z9 183→112 ms、z11 126→111、z13 79→70。

### 视觉代价（A/B 像素对比，不是凭感觉）

| zoom | 差异像素   | 平均色差 |
| ---- | ------ | ---- |
| z13  | 0.768% | 53   |
| z11  | 1.006% | 36   |
| z9   | 1.528% | 31   |

按颜色统计覆盖率：z9 绿地两档合计 51152 vs 51650（差 <1%）—— 是"描边色深  
→ 填充色浅"的深浅转移，不是要素消失。

### 判别实验（省了瞎猜）

- 先怀疑"粗线画太多遍"：临时让所有路只画一遍 → z9 路 179→143 ms，只省 36 ms  
  ⇒ **遍数不是主因**，剩下的是遍历/投影，于是才去找投影。
- 先怀疑"小面多=成本高"：量完分布才发现数量占 70% 而成本只占 30%  
  （小面顶点少），收益要实测才知道，不能按数量推算。

### 已知剩余

- z9 251 ms 里路仍占 98 ms（273770 条边）。下一步是**合并连续边成折线**  
  走 `SDL_RenderDrawLines`：z9 时 1 px ≈ 305 m，一条高速被切成几十段，  
  段段 <1 px；合并后调用次数能降一个数量级，且视觉更正确（连续线而非点列）。  
  注意不能反过来"跳过短边"——那会把整条路删掉。
- fill_polygon 仍是逐行对全部 n 条边求交，改活跃边表可再降一档。

---

## 19. 帧时间第五刀：投影查表（2026-09-22）

上一刀留下的清单里点名要"合并连续边成折线"（z9 路 98 ms）。做完一轮的答案是：  
**那条路不成立，真正值钱的是把墨卡托投影本身做快**。

### 判别实验：先把<｜hy_place▁holder▁no▁813｜>成本拆开，再决定动哪里

给 paint.c 临时加位开关 `NAVIMAP_PAINT_DIAG`（bit0 不落笔 / bit1 用线性近似代替  
投影 / bit2 不设色），同一帧跑四种组合差出差值。北京 z9（273770 条边，路网 101 ms）：

| 组分                          | 原始        | 查表后   |
| --------------------------- | --------- | ----- |
| 投影（2 次/边）                   | **30 ms** | 9 ms  |
| 落笔（光栅化 + 设色）                | 33 ms     | 32 ms |
| 其余（tile 遍历 / 取节点 / 裁剪 / 去重） | 38 ms     | 39 ms |

两点由此坐实：

- 投影是 30% 的大头，不是"个位数毫秒"（第四刀时的印象在此处不成立）；
- "省调用次数"没有想象的值钱：改成 DrawLines 后逐像素成本一个没少，  
  DrawLines 本身也不比 DrawLine 便宜。

### 采纳：纬度 → 墨卡托 y 走查表插值（`nm_mercator_y_fast`）

y 只依赖纬度且足够光滑 → 首用建 2^15 格的表（256 KB），线性插值。误差账：  
世界坐标误差 = h²/8·|y''| ≈ 2.3e-10，z18 下折合 **0.016 px**，远小于取整误差。  
只给 `mv_project` 用；`nm_mercator` 精确版留给逆算等非热路径。

第二帧帧时间（1280×800，北京 39.90,116.40，地图不动的第二帧）：

| 场景      | 改前     | 改后         | 变化   |
| ------- | ------ | ---------- | ---- |
| 单省 z9   | 252 ms | **195 ms** | −23% |
| 单省 z11  | 193 ms | **156 ms** | −19% |
| 单省 z12  | 126 ms | **102 ms** | −19% |
| 单省 z13  | 143 ms | **115 ms** | −20% |
| 8 省 z9  | 853 ms | **696 ms** | −18% |
| 8 省 z11 | 288 ms | **243 ms** | −16% |
| 8 省 z13 | 200 ms | **162 ms** | −19% |

视觉代价用新的 `scripts/bmpdiff.py` 量化（不再靠肉眼）：差异像素占比  
z9 0.000% / z11 0.004% / z13 0.016%，且转移全是"相邻要素边界换了一个像素"  
（描边 ↔ 填充 ↔ 背景互相等量互换），没有任何要素消失。

### 没采纳：把连续边合并成折线

实现过一版（node → 唯一出/入边的开放寻址表 + 两遍行走保证每条边落到画面上），  
结论留在 `src/render/paint.c` 顶部的注释里，要点：

- 合并率其实很漂亮：z9 273770 条边 → 44435 条折线，平均每条 6.16 条边；
- 但帧时间变差：单省 z9 路 81 → 113 ms、z11 43 → 54 ms（+30 ms），  
  8 省同理；edges_drawn 前后完全一致（确认没有把路画丢，纯粹是更慢）；
- 原因：找"后继"要每条边 2 次哈希插入 + 多次查询，随机探表≈一次访存缺失，  
  摊下来 ~110 ns/边，超过它能省下的东西；
- 而它能省的只剩"接点被重复投影"，投影变便宜后这部分只剩 ~4.5 ms/帧。

⇒ 除非先把"找后继"变成不查表（例如边记录里带 way id + 段序号），否则别再做。

### 顺手固化的两样东西

- `scripts/regress.sh`：把散在各会话里的验收固化成一件事——8 个省各渲一帧  
  （含"无地图区域必须出提示横幅"）、跨省路线+里程区间自检、两条模拟导航、  
  `skedaddle_restr_ab` / `skedaddle_tile_xcheck` / `skedaddle_coverage_cli` / `skedaddle_search_cli`，共 21 项。  
  判据是"出现期望标记 且 无 ASSERT-FAIL 且返回值为 0"。
- `scripts/bmpdiff.py`：本机没 PIL/ImageMagick，用它做优化前后的逐像素对比 +  
  颜色转移统计（区分"像素抖动"和"要素丢了"）。

### 已知剩余（下一刀的候选）

- 单省 z9 路 76 ms 里，"tile 遍历 + 取节点 + 裁剪 + 去重"仍占 39 ms——  
  8 省时这部分会放大到 200 ms 以上，比投影更值得盯。
- 面要素才是 z9 的绝对大头（8 省 238 ms / 单省 85 ms）：给 areas 也做 tile 索引  
  （接口已就绪），以及把 `fill_polygon` 的"逐行对全部 n 条边求交"改成活跃边表。

---

## 20. 帧时间第六刀：描边/线批量化（2026-09-22）

沿用第五刀那套"位开关拆解再动手"，这次的目标换成了**面要素**——因为拆完之后  
它才是 z9 的大头。结论：面要素里真正贵的是**描边**，不是填充。

### 归因（单省北京第二帧）

| ms                      | z9     | z13    |
| ----------------------- | ------ | ------ |
| 描边（每个多边形一圈，逐条 DrawLine） | **50** | **29** |
| 扫描线填充                   | 19     | 23     |
| 顶点投影                    | 8      | 8      |
| 其余（bbox 预筛 / 去重 / 遍历）   | 12     | 2      |
| 合计                      | 89     | 62     |

零长边是哪来的：低 zoom 下一个湖泊/绿地的边界被切成几十上百个点，投影后大量相邻点  
落在同一像素上——**零长边**占比很高。两项实验各自省下（z9 / z13）：

- 跳过零长边：91→70 / 60→56
- 一个多边形合成一次 `SDL_RenderDrawLines`：91→59 / 60→42
- 两者兼施：**91→57 / 60→43**

### 采纳：`stroke_begin/push/flush/ring`（src/render/paint.c）

面多边形的描边、超 256 顶点退化为描边的大面、以及线要素（width=1 时）都改成  
"攒点 → 合并连续重复像素 → 一次 DrawLines"，超过 512 点自动分段并保证接缝点连续。

> 有意思的一点：同样的"合并 draw call"手段，第五刀用在**道路的合链**上是亏的  
> （那时还要额外查哈希找后继），这次用在**面/线的既有顶点序列**上是净赚——  
> 差别在于有没有额外的机器开销。手段本身没有对错，要看它带来多少附带成本。

### 帧时间（第二帧，1280×800，北京 39.90,116.40）

| 场景      | 第 4 刀后 | 第 5 刀（投影查表） | **第 6 刀（描边批量化）** | 相对第 4 刀 |
| ------- | ------ | ----------- | ---------------- | ------- |
| 单省 z9   | 252 ms | 195 ms      | **140 ms**       | −44%    |
| 单省 z11  | 193 ms | 156 ms      | **120 ms**       | −38%    |
| 单省 z12  | 126 ms | 102 ms      | **84 ms**        | −33%    |
| 单省 z13  | 143 ms | 115 ms      | **99 ms**        | −31%    |
| 8 省 z9  | 853 ms | 696 ms      | **561 ms**       | −34%    |
| 8 省 z11 | 288 ms | 243 ms      | **205 ms**       | −29%    |
| 8 省 z13 | 200 ms | 162 ms      | **145 ms**       | −28%    |

分项变化（单省）：面 z9 85→46、z13 57→41；线 z9 26→12、z11 17→9。

视觉代价：与第五刀的产物逐像素对比，差异 0~95 个像素（0.000%~0.009%），  
全部落在水系线段的端点上（1 px 抖动）；建筑/水面/绿地的填充与描边一张不差。

### 已知剩余（下一刀）

- 单省 z9 分解成：路 73 / 面 46 / 线 12 / 标注 8 ms。
  - 面还剩填充 19 ms 左右：`fill_polygon` 仍是逐行对全部 n 条边求交，  
    改成活跃边表（AET）可再降。
  - 线的 tile 索引仍然没有（靠 bbox 预筛兜着），8 省时和面一起放大。
- **8 省 z9 的路仍是 381 ms**，而同样 zoom 单省只要 73 ms——多出来的部分来自  
  跨省去重哈希 + 8 份nodes 数组导致的访存缺失。这个"多省放大效应"比单省继续抠  
  细节更值钱，是下一刀的首选。

---

## 21. 帧时间第七刀：查了"多省放大"，结论是**得改数据格式**（2026-09-22）

第六刀之后 8 省 z9 仍是 561 ms，其中路 381 ms；而同样的视口单省北京只要 73 ms。  
先按老规矩做判别实验，结果把原假设推翻了，也把剩下该做什么钉死了。

### 判别实验一：1/2/4/8 省逐级加载

- 用**硬链接**构造只含 1/2/4/8 省的临时图集（★ 不能用 symlink：mapset 会解析成真实  
  路径，结果 8 省全加载，第一次实验就是这么白做的）。
- 结果：单河北一省就要 379 ms / 558839 条边，和 8 省几乎一样。  
  ⇒ **不存在"多省放大效应"**，是河北自己的数据比北京大一截（11.1M 边 vs 2.8M）。  
  之前"8 省 vs 单省"的对比其实比的是"河北 vs 北京"。

### 判别实验二：河北单省 z9 的耗时拆解（路网 469 ms / 558839 条画出边）

| 组分                                      | ms      | 怎么量的          |
| --------------------------------------- | ------- | ------------- |
| 落笔                                      | 85      | 关掉绘制          |
| **随机读 nodes.bin（每边 2 次）**               | **195** | 把节点读改成读同一个热地址 |
| 投影                                      | 39      | 用线性近似代替查表版投影  |
| 其余（edgelist 扫描 + 读 edges.bin + 去重 + 裁剪） | ~150    | 差值            |

再加上 tile 路径的各级过滤计数（北京单省 z9，同一帧）：

```
扫描 2880726  →  等级不够 2512718（87%）→ 反向 94238  →  视口裁剪 0  →  画出 273770
```

⇒ 两个事实：**① 87% 的扫描条目是为了查等级而读一次 edges.bin，然后被丢掉；  
② 真正画出来的每条边要随机读两次 nodes.bin，这一项最贵。**

### 三种不改数据格式的办法都试过，全都不成立（已撤掉代码）

| 办法                                             | 结果                  |
| ---------------------------------------------- | ------------------- |
| 三级软件预取（未来的 edge 记录 / from 节点 / to 节点，深度 48/24） | 455 → 452 ms，无效     |
| 缓存"上一条目的起点"（盼 edgelist 按节点分组）                  | 命中 388/27 万 ≈ 0，无效  |
| 16K 槽的节点坐标小缓存                                  | 460 → 487 ms，**更慢** |

⇒ 这不是能靠预取/缓存藏住的延迟。要在渲染时少付费，只能**不碰 nodes.bin**。

### 下一刀（需要改数据格式 + 重跑 sync_maps，所以先问再做）

给 tile 索引的条目（edgelist.bin）补齐绘制所需的全部字段，让渲染**只读顺序数据**：

```
现在:    { node_idx, edge_idx }                → 每条目再随机读 edges.bin / nodes.bin
改成:    { from_lat, from_lon, to_lat, to_lon, cls, flags }  (+ edge_idx 供路名用)
```

- 顺带解决"87% 白读"：等级就在条目里，不必为了查它去读 edges.bin。
- 跨省去重改为用"两端量化坐标的有序对"当身份键（同一条路在两省里坐标逐位相同），  
  不必再读节点的 OSM id。
- 粗估：河北 z9 路 469 → ~140 ms（扫描变顺序、省掉 2.88M 次 edges.bin 随机读和  
  1.14M 次 nodes.bin 随机读）。面/线的 polyrefs 是同一类问题，可一并处理。
- 代价：edgelist.bin 约 ×2.5（河北 92 MB → ~270 MB，8 省合计 +1.5 GB 左右），  
  以及重跑 8 省提取；渲染端要按版本号兼容旧目录。

---

## 22. 帧时间第八刀：edgelist v2 —— 让渲染只读顺序数据（2026-09-22）

第七刀的三种"藏延迟"办法全灭之后，唯一剩下的路就是**不访问**。本节记录把 tile  
索引条目扩成自带绘制字段的全过程、实测数字，以及它暴露出来的下一个瓶颈。

### 改了什么

```
v1（8B）  { node_idx, edge_idx }
v2（28B） { from_lat, from_lon, to_lat, to_lon, flags u16, cls u8, pad,
            edge_idx, node_idx }
```

- 渲染端（paint.c）按 `NMBinHeader.rec_size` 分叉：8 = v1 走旧路径，28 = v2 走  
  新路径；v2 **一次都不碰 nodes.bin**，edges.bin 只在 z≥15 取路名时读  
  （那时视口里就几百条边）。
- 跨省去重的身份键从"两端 OSM node id"改成"两端 e7 坐标的有序对"（coord_key_e7），  
  与 coordkey.h 里跨省缝合的口径统一。
- ★ 28 而不是 24 字节：留了 node_idx。edgelist 不只给渲染用——跨省缝合  
  （route.c sew_cb）要 from 节点下标去展开，重复省检测（mapset.c）要拿条目采样。  
  让正确性路径退化成"再随机读一次 nodes.bin"去省 4 字节不划算。
- 读取端统一出口 `xl_from_key()` / `xl_entry()`（mapset.c），三条非渲染路径都不用  
  知道目录是哪一版。

### 两个兼容性决定

1. **NMBIN_VERSION 2 → 3，强制全量重抽。** 虽然读取端能同时认两版、旧目录也能  
   正常画（走慢路径），但混合图集要不得：一半省 v1、一半省 v2 时体积和帧时间  
   都不一样而画面完全一样，出问题没人会想到版本混装。
2. **v1 路径的去重键也改成坐标。** 否则混合图集里两套键互不相认，跨省重复路会被  
   画两遍（这不是性能问题，是画错）。

### 实测（第二帧 1280×800，改前 / 改后各跑一遍同一组视口）

| 用例       | 帧 v1 → v2     | 路 v1 → v2           |
| -------- | ------------- | ------------------- |
| 单省北京 z9  | 142 → **119** | 75 → **52**（−31%）   |
| 单省北京 z11 | 122 → **109** | 41 → **28**（−32%）   |
| 单省北京 z13 | 100 → **97**  | 37 → **32**（−14%）   |
| 单省河北 z9  | 295 → **251** | 158 → **113**（−28%） |
| 单省河北 z11 | 137 → **112** | 40 → **28**（−30%）   |
| 8 省 z9   | 551 → **464** | 370 → **284**（−23%） |
| 8 省 z11  | 196 → **166** | 97 → **67**（−31%）   |
| 8 省 z13  | 145 → **131** | 68 → **55**（−19%）   |

验收：8 个视口 **逐像素 0 差异**（比前几刀的"几十像素抖动"更干净——几何数据  
一格没动，只是搬了位置）；`regress.sh` 21/21；编译零警告。

代价：edgelist 328 MB → 1.14 GB（×3.5，8 省合计 +812 MB）；`products/graph`  
3.0 G → 3.8 G；`sync_maps.sh --force` 全量重抽 2 分钟。

### 归因：剩下的路网时间花在哪（8 省 z9，路 284 ms）

用临时位开关（关掉某个算子，看省多少）拆的：

| 组分                 | 河北单省 z9（112 ms） | 8 省 z9（293 ms） |
| ------------------ | --------------- | -------------- |
| 落笔（SDL 画线）         | 74 ms（66%）      | 70 ms          |
| 投影                 | 21 ms           | 64 ms          |
| **跨省去重哈希**         | **0**（单省不启用去重）  | **95 ms（33%）** |
| SetRenderDrawColor | 2 ms            | 6 ms           |
| 其余（扫描/解码/裁剪）       | 15 ms           | 58 ms          |

⇒ 多省场景里**去重已经超过落笔成了第一大头**：1.15 M 次 128 位键的开放寻址  
探表（589 K 条被判重复——各省 pbf 按多边形裁切，北京/天津整块被河北包进去）。  
单省时去重压根不启用（window.c 里 ≥2 个省盖住视口才开）。

### 下一刀候选

1. **整省被包含的省直接不画**（`dup_of >= 0`）。路由早就这么干了  
   （route.c 里 `if (src->dup_of >= 0) continue`），渲染还在老老实实扫它、  
   再靠去重把 589 K 条判掉。判据是现成的（≥99% 采样命中），风险是那 1%  
   未命中的边会消失——要先量"只加载河北"和"只加载北京+河北"在同一视口的  
   像素差，确认是 0 再动。
2. 去重键降成 64 位（单键哈希），省一半探表访存；碰撞概率 ~1e-8，最坏是丢  
   一段 1 px 的路。
3. 落笔 70 ms：z<12.5 时路宽都是 1，且缩放比下大量路段不足 1 px，可以攒成  
   `SDL_RenderDrawPoints` 批（按等级分 5 个数组，颜色只设 5 次）。

---

## 23. 帧时间第九刀：多省渲染的"重复劳动"整块砍掉（2026-09-22）

第八刀把路网从 370 降到 284 ms 之后重做归因，8 省 z9 路网 272 ms 的构成是：

| 组分         | ms      | 怎么量的                          |
| ---------- | ------- | ----------------------------- |
| **跨省去重哈希** | **116** | 关掉去重（多画的 48 万条抵消掉约 40 ms 后反推） |
| 落笔         | 75      | 关掉绘制                          |
| 投影         | 55      | 坐标换成常数                        |
| 扫描 + 裁剪    | 28      | 三个都关                          |

去重从"没上榜"直接变成第一大头。而它防的 48 万条重复，全部来自**北京/天津  
整块被河北包进去**——也就是说，一半的扫描、投影、探表是在处理注定被丢掉的东西。

### 措施一：整省被包含的省，渲染直接跳过（`dup_of >= 0`）

路由早就这么干了（`route.c` 里 `if (src->dup_of >= 0) continue`），渲染还留着它。  
三条理由，其中 ② 是专门去验证过的：

1. 判据现成：bbox 完全包含 + 节点数不多于对方 + 抽样 1000 个节点 ≥99% 命中  
   （本图集实测北京、天津都是 1000/1000）。
2. **"包含方是超集"必须实测，不能靠"bbox 包含"推断**。拿同一视口对比两省单省  
   渲染：`z13 / z15 逐像素 0 差异、边数面数完全相同`；`z11` 河北反而比北京多画  
   （差 8282 px，转移全是"背景 → 要素色"，即河北更全，没有反向丢失）。  
   三个视口 + 天津都是这个结论 ⇒ 跳过被包含的省不会丢东西。
3. 顺带统一分级口径：以前同一条路由"图集里排前面的省"说了算，跨省界会出现  
   同等级路两种颜色。

### 措施二：去重只在"这一格瓦片有 ≥2 个省"时做

跳过后剩下的重复只有 3 万条（省界交界），却要为 55 万条边各付一次探表。所以给  
`paint_roads` 传一份"各省 tiles.bin"，按格子数覆盖数，`cov < 2` 就把去重表摘掉：

```c
DedupSet *ddt = dd;
if (ddt && n_peer > 0) {
    int cov = 0;
    for (int p = 0; p < n_peer && cov < 2; p++)
        if (tile_cnt_at(peer[p], tx, ty)) cov++;
    if (cov < 2) ddt = NULL;
}
```

单格里同一条边不会登记两次，跨省的重复两边落在同一格，所以这是**等价判据**，  
不是近似。

### 措施三：面/线要素不去重（22 ms，0 像素差异）

面/线的填充和描边全是不透明色（代码里所有 `SetRenderDrawColor` 的 alpha 都是  
255），同一条要素被两省各画一遍落在同样的像素上，去重纯粹是白花哈希钱。实测  
关掉面/线去重：8 省 z9 一帧省 22 ms、逐像素 0 差异。于是 `paint_areas` /  
`paint_lines` 的 `DedupSet *dd` 参数直接删掉。只有路网需要去重——因为同一条路  
在两省里的**分级可能不同**，不判重会出现"谁后画谁说了算"的花色。

### 顺带：去重槽改成 64 位指纹 + 键与世代同一槽（16 字节）

原来 hi/lo/gen 是三个平行数组（1M 槽时 16+16+8 MB），一次探表最多摸 3 条 cache  
line；合成一个 16 字节槽后只摸 1 条。实测省 20 ms。碰撞概率 ≈ N²/2^64 ≈  
3.6e-8/帧，中一次也只是少画一段 1 像素、下一帧就恢复。

**没成立的两个想法（别再试）**：

- 钉小去重表（1 MB 而不是 16 MB）：微基准里单次探表 32 → 11 ns，但现场只省  
  18 ms——热路径上 cache 早被 29 MB 的 edgelist 顺序流冲掉了，微基准高估了 3 倍。
- 投影里 `/1e7` 换成 `* 1e-7`：0 收益（GCC 已经把它优化掉了），0 像素差异，已撤掉。

### 实测（第二帧 1280×800）

| 用例              | 刀8 后       | 刀9 后       | 路网分项          |
| --------------- | ---------- | ---------- | ------------- |
| 8 省 z9          | 472 ms     | **250 ms** | 284 → **111** |
| 8 省 z11         | 171 ms     | **113 ms** | 66 → **28**   |
| 8 省 z13         | 131 ms     | **97 ms**  | 56 → **31**   |
| 单省北京 z9/z11/z13 | 119/109/95 | 120/108/94 | 不变（单省本就不启用去重） |
| 单省河北 z9/z11     | 250/113    | 248/113    | 不变            |

### 验收

- 8 个视口与改前对比：z9 463 / z11 332 / z13 2069 像素差异（0.045%~0.2%），  
  转移**全部是路色↔路色**（分级不同），没有任何"要素色 → 背景色"，即没有丢要素。
- ★ 新不变量：**多省加载的画面 == 只加载包含方的画面**（8 省 vs 只河北，z11/z13  
  都是 0 像素差异）。以前两省重叠区是"图集里排前面的省"说了算，现在没有仲裁了。
- `regress.sh --build`：21/21，编译零警告。

### 下一刀

现在 8 省 z9 的路网（111 ms）已经和单省河北（110 ms）一样，构成也一样：  
**落笔 ~74 / 投影 ~21 / 其余 ~15**。也就是说多省这条路已经挖到底，再往下是  
单省也有的开销：

1. 落笔 74 ms：z<12.5 时路宽恒为 1，且该缩放比下大量路段不足 1 px，可以攒成  
   `SDL_RenderDrawPoints` 批（按等级分 5 个数组，颜色只设 5 次）。
2. 面 107 ms（8 省 z9）：上一轮只做了描边批量化，填充仍是逐面扫描线。
3. 投影 21 ms：可以试着把"经度 → 屏幕 x"的线性部分摊进瓦片循环外的常量。

---

## 24. 帧时间第十刀：落笔与面要素的归因（2026-09-22）

刀9 之后 z9 路网降到 111 ms，重新按算子拆，得到的新排名是：**去重没了、  
落笔 74、投影 55、扫描 28**。本节沿着"落笔"往下挖，最后挖到的其实是  
**面要素**（它才是当前单省 z9 的第一大头），并记录 9 个不成立的做法。

### 采纳：退化成单像素的路段，紧邻同像素同等级就跳过

先量线段在屏幕上的长度分布（8 省 z9，588302 条/帧）：

| max( | dx        | ,     | dy   | )    | 0    | 1 | 2 | 3 | ≥4 |
| ---- | --------- | ----- | ---- | ---- | ---- | - | - | - | -- |
| 占比   | **67.2%** | 28.4% | 3.1% | 0.8% | 0.5% |   |   |   |    |

也就是说 z9 下**三分之二的路段两端投影到同一个像素**，95.6% 不超过 1 像素。  
于是：若当前段退化成单像素、且紧邻的前一条落在同一像素、等级也相同，就跳过  
不画（一帧跳掉 12.1 万条，占 20.6%）。

★ **只认"紧邻的前一条"是刻意的**。先做的是逐像素记录表（每像素记"本帧最后  
写它的路等级"），看起来是构造上等价的，实测却冒出 16 个像素差异——原因是  
**非退化线段穿过某像素时不会更新记录表**，于是同等级的后续点被误判为"已写过"  
而跳过，正好落在两条不同等级的路交叉处换了赢家。改成只认紧邻前一条后，  
中间不可能插进别的等级，等价性才真正成立。

### 实测（第二帧 1280×800）

| 用例            | 刀9 后     | 刀10 后         |
| ------------- | -------- | ------------- |
| 单省北京 z9       | 120      | **110**       |
| 单省河北 z9       | 250      | **230**       |
| 8 省 z9        | 250      | **228**       |
| 8 省 z11 / z13 | 114 / 97 | 112 / 98（噪声内） |

路网分项：z9 111 → **88 ms**；z11/z13 基本不动（z11 只有 36.8% 退化段、  
z13 因为路宽 >1 压根不走这条路径）。8 个视口**逐像素 0 差异**，regress 21/21。

### 面要素的新分解（这才是剩下的大头）

8 省 z9 面 108 ms，构成（用临时开关逐个关掉量）：

| 组分                     | ms  |
| ---------------------- | --- |
| 填充（扫描线求交 + 逐行画 span）   | 38  |
| 描边                     | 29  |
| 取顶点（nodes.bin 随机读）+ 投影 | 35  |
| 循环 / bbox 甄别           | ~10 |

一帧扫 585678 个面条目，其中 **39.1 万个是建筑、在 z<13 直接跳过**（67%）；  
真正处理 113932 个（38003 走"画点"快路径、75538 走全量），**154 万个顶点**。  
"取顶点+投影"35 ms 是用"把顶点循环跑两遍"量出来的：第二遍（数据已在缓存）  
23 ms、第一遍 35 ms，差 12 ms 才是真正的冷访存成本——所以就算把坐标内联进  
polyrefs，上限也只有 ~12 ms，不值得为此再动一次数据格式。

### 九个不成立的做法（别再试）

| 做法                          | 结果                                                                   |
| --------------------------- | -------------------------------------------------------------------- |
| 攒 `SDL_RenderDrawPoints` 批  | 平均批长只有 **1.2**（edgelist 里同等级的点并不连续），省 8 ms 却带来 803 px 的"谁后画谁赢"变化 → 撤 |
| 逐像素记录表做去重                   | 非退化线穿过像素不更新记录表 → 16 px 换了赢家 → 撤                                      |
| 退化段改用 `SDL_RenderDrawPoint` | 与 `DrawLine` **像素完全相同**（0 差异），但只省 7 ms                               |
| 退化段整段不画                     | 省 46 ms，但丢 112 px（证明 SDL 的零长线确实画了一个像素）                               |
| `lround` 换内联取整              | 0 收益（GCC 已内联），0 差异                                                   |
| `(lon+180)/360` 换乘倒数        | 0 收益（GCC 已优化），0 差异                                                   |
| 顶点节点 `__builtin_prefetch`   | 反而慢 6 ms                                                             |
| `fill_polygon` 按视口裁剪扫描行     | 0 收益（z9 视口外的行本来就少）                                                   |
| 填充改整数扫描转换                   | 0 收益 + 4756 px 差异（舍入变了）                                              |

★ 由此得到一条对 SDL 软渲染的认识：**每次绘制调用的固定开销约 100 ns，和画  
多少个像素关系不大**。"换成更便宜的画法"几乎没用，只有"不画"才省得下来。

### 待你定夺：面顶点按屏幕尺寸抽稀（LOD）

面的成本几乎全部与顶点数成正比。按屏幕外接框抽稀到 12 个顶点（只对外接框  
≤ N px 的面生效，大面不动）：

| N  | 面 108 →    | 像素差异        |
| -- | ---------- | ----------- |
| 2  | 103（−6 ms） | 1705（0.17%） |
| 4  | 94（−15 ms） | 8864（0.87%） |
| 6  | 88（−21 ms） | 15131（1.5%） |
| 8  | 84（−25 ms） | 19131（1.9%） |
| 16 | 79（−30 ms） | 28111（2.7%） |

差异方向**全部是"填充色 ↔ 描边色"互换**（小多边形的轮廓变粗/变细），没有  
"要素色 → 背景色"，即没有丢要素。但 1.9% 的像素变化比前几刀的验收标准  
（≤0.2%）高一个量级，是"画面细节换帧时间"的产品取舍，所以我没有擅自采纳，  
实验代码已撤掉。N=2 的那档（0.17%、−6 ms）落在既往标准内，可以直接要。

---

## 25. skedaddle_places_build —— 补上地名 / POI 抽取（2026-09-23，首轮落地）

### 25.1 为什么必须有它

`graph_build` 在 pass1 里**一个 node 都不留**（只读 ways / relations），而地名与 POI  
恰恰挂在 node 上（`place=city/town/village`、`amenity`、`tourism`、`shop`…）。所以：

- 以前那份 `products/places_auto.db` 是旧工具留下的存量，**生成器随源码丢了**；
- 新下载一张图 → 有路网、有路名（`names.bin`），但**永远补不上地名和 POI**；
- 表现不是报错，是"搜索安静地变差、地图安静地变空"。这类缺口只有回头盘点才看得见。

### 25.2 工具形态

`tools/places_build.c`（已进 CMake target `skedaddle_places_build`）：

```
--sources=sources --db=products/places_auto.db [--meta=…] [--verbose]
--pbf=sources/beijing.osm.pbf --db=/tmp/bj.db
--db=<已建好的库> --selftest
```

两遍扫描，与 graph_build 同套路：pass1 抽节点 + 记下"有名字的闭合 way"引用的节点，  
pass2 只回填这批节点的坐标（排序 + 二分），**不建全图索引**。  
8 省全量 38 s（place 387,540 / gram 1,536,778），单省北京 3.5 s / 46,494 条。

产出 schema 与旧库存量完全一致（`place(id,name,lat,lon,kind,weight)` +  
`gram(gram,place_id) WITHOUT ROWID` + `place(lat,lon)` 索引），**加载端一行没改**。

★ bigram 一律走 core 里的 `search_bigrams()`，不另抄一份。两边各写一遍迟早分叉，  
分叉的表现是"某些词搜不到、但又看不出规律"——这是最难查的一类 bug。

### 25.3 kind 表与两个刻意的选择

| kind | 含义                        | 备注          |
| ---- | ------------------------- | ----------- |
| 1    | 省会 / 直辖市                  | 5 留空档，与旧库一致 |
| 2    | 地级市                       |             |
| 3    | 县 / 镇 / 乡                 |             |
| 4    | 城市片区（街道、开发区）              |             |
| 6    | 村                         |             |
| 7    | 自然村 / 屯                   |             |
| 9    | 小区 / 街区                   |             |
| 10   | 自然地物（山峰 / 水体 / 岛）         |             |
| 11   | 有名字的设施（车站 / 学校 / 景区 / 商场） |             |
| 12   | 其他有名字的地物（桥梁 / 堤 / 城墙）     |             |
| 13   | **路名**（只进搜索，永不画）          | 见 §25.6     |

- **不收 `office=*` 与公司名**：几百家"XX研究所"不是地名。
- **黑名单先于判定**：`bus_stop` / `crossing` / `street_lamp` / `ATM` / `vending_machine`  
  / `toilets` / `parking_space` / `tree` / `power_tower`… 以上合计过滤掉 40 万量级。  
  这一步是看着北京的数据逐个审出来的：不过滤的话公交站一项就灌进来 2.3 万条。
- **纯阿拉伯数字编号的名字不要**（"226"、"551"，那种是公交站/线路的内部编号）。

### 25.4 ★ 数据坑：中国的 `place=city`，以及没有 place 标签的省城

写第一版时北京抽出来没有"北京市"，16 个 `place=city` 全是市辖区（`capital=6`）。  
怀疑是过滤误杀，于是临时写了个探针直扫 PBF（见 [§25.9](#259-临时探针scan_placec)），  
结果 node 25248662 的完整 tag 是这样的：

```
admin_level=2  capital=yes  name=北京市  importance=international  (一堆 name:xx)
```

**没有 `place=*`**。而它下辖的区反而都有 `place=city capital=6`。  
⇒ 只认 `place=*` 会把全省最重要的一条地名丢掉，并且因为 `kind_max_for_zoom()`  
在 z<6 只画 kind<=1，直接导致低缩视图**一个地名都不剩**。

修法是加一条 fallback：`capital=yes|2|4` 且 `admin_level<=4` → kind 1；  
`admin_level<=2`（首都）权重 320，压过省会（300）一点点。  
`admin_level` 6/8（区县、乡镇）不在这里收，那是 `place=city capital=6` 已经覆盖的范围。

### 25.5 ★ 与旧存量库的 A/B：标注变好了，但丢了一个能力

两库覆盖同一范围（旧库 226,285 条全部落在我们这 8 省的 bbox 内），所以是直接可比：

| 视口  | 旧库（按 weight 排前几条）                      | 新库                      |
| --- | -------------------------------------- | ----------------------- |
| z13 | 朝阳公园 / 地铁 12 号线 / 榴乡桥…（POI 权重虚高到 352）  | 北京市 / 朝阳区 / 西城区 / 各乡    |
| z9  | 北黄路 / 418 县道 / 潭王路…（**路名**，w 500~1100） | 北京市 / 唐山市 / 张家口市 / 廊坊市  |
| z6  | 西镇高速 / 济新高速…（全是高速名）                    | 济南 / 太原 / 石家庄 / 沈阳 / 北京 |

⇒ 标注这头**新库明显更合理**：旧库把路名塞进 kind 1/3/4/8/12 且权重虚高  
（一条县道 1112，压过所有行政区），新库恢复成行政层级优先。

但同一张表也暴露了唯一的倒退：**旧库那 2.7 万条 kind 8 / 12 其实就是路名**，  
于是「中关村大街」能搜到（kind 3），新库只剩"中国邮政中关村大街邮政支局"。

### 25.6 路名：kind 13 —— 只进搜索，永不画

补回来的办法是给路名一个专门的档位：

```c
PLK_ROAD = 13,   /* > labels.c kind_max_for_zoom() 的上限 12 ⇒ 标注查不到它；
                    app_search_query() 传的 kind_max 是 0（不限）⇒ 搜索照样命中 */
```

一个字段解决两个需求，`labels.c` 和 `search.c` 一行都不用改。

- 原材料：8 省共 **53.6 万条**带名字的 highway way（北京 6.3 万 / 河北 14.6 万 /  
  山东 11.2 万 / 河南 6.7 万…），按名字归并后 **75,998 条**。
- **不攒整条路的顶点**：代表点取 `refs[n/2]` 一个节点就够了 —— 街名只要落在"这条街上  
  某个点"，差几十米不影响用户找得到它。全量攒 refs 是几百 MB 内存换一堆用不上的精度。
- ★ **去重范围必须是"同城"而不是"全局同名"**：第一版按名字全局去重，结果搜"长安街"  
  给你报到河北（40.37,116.84）去，因为建库顺序上河北先遇到它，而它真正所在的城市  
  一个点都不剩。改成**名字指纹 ^ 1°×1° 格**（约 110 km，一个城市的量级）之后：  
  「解放路」保留 43 个城市各一条、「建国路」北京那条回到 39.91,116.47。

成品（8 省，39 s）：place **463,319** / gram **1,766,113**，库 60 MB（旧库 30 MB）。  
标注层完全不受影响（`kind<=12` 的 SQL 验过 z6/z9/z13 三档）。

### 25.7 已替换旧库并接入自动链路

- `products/places_auto.db` 已换成 skedaddle_places_build 产出的库；旧的那份留在  
  `products/places_auto.db.pre-places-build.bak`（含 .meta），随时可换回去。
- `scripts/sync_maps.sh`：各省循环之后多一步 places。判据是 sources 的字节数 / mtime  
  全对上才跳过；建库先落临时文件，`--selftest` 过了才 mv 就位（地名库是运行时  
  sqlite3 直接打开的，绝不能留下写了一半还能打开的库）。实测：删掉 meta 让它重建  
  → 42 s 出 `[ OK ]`，之后连续两次 `[SKIP ]`。  
  ★ 两个写错就出问题的细节：meta 记的是**带下标**的键（`src_bytes[0]`）且键值之间是  
  `= `，跟各省 manifest.txt 不是一套；脚本传的是绝对路径，而直接跑 skedaddle_places_build 传  
  相对路径时记的是相对路径 —— 比对前必须两边都剥一次 `$ROOT/`。
- `scripts/regress.sh`：新增 `places_selftest`（第 22 项）。**这条判据是踩出来的**：  
  加上路名那一刻，库里 5.7 万条被自检判为非法，而当时 21 项里没有一项会报警  
  （地图照画、路照通，只有自检知道）。

### 25.8 清理 3.0 GB 残留

`products/graph/*.old.<pid>` 8 份 + 8 个 `*.tmp.*.check.log` 已清（56 G → 59 G 可用）。  
依据：`src/map/mapset.c:376-377` 明确跳过 `.tmp.` / `.old.`，不会被加载。  
清单存证在 `products/OLD-RESIDUE-MANIFEST.txt`。  
删除必须用 `/bin/rm` —— 本机 `rm` 被 safe-delete 包过（只改名，返回值还是 0），  
用它会"删完了但东西还在"。

### 25.9 临时探针 scan_place.c

放 `/tmp`，不入库。用 repo 自己的 pbf API 直扫 PBF，支持三种模式：  
数 `place=*` 取值分布 / 按精确名字找节点 / 按 OSM id dump 全部 tag。

```bash
gcc -O1 -I src -I build/generated -I $HOME/.local/usr/include \
    -o /tmp/scan_place /tmp/scan_place.c build/libnavimap_core.a \
    -L$HOME/.local/usr/lib/aarch64-linux-gnu -L/usr/local/lib \
    -lz -lsqlite3 -lm -lprotobuf-c
export LD_LIBRARY_PATH=$HOME/.local/usr/lib/aarch64-linux-gnu:$HOME/.local/lib:/usr/local/lib
```

（`protobuf-c` 装在 `~/.local`，默认 include/lib 路径都看不见它。）  
「这条数据到底有没有这个 tag」拿不准时，先直扫一遍原始数据再写判定 —— 比猜快一个  
数量级；§25.4 那个坑就是这么挖出来的，而不是反复改分类器去试。

---

## 26. 帧时间第十一刀：面填充改成活动边表（2026-09-23）

起点（8 省 1280×800 第二帧）：**z9 225 / z11 109 / z13 93**，面要素 z9 占 106 ms。  
面已经是第一大头，所以这一刀只问一件事：**这 106 ms 里每一项各值多少**。

### 26.1 归因（判别实验，见 §2 的做法）

临时位开关 `NAVIMAP_FILL_DIAG`，量完即删：

| 关掉 / 替换掉的东西       | 面耗时    | 归因                                            |
| ----------------- | ------ | --------------------------------------------- |
| 全开（基线）            | 107 ms | —                                             |
| bit0 不真的画 span    | 96 ms  | **SDL 画线调用 = 11 ms**（一帧仅 8.2 万个 span）         |
| bit1 不做扫描线求交      | 71 ms  | **填充合计 = 36 ms**                              |
| bit2 不描边          | 87 ms  | **描边 = 20 ms**                                |
| bit3 墨卡托 y 换成线性近似 | 91 ms  | **非线性投影 = 16 ms**                             |
| bit5 每个顶点多读一个远处地址 | 113 ms | **一次随机访存 ≈ 6 ms**                             |
| —                 | —      | 余下 ≈ 31 ms（遍历 58 万个面记录 + 小面打点 + polyrefs 顺序读） |

### 26.2 ★ 两次被自己的替换版骗了

1. **"取顶点+投影值 55 ms"是假的。** 第一版 bit3 把投影换成了 `la>>8`、`la>>8`  
   这种"廉价坐标"，结果坐标跑到屏幕外 → 扫描线直接 `return`、描边被去重吃光，  
   **填充和描边的钱也被一起算进了"投影"**。这正是 §2 那条"替换版要保持下游工作量"  
   的规矩，不是空话。改法：x 本来就是线性的（只换 y），并且用视口中心的屏幕空间  
   系数做线性近似，坐标仍落在视口内 —— 量出来投影只值 **16 ms**。
2. **"span 只有 8.2 万个，批量化一定赚"也是假的。** §5.9 说"只有不画才省"，  
   但实测 SDL 调用只占填充 36 ms 里的 11 ms，剩下 **25 ms 是"每行都扫一遍全部  
   n 条边"的算术**。所以方向不是省调用，是**少扫边**。

### 26.3 采纳：活动边表（AET）

原来 `fill_polygon` 是：

```c
for (y = ymin; y <= ymax; y++)
    for (i = 0; i < n; i++)          /* 每一行都把全部 n 条边重扫一遍 */
        if (与这一行相交) 记交点;
```

改成：先把边按 `ylo` 排序，逐行增量维护活动集合，**每行只算真正与这一行相交的那几条**。

★ 交点公式、半开区间 `[ylo, yhi)`、插入排序、跨度画法**一个字都没改**，  
所以这不是"画面换速度"的取舍，是纯赚 —— 验收也是这么要求的：

| 场景      | 改前     | 改后         | 差异像素  |
| ------- | ------ | ---------- | ----- |
| 8 省 z9  | 225 ms | **205 ms** | **0** |
| 8 省 z11 | 109 ms | **98 ms**  | **0** |
| 8 省 z13 | 93 ms  | **90 ms**  | **0** |
| 单省北京 z9 | 107 ms | **95 ms**  | —     |

三档缩放**逐像素 0 差异**，比"≤0.5%"的既往验收标准还硬。`regress.sh --build` 22/22 零警告。

顺带把两条顶点循环（小面 / 超大面）收敛成一个 `project_ring()`，行为不变  
（越界整条跳过、小面上限 256 顶点都与原来一致）。

### 26.4 否决：投影之前按像素抽稀顶点（有数据，别再试第二遍）

思路本来很诱人：面尺寸分布实测（z9，走完整管线的面 11.4 万个）——

| 屏幕最大边长 | 占比               | 累计    |
| ------ | ---------------- | ----- |
| ≤1 px  | 66.7%（这些走"小面画点"） | 66.7% |
| 2 px   | 17.8%            | 84.5% |
| 3 px   | 7.1%             | 91.6% |
| 4 px   | 3.3%             | 94.9% |
| ≥5 px  | 5.1%             | 100%  |

**91% 的完整管线面 ≤ 5 px**，却要走"投影全部顶点 → 每行求交 → 描 n 条边"一整套。  
上一轮试过"投影**之后**按固定个数抽稀到 N 个顶点"（PLAN.md §24），只省了填充+描边、  
省不到投影，N=2 才 −6 ms。所以这次改成**投影之前**用地理坐标与"一个像素"比整数先筛掉，  
期望把投影/填充/描边三项一起省 —— 实测确实快，但**画面不能接受**：

| 阈值 K（像素） | z9 帧      | 面  | 差异像素  | 最大转移方向                                    |
| -------- | --------- | -- | ----- | ----------------------------------------- |
| 0.5      | ~220（噪声内） | 97 | 1.30% | 要素 → 背景                                   |
| 1        | **182**   | 68 | 3.41% | 绿地(170,214,178) → 背景(234,241,234) 8396 px |
| 2        | 181       | 67 | 5.62% | 同上 15549 px                               |

K=1 能省 38 ms，但 z9 一帧丢掉**约 10% 的绿地面积**（8.5 万 → 少 8.4 千像素），转移  
方向是单方向的"要素色 → 背景色"—— §5 的判据里这是**丢要素**，不是"轮廓粗细变化"。  
原因：抽稀是"削角"，小面的每个凸角被削掉就直接少一块面积，而且小面本来就只有几个像素。

⇒ 实验代码已撤。它和上一轮那个 N=2（−6 ms / 0.17%、方向是"填充↔描边"互换）不是  
一回事：那档还能算取舍，这档是丢东西。**要速度就得接受丢面积，这得由产品决定，  
不由做优化的人决定。**

### 26.5 这一刀顺带否掉的一个大改动

"把顶点坐标内联进面索引以避免随机读 nodes.bin"（§5.5 那条最后手段）：  
bit5 量出**一次随机访存只值 6 ms**，也就是这个改动的上限就这么点，  
却要动一次数据格式 + 全量重建 8 省。**先量上限再决定要不要动格式**（§5.11）——又应验一次。

### 26.6 下一刀的候选（按当前归因排序，z9 面还有 91 ms）

1. 余下的 31 ms：遍历 58 万个面记录 + 小面打点（7.5 万个 `DrawPoint`）。  
   面要素没有 tile 索引，z9 视口几乎覆盖全省，空间索引未必有收益；打点可以按  
   颜色分批，但会破坏压盖顺序（§5.9 的 803 px 教训），要量过才知道。
2. 填充剩约 20 ms（span 绘制 + 排序）。
3. 描边 20 ms、投影 16 ms —— §5.9 说过"只有不画才省"，可动的地方不多。

---

## 27. 参考 OsmAnd 的性能算法：一次盘点与两处落地（2026-09-23）

参考对象是 `/home/along/osmand-app`（OsmAnd 完整仓库，8.6 G / 37 万文件）。  
本节先记"它到底有什么"，再记"哪些能搬、哪些不能搬、搬了哪些"。

### 27.1 OsmAnd 的性能手段清单

| 手段                                               | 位置                                                                         | 本项目现状                                                         |
| ------------------------------------------------ | -------------------------------------------------------------------------- | ------------------------------------------------------------- |
| **坐标键当搜索主键** `id=(x31<<31)\|y31`                 | `RoutePlannerContext.cpp:132`                                              | **早就搬了**（`route.h` 注释里写了出处）。这是本项目跨省算路能跑起来的前提：各省互相包含时同一路口不重复展开 |
| **双向 A*** + 双堆交替 + 相遇点检查                         | `RoutePlanner.cpp:492-600`                                                 | 已有（`heap[2]`、`best_cut`、接缝转向补查）                               |
| **加权 A*** `f = g + w·h`                          | `RoutePlanner.cpp:746` `roadPriorityComparator`                            | **本次搬了** ↓ §27.2                                              |
| **启发系数可配置**                                      | `RoutePlannerContext.cpp:27` 从 routing.xml 读 `heuristicCoefficient`，默认 1.0 | 本次加 `--heur-w=`                                               |
| **HH 层次路由**                                      | `core-legacy/native/src/hhRoutePlanner.cpp`（1519 行）                        | 未搬，见 §27.4                                                    |
| **构建期多级几何 LOD**：每级 zoom 存一份 Douglas-Peucker 简化几何 | `IndexVectorMapCreator.java:400-436,844-853`                               | 未搬，**简化本身实测不成立** ↓ §27.3                                      |
| **视口外几何简化**                                      | `Utilities.h:997` `simplifyPathOutsideBBox`                                | 不需要（已有视口裁剪）                                                   |
| **运行时按 zoom 选数据级**（MapZooms / minZoom-maxZoom）   | `IndexVectorMapCreator.java:536` `mapZooms.getLevel()`                     | 未搬，见 §27.4                                                    |


### 27.2 采纳：加权 A*（默认 w=1.2）

`f = g + w·h`。w=1 是标准 A*，w>1 让堆更偏向目标方向。

★ 它和已有的 `exact` 是**两个独立旋钮**：`exact` 管终止条件（要不要把两侧都搜到最优），  
`w` 管搜索的贪心程度。别混为一谈。

档位表（京→郑 693 km，1280×800 同机）：

| w       | 里程                | 展开节点  | 耗时          |
| ------- | ----------------- | ----- | ----------- |
| 1.0     | 693.14 km         | 234 万 | 9436 ms     |
| **1.2** | **693.14 km**     | 82 万  | **2797 ms** |
| 1.5     | 715.14 km (+3.2%) | 3.9 万 | 245 ms      |
| 2.0     | 750.93 km (+8.5%) | 3.3 万 | 385 ms      |
| 3.0     | 744.98 km (+7.6%) | 1.6 万 | 198 ms      |

**判据是"零劣化"，不是"平均差不多"** —— 15 组路线（8 组跨省长途 + 4 组省内中长途

- 3 组城内短途，最长 1172 km）逐个比对 w=1 与 w=1.2 的里程：  
  **14 组完全一致，1 组 w=1.2 反而更短**（郑州→青岛 713.45 < 714.87，说明 w=1 那个  
  714.87 本来就不是最优——默认终止条件 `lb=ff+fb` 自身就是近似的），**零劣化**。

各档提速（w=1 → w=1.2）：

| 路线    | 里程      | w=1      | w=1.2   | 倍数   |
| ----- | ------- | -------- | ------- | ---- |
| 京→郑   | 693.14  | 9128 ms  | 2766 ms | 3.3× |
| 青岛→西安 | 1172.66 | 14702 ms | 6906 ms | 2.1× |
| 临沂→西安 | 937.00  | 6058 ms  | 1300 ms | 4.7× |
| 太原→天津 | 503.44  | 2961 ms  | 1688 ms | 1.8× |
| 沈阳→天津 | 661.88  | 2986 ms  | 1466 ms | 2.0× |
| 西安→太原 | 598.13  | 2645 ms  | 1241 ms | 2.1× |
| 济南→郑州 | 437.90  | 2694 ms  | 1463 ms | 1.8× |

城内短途（<15 km）几乎无收益（411→403 ms）但也无损失，因为展开数本来就只有几千。

**为什么在这个图上 w=1.2 是安全的**（理论上加权 A* 只保证"解 ≤ w × 最优"）：  
本项目的 h = 大圆距离 ÷ 130 km/h，而实际绕行系数 1.11~1.21、道路限速远低于 130，  
所以 h 是**很松的下界**；乘 1.2 之后仍远低于真实代价，等价于"在一个仍接近 admissible  
的启发式下搜索"。路网又有强方向性（高速），自然还是走到同一条路。

⇒ 默认改成 `ROUTE_HEUR_W_DEFAULT 1.2`。★ **验收/对拍工具必须显式传 1.0**  
（`skedaddle_restr_ab` 两遍、以及 `--exact` 对拍）：否则里程差异不再只反映被测因素  
（转向限制）本身的代价，A/B 就失效了。

### 27.3 否决：Douglas-Peucker 简化几何（有数据，别再试第二遍）

OsmAnd 敢在构建期大规模简化，是因为它低缩只画大要素。本项目的分布完全不同 ——  
实测（z9，走完整管线的面）：**91% ≤ 5 px**（2 px 占 17.8%）。小面顶点本来就只有几个，  
DP 丢一个点形状就变，省下的填充/描边却没多少，还要额外付 DP 自身的开销。

在屏幕坐标上做 DP（ε 单位像素），跑出来的档位：

| ε     | 帧时间（3 次取最小）    | 差异像素   | 最大转移方向              |
| ----- | -------------- | ------ | ------------------- |
| 0（基线） | 224 ms         | —      | —                   |
| 0.5   | **243 ms（更慢）** | 1.145% | 绿地 → 背景 1686 px（单向） |
| 1     | 221 ms         | 4.973% | 绿地 → 背景 10434 px    |
| 2     | 206 ms         | 9.899% | 建筑/绿地 → 背景 26808 px |

两个结论：

1. **DP 也丢面积**，转移方向是单方向"要素色 → 背景色"（§5 的判据里这是丢要素）。  
   而且比上一轮否决的"按像素阈值均匀筛点"更糟 —— 因为它在投影**之后**做，  
   省不到投影那 16 ms，却一样把面积削掉了。
2. **ε=0.5 反而更慢**：DP 自身的开销吃掉了收益。

⇒ §26.4（阈值筛）与本节（DP）一起说明：**"简化几何"这条路在本项目的要素分布下  
不成立**。要省面要素的钱，只能走"精确等价"的路（§26.3 的活动边表那种）。

### 27.4 盘点下来没搬的，以及为什么

- **HH 层次路由**（`hhRoutePlanner.cpp`）：真正能把长途算路压到毫秒级的做法，  
  但需要**构建期**生成一整套层次网络索引（`NetworkDBPoint` / `NetworkDBSegment`  
  / 分区域 `HHRouteRegionPointsCtx`），是 1500 行量级的子系统，还要重跑 8 省抽取。  
  加权 A* 用 3 行改动拿到了 1.8~4.7 倍，先把这个便宜的用上；HH 留给真的需要  
  "毫秒级 + 支持多 profile"的时候。
- **构建期多级 LOD**（按 zoom 存不同精度几何）：简化本身就被 §27.3 否掉了，  
  多级只是把同样的简化预计算，省的是运行时的 DP 开销 —— 而 DP 的**收益**本来就不成立。
- **按 zoom 过滤要素（minZoom）**：这个是**取舍而不是优化** —— 低缩时丢掉小要素，  
  城市会变空。本项目 z9 时那 39 万个建筑已经在画（否则城市是背景色），要动得由产品定。
- **basemap**（低缩专用精简数据）：同上，且要重新生成一份数据。

### 27.5 一条调研经验

`route.h` 顶部的注释里本来就写着"坐标键这个做法是 OsmAnd 的"。也就是说本项目  
**早就在参考它了**，只是没记在 PLAN 里。⇒ 从别的项目搬算法时，把**出处写进注释**  
（`// OsmAnd RoutePlanner.cpp:746`），后来的人才能接着搬、也才知道去哪对拍。

---

## 28. 第十二刀的探索：面要素与路网各试一轮，全部否决（2026-09-23）

这一刀**没有产出提速**，但把两条路都量到了底，结论值得留着——否则下次还会  
从头试一遍。起点是第十一刀后：**z9 206 / z11 100 / z13 91 ms**，面 91、路 87。

### 28.1 面要素 91 ms 的完整归因（z9）

用临时位开关逐项量出来（量完即删）：

| 项              | 耗时         | 怎么量的                                     |
| -------------- | ---------- | ---------------------------------------- |
| 遍历面记录          | **3 ms**   | 关掉全部绘制，只留 bbox 预筛（z13 扫 58.5 万条也只要 3 ms） |
| 小面打点（76,007 个） | **6 ms**   | 关掉 `SDL_RenderDrawPoint`                 |
| bbox 的 2 次投影   | **4 ms**   | 额外多做 2 次投影 ⇒ 单次 **17.6 ns**              |
| 顶点随机访存         | **6 ms**   | 每个顶点多读一个远处冷地址                            |
| 填充（AET）        | **21 ms**  | 关掉 `fill_polygon`                        |
| 描边             | **17 ms**  | 关掉 `stroke_ring`                         |
| 大面顶点投影 + 循环    | **≈32 ms** | 余项                                       |

★ **"遍历 58 万个面记录"只值 3 ms** —— 所以"给面要素加 tile 索引"（OsmAnd obf  
按瓦片分块的做法）**上限就这么点**，不值得改数据格式 + 重建 8 省。**先量上限**  
（§5.11）又一次把一个大改动挡在了动手之前。

★ **一次 `mv_project` 只有 17.6 ns**，不是 300 ns。之前按"命中数 × 2 次投影"  
反推出的 40 ms 是错的——反推会把"大面完整管线"的成本也算进去。**能直接量的  
就不要反推**。

### 28.2 否决：提高 `AREA_DOT_PX`（让更多面走"打点"快速路径）

小面打点 80 ns/个，完整管线 2.13 μs/个，差 27 倍，所以放宽阈值看着很诱人：

| 阈值 K  | z9 帧   | 面  | 差异像素   | 最大转移               |
| ----- | ------ | -- | ------ | ------------------ |
| 2（现状） | 208 ms | 92 | —      | —                  |
| 3     | 187 ms | 70 | 3.18%  | 建筑灰 → 背景 10690（单向） |
| 4     | 172 ms | 56 | 5.98%  | 同上 22448           |
| 6     | 157 ms | 42 | 9.67%  | 同上 36245           |
| 8     | 150 ms | 35 | 11.69% | 同上 42107           |

**全部是单向"要素色 → 背景色"**（§5 判据：丢要素）。K=3 就已经 3.18%。  
原因和 §26.4 / §27.3 一样：一个 3~8 px 的面打一个点只占 1 px，其余像素变背景。  
⇒ 与"抽稀""DP"是同一个坑的三种写法，**简化几何在本项目一律不成立**。

### 28.3 否决：去掉描边

描边 17 ms，看着可省。实测差 **13.47%**，而且转移里含  
`建筑填充色 → 背景色` 28492 px：**小面（2~3 px）的"描边"就是它本体**，  
去掉之后整个面只剩中心一个填充点，等于丢要素。⇒ 描边也不能省。

### 28.4 否决：顶点坐标内联进面索引（省随机访存）

bit7 量出**一次随机访存只值 6 ms**，这就是该方案的天花板；代价是改数据格式

- 全量重建 8 省。§5.5 说它是"最后手段"，这次把上限量出来了 ⇒ 不做。

### 28.5 否决（重测）：路网合并连续边成折线

第五刀试过并否决，但**前提是 v1 记录**（判断"是否首尾相接"要去 `nodes.bin`  
随机读一次）。v2 记录自带两端坐标，判断只是比 4 个 int32，所以值得重测。

潜力确实大：z9 589,300 条边里 **84%（495,069）能接上一条且同等级**，  
平均折线 6.3 段。若成立，调用数应降一个数量级。

实测：

| 缩放  | 帧                     | 路       | 差异像素      |
| --- | --------------------- | ------- | --------- |
| z9  | 206 → **213（慢 7 ms）** | 87 → 93 | 0         |
| z11 | 100 → 102（慢 2 ms）     | 27 → 29 | 0         |
| z13 | 91 → 78（快 13 ms）      | 31 → 19 | **8.09%** |

- z9/z11：**0 差异但更慢**。因为 SDL 的 `SDL_RenderDrawLines` 在软件渲染器里  
  就是循环调 `RenderDrawLine`，**每条线段的开销一分没省**；省下的只有  
  `SetRenderDrawColor`，而攒点/判连接性又把它吃回去了。  
  ⇒ 描边的 `stroke_flush` 之所以有效，是因为它顺带省了**每个面一次**的  
  颜色设置，不是因为 DrawLines 本身便宜。
- z13 那 13 ms 是**假收益**：我漏了 `width>1`（zoom≥12.5 走 `s->width`），  
  合并后用 DrawLines 把粗线画成了 1 px，所以才有 8% 差异。  
  ★ 这是个好例子：**"加速"和"画错了"经常是同一件事**，像素对比一定要做，  
  只看耗时会把 bug 当成成果。

⇒ 第五刀的结论在 v2 下**仍然成立**。实验代码已撤，三档缩放回到 0 差异。

### 28.6 这一刀的净成果

代码零改动，但面要素与路网两条线都被量到了底：

- 面 91 ms：大面顶点投影+循环 32 / 填充 21 / 描边 17 / 打点 6 / bbox 投影 4 /  
  访存 6 / 遍历 3 —— **每一项的上限都量出来了，没有一项剩下"没试过的明显空间"**。
- 路 87 ms：588K 条边 ×（SDL 调用 ~100 ns + 投影）已逼近地板；  
  简化几何、合并折线、批量化三条都实测否决。

⇒ **剩下的只有"换掉 SDL 软渲染、自己写像素"这一条路**（588K 次调用的固定开销  
约 59 ms 是可以直接写 framebuffer 省掉的），但那是要重写 `fill_polygon` /  
`stroke_ring` / `paint_thick_line` 的大工程。**要不要走，是产品决策。**

## 29. 第十三刀：热点替换 —— 面/路/线改自己写像素（2026-09-23 落地）

§28.6 把"剩下的唯一一条路"指向了换掉 SDL 软渲染。**这一次真的走了**，而且  
走的是**热点替换**而不是整体重写：只把面填充/描边、路网、线要素换成直写像素，  
文字和 UI 继续用 SDL。

### 29.1 先量上限：SDL 那一层到底值多少（不改一行业务代码）

用 `LD_PRELOAD` 把绘制调用换成**空操作**，得到"SDL 层最多能省多少"：

| 缩放  | 现状     | 空操作后   | SDL 层上限         |
| --- | ------ | ------ | --------------- |
| z9  | 206 ms | 125 ms | **≤81 ms（39%）** |
| z11 | 102 ms | 57 ms  | ≤45 ms（44%）     |
| z13 | 90 ms  | 38 ms  | ≤52 ms（58%）     |

z9 一帧的调用量：`SDL_RenderDrawLine` 等价 **913,894** 次、`DrawLines` 61,593、  
`DrawPoint` 76,007、**`SetRenderDrawColor` 773,543**。

微基准（913,894 段，60% 退化）：SDL `RenderDrawLine` **59.6 ms** vs 自写  
Bresenham **19.9 ms**；`SetDrawColor` 单独 3.9 ms；`FillRect` 全屏 0.4 ms vs  
`memset` 0.1 ms。

★ 三个实测事实决定了方案：

1. `SDL_CreateSoftwareRenderer(surface)` **没有双缓冲**，就是直接写  
   `surface->pixels`，所以自写像素和 SDL 调用**可以混在同一张图上**，也  
   不需要 `RenderPresent` 同步 ⇒ 可以增量替换，不必一次重写。
2. 本机 surface 格式是 **`SDL_PIXELFORMAT_RGB888`（Amask=0）**，不是 ARGB8888；  
   打包颜色要按 Rmask/Gmask/Bmask 移位，`amask` 补 0。
3. SDL 对越界坐标**不崩**（它裁过了），自写**必须自己裁**，否则段错误。

### 29.2 落地：`src/render/raster.{h,c}`

新增光栅原语：`raster_rgb` / `raster_point` / `raster_hspan` /  
`raster_line` / `raster_thick_line`，`Raster` 结构持有像素指针 + 跨度 + 裁剪框

- 通道掩码。三处热点全部改成直写：
- `fill_polygon` 的扫描线跨度：`SDL_RenderDrawLine(x0,y,x1-1,y)` → `raster_hspan`
- 面/线的描边：不再攒点凑 `DrawLines`，逐段 `stroke_push` 直写
- 路网：`paint_thick_line` → `raster_thick_line`；5 个等级的颜色**一帧打包一次**  
  （原来每条边一次 `SetRenderDrawColor`，z9 77 万次纯状态切换）
- 窗口路径统一：先在**离屏 surface** 上画完（光栅画地图、SDL 画文字/UI，混在  
  同一张图上），再 `UpdateTexture` + `RenderCopy` 贴到窗口。  
  代价是每帧一次 1280×800×4 的上传（实测 **~2.6 ms**），换来窗口和 `map_shot`  
  **共用一套 `paint_frame`**，不再有两份渲染代码。

### 29.3 ★★ 与 SDL 逐像素等价：五条规则，第 ④ 条踩了坑

照 SDL 2.30 `src/render/software/SDL_drawline.c` 复刻的真实管线：

1. 先 `SDL_IntersectRectAndLine` 把线段**端点真的移动**到裁剪框内（不是逐像素  
   丢弃，斜率都可能变）。第一版只做"越界不写" ⇒ 差 832/4000。
2. 再按 bpp 分派：水平 `HLINE` / 垂直 `VLINE` / `|dx|==|dy|` 的 `DLINE` /  
   其余 `BLINE`（整数 Bresenham，带 `d/dinc1/dinc2`）。
3. `SDL_RenderDrawLine` 内部是 `DrawLines(count=2)`，`draw_end` 只在"终点被裁  
   过"时为真，末尾补一次 `DrawPoint`；因为我们的实现总是画满两端，那次补点恒  
   等于重复写同一个像素，可以省掉。
4. **★ 退化成一个点的线段画 1 个像素**（走 `HLINE`，min==max 照样落一个点）；  
   点落在裁剪框外才什么都不画。  
   ★★ 这条踩过坑，**别凭直觉改**：第一版按"零长线不产生像素"直接 `return`，  
   z9 差 216 个像素、逐段隔离量出 **27 万个退化段各差 1 像素**（z9 有 67% 的段  
   是退化的）。修正后 0 差异。
5. 32bpp 无 alpha 走非混合分支，写整整 32 位。

验证：4000 条随机线段（含越界/水平/垂直/对角/退化）+ 1500 条随机折线逐像素  
比对；**以及整帧**对改造前的旧 SDL 构建做 `bmpdiff`。

### 29.4 怎么把 216 个像素定位到根因（方法留档）

像素差异 0.011% 看着像噪声，但它是**真 bug**。三次定位按这个顺序做：

1. **分层落盘**（面后/路后/线后各存一张 bmp）⇒ 面层 0、路层 216、线层 113  
   （线层是继承了路层的差异）⇒ 定位到路网。
2. **同帧双画**：同一帧、同一批段，光栅写进主画布、SDL 写进另一张**起点内容  
   完全相同**的画布，帧末各存一张。这样"两次运行"的不确定性**被彻底消掉**，  
   剩下的差异只可能是光栅化本身。（这一步之前"两次构建的段清单逐字节相同"  
   已经排除了数据差异，但同帧双画才是决定性的一步。）
3. **逐段隔离**：每段单独画进两张清空的小窗口（只清该段的包围盒），逐像素比  
   对 ⇒ 直接打印出不一致的段：`(3,1)-(3,1)` 这种退化段，自写 `000000`、  
   SDL `d65e28`。汇总：**退化段 270,540，非退化段 0** ⇒ 根因一句话就出来了。

★ "整帧只差 216 像素"而"隔离量差 27 万像素"并不矛盾：退化段的那个点，  
**绝大多数随后被同等级的邻段覆盖了**，只有 216 个落到了别的上。  
⇒ **小差异不要当噪声放过**，也不要拿"差异小"去推断"原因小"。

### 29.5 成果

| 缩放  | 改造前    | 改造后        | 空操作天花板 |
| --- | ------ | ---------- | ------ |
| z9  | 206 ms | **130 ms** | 125 ms |
| z11 | 102 ms | **61 ms**  | 57 ms  |
| z13 | 90 ms  | **45 ms**  | 38 ms  |
| z15 | —      | **33 ms**  | —      |

z9 分解（第 2 帧）：背景 0 / **面 65** / 路 43 / 线 20 / 标注 3。

- **像素：z9 / z11 / z13 / z15 全部 0 差异**（对改造前的旧 SDL 构建逐像素比）。
- z9 已经**贴到空操作天花板**（130 vs 125）——SDL 那一层基本被榨干了。
- 新排名：**面 65 > 路 43 > 线 20 > 标注 3**。面要素重新变成绝对大头，  
  下一刀该冲它（§28.1 的归因仍然有效：大面顶点投影+循环 32 / 填充 21 / 描边 17）。
- `regress.sh --build` 22/22，编译零警告。

### 29.6 副作用与已知代价

- 窗口模式每帧多一次纹理上传 **~2.6 ms**（换来渲染代码只有一份）。
- resize 会重建离屏渲染器，必须先 `font_drop_textures()` 把字形缓存作废：  
  新 renderer **可能分配到同一个地址**，缓存里的悬空指针会被当成有效纹理。
- `Raster` 只支持 32bpp、不需要锁的表面；其它情况 `raster_bind` 返回 -1，  
  调用方退回 SDL 路径。




### 50.5 待定夺 / 待办（按优先级）

1. **【待用户定夺】w 默认值**：建议改成"**有 lm.bin → 1.2（hard 1.3）；没有 → 1.4
   （hard 1.8，维持现状）**"，理由是保持**有效贪心度**而不是 w 的字面值。
   这是**产品行为**（所有路线的结果都变），不再只是性能旋钮。
   实现点：`router_create` 里把兜底改成 `r->lm.loaded ? ROUTE_HEUR_W_LM : ROUTE_HEUR_W_DEFAULT`
   （调用方显式给了 `--heur-w` 就照用，不受影响）。
2. **44 条城市对的完整分布 A/B**（`scripts/citypairs.py`，已脚本化）：
   基线 vs LM+w=1.2，跑在后台。这是"中位/p90/最慢"的最终数字，待补。
3. **`lm.bin` 还没接进 `sync_maps.sh`** ⇒ 全新检出没有表，会静默退回旧启发式。
   需要加一段（86 s / 2.6 GB，按"省 manifest 指纹"决定是否重建）。
4. **`LmHeader` 缺图指纹**：省数据重抽之后旧 `lm.bin` 不会失效，会静默给出次优
   路线（不崩、不报错）。建议加 `nprov` + 总节点数，加载时校验，版本顶到 2。
5. **负向对照还没做**：开表 w=1.0 的结果应**不差于**无表 w=1.0（后者是 admissible 的）。
   若某条更差 ⇒ 聚合下界在该处越界（罚项 4045 m 不够），需要加大 penalty 重测。
6. 【**已解决** 2026-09-28】删 `/home/along/rev_v2`（511 MB）+ `rev_v1_bak`（355 MB）
   —— 见 §51.10，复核时已不在任何位置，866 MB 已回收。

### 50.6 状态

- 编译**零警告**；`regress.sh` **39/39**（地标表默认已开）。
- 新增文件：`src/route/lmtable.{h,c}`、`tools/lmbuild_cli.c`、`scripts/citypairs.py`；
  改动：`src/route/route.c`（heur / Router 字段 / 生命周期）、`CMakeLists.txt`。
- `products/graph/lm.bin` 已生成（6.9 MB）。

### 50.7 ★★ 44 条完整分布：w=1.2 救了尾巴，却把中位拖慢 3.4×（**推翻 50.4 的推荐**）

`scripts/citypairs.py` 跑完两组（同一 seed(7)、同一 44 条）：

| 指标 | 现状（无表 + w=1.4 自适应） | 有表 + w=1.2 | 变化 |
|---|---|---|---|
| 展开中位 | 78,497 | **378,669** | **4.8× 更差** ✗ |
| 展开 p90 | 1,779,235 | 1,188,349 | 1.5× 更好 ✓ |
| 展开最慢 | 6,987,626（济南→大连） | 3,508,108（烟台→宝鸡） | 2.0× 更好 ✓ |
| 展开合计 | 23,453,275 | 25,273,078 | +7.8% ✗ |
| 耗时中位 | 202 ms | **690 ms** | **3.4× 更慢** ✗ |
| 耗时 p90 | 2,693 ms | 1,882 ms | 1.4× 更好 ✓ |
| 耗时最慢 | 10,245 ms | **6,015 ms** | 1.7× 更好 ✓ |
| >1s | 7 条 | 14 条 | ✗ |
| **>3s** | **4 条** | **1 条** | ✓ |
| **预计分钟合计** | 20,028 | **19,342** | **−686（−3.4%）** ✓ |

- **质量提升的大头不在那 6 条最慢路线上**：6 条合计只省 103 分，
  总提升 686 分 ⇒ **其余 38 条中短路线贡献了 583 分**。
  ⇒ 基线 w=1.4 在中短路线上的确长期损失 ~3% 质量，只是以前量不出来。
- ★★ **我又踩了 §45.7 / §47.3 那个坑的变体**：拿"手挑的 6 条最慢路线"下结论
  （说 w=1.2 全面占优），40+ 条随机样本一跑就露馅 —— **中位是反的**。
  ⇒ 50.4 那句"开表就该配 w=1.2"**不能照做**。
- ⇒ **真正的未知数是有表 + w=1.4（以及 1.3）的 44 条分布**：它应当在
  "中位展开比基线更低"（h 更准）和"质量介于 1.2 与基线之间"之间。
  **下一次开工第一件事就是补这一组**，然后才有依据定 w。
- 备选思路（如果 1.4 的质量不够）：把自适应改成**反向**——先跑 w=1.2，
  展开超过 `ROUTE_ADAPT_TRY` 才换 1.4。这样只有真的难走的路线付"贪心"的代价，
  而中短路线拿到 w=1.2 的质量。现行自适应是"撞顶 → 换**更大**的 w"，
  逻辑上正好相反，需要改的不只是常数。

## 51. 吸附撞上"死节点"导致整条路线失败（2026-09-28）

查 §50.7 那个"中位展开反而涨 4.8×"时顺手发现的**真 bug**，与地标表无关，
但会污染一切算路基准（44 条城市对里有 6 条是**这么**失败的，不是数据不通）。

### 51.1 症状与定位

- 保定(38.87,115.46) → 石家庄(38.04,114.51)：**附近 5 个坐标全部"不连通"**，
  偏差只有 54~214 m，看不出哪里不对。
- 换成"保定→北京 / 保定→济南"都正常，**石家庄→北京 / 北京→石家庄双向都失败**
  ⇒ 问题在石家庄吸附到的那个节点 `hebei #6204583`。
- ★ 第一反应是"吸附到了没有出边的孤立节点"（§50.1 说六成节点出度为 0），
  加了 `NF_EDGE_COUNT > 0` 过滤后**吸附结果一个都没变** ⇒ 假设错了：
  `#6204583` **有出边**，只是整个悬在一个与主干断开的小连通块里。
- ★ 关键观测：**"不连通"的判定只要 27 ms**（小连通块立刻穷尽）
  ⇒ "换下一个候选重试"几乎不要钱。

### 51.2 修法：`router_nearest_k` + `router_route_ll`（SNAP_TRY=4）

- `router_nearest_k()`：一次给出**最近的 k 个**候选（按距离升序），
  只在"有出边"的候选里挑；整圈都没有才退回"最近节点"。
- `router_route_ll()`：经纬度→经纬度算路，**仅当 `router_plan` 返回 -1（不连通）
  时换下一组候选**；按 (i+j) 升序试（优先只换一端、先换起点）；
  -2（超限）立刻放弃，不越试越糟。
- `skedaddle_route_cli` 改用它，并**复用首次成功的结果** ⇒ `--repeat=N` 不多跑一趟。
- 石家庄现在吸附到 `#116278`（196.8 m），保定→石家庄 139.04 km（绕行 1.12）、
  石家庄→北京 290.36 km —— 都合理。
- ★ **不会改变原先成功的路线**：新过滤只在"最近节点没有出边"时才起作用，
  而那种情况原来必然失败。⇒ 历次基准仍可比。

### 51.3 收益

44 条城市对 → **50 条全部有效**（seed(7) 抽的那 6 条原先全部吸附到死节点），
重跑基线：

| 指标 | 修复前（44 条） | 修复后（50 条） |
|---|---|---|
| 有效条数 | 44 | **50** |
| 展开中位 | 78,497 | 74,769 |
| 耗时中位 | 202 ms | 196 ms |
| 最慢 | 10,245 ms | 8,913 ms |
| 分钟合计 | 20,028 | 22,217（多了 6 条） |

回归 **39/39**，`route_hard_expand` 仍是 944,426（未受影响）。

### 51.4 w 定案：有 lm.bin → **1.3，并关掉"撞顶升级"**（用户 2026-09-28 拍板）

#### ★★ 先更正一个我自己说错的数字

我在拍板用的选项里写"1.3 的速度仍是基线的 5.7 倍"——**那是 8 条路线的
A/B，而我挑的 8 条里有一半是难走路线，样本偏了**。50 条全量的真实分布是：

| 耗时分位 | p10 | p25 | **中位** | p75 | **p90** | **最慢** |
|---|---|---|---|---|---|---|
| 基线（无表 1.4→1.8） | 41 | 89 | **196 ms** | 275 | 3131 | 8913 |
| 发货（有表 1.3） | 45 | 82 | **361 ms** | 638 | **1127** | **3790** |

⇒ **中位慢 1.8×，尾部快 2.4~2.8×**。不是全面提速。

#### 为什么"h 更准了中位反而更慢"——基线是在**过早收工**

拿最扎眼的一条（南阳→烟台）把机理钉死：

| 配置 | 展开 | ms | 预计 |
|---|---|---|---|
| 无表 默认 1.4→1.8（基线） | 27,467 | 214 | **649 分** |
| 有表 默认 1.3（发货） | 1,710,129 | 2948 | **610 分** |
| 有表 w=1.0（精确 A\*） | 3,195,857 | 4968 | 610 分 |
| 无表 w=1.0（精确 A\*） | 3,692,708 | 6027 | 609 分 |

- 真最优是 **609~610 分**。**基线用 2.7 万展开给了 649 分（差 6.5%）** ——
  它在好走的路线上一撞到第一个解就收工了。
- 发货配置给 **610 分 = 最优**，而且只用了精确 A\* 的 **一半**展开。
- ⇒ 中位那 2~4× 的展开增长**不是退化，是把"过早收工"换成了"搜到最优"**。
  w 只调节这个幅度（有表 1.4 中位 157,844 / 1.3 中位 214,791；基线 74,769）。

#### 50 条逐条配对（发货 vs 基线）

- 展开：**变多 35 条 / 变少 15 条**。变少的 15 条正是难走的那些
  （基线展开中位 61 万）：济南→大连 699 万→38 万（0.06×）、
  秦皇岛→青岛 178 万→7.7 万（0.04×）、唐山→青岛 61 万→13.7 万（0.22×）。
- 展开合计 23.88 M → **19.62 M（−17.9%）**；分钟合计 22,217 → **21,592（−2.8%）**，
  **29 条更好 / 12 条更差**。
- >1s 7→6 条，**>3s 4→1 条**。

#### 实现（`ROUTE_HEUR_W_LM 1.3`）

- `router_create`：有表时 `heur_w` **和** `heur_w_hi` 都兜底成 1.3
  （hi 同值 = 关掉升级）；无表维持 1.4 / 1.8。显式传了 `--heur-w` 不受影响。
- ⚠️ **踩到的坑**：`skedaddle_route_cli` / `skedaddle_navsim_cli` / `window.c` 都在 `RouteOpts` 里
  写死了 `.heur_w = ROUTE_HEUR_W_DEFAULT`，把兜底**顶掉了**（命令行没给
  `--heur-w` 却一直是 1.4）。三处都改成留 0 —— 兜底逻辑只能有一个入口，
  调用点写死就等于废掉它。
- `router_lm_loaded()` / `router_heur_w()` 两个访问器，`skedaddle_route_cli` 打印
  `地标表 已加载（生效 w=1.30）`，回归新增 `lm_loaded` 判据。

#### 为什么"撞顶升 1.8"必须关掉

有表默认（1.4→1.8）在 50 条上质量 **+4.1%（7 条变好 / 37 条变差）**，
比不开表还差；锁死 1.4 是 −1.0%，锁死 1.3 是 −2.8%。
⇒ 升级到 1.8 换来"合计展开 970 万（1.3 的一半）"，但质量倒退 5.1 个点 —— 不划算。

### 51.5 LmHeader 加图指纹（LM_VERSION 1 → 2）

- 新增 `nprov` + `total_nodes`（Σ 各省 nodes.bin 记录数，**含**副本省）。
  ★ 不校验的话：省数据重抽之后旧表**不崩、不报错**，只是给出偏大的下界
  ⇒ 路线悄悄变差，极难归因。
- 负向对照：只加载 1 个省时对 8 省的表 →
  `lm.bin 与当前路网不匹配（表：8 省 / 52276894 节点；现在：1 省 / 4322181 节点）
  ——已忽略，退回直线启发式`，且 w 自动回到 1.40。✓
- `skedaddle_lmbuild_cli` 打印 `图指纹 8 省 / 52276894 节点`；**两边算法必须一致**，
  否则每次加载都误判不匹配而把表丢掉。

### 51.6 lm.bin 接进 sync_maps.sh

- 指纹 = 各省 manifest 的 (nodes, edges, binfmt_version) + `LM_WANT_VER`
  （**改 LM_VERSION 必须同步改这个常量**），记在 `products/graph/lm.bin.meta`。
- 失败只 `[WARN]` 不计 `n_fail`：表是可选的，没有就退回直线启发式。
  但必须说出来，否则表现是"新检出算路很慢"这种极难归因的问题。
- 写临时文件 + `mv` 就位：构建中途 OOM 不能把半截表留在正式路径上
  （半截表的 magic 是对的，会被当成有效表加载）。
- 实测：`[ OK ] lm 85 秒，7056456 字节`。

### 51.7 状态

- 编译**零警告**；`regress.sh` **40/40**（新增 `lm_loaded`）。
- `products/graph/lm.bin` v2 已重建（7.06 MB）+ `lm.bin.meta`。
- 剩余：删 `/home/along/rev_v2`(511 MB) + `rev_v1_bak`(355 MB)（已问两次未确认）。

### 51.8 ★★ 自适应在"w == w_hi"时空转一整遍（20 万展开白扔，占实际工作 26.5%）

关掉撞顶升级之后（`heur_w_hi` 兜底成和 `heur_w` 一样的 1.3），自适应那段逻辑
就变成了纯浪费，而且**从报告的数字上看不出来**：

```c
if (try_cap) {
    rc = plan_search(r, from, to, out, w0, try_cap);   /* 第 1 遍，封顶 20 万 */
    if (rc != -3) goto done;
    route_result_free(out);
}
rc = plan_search(r, from, to, out, r->opt.heur_w_hi, r->max_expand);  /* 换 w 重跑 */
```

- 撞顶之后要换的是**同一个 w** ⇒ 第 1 遍那 ≤20 万次展开（≈200~300 ms）全部白扔。
- `r->expanded` 在每次 `plan_search` 开头重置 ⇒ **报告的展开数只含第 2 遍**，
  这 20 万根本不出现。只有 `elapsed_ms`（两遍合计）能看出来，而它通常不当判据。
- 规模：50 条里 **26 条**展开 > 20 万 ⇒ 白扔 26 × 20 万 = **520 万**，
  是实际工作（1961 万）的 **26.5%**。中位展开 21.5 万刚好在顶上 ⇒ 一半以上中招。

**修法**：`two_pass = (try_cap != 0) && (heur_w_hi != w0)`；相等就直接单遍搜到
`max_expand`。结果与"两遍"**逐位相同**（第 1 遍没撞顶时本来就是同一个 w 同一个解）。

**实测（50 条，展开与分钟逐位一致 = 零行为改变）**

| | p10 | 中位 | p75 | p90 | 最慢 | 合计 |
|---|---|---|---|---|---|---|
| 基线（无表） | 41 | 196 | 275 | 3131 | 8913 | 37,049 ms |
| 改前（两遍） | 45 | 361 | 638 | 1127 | 3790 | 24,951 |
| **改后（单遍）** | 51 | **210** | 506 | **1085** | 3998 | **22,694** |

⇒ **中位从 361 回到 210 ms，追平基线（196）；p90 1085 vs 基线 3131；最慢 3998
vs 基线 8913；质量仍是 −2.8%**。§51.4 里那个"中位慢 1.8×"的代价**基本被这一改抵消**。

- ★ 教训：一条逻辑（"先省着搜、搜不动再加重"）在**前提条件被改掉之后**
  （不再有"更重的 w"可换）会退化成纯浪费，而且浪费量恰好落在中位附近 ⇒
  表现是"中位变慢"这种最难归因的症状。改旋钮默认值时，**回头检查依赖它的
  那条控制流还成不成立**。
- 回归 40/40；`route_hard_ms` 3314 → 3114。

### 51.9 补上主程序算路路径的回归覆盖（43 项）

发现一个**覆盖盲区**：`navsim_*` 三项用的是独立工具 `skedaddle_navsim_cli`，**不走 window.c**；
`map_shot` 不带 `--from/--to` 也不算路 ⇒ 主程序的 `nav_worker` / `app_nav_plan_sync`
长期没有任何自动化判据，而它们才是对外行为。我在 §51.2 刚改过这两处。

新增三项（`map_shot --from --to --fit-route`，一次盖住 吸附→算路→指令→画到画面）：

| 项 | 判据 | 实测 |
|---|---|---|
| `app_route` | `ASSERT-OK`（算路成功 + 渲染自检通过） | OK |
| `app_route_km` | 里程在 110~180 km | 139.04 km |
| `app_route_px` | 路线像素 > 200 | 1891 |

- 起终点特意选 **保定→石家庄** —— 它就是 §51.1 那个吸附 bug 的原生样本
  （修复前双向都"不连通"），所以这三项同时是那个修复的负向对照。
- 负向对照（判据要能失败）：
  - 起点挪到渤海(38.5,120.0) ⇒ `ASSERT-FAIL 算路失败：起终点附近没有可通行的路`
    ⇒ 判据非空转，而且顺带验证了 `router_route_ll` 返回 -1 时 window.c 的分支。
  - 换成保定→北京 ⇒ 158.96 km，说明 110~180 这个区间是有分辨力的（不是恒真）。
- 回归 **43/43**。

### 51.10 清理早先的对照实验残留（866 MB）

- `/home/along/rev_v2`（511 MB，rev.bin v2 存 edge_idx 的实验，§44 实测否决并回退）
  与 `/home/along/rev_v1_bak`（355 MB，v1 的备份）都是 `beijing…tianjin` 的
  逐省 rev.bin 产物，无个人文档。
- 用户确认后用 `gio trash` 移入回收站（**可恢复**，不是直接删）。
  ⇒ 866 MB 仍在 `~/.local/share/Trash`，要真正腾出空间需再清空回收站。
- rev.bin 本身是**可重建**的（`skedaddle_revbuild_cli` / 运行时自动建），删了不影响功能。
- **2026-09-28 12:59 复核：已不存在了。** 全盘 `find`（含 `~/.local/share/Trash`、
  `/home/.Trash-1000`、`~/Downloads/.Trash-1000`）都找不到 `rev_v2` / `rev_v1_bak`；
  回收站里只剩一条无关条目 `nav-desktop`（108 MB，来自 `/home/along/osmand-app`）。
  ⇒ 866 MB **已经回收**，无需再操作。各省在用的 `rev.bin` 8 份完好（合计 ~372 MB），
  回归仍 45/45。

---

## 52. 地标扩到 30 个，w 跟着降到 1.25（2026-09-28）

§51 结束时发货的是 **10 地标 + w=1.3**。剩下一个没平的趋势是"地标越多越好吗"：
§50 那张表只用了 10 个地标，下界只有真值的 92~97%。本节把它推到 30 个，并重新定 w。

### 52.1 先把地标列表固化进源码（补一个**可复现性**的洞）

**洞是怎么来的**：为了试"地标越多越好吗"，我用命令行 `--lm=lat,lon` 反复传了
20/30 个坐标，建出 `/tmp/lm20.bin` `/tmp/lm30.bin` 并测完。而 `lm.bin` 的 header
里**根本不存地标坐标**（只有 `nlm`）—— 于是那份 21 MB 的表**再也重建不出来**，

> 直接违反项目约定："`products/` 全部可删、可由 `./scripts/sync_maps.sh` 确定性重建"。

★ 教训：**实验参数不能只活在命令行上**。凡是"只传了一次、之后要靠它复现产物"的
输入，必须落进源码或脚本，否则那次实验的产出就是一次性的。

**修法**：

1. 把 30 个地标写进 `tools/lmbuild_cli.c` 的 `k_lm[]`，新增 `--nlm=N`（**取前 N 个**）。
   - 顺序有意义：任意前缀都是一个自洽配置 ⇒ 10/20/30 三档可比、且都可复现。
   - 第 **1~10 位 = §50 发货的那 10 个，逐位未动**；
   - 第 **11~30 位 = 在 48 个候选城市上跑"贪心最远点"**（每步选"到已选集合的
     最小距离最大"者，即最大化铺开程度）。生成脚本的算法与结果记在本节。
2. 验证前缀属性：`--nlm=10` 重建的表与发货表 `cmp` **逐字节相同**（7,056,456 B）✓

★ 顺带的发现：**贪心选的比昨天手挑的更好**。同为 30 地标 w=1.25：

| 30 地标列表 | 中位 | 展开合计 | 分钟合计 |
|---|---|---|---|
| 昨天手挑 | 155 ms | 1177 万 | 21511 |
| **今天贪心** | **123 ms** | 1043 万 | **21503** |

⇒ 地标这种"在平面上选点"的问题别手挑，写个贪心；手挑只能保证"看起来合理"。

贪心选出的 11~30 位（括号内是当时"到已选集合的最近距离"）：

```
11 延安(256km) 12 朝阳(249) 13 衡水(248) 14 汉中(227) 15 东营(215)
16 三门峡(212) 17 丹东(203) 18 秦皇岛(195) 19 榆林(189) 20 商丘(189)
21 安康(184) 22 承德(175) 23 信阳(173) 24 济南(168) 25 长治(168)
26 威海(162) 27 营口(161) 28 张家口(154) 29 宝鸡(145) 30 临汾(144)
```

### 52.2 50 条实测：**w 的最优值随地标数往下走**

`scripts/citypairs.py`（seed(7)，50 条全部有效），全部与"10 地标 w=1.3"**逐条配对**：

| 配置 | 表 | 中位 | p90 | 最慢 | >3s | 展开合计 | 分钟合计 | 分钟 好/差 |
|---|---|---|---|---|---|---|---|---|
| 无表基线 | — | 196 | 3131 | 8913 | 5 | 2388 万 | 22217 | — |
| 10 地标 w=1.3（旧发货） | 7.1MB | 210 | 1085 | 3998 | 2 | 1962 万 | 21592 | 基准 |
| 30 地标 w=1.3 | 21.2MB | **103** | **304** | **462** | 0 | 680 万 | 21585 | 好14/差14 |
| **30 地标 w=1.25 ★发货** | 21.2MB | 123 | 753 | 1503 | 0 | 1043 万 | **21503** | **好25/差10** |
| 30 地标 w=1.2 | 21.2MB | 191 | 887 | 3783 | 1 | 1576 万 | 21477 | 好24/差11 |

相对无表基线：w=1.25 是 **−3.21%（32 好 / 11 差）**，是这几档里最一致的。

**读法 / 为什么最优 w 会变**：

- ★★ 复述 §50 那条规则：**w 的有效贪心度 = w × h/真值**。地标越多 ⇒ h 越紧
  ⇒ **同一个 w 变得更有害**。所以"扩地标"和"定 w"是**同一件事的两半**，
  必须一起定，先定了一个再定另一个必然要回头改（这是 §51.4 → §52 第二次印证）。
- **w=1.25 是唯一"速度与质量同时优于旧发货配置"的档位**：快 1.7×（中位 210→123）、
  尾巴好 2.7×（3998→1503）、>3s 从 2 条降到 0，质量还净好 89 分钟（25 好 / 10 差）。
- w=1.3 速度拉满（50 条**全部在 462 ms 内**跑完），但质量只是**持平**（14 好 / 14 差）
  ⇒ 那是"用质量不涨换快一倍"，不适合当默认。
- w=1.2 只再多买 26 分钟，最慢却从 1503 炸到 3783 ms ⇒ 尾巴不划算。
- 用户 2026-09-28 拍板：**30 地标 + w=1.25**。

代价：表 7.1 → 21.2 MB，`sync_maps.sh` 构建 86 → **244 秒**（线性，~8 s/地标）。

### 52.3 第二个"静默过期"洞：`sync_maps.sh` 的指纹不含地标集

跟 §51.5 同源：**改了源码里的地标列表，路网指纹不变 ⇒ 重建被 SKIP，旧表继续被用**。
不报错、不崩，只是算路悄悄慢 1.7 倍、质量悄悄差 0.4% —— 极难归因。

**修法**：`skedaddle_lmbuild_cli --print-lmfp` 打印地标集指纹（FNV-1a 64，喂的是坐标的
**e7 整数**而不是 double 字节，否则 `39.9` 与 `39.90000000000001` 会算出两个指纹），
不加载路网所以开销可忽略；`sync_maps.sh` 把它和 `--nlm=` 一起并进指纹：

```
nlm=10  lmfp 63d89af2fc98c7ae
nlm=20  lmfp 02eedb0c7dd2d3a2
nlm=30  lmfp fee5ccb6b3124ebb
```

`lm.bin.meta` 现在记 4 项：`fingerprint / lmver / cell_m / nlm / lmfp`。

### 52.4 回归：43 → 44 项

新增 **`lm_nlm`**（钉住地标个数 == 30），与已有的 `lm_loaded` 分工：

- `lm_loaded` 挡"表没加载"；`lm_nlm` 挡"表是用旧地标集建的"。
- `route_hard_expand` 阈值 **350 万 → 100 万**：30 地标 w=1.25 下这条路线只要
  **53.2 万**展开（10 地标 w=1.3 是 263.9 万、无表 599.8 万）⇒ 收回到 100 万之后，
  "地标数被改小"也会 FAIL，与 `lm_nlm` 互为交叉验证（单项可能被一起改掉，两项难）。

实测 **44/44**，`route_hard_ms` 454 ms，`lm_loaded` 打印 `已加载（30 个地标，生效 w=1.25）`。

### 52.5 改了哪些文件

| 文件 | 改动 |
|---|---|
| `tools/lmbuild_cli.c` | `k_lm[]` 10 → 30 个（含贪心选点注释）；新增 `--nlm=N`、`--print-lmfp`；构建时打印地标集指纹 |
| `src/route/route.h` | `ROUTE_HEUR_W_LM` 1.3 → **1.25**（注释补上"地标越多 w 要越低"） |
| `src/route/route.c` | 新增 `router_lm_nlm()` |
| `tools/route_cli.c` | 加载打印改成 `已加载（%d 个地标，生效 w=%.2f）` |
| `scripts/sync_maps.sh` | `LM_NLM=30`；指纹并入 `nlm` + `lmfp`；构建命令带 `--nlm=` |
| `scripts/regress.sh` | 新增 `lm_nlm`（第 44 项）；`route_hard_expand` 阈值收回 100 万 |
| `tools/lmbuild_cli.c`（§52.6） | `MAX_LM` 32 → 64；列表追加 31~50；`N_DEFAULT_LM` 钉成 **30**（不带 `--nlm` 时不能意外变成 50），上界检查改用 `N_ALL_LM` |

### 52.6 地标这条杠杆的**天花板**：50 个是平的就停手

30 个明显优于 10 个之后，趋势看着还没平（`MAX_LM` 只是 32 挡住了）。把 `MAX_LM`
提到 64、用同一套贪心再追加 20 个（31~50：吕梁/黄骅/蓬莱/庄河/商洛/武安/兴城/
平顶山/定州/韩城/枣庄/容城/聊城/阳泉/永城/瓦房店/遵化/凤县/卢氏/宽甸），
建出 34.5 MB 的表（构建 502 s）后实测 50 条：

| 配置 | 中位 | p90 | 最慢 | >3s | 展开合计 | 耗时合计 | 分钟 |
|---|---|---|---|---|---|---|---|
| **30 地标 w=1.25（发货）** | 123 | 753 | 1503 | 0 | 1043 万 | 11206 ms | 21503 |
| 50 地标 w=1.25 | 129 | 715 | 1372 | 0 | **939 万** | 11236 ms | **21483** |
| 50 地标 w=1.2 | 192 | 914 | 3122 | 1 | 1435 万 | 18900 ms | 21478 |
| 50 地标 w=1.15 | 503 | 2128 | 5503 | 2 | 2857 万 | 43647 ms | 21468 |

**判读（两条判据给出相反的方向，所以要分开看）**：

- **展开数（确定性主判据）说 50 更好**：−10.0%，**41/50 条展开更少**。
- **耗时合计却完全打平**：11206 → 11236 ms（**+0.3%**）。中位 123 → 129 ms（**+5%**）。
- ⇒ 唯一解释：**每展开的成本涨了约 11%**，正好吃掉省下的展开。
  反推：11206/1043万 = 1.074 µs/展开；11236/939万 = 1.196 µs/展开 ⇒
  **每多一个地标，每次展开贵 ~6 ns**（`lm_bound_m` 里那个遍历 nlm 个 uint16 的循环）。
- 质量上 **43/50 条完全没变**，只有 6 条变好 1 条变差（−20 分 = −0.09%）。
  ⇒ 多出来的 20 个地标对绝大多数路线的结果**根本没影响**。

**结论：维持 30 个**。50 个要付 34.5 MB（+63%）和 502 s 构建（+101%），换来的只是
"展开少 10%、总耗时打平、尾巴好 5~9%" —— 而中位反而 +5%。
★ 这条杠杆到此**封顶**：再加地标，省下的展开会被"遍历更多地标"抵消。

★ 教训（值得单独记）：**同一条杠杆上，确定性判据（展开数）和最终判据（耗时）
可以给出相反结论**。这时必须把"每单位成本"一起算出来（本例 µs/展开），
否则会拿着 −10% 的展开数去下"更快"的结论。判据要配成对：**数量 × 单位成本**。

后续若还想在这条路上拿收益，方向不是加地标，而是**让每展开遍历地标更便宜**
（例如表改成 uint8 存、单位放大到 400 m ⇒ 50 个地标只占 50 B 一条 cache line；
罚项从 4045 m 涨到 ~4345 m，对 400 km 的行程只多 0.075%，看起来是划算的）。
未做，留档。

### 52.7 端到端分解：剩下的时间**不在**固定成本上

量热态端到端（同一条路线连跑 4 次取后 3 次，`--repeat=1`）：

| 路线 | 端到端 | 搜索 | 固定（差） |
|---|---|---|---|
| 北京→天津 | 121 ms | 68 ms | ~53 ms |
| 保定→石家庄 | 58 ms | 19 ms | ~39 ms |
| 朝阳→宝鸡（难） | 568 ms | 503 ms | ~65 ms |

- 第一次跑会贵 3~5 倍（北京→天津首次 1098 ms），那是 **mmap 冷页**不是固定成本；
  上面三行是热态。⇒ 量"固定成本"必须先跑一遍把页摸热，否则会得出 160 ms 这种
  被冷页污染的数字（我第一遍就先量错了）。
- 热态固定成本只有 **39~65 ms**，且它随路线区域略涨（碰到的省越多越贵）。
- ⇒ 中位路线的 ~123 ms 里**九成是搜索本身**，优化重点仍在"少展开 / 每展开更便宜"，
  不用去抠启动。

---

## 52.8 地标下界**越界**（真 bug）：开表 w=1.0 比无表差

§50.5 第 5 条那条"负向对照还没做"补上了 —— **做了，而且确实有问题**。
新增常驻工具 `scripts/lmadmissible.py`。

### 判据

双向 `--exact` + `w=1.0` 在 h 可采纳时**保证返回最优解**（route.c 里那段证明）。
无表时 h 是直线距离 ⇒ 它就是**真值**。所以：

> **开表 w=1.0 的预计分钟 必须 == 无表 w=1.0 的预计分钟**。差哪怕 1 分就是下界越界。

前提已验：`H_SPEED_KMH=130`，而全图**没有一条边的限速超过 130**（实测最大
**120 km/h**，46,257 条；`fallback_speed` 最大 100）⇒ 直线 h 确实可采纳。
（扫的是各省 `edges.bin` 自描述头里的 `speed` u8 字段。）

★ 两个坑，都踩过才跑通：
- `--heur-w` 和 `--heur-w-hi` **必须一起给**。`hi` 留默认会让第一遍封顶 20 万后
  换 1.25/1.8 重跑 ⇒ 根本不是 w=1.0（§49 那条"兜底只能有一个入口"的延伸）。
- 不能用 `--bidir=0`：单向 w=1.0 太贵，保定→石家庄 139 km 就超 1200 万上限。
  双向 `--exact` 便宜一到两个数量级。
- 上限要抬到 **2000 万**（峰值内存 2.9 GB，本机 3.96 GB 可用才够）。

### 结果（50 条，seed(7)，与 citypairs.py 同一批城市对）

**一致 46 / 越界 2 / 跑不完 2**，退出码 1：

| 路线 | 开表 | 无表（真值） | 判定 |
|---|---|---|---|
| 南阳→天津 | 501 分 / 897.6 万展开 | **500** 分 / 1102.5 万 | ★越界 +1 |
| 太原→石家庄 | 129 分 / 54.6 万 | **128** 分 / 66.5 万 | ★越界 +1 |
| 西安→秦皇岛 | 737（1736.9 万） | 跑不完 | INCONCLUSIVE |
| 烟台→宝鸡 | 837（1722.7 万） | 跑不完 | INCONCLUSIVE |

- 时间口径只差 1 分，看着像舍入噪声 —— **换 `--mode=dist` 立刻放大**：
  南阳→天津 开表 **895.16 km** vs 真值 **878.41 km**，差 **16.75 km（+1.9%）**。
  时间口径之所以小，是因为 h 换算用 130 km/h 而实际最高限速 120，那 8% 余量
  恰好吸收掉了一部分。**判据精度不够会让人误判成"没事"**，这条要记。
- 有表一侧的展开数**总是更少**（如 897.6 万 vs 1102.5 万）⇒ 表确实在起作用，
  只是不是严格的下界。

### 三个假设，两个已证伪（都做了判别实验，不是猜）

| 假设 | 实验 | 结论 |
|---|---|---|
| 罚项 4045 m 不够，加大就行 | `NAVIMAP_LM_PENALTY_ADD_M` 扫 0/2k/4k/8k/16k（时间）、0/5k/10k/20k/40k（距离） | ✗ 时间口径**完全不变**；距离口径 895.16→894.30→890.68→890.44→882.73，**+40 km 都没收敛到 878.41** |
| 双向 `lm_tc[dir]`（两端目标格）搞错 | 改单向 `--bidir=0` 重跑 | ✗ 单向同样越界（895.16 vs 878.41），与双向无关 |
| 地标 Dijkstra **每节点只存一个状态**，而算路用 (节点,入边) 状态 ⇒ 有转向限制时地标距离偏乐观 | 加 `RouteOpts.lm_no_restr`，建一张**不做转向限制**的表（`skedaddle_lmbuild_cli --no-restr`）重测 | ✗ 照样越界（129 vs 128；895.29 vs 878.41）⇒ **转向限制不是原因** |

顺带：不带限制时 Dijkstra 反而从 7.8 s 涨到 15.0 s（限制被跳过 ⇒ 松弛的边更多）。

### 剩下的候选原因（未证实）

**格内聚合取了 min，而 u 侧要的是 max**：

```
d(u,t) ≥ d(L,t) − d(L,u)
```
- t 侧要 **下界**：格内 min 天然满足（`d(L,t) ≥ min`）✓
- u 侧要 **上界**：现在用的也是格内 min，而 `d(L,u) ≥ min` —— **方向反了**。
  正确做法要减掉一个 ≥ d(L,u) 的量，即**格内 max**（或 min + 「格内两点间的
  **路程**」上界）。而「格内两点的路程」**可以远大于格对角**（3 km 的格子里，
  隔着黄河/高速、两侧没有桥或立交的两个节点，路程能差几十 km）。
  penalty 用的是格对角（~3945 m）⇒ 覆盖不住。

这与"penalty 加到 40 km 都收不住"是**一致**的（需要的余量是"绕行代价"量级）。
要坐实它，得把表改成 **min + max 两份**（`nlm×2` 个 uint16，表 21.2 → 42.4 MB），
再看越界是否消失 —— 这同时也是修法本身。

### 影响面（**这点很重要，别只盯着 bug**）

- 发货配置是 **时间口径 w=1.25**，本来就不承诺最优（w>1 是有意的质量换速度）。
- 50 条实测（§52.2）：开表 w=1.25 比无表基线**质量好 3.21%**（32 好 / 11 差）。
  ⇒ **越界让个别路线差 1 分，整体上开表仍是明显正收益**。
- 真正受损的是**"最优性"这个承诺**：`--exact` 和 `skedaddle_restr_ab` 的对拍依赖它。
  现在这个承诺在开表时不成立。

### 定夺：**接受并写明**（用户 2026-09-28 拍板）

理由：发货是 w=1.25 本就不承诺最优，而开表比无表质量**好 3.21%** ⇒ 整体正收益；
越界只让个别路线差 1 分。要修（min+max）得付表 ×2 + 下界变弱 + 重新定 w，
收益不确定。⇒ 把它**如实写成"加速启发式，不是严格下界"**，并加门禁防恶化。

写进代码的三处：
- `src/route/lmtable.h` 顶部：结论 + 实测数字 + 三个已证伪假设 + 修法线索。
- `src/route/route.h` 的 `exact` 字段：★ 最优性前提是 h 可采纳 ⇒
  **开表时 `--exact` 不再保证最优**，要真值用 `NAVIMAP_LM=0`。
- `src/route/route.c` 双向终止条件那段证明旁边：同一条提示。

### 门禁：不许恶化（regress.sh 第 45 项 `lm_admissible`）

判据故意写成**上界**而不是"相等"：

    开表 w=1.0 的分钟  <=  无表 w=1.0 的分钟 + 1

⇒ 现在（129 vs 128）通过；**将来修好了（128 vs 128）也通过**；只有变得
更差（130 vs 128）才 FAIL。**判据不该在修好那天反而报错。**
真值那一侧是现算的（不是写死 128），换数据/换图集也不会误判。

- 选 **太原→石家庄**：两条已知越界里最便宜的一条（54.6 万 / 66.5 万展开，几秒）。
  另一条南阳→天津要 900~1100 万展开，留给 `scripts/lmadmissible.py` 全量跑。
- **负向对照已验**：把 slack 改成 0 ⇒ `FAIL（开表 129 > 真值 128 + 0）`
  ⇒ 判据有分辨力，不是恒真。
- 实测：`lm_admissible OK（开表 129 分 <= 真值 128 + 1；已知越界 +1 分，未恶化）`。

### 状态

- 回归 **44 → 45 项，45/45 通过**。
- 新增 `scripts/lmadmissible.py`（50 条约 25 分钟，太慢 ⇒ 只放了一条进 regress）。
  加了 `--slack=N`：`--slack=1` = 接受当前已知的最多差 1 分（退出码 0）；
  不加就是严格的"必须相等"判据。
- 诊断旋钮（保留，后续还要用）：`NAVIMAP_LM_PENALTY_ADD_M`（在表自带 penalty 上
  再加 N 米，默认 0）、`RouteOpts.lm_no_restr` + `skedaddle_lmbuild_cli --no-restr`
  （建表时的全图 Dijkstra 不做转向限制）。两者都只查一次 getenv / 只影响建表。
- `products/graph/lm.bin` 仍是 30 地标那份（实验中临时换过的都已恢复并 cmp 校验）。

---

## 53. 分层重构：消掉 map→render 与 render→ui 两处倒置（2026-10-28 立项）

> 起因：2026-10-28 检查"是否一功能一模块、同类功能一个文件"时，发现两处**模块级分层倒置**。
> 与 `ARCH_ASSESSMENT.md` §1.3 记录的一致（那份评估早就画出这两条边，但只标注"语义上合理、无需破坏"）。
> 本章把它们当作**待消除的技术债**立项处理，并补上当初缺的门禁（分层方向没人查 ⇒ 靠自觉 ⇒ 会回流）。

### 53.1 为什么要做

`ARCH_ASSESSMENT.md` §4 的 L2 已经把模块拆成独立 STATIC 库，**链接器强制边界**成了模块契约。
但契约只能防"新的反向依赖"，**不能纠正已经存在的倒置**——倒置还在，边界就是假的：
map 层实际上依赖 render 层、render 层实际上依赖 ui 层。

具体代价：
1. **map→render**：`navmap_map` 库名义上只依赖 `navmap_pbf` + 叶子库，实际却要 render 的符号才能链接。
   任何"只要地图数据、不要渲染"的工具（如 `skedaddle_gen_contours`）都被迫把 render 拖进来。
2. **render→ui**：`navmap_render` 名义上不依赖 `navmap_ui`，实际 `chrome.c`/`audit.c` 调 `ui/window.c` 的
   `app_*` ⇒ 迫使 CMake 用 `-Wl,--start-group` 包裹全部模块库来消解符号级环（CMakeLists.txt:430-450）。
   环一旦存在，就**无法用链接器暴露新的错误**，边界形同虚设。

### 53.2 依赖基线（2026-10-28 实测，非推测）

模块级依赖矩阵（剥注释后按真实 `#include` 构建）：

```
utils  ◄ pbf  ◄ map  ◄ route ◄ render ◄ ui ◄ ui_qt
                   ▲          ▲       │
                   └── search ┘       └── settings（横切）
```

**两处倒置（就是本次要消掉的）：**

| # | 倒置边 | 唯一入口 | 原因 |
|---|---|---|---|
| ① | `map → render` | `src/map/mapset.h:6` `#include "render/labels.h"` | 只为拿 `NamePool` 类型 + `name_pool_open/close` |
| ② | `render → ui` | `src/render/chrome.h:4`、`src/render/audit.h:24` 两处 `#include "ui/window.h"` | 只为拿 `App` 全量定义 |

**①的证据链**（关键：为什么 `NamePool` 该属于 map）：
- `mapset.h` 用到 `labels.h` 的**全部**符号只有 `NamePool`（`MapSrc.np` / `MapSrc.lnp`）+
  `name_pool_open/close`；`LabelCache`/`LabelItem`/`label_*`/`paint_place_labels` 在 map 下**零实际引用**
  （只在 `coverage.h` 的注释里被提及）。
- `NamePool` 的定义现在落在 `src/render/paint.h:88-98`；`name_pool_open/close` 的**实现在
  `src/render/labels.c:12-50`**，而它们的内部只用 `map/binfmt.h` 的 `nmbin_open` / `nm_get_u32` /
  `nmbin_close`（`labels.c:17-33`）——**零 render 依赖**。
- ⇒ `NamePool` 是**纯地图数据概念**（每省 names.bin 的解析结果，随 `MapSrc` 走），放在 render 是历史遗留。
- 附带问题：`name_pool_open/close` 在 `paint.h:100-101` 与 `labels.h:55-56` **重复声明**了一遍。

**②的证据链**（关键：为什么 chrome/audit 属于 ui 而不是 render）：
- render 层 8 个 .c 里，**真正读 `App` 的只有 2 个**：`chrome.c`（35 处提及 + 约 15 个 `app_*` 回调）
  与 `audit.c`（读 `a->gpx/man/ms/rr/sim/view`，调 `app_focus_point`/`app_ovl_a`）。
  `navdraw.c` 只是**注释里**提了一句 App，**零实际依赖**。
- `chrome.c` 的职责（自绘标题栏 / 顶栏 / 右栏 / 面板 / 菜单）本质是 **UI 层**，
  它调 `app_nav_*`/`app_search_*`/`app_gpx_*`/`app_persist_settings` 等**应用动作**——
  渲染层反过来指挥应用层，是标准的倒置信号。
- `audit.c`（离屏像素审计）被 `ui/offscreen_shot.c` 与 ui_qt 共用，是**验收通道**的一部分，不是渲染算法。
- ⇒ chrome/audit 的正确归属是 **ui**，render 应退化为"纯光栅层：只把要素画进 Raster"。

### 53.3 阶段划分（每阶段独立可验证、独立可回滚）

沿用 `ARCH_ASSESSMENT.md` §4 已验证的施工模板：**耦合从低到高、每步独立 commit + 独立等价证据**。

| 阶段 | 做什么 | 消除的边 | 风险 |
|---|---|---|---|
| **R0** | 写本章设计 + 依赖基线（不改代码） | — | 无 |
| **R1** | `NamePool` 从 render 下沉到 map（新建 `src/map/namepool.{h,c}`） | `map→render` | 低（纯搬家） |
| **R2** | `chrome.{c,h}` + `audit.{c,h}` 从 render 移到 ui | `render→ui` | 中（改 CMake 库归属） |
| **R3** | 加 `scripts/check_layers.sh` 分层方向门禁 | 防回流 | 低 |

### 53.4 每阶段的验收判据（统一口径）

- **编译零警告**（`-Werror`，regress.sh 自带警告检查）。
- **依赖方向**：`grep -rn '#include "render/' src/map/` 为空（R1 后）；`grep -rn '#include "ui/' src/render/` 为空（R2 后）。
- **行为等价**：R1/R2 前后各出一张 `--shot-offscreen` 图，`python3 scripts/bmpdiff.py 旧 新` 判 **IDENTICAL**
  （不是"看起来一样"——渲染改动最常见的失效模式就是"长得还是那样但要素少了"）。
- **UI 逻辑**：`skedaddle --ui-test` 33 项全过。
- **全量回归**：`./scripts/regress.sh` 不劣于重构前（本机 Linux 数据齐全，可跑全量 77/3）。

### 53.5 R3 的门禁为什么必须"能判负"

分层脚本如果只会报"零违规"，那它和没有一样——正是本项目反复踩的坑（负向对照假通过）。
所以 R3 的验收里**强制包含一条负向对照**：人为造一条 `map→render` 的 include，脚本必须 FAIL
并指出具体文件行号；撤掉后必须 PASS。判据没有分辨力就不算门禁。

### 53.6 R2 的额外收益：链接器防线回来了

R2 消除符号级环后，`-Wl,--start-group` **不再需要**，已从 CMake 移除
（`navmap_core` 与 `nm_link_tool` 的 MSVC/GNU 双分支因此合并为一份）。

为什么坚持去掉而不是"留着无害"：

- `start-group` 让链接器**多轮扫描**全部库 ⇒ 新引入的反向依赖会被**悄悄补上**，
  不报 undefined reference ⇒ 模块边界形同虚设，正是"倒置能待几个月"的根因之一。
- 去掉后，任何新引入的 `render→ui` 反向依赖会**立刻 undefined-reference**，
  链接器成为第二道防线（第一道是 `scripts/check_layers.sh`）。

实测：全新目录 configure + build（无 start-group）**零错误零警告**，
出图与基线**逐字节一致**（`diff_pixels 0/1024000`，md5 相同），UI 自测 33 项全过。

### 53.7 R3 的门禁：分层方向脚本

新增 `scripts/check_layers.sh`，并接入 `scripts/regress.sh`（用例名 `layer_dirs`）。

判据：解析每个源文件的**真实** `#include`（剥掉块注释与行注释），
算出模块级依赖图，与允许方向表比对；违规则退出码非 0 并打印**具体文件行号**。

允许方向（同层依赖允许，那是模块内部拆分；反向不允许）：

| 模块 | 允许依赖 |
|---|---|
| `utils` / `pbf` | 叶子，零内部依赖 |
| `map` | pbf, utils |
| `route` | map, utils |
| `search` / `settings` | utils |
| `render` | map, route, search, settings, utils（**零 ui**） |
| `ui` | map, render, route, search, settings, utils |
| `ui_qt` | map, render, settings, ui, utils |

**负向对照（已实测，三项）**：

1. 往 `src/map/coverage.h` 注入 `#include "render/paint.h"` ⇒ **FAIL，exit 1**，
   精确指出 `src/map/coverage.h:67`；撤掉恢复 PASS。
2. 往 `src/render/font.h` 注入 `#include "ui/window.h"` ⇒ **FAIL，exit 1**，
   指出 `src/render/font.h:64`；撤掉恢复 PASS。
3. 注释里写 `#include "render/labels.h"`（历史说明文本）⇒ **PASS**（不算违规）。

第 3 项是必要的：注释里出现 include 路径是本项目的常态（历史说明），
脚本若不做剥注释会产生大量假 FAIL。

### 53.8 状态

- 2026-10-28：**R0/R1/R2/R3 全部完成**。
  - R0：本章 + `ARCH_ASSESSMENT.md` 同步（提交 `9d7aeb7`）
  - R1：下沉 `NamePool`，消除 `map→render`（提交 `9775def`）
  - R2：chrome/audit 移出 render，消除 `render→ui` 与符号级环（提交 `4b3deb4`）
  - R3：新增 `scripts/check_layers.sh` 并接入回归
  - 终态依赖图（纯 DAG，零环）：
    ```
    utils ← pbf ← map ← route ← render ← ui ← ui_qt
                    ↑        ↑      ↑
                  search settings
    ```
  - 回归：**78 通过 / 3 失败**（3 项仍是环境缺失：beijing 无 `lanes.bin` / `disp.idx`）
  - 编译零警告（`-Werror`）；出图逐字节一致；UI 自测 33 项全过

