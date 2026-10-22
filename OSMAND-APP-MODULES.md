# OsmAnd-app 项目功能 / 模块汇总

> 用途：本文件汇总 `/data/home/along/osmand-app` 工作区的功能与模块结构，
> 作为 navimap 的**参考与路线图**（不是复制对象）。
> 数据来源：2026-10-22 对本机工作区的目录勘察 + 同工作区 `nav-desktop/NAVIMAP-COMPARISON.md`
> 的功能对照。navimap 自身只**参考 OsmAnd 的少数算法思路**（坐标键、加权 A\*，
> 见 `arch.md` §2-3 与 `src/map/coordkey.h` 注释），不复制其代码、设置或界面。

---

## 1. 总览

| 维度 | 数值 |
|---|---|
| 工作区构成 | **13 个并排 git 仓库**（非单仓、无 `.gitmodules`），共约 11 GB |
| 真正在开发的项目 | `nav-desktop`（你的桌面导航私仓） |
| 其余 12 个 | OsmAnd 官方生态的依赖 / 源码 / 资源仓库，平时只拉不改（除本机已做的 aarch64 适配） |
| 技术栈 | C++（核心引擎）+ Java（android / nav-desktop）+ Qt / Skia（渲染依赖） |

**一句话**：osmand-app 是完整导航产品（安卓 + 桌面），navimap 是其**功能子集 + 极致性能**的从零重写（纯 C + SDL2）。

---

## 2. 顶层仓库清单（13 个）

| 仓库 | 体积 | 角色 |
|---|---|---|
| **nav-desktop** | 562 M | 你的桌面导航私仓：抄写 OsmAnd Android 界面、**直接复用官方引擎**跑起来的桌面程序（Java/Swing + Java2D，跨 Win/Linux/macOS，含 arm64/UOS20） |
| **android** | 1.6 G | OsmAnd Android 客户端（最大的官方仓库） |
| **OsmAnd-core** | 5.4 G | 官方 **C++ 导航引擎**；本机已做 aarch64 适配（10 处改动已 commit） |
| **OsmAnd-resources** | 1.2 G | 地图资源 / 图标 / 字体 / 样式 |
| **docs** | 788 M | 官方文档站（已加 `zh-CN/` 中文文档） |
| **OsmAnd-external-qtbase** | 552 M | Qt 基线（构建依赖） |
| **OsmAnd-external-skia** | 490 M | Skia 图形库（渲染依赖） |
| **OsmAnd-misc** | 188 M | 杂项资源（fastlane / planet 等） |
| **OsmAnd-tools** | 205 M | 官方工具（obf 生成 / java-tools 等） |
| **OsmAnd-build** | 868 K | 跨平台构建脚本（已加 `aarch64-linux-gcc.*` 配置） |
| **OsmAnd-manifest** | 964 K | repo 多仓聚合清单 |
| **core-legacy** | 12 M | 旧版 C++ 核心（历史 / 嵌入式） |
| **baked / binaries** | — | 构建产物缓存 |

---

## 3. 核心引擎 `OsmAnd-core/src` 模块详解

### 3.1 Data/ —— OBF 数据读取与数据模型
- **OBF 分段读取器**：`ObfMapSectionReader`（地图要素）、`ObfAddressSectionReader`（地址/门牌）、`ObfPoiSectionReader`（POI）、`ObfRoutingSectionReader`（路网）
- **文件与元数据**：`ObfFile`、`ObfInfo`、`ObfReader`、`ObfReaderUtilities`
- **数据模型**：`MapObject`、`BinaryMapObject`、`Amenity`、`Building`、`Address`

### 3.2 Map/ —— 渲染引擎
- **主渲染器**：`AtlasMapRenderer`（多阶段渲染管线）
  - `AtlasMapRendererMapLayersStage`（图层）
  - `AtlasMapRendererSkyStage`（天空）
  - `AtlasMapRendererSymbolsStage` / `SymbolsStageModel3D`（符号 / 3D 模型）
  - `AtlasMapRendererDebugStage`（调试）
- **符号与图标**：`AmenitySymbolsProvider`、`CoreResourcesAmenityIconProvider`（官方 4878 个 SVG 图标）
- **几何 / 地形**：`GeoContour`（等高线）、`GeometryModifiers`、`Frustum2D/3D`（视锥裁剪）
- **收藏 / 动画**：`FavoriteLocationsPresenter`、`ValueAnimator`、`TextRasterizer`（文字光栅化）

### 3.3 Routing/ —— 路由引擎
- **主规划器**：`RoutePlanner`、`RoutePlannerContext`、`RoutePlanner_Analyzer`
- **路段与配置**：`RouteSegment`、`RoutingProfile`、`RoutingProfileContext`、`RoutingConfiguration`、`RoutingRuleset`、`RoutingRuleExpression`（路由规则表达式）
- **转向**：`TurnInfo`（转向类型 / 车道解包）

### 3.4 Search/ —— 搜索
- **基类**：`BaseSearch`、`ISearch`
- **按名称 / 区域**：`AmenitiesByNameSearch`、`AmenitiesInAreaSearch`、`AddressesByNameSearch`
- **特殊搜索**：`CoordinateSearch`（坐标反查）、`ReverseGeocoder`（逆地理编码）、`TransportStopsInAreaSearch`（公交站点）
- **辅助**：`Abbreviations`、`CommonWords`

### 3.5 顶层通用模块（core/src 根）
- **资源管理**：`ObfsCollection`、`ResourcesManager`、`IObfsCollection`
- **路网定位**：`RoadLocator` / `CachingRoadLocator`、`IRoadLocator`
- **收藏 / GPX**：`FavoriteLocation(s)`、`FavoriteLocationsGpxCollection`、`GpxDocument`
- **区域**：`WorldRegions` / `WorldRegion`
- **网络 / 在线路由**：`WebClient`、`NetworkRouteContext` / `NetworkRouteSelector`
- **瓦片 / 栅格**：`TileSqliteDatabase(s)`、`TileSqliteDatabasesCollection`、`GeoTiffCollection`、`MvtReader`（矢量瓦片）
- **文字 / 图形**：`TextRasterizer`、`SKIA`、`HarfbuzzUtilities`、`ICU`、`Color`
- **压缩 / 归档**：`ArchiveReader` / `ArchiveWriter`、`zlibUtilities`
- **基础设施**：`Utilities`、`Logging`、`Metrics`、`Concurrent`

---

## 4. 桌面应用 `nav-desktop/src/main/osmand/nav` 包划分

| 包 | 职责 |
|---|---|
| `nav/core` | 引擎桥接（调用官方 OsmAnd Java API） |
| `nav/data` | 数据模型 |
| `nav/route` | 路由集成（前端 + GeneralRouter） |
| `nav/render` | 渲染集成 |
| `nav/model` | 领域模型 |
| `nav/ui` | 界面主体 |
| `nav/ui/panel` | 侧栏 / 面板 |
| `nav/ui/fx` | 界面特效 |

> 注意：`nav-desktop` 自身只写**产品层**（UI/交互/功能广度），底层读 OBF、官方路由、
> 样式引擎、GPX、区域下载全部**直接调用官方引擎**，官方仓库一行未改。

---

## 5. Android 客户端 `android/OsmAnd/src/net/osmand/plus` 主要模块（节选）

`activities`、`api`、`auto`（Android Auto）、`avoidroads`（避让）、`backup`、`base`、
`card`、`dashboard`、`dialogs`、`download`（下载管理）、`exploreplaces`、`feedback`、
`firstusage`、`gallery`、`mapcontextmenu`、`mapmarkers`、`measurementtool`（测量工具）、
`media`、`myplaces`（我的地点）、`configmap`、`charts`、`liveupdates`、`inapp`、
`help`、`keyevent`、`importfiles`、`mapsource`、`chooseplan` 等。

---

## 6. 资源 / 工具 / 外部依赖

| 仓库 | 内容 |
|---|---|
| **OsmAnd-resources** | `icons/`（SVG 图标）、`fonts/`（字体，含思源/Noto）、`color-palette`、`countries-info`、`astro`（天文/日出日落） |
| **OsmAnd-tools** | `java-tools`、`obf-generation`（PBF→OBF，即 `IndexCreator`）、`cpp-tools`、`python`、`docker`、`db`、`ais`、`mirror_externals.sh` |
| **OsmAnd-misc** | `fastlane`（发布）、`osm-live`、`osm-planet` |
| **OsmAnd-external-qtbase / -skia** | 构建期渲染依赖（Qt 基线 / Skia 图形库） |

---

## 7. 关键功能域总览（对照 navimap 已有能力）

| 功能域 | osmand-app 能力 | navimap 现状 |
|---|---|---|
| **地图数据** | OBF 官方格式、PBF→OBF 转换、在线下载（约 1048 项）、无图区反查 | 自研 NMBIN v3，只吃 `.osm.pbf`；手动放 `sources/`；离线转图 `sync_maps.sh` |
| **渲染** | render.xml 样式引擎（14+9 套）、昼夜、POI 图标 4878、3D、地形/等高线、在线瓦片 | 自写软件光栅化 `raster.c` + 固定 4 层样式；无图标/昼夜/3D/地形 |
| **搜索** | 短语式递进搜索、地址门牌、POI 19 类分类、在线反查 | SQLite bigram 倒排 + 子串复核 + 距离排序；仅地名库 |
| **路由** | car/bicycle/pedestrian/**public_transport**、32 参数、多方案、途经点、手动避让、GPX 贴合 | A\*/双向 A\* + 30 地标 ALT + 自适应 w；只车行；单一方案 |
| **导航引导** | 语音播报、车道指示、13 类告警、转向箭头、自动缩放/指北针、ETA 单位制、真 GNSS | 状态机（偏航重算/ETA）；无语音/车道/告警；仅模拟注入定位 |
| **产品功能** | 收藏（与 `favourites.gpx` 互通）、历史、轨迹记录、GPX 导入导出、测量、坐标系/网格、区域管理、**设置持久化**、菜单/对话框、无标题栏窗口 | 无收藏/历史/轨迹/GPX/测量/坐标系；**零持久化**；无菜单；系统窗口 |

---

## 8. 与 navimap 的关系

- **可借鉴（.navimap 路线图来源）**：第 7 节里标「❌/缺失」的每一项，都可按"先算法后产品"的顺序补进 navimap。
- **不可照搬**：osmand-app 是 GPL（引擎）/ MIT（PR）授权；navimap 应继续坚持
  「借鉴思想 + 注明出处 + 不同语言/框架独立实现」的干净路径（见 `coordkey.h` 注释范式），
  不逐字复制代码。
- **已验证的非复制证据**：navimap 全代码库 0 个 OsmAnd 专有符号作为真实代码
  （`RouteCalculationSegment`/`ObfRoutingSectionReader`/`shared_ptr`/`QMap` 仅出现在注释里）；
  坐标键实现已从 OsmAnd 的 `(x31<<31)|y31` 改写为 navimap 的 e7 精度 `(lat_e7<<32)|lon_e7`。

---

*生成：2026-10-22，基于本机 `/data/home/along/osmand-app` 目录勘察。*
