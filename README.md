# navimap

纯 C 实现的**离线地图渲染与导航**程序。

自写 OSM PBF 解码器 → 编译成自有的 `.nmbin` 产物 → `mmap` 懒加载 → SDL2 自绘渲染 +
双向加权 A\* 路由。**零第三方地图/导航库**，除 SDL2（渲染）、SQLite3（地名库）与 zlib（PBF 解压）。

---

## 特性

| 方面 | 说明 |
|---|---|
| **渲染** | 面 / 路 / 线要素，自写像素光栅化（`src/render/raster.{h,c}`），地名与路名标注 |
| **路由** | 双向加权 A\*，ALT 地标下界加速，CSR 邻接 + 4 叉堆 + 状态哈希表 |
| **转向限制** | 允许集（`only_*`）/ 必经（`via=way`）/ 单点禁转三类，跨省接缝补查 |
| **搜索** | 地名 / POI（`amenity`/`shop`/`tourism`/… 分类加权），bigram 匹配 |
| **导航** | 偏航重算、ETA、转向指令（直行 / 左右转 / 环岛第 N 出口 / 掉头） |
| **空间索引** | 瓦片级索引 + 查表化，跨省查询与覆盖度检测 |
| **按需加载** | 运行时只 mmap 视口所在省份，滚出即卸载（`src/map/mapset.c`） |

### 实测性能（本机，8 省数据）

- **渲染**（第二帧，1280×800）：z9 115 ms / z11 56 ms / z13 42 ms / z15 31 ms
- **算路**（50 条城市对，中位 / p90 / 最慢）：123 ms / 753 ms / 1503 ms，>3 s **0 条**
- 图规模：5228 万节点 / 4072 万边；全部数据 4.2 GB（路由图占 94%）

---

## 依赖

- **SDL2** + **SDL2_ttf**（渲染与字体）
- **SQLite3**（地名库 `places.db`）
- **zlib**（PBF blob 解压）
- **protobuf-c** + **protoc-c**（**仅构建期**：把 `proto/*.proto` 编成 C 代码；
  `src/pbf/` 的解码逻辑是手写的，生成产物不入库）
- CMake ≥ 3.13、支持 C11 的编译器

> 本机的 SDL2_ttf / protobuf-c 装在 `~/.local`，默认 `pkg-config` 看不见 ⇒ **构建前先 source 环境脚本**。

## 构建

```bash
source env.sh          # 修正 PKG_CONFIG_PATH / LD_LIBRARY_PATH / PATH
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
```

`env.sh` 会在 source 时自检三条依赖并打印版本（`sdl2` / `SDL2_ttf` / `libprotobuf-c`），
没有输出说明环境没生效。

## 跨平台

目标平台：**Linux / macOS / Windows**（MSVC 与 MinGW）。所有"只在某平台成立"的调用
统一收进平台抽象层 `src/utils/plat.{h,c}`，业务代码里不再出现 `#ifdef _WIN32`：

| 平台差异 | 抽象层提供 |
|---|---|
| 路径分隔符 / 盘符 / 拼接 | `nm_sep` / `nm_path_join` / `nm_base_name` / `nm_is_abs` |
| 临时目录（`/tmp` 在 Windows 不存在） | `nm_temp_dir`（TMPDIR/TEMP/TMP → `/tmp` → `.`） |
| 字体路径（`/usr/share/fonts` vs `%WINDIR%\Fonts`） | `nm_font_candidates`（宋体优先链，逐平台） |
| `mmap` / `madvise` | `nm_map_file` / `nm_unmap_file`（Windows: CreateFileMapping） |
| `opendir` / `mkdir` / `unlink` / `rmdir` / `stat` | `nm_dir_*` / `nm_mkdir_p` / `nm_rmtree` / `nm_unlink` / `nm_stat_file` |
| `/proc/self/exe` / `fork`+`execv` / `kill` / `usleep` | `nm_exe_path` / `nm_spawn_wait` / `nm_pid_alive` / `nm_sleep_ms` |
| `clock_gettime` / `getrusage` | `nm_now_ms` / `nm_now_us` / `nm_peak_rss_mb` |

各平台构建：

```bash
# Linux / macOS
source env.sh && cmake -B build && cmake --build build -j8

# Windows（MSVC + vcpkg 装好 sdl2 / SDL2_ttf / sqlite3 / zlib / protobuf-c）
cmake -B build -A x64 && cmake --build build --config Release
```

Windows 注意：入口 `main()` 直接用（CMake 定义 `SDL_MAIN_HANDLED`，不链 SDL2main）；
数学函数无需显式链 `libm`（仅类 Unix 平台链）；峰值内存走 `psapi`。
字体优先级：`--font=` 命令行 > `NAVMAP_FONT` 环境变量 > 平台候选链。

平台层契约由 `tools/plat_selftest.c` **正反双向**钉住（回归门禁第 46/47 项）：
正向断言"该成功的必须成功"（含 spawn 自身并原样带回退出码），负向断言"该失败的
必须失败"（不存在路径 / 空 PATH / 起不来的进程）。本机只有 POSIX 能真跑，Windows
分支按同一契约编写——新增平台分支时先在 plat_selftest 里补契约，再动业务代码。

## 验收

```bash
./scripts/regress.sh            # 用现有 build/
./scripts/regress.sh --build    # 重新构建 + 47 项回归
```

**47 项回归全绿、编译零警告**是硬门禁（`--build` 把任何新警告当失败）。回归覆盖：
逐省 + 跨省渲染、跨省算路、导航模拟、转向限制正/负对照、反向索引校验、四叉堆自检、
瓦片索引全图对拍、覆盖度、搜索、POI 分类自检、ALT 地标表（生效性 / 地标数 / 下界不越界）、
平台抽象层正/负向契约。

## 显示层按需解码（PBF 模式）

`--pbf-display=<sources目录>[:LRU上限]` 让显示层（面/线要素）**从原始 PBF 运行时解码**，
不再从 nmbin 打开 `lines/areas/polyrefs` —— 这三个文件因此可以整个不落盘。

```bash
./build/navimap --graph=products/graph --pbf-display=sources:3
```

解码结果与落盘文件**8 省逐字节相同**（`build/pbfmap_check` 可验证），渲染端零改动。
代价是首次进省要同步解码（天津 0.6 s / 北京 1.6 s / 河北 4.4 s），适合后台预热。

⚠️ 省磁盘的收益有限：nmbin 的 4.2 G 里 **94% 是路由图**（A\* 需要全图），那部分省不掉。
详见 [`PROGRESS.md`](PROGRESS.md) 第五节。

## 数据

```bash
./scripts/sync_maps.sh          # 从 sources/*.osm.pbf 重建全部产物
```

| 目录 | 内容 | 是否入库 |
|---|---|---|
| `sources/` | 原始 `*.osm.pbf`（8 省，446 MB） | ❌ 上游下载获得 |
| `products/` | `.nmbin` 产物：瓦片 / 路由图 / 反向索引 / 地标表 / 地名库（4.2 GB） | ❌ 可确定性重建 |
| `build/` `build-san/` | 编译产物 / Sanitizer 构建 | ❌ |

`products/` 由 `sync_maps.sh` **确定性重建**（路网按 manifest 指纹、地名库按 `.db.meta` 指纹）。
`.nmbin` 格式自带版本号（`NMBIN_VERSION`），改格式必须顶版本。
用 `--pbf-display=sources` 可让显示层改为运行时从 PBF 解码，`lines/areas/polyrefs` 就不必落盘。

## 目录

```
src/pbf/     自写 OSM PBF 解码（zlib blob / DenseNodes / delta 编码）
src/map/     图集、路由图构建、CSR 图、瓦片索引、覆盖度
src/route/   双向 A*、转向限制、ALT 地标表、反向索引、堆、转向指令
src/render/  渲染管线、自写像素光栅、字形与地名标注
src/search/  地名 / POI 搜索
src/ui/      SDL2 窗口与交互
src/utils/   平台抽象层（plat）、内存与文件映射约定
tools/       20 个命令行工具（构建 / 自检 / 验收 / 诊断）
scripts/     回归门禁、地图同步、像素对比
proto/       OSM 官方 osmformat.proto（字段编号一致，删掉了导航用不到的 osmesis 字段）
```

## 命令行工具

`map_shot`（渲染截图，含像素级断言）、`route_cli`（算路基准测试）、`navsim_cli`
（导航模拟）、`restr_ab`（转向限制双向判定 + 负向对照）、`heap_selftest`（堆对拍）、
`tile_selftest`（瓦片索引全图对拍）、`graph_check` / `graph_extract`（图结构校验）、
`places_build`（地名库构建）、`lmbuild_cli`（地标表构建）等。

---

## 文档

- [`PLAN.md`](PLAN.md) —— 设计与实现记录（渲染 §19-32 / 算路 §33-50 / 转向限制 §14·§33）
- [`arch.md`](arch.md) —— 需求与架构
- [`OSMAND-APP-MODULES.md`](OSMAND-APP-MODULES.md) —— OsmAnd 模块汇总与功能域对照
- `.workbuddy/memory/` —— 长期约定与各阶段工作日志

## 授权与参考

本项目为**独立实现**。对已有开源项目的借鉴均**在代码注释中点名注明出处**（如坐标键编码
借鉴自 OsmAnd `RoutePlannerContext.cpp`，加权 A\* 启发式借鉴自 OsmAnd `RoutePlanner.cpp`），
实现均为自行改写。`proto/` 下的文件是 OSM 官方 `osmformat.proto`（字段编号与官方一致，
仅删除导航用不到的 osmesis 字段），另有 `fileformat.proto`（OSM 官方 PBF 容器格式）。

地图数据 © OpenStreetMap 贡献者，遵循 ODbL。
