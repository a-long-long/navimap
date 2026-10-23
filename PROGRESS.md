# 项目进度

> 本文件是**面向人的当前状态总览**：已完成什么、现在什么水平、接下来做什么。
> 详细施工过程见 [`PLAN.md`](PLAN.md)（§0–§52.8，按章节累积）；协作约定与"别再试"清单见
> [`.workbuddy/memory/MEMORY.md`](.workbuddy/memory/MEMORY.md)；每日工作日志见
> `.workbuddy/memory/YYYY-MM-DD.md`。

**最后更新：2026-10-23**

---

## 一、总体状态

| 维度 | 状态 |
|---|---|
| **验收门禁** | ✅ **47/47 通过 + 编译零警告**（2026-10-23 实测） |
| **代码规模** | 76 个源文件 / **19 818 行**（src 56 文件 13 925 行 + tools 20 文件 5 893 行） |
| **数据规模** | 8 省 / **5228 万节点** / **4072 万边** / 去重后 4560 万唯一坐标 |
| **渲染** | ✅ 面/路/线 + 中文标注 + 跨省去重 + 按需加载 |
| **路由** | ✅ 双向加权 A\* + ALT 地标下界 + 三类转向限制 + 跨省缝合 |
| **搜索/导航** | ✅ 地名/POI 检索、偏航重算、ETA、转向指令 |
| **跨平台** | ✅ Linux / macOS / Windows：平台差异统一收进 `src/utils/plat.{h,c}`，业务代码零 `#ifdef`（详见第六节） |
| **未做** | 语音播报、车道指示、多方案对比、GPX、收藏、POI 图标、3D/地形、设置持久化 |

> 渲染与算路的**实测数字**见下节，全部来自 2026-10-22 的回归与基准测试。

---

## 二、当前性能水平（实测）

### 渲染（`map_shot` 第二帧，1280×800，8 省数据）

| 缩放 | 帧时间 |
|---|---|
| z9 | **116 ms** |
| z11 | **56 ms** |
| z13 | **42 ms** |
| z15 | **31 ms** |

（2026-10-22 实测，每档 4 轮取最小值；各轮波动 ±1 ms）

对照起点：2026-09-22 时 z12/z14/z16 分别是 248 / 331 / 269 ms，且 §21 的结论是"多省放大
必须改数据格式"。现在靠**分阶段遍历 + 查表化 + 自写像素光栅化**把这个问题解决了，
数据格式未动。已开 LTO（`-O2 -flto`）；`-O3`、`-march=native` 实测无额外收益。

### 算路（50 条城市对，30 地标 + w=1.25）

| 指标 | 当前 | 无表基线 |
|---|---|---|
| 中位 | **123 ms** | 196 ms |
| p90 | **753 ms** | 3131 ms |
| 最慢 | **1503 ms** | 8913 ms |
| > 3 s 的条数 | **0** | — |
| 展开数 / 里程 | 10.43 M / 21 503 分 | 23.88 M / 22 217 分 |

⇒ **快 1.6×、p90 快 4.2×，质量好 3.21%**。逐条配对实测中，30 地标 @ w=1.25 是**唯一
"速度与质量同时更优"的档位**（好 25 / 差 10）。

---

## 三、已完成的功能（按模块）

| 模块 | 内容 | 关键实据 |
|---|---|---|
| **PBF 解码** | 自写 zlib blob / DenseNodes / delta 编码解码 | `src/pbf/`（4 文件 346 行） |
| **路网构建** | OSM → CSR 邻接 + 转向限制 + 瓦片空间索引 | `src/map/graph_build.c` |
| **图集管理** | 多省加载、**按视口按需 mmap**、副本省检测 | `src/map/mapset.c` |
| **路由引擎** | 双向加权 A\*、ALT 地标下界、跨省缝合、接缝补查 | `src/route/route.c`（80 KB） |
| **转向限制** | 允许集 `only_*` / 必经 `via=way` / 单点禁转 | 全量 3387 条 |
| **反向索引** | 落盘 `rev.bin`（8 省 355 MB），坏了自动重建 | `src/route/revidx.c` |
| **地标表** | 30 地标下界表，460×767 格，21.2 MB | `src/route/lmtable.c` + `lmbuild_cli` |
| **渲染管线** | 面 → 路 → 线 → 标注分阶段；自写像素光栅 | `src/render/paint.c` + `raster.{h,c}` |
| **中文标注** | 字形缓存、字体可选、字号运行期可调 | `src/render/font.c` + `labels.c` |
| **搜索** | 地名/POI 分类加权 + bigram 匹配 | `src/search/search.c` |
| **导航** | 点选起终点、模拟导航、偏航重算、转向指令 | `src/ui/window.c` + `maneuver.c` |
| **地名库** | PBF → SQLite `places.db`，POI 已入库（PLK_POI/FEATURE） | `tools/places_build.c` |

---

## 四、工具链（20 个）

**验收/自检类**（把 GUI 无法无头验证的逻辑做成可断言的 CLI）：

- `map_shot` 渲染截图 + 像素级断言 ｜ `route_cli` 算路基准 ｜ `navsim_cli` 导航模拟
- `restr_ab` 转向限制**双向判定 + 负向对照** ｜ `heap_selftest` 四叉堆对拍（含 `--expect-fail`）
- `tile_selftest` 瓦片索引**全图 5200 万节点逐点**对拍 ｜ `tile_xcheck` 跨省坐标一致性
- `graph_check` / `graph_extract` 图结构校验 ｜ `revbuild_cli` 反向索引落盘校验
- `coverage_cli` 覆盖度 ｜ `search_cli` 搜索 ｜ `places_build --selftest` POI 分类自检
- `plat_selftest` **平台抽象层契约**正/负双向验收（2026-10-23 新增）

**构建类**：`places_build` 地名库 ｜ `lmbuild_cli` 地标表 ｜ `pbf_info` ｜ `tag_stats` ｜ `restr_probe`

---

## 五、显示层按需解码（PBF 模式，2026-10-22 新增）

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
- **等价性**：解码结果与落盘文件**8 省逐字节相同**（`pbfmap_check` 工具，8 省全 IDENTICAL）。
  渲染端零改动——解码出的缓冲直接挂到 `MapSrc` 的 `lines/areas/polyrefs` 上。
- **实测代价**：解码耗时 天津 0.6 s / 北京 1.6 s / 河北 4.4 s；峰值 RSS 104 / 275 / 714 MB。
  ⚠️ 解码是**同步**的，首次进省会停顿，应在后台线程预热。
- **收益**：磁盘省 **195 MB**（8 省 lines+areas+polyrefs 合计）。
  ⚠️ 但 nmbin 的 4.2 G 里 **94% 是路由图**（nodes/edges/edgelist/rev），
  那部分**省不掉**（A\* 需要全图），仍走 mmap。

**两种模式现已逐像素完全一致（12 组实测 0 差异）**

原先有一处 12.357% 的文字像素差异，根因不是数据而是**一个悬垂纹理指针**，已修复：

`app_render_to_surface` 每帧新建/销毁一个软件渲染器，但销毁前没有作废字形缓存。
`glyph_get` 靠 `tex_owner != r` 判断"renderer 换了没有"，而判据是**指针相等** ——
中间的 `SDL_DestroyRenderer` 把那块内存还回分配器，下一帧 `SDL_CreateSoftwareRenderer`
很可能拿回**同一个地址**（实测 8 省图集上就是如此）。于是判据失效，缓存里的 `g->tex`
成了悬垂指针，下一帧 `RenderCopy` 画出错的字。

之所以难以定位：它**只在"两帧地址恰好相同"时显形**，而地址是否相同取决于堆布局 ——
任何改变分配顺序的改动都可能把它从"不出现"推到"出现"。ASan/valgrind 也查不到
（不是 use-after-free，SDL 内部仍认为该纹理有效）。

修复就是在销毁 renderer 前无条件 `font_drop_textures()`（`app_ensure_canvas` 早就这么做，
只是漏了这条路径）。详见 `src/ui/window.c` 的注释。

**教训**：SDL 对象销毁/重建的成对约定，必须在**每条**销毁路径上落实，不能只靠
"换个对象自然能判出来"——尤其当判据是**指针相等**时，它对"地址被复用"完全失效。

---

## 六、跨平台与硬编码清理（2026-10-23）

**动机**：代码里散落一批"只在本机成立"的写法——编译期全绿，换平台才炸，且报错位置
离写的地方很远。一次性收进平台抽象层，并配正反双向门禁。

**新增 `src/utils/plat.{h,c}`（平台抽象层，唯一允许出现平台差异的地方）**

| 收编的对象 | 原来散落在 | 现在 |
|---|---|---|
| `/usr/share/fonts/...` 字体链 | `render/font.c` 写死 5 条 | `nm_font_candidates`：Linux 原链不动 + Windows `%WINDIR%\Fonts`（simsun/msyh/simhei/simfang/simkai）+ macOS `Songti/STHeiti/PingFang/Arial Unicode` |
| `/tmp/navimap_shot.bmp` | `tools/map_shot.c` | `nm_temp_dir()`（TMPDIR/TEMP/TMP → /tmp → .），本机取值与旧版逐字节一致 |
| `mmap`/`munmap`/`madvise`、`mallopt` | `utils/mem.c`、`map/binfmt.c`、`route/revidx.c` 各自内联 | `nm_map_file`/`nm_unmap_file`（Windows: CreateFileMapping + MapViewOfFile，映射句柄按基址登记）；`mallopt` 构造函数加 `M_MMAP_THRESHOLD` 宏守卫，不满足条件整段消失（不留空函数，零警告门禁） |
| `opendir`/`readdir`/`mkdir`/`unlink`/`rmdir`/`lstat`/`stat` | `map/mapset.c`、`map/autobuild.c`、`tools/places_build.c`、`tools/graph_extract.c` | `nm_dir_*` / `nm_mkdir_p` / `nm_rmtree` / `nm_unlink` / `nm_stat_file` |
| `/proc/self/exe`、PATH 按 `:` 拆、`fork`+`execv`、`kill`、`usleep`、`getpid` | `map/autobuild.c` | `nm_exe_path`（Linux `/proc` / macOS `_NSGetExecutablePath` / Windows `GetModuleFileName`）、`nm_find_in_path`/`nm_which`（Windows 补 `.exe`、按 `;` 拆）、`nm_spawn_wait`、`nm_pid_alive`、`nm_sleep_ms`、`nm_get_pid` |
| `open(O_CREAT\|O_EXCL)` 转换锁 | `map/autobuild.c` | `nm_create_exclusive`（Windows: `CREATE_NEW`） |
| `clock_gettime` × 11 处、`getrusage` × 3 处 | 全库 | `nm_now_ms`/`nm_now_us`/`nm_peak_rss_mb` |
| `strcasecmp`、`strdup`、`rename` 覆盖语义 | 3 个文件 | `nm_strcasecmp`、`nm_strdup`、`nm_rename`（Windows 显式 `MOVEFILE_REPLACE_EXISTING`） |

**CMake 跨平台化**：`m` 库仅类 Unix 链接；Windows 链 `psapi` 并定义
`SDL_MAIN_HANDLED`（不链 SDL2main）+ `_CRT_SECURE_NO_WARNINGS`；
protobuf-c / SDL2_ttf 的查找**去掉 `NO_DEFAULT_PATH`**（原来只认 `~/.local`，
换一台干净机器直接 FATAL）改为 HINTS 优先、默认路径兜底，库名加 `libprotobuf-c` 形式。

**门禁（第 46/47 项，`tools/plat_selftest.c`）**：正向断言"该成功的必须成功"
（路径拼接含溢出拒绝、目录枚举一个不少、覆盖式 rename、文件映射读回、
**spawn 自身并原样带回退出码 42/7**、PATH 查找、pid 存活、Linux 首选字体不变）；
负向断言"该失败的必须失败"（不存在路径、空 PATH、起不来的进程）。
**判据有齿已实证**：把 `nm_file_exists` 临时改成恒真，正向 5 处 / 负向 2 处报红，
还原后恢复——不是"永远绿"的门禁。

**影响面**：POSIX 侧行为逐字节不变（`nm_path_join` 与旧 `"%s/%s"` 同输出、
字体链不变、渲染管线未动）⇒ 47/47 回归全绿即证明无回归。Windows 分支按契约编写，
本机无 mingw 未能实机编译，已通过 plat_selftest 把接口契约钉死。

**二次排查（同日，全仓五类重扫）**：C 代码确认干净后，残余硬编码全在构建脚本层——
`env.sh` 注释里的绝对路径与写死的 `aarch64-linux-gnu` 三元组（改 `gcc -dumpmachine`
推导）、CMake `find_library` 的同款三元组（改 `${CMAKE_LIBRARY_ARCHITECTURE}`）、
`sync_maps.sh` 的 GNU 专有 `stat -c` / `nproc` / `md5sum`（加 `fsize`/`fmtime`/`md5of`
双兼容封装）、`regress.sh` 的 `dd status=none`（改 `2>/dev/null`）。
`sync_maps.sh --dry-run` 8 省全 SKIP 证明封装与旧管线输出逐位一致；
47/47 回归复跑全绿。

---

## 七、近期工作（2026-10-22）

| 工作 | 结果 |
|---|---|
| **图集按需加载** | 运行时只 mmap 视口所在省份，滚出即卸载。北京 z11 实测：**加载 3 省 / 卸载 5 省**；换中心动态换省；z7 大范围时全省保持加载。省份下标恒定（`prov` 写进路线结果，不可重排 `srcs[]`） |
| **PBF vs OBF 评估** | 实测磁盘构成：PBF 446 M vs nmbin 4.2 G，其中**显示相关仅 ~19 M/省、94% 是路由图** ⇒ 结论"nmbin 值得保留，可不演化 v3"；**不引入 OBF**（POI 已从 PBF 抽取并入库） |
| **全面代码审查** | 静态通读 24 个运行时源文件 + 18 个工具 + 动态 45/45 回归 + 50 路 ASan/UBSan 巡检 ⇒ **未发现需修复的 bug** |
| **建立版本管理** | 首次纳入 git（原先不是仓库），推送到 GitHub `a-long-long/navimap` |

---

## 八、接下来的候选（按性价比排序）

1. **PBF 模式的后台解码**——现在是同步的，河北首次进省卡 4.4 s，需预热线程 + 进度提示。
2. **补 POI 图标与更多显示要素**（建筑 / 等高线 / 土地利用着色）——从 PBF 侧补，
   不换数据源。
3. **设置持久化**——目前只有运行时开关（细节等级、面板、字号、缩放），**无任何配置文件**。
4. **语音播报 / 车道指示 / 多方案对比**——导航功能补齐。

> ⚠️ 改全局行为（权重 w、地标数、启发式）前必读 `.workbuddy/memory/MEMORY.md` 的
> "方法论"与"别再试"清单：手挑样本下结论已翻车 4 次，改地标数必须连带重定 w。

---

## 九、怎么继续

```bash
source env.sh                  # 修正 PKG_CONFIG_PATH / LD_LIBRARY_PATH
./scripts/regress.sh --build   # 构建 + 47 项回归 + 零警告门禁（必须全绿）
./scripts/sync_maps.sh         # 重建 products/ 产物
```

**验收纪律**（详见 `MEMORY.md`）：布尔判据用**确定性量**（展开数 / 里程 / 像素差），
不用墙钟时间；A/B 至少 4 轮；探针量完撤干净。
