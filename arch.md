
用 C 语言，在本机（UOS Desktop 20 / aarch64）从零写一个离线地图导航程序（可以自动识别并加载后续下载的地图）（地图以pbf格式为主，后续再添加其他格式）


## 1. 本机环境探测结果（已实测，非推测）

我实际在你的机器上跑了一轮检测，所有结论都是可信的：

1. 已满足的基础环境
项目	状态	检查结果
CPU 架构	已满足	aarch64
操作系统	已满足	UOS Desktop 20
GCC	已满足	gcc 8.3.0
CMake	已满足	3.22.1
Make	已满足	GNU Make 4.2.1
pkg-config	已满足	0.29
SDL2	已满足	2.30.0，位于 /usr/local
zlib	已满足	1.2.11，头文件存在
SQLite3	已满足	3.39.3，头文件存在
FreeType	已满足	libfreetype6-dev 已安装
Fontconfig	已满足	libfontconfig1-dev 已安装
中文字体	已满足	Noto Sans CJK、思源黑体等
Wayland 运行环境	已满足	当前环境可检测到 Wayland
X11 头文件	已存在	可作为兼容环境使用
SDL2_ttf   2.0.15
protoc          3.6.1
protoc-c        1.3.1
protobuf-c 头文件
protobuf C/C++ 头文件
protobuf 静态库
libprotoc.so.17


## 2.参考/home/along/osmand-app项目的功能实现代码
## 3.GUI抄写/home/along/osmand-app项目的GUI界面


一、整体实现思路
目标是：

用 C 语言在 UOS Desktop 20 / aarch64 上从零做离线地图导航
以 PBF 为主地图格式，后续再扩展其他格式
支持自动识别并加载后续下载的地图
GUI 用 SDL2，兼容 Wayland
采用本地 SQLite 做地名/搜索索引
依赖 zlib、sqlite3、SDL2 等本机已装组件，尽量少装依赖
二、推荐实现步骤

项目骨架和构建系统
用 CMake 创建项目结构:
app/
core/
map/
render/
ui/
db/
utils/
目标是先跑起来，再往功能加。
先建立最小可运行窗口：
SDL2 初始化
创建窗口
主循环
事件处理
渲染背景
这一步不要求真正显示地图，只要能确认 SDL2 + Wayland 场景可用。
基础依赖与环境适配
先确认开发环境:
编译器 gcc 8.3.0，C11 可用
使用 pkg-config 自动查找 SDL2、sqlite3、zlib 头文件和库
地图数据入口：PBF 解析
这是最关键的一层。
先用 protobuf 解析 .pbf 文件
需要支持 OSM PBF 的块结构：
PrimitiveBlock
DenseInfo
nodes
ways
relations
抽象出最小数据模型：
Node: id, lat, lon
Way: id, point ids, tags
Relation: id, members, tags
先只做“导航所需最小信息”：
道路/高架/步行道/河流等几类核心要素
坐标和标签必须保留
不必要一开始就做全量 OSM 数据结构，先完成“能读出路网”。
地图数据加载与索引
需要支持“自动识别并加载后续下载的地图”
推荐思路：
扫描地图目录
识别 .pbf 文件
读取文件名/元数据/世界范围/坐标系
将其注册到地图列表
允许用户切换地图
建议把地图资源组织成：
地图目录
metadata.json 或文件索引
地图文件
预处理缓存
最关键是“数据加载尽量分离”，让以后扩展 .osm, .mbtiles, .map 等格式更简单。
地图存储：SQLite + 轻量索引
你机器上 sqlite3 已装，适合做地名和搜索
数据表建议：
places(name, type, lat, lon, country, admin_level)
roads(id, class, name, geometry_ref)
map_files(path, name, bounds, loaded)
这样可做到：
地名搜索
POI 查询
地图文件识别与切换
重点：不要把所有地图数据直接塞进 SQLite 作为主存储；SQLite 更适合索引和查询，而不是把 PBF 解析结果直接保留为完整数据库结构。
地图渲染与视图
使用 SDL2 进行窗口和绘制
采用“屏幕坐标 -> 地理坐标”的映射
需要实现：
缩放
平移
屏幕中心位置追踪
视口裁剪
路网渲染
先做最小渲染：
打开地图
画出背景
画出道路线
画出城市边界/道路名
画出当前定位点
路网渲染建议按层分级：
0: 国家/省级道路
1: 城市道路
2: 局部道路
3: 步道/小路
这样不会一开始就把所有对象都画死。
导航功能
导航程序不是单纯显示地图，而是需要“路线计算”
先做最小可用导航：
从 A 地到 B 地
选一个起点和终点
从道路网络中找路径
路径搜索可按：
先做最简单的图搜索（道路节点有邻接表）
让每个道路节点作为图顶点
路段权重用距离或道路等级
这一步可以后置，但最好一开始就留接口，后面扩展导航引擎。
GUI 交互与可视化
参考 osmand-app 的 GUI 结构和交互方式，但不要直接照搬大而全的代码
建议 UI 分层：
顶部工具栏：地图选择 / 搜索 / 路线
左侧/右侧面板：POI、搜索结果
右下角：缩放控制
底部：状态栏
需要注意：
Wayland 下不依赖 X11 特性
SDL2 事件处理要正确处理鼠标滚轮和拖拽
触摸/鼠标/键盘事件需要统一处理
中文显示与字体
arch.md 明确说明：
中文字体已存在
Noto Sans CJK / 国标宋体可用
所以在 SDL2 中引入 TTF 渲染：
先加载中文字体
路名、城市名、搜索结果都用中文
需要特别注意：
选择字体时不能依赖 GTK3
需要单独设置字体路径
可能需要准备 fallback 字体
后续扩展：多地图格式
PBF 为主，后续再扩展其他格式
最好先设计统一抽象层：
IMapSource
OpenMap()
GetBounds()
QueryPlaces()
Render()
这样扩展 .osm, .osm.pbf, .mbtiles 等时，只要实现一个适配器即可，不要把渲染逻辑和文件格式耦合在一起。
三、最需要注意的部分

ARM64 兼容性
不能使用 x86 指令和 intrinsics
要注意：
struct alignment
endianness
64-bit / pointer 大小差异
所有底层数据结构都要考虑 aarch64 的真实内存布局。
Wayland 兼容性
这是最重要的“平台限制”
虽然 X11 兼容层在，但不要假设 X11 可用
SDL2 对 Wayland 很适合，但必须：
不依赖 GTK
不依赖 X11 API
不调用平台特定的窗口代码
依赖安装策略
arch.md 里很关键的一点是：
zlib、sqlite3、SDL2 已存在
GTK3/Cairo/Pango 缺失
所以建议优先对“已装的依赖”做抽象和封装，先把程序跑起来，再补齐小型缺失库。
PBF 解析不宜一开始做得太重
这是个大坑：
直接做完整 PBF 到导航网络的全量实现，很容易卡住
更稳妥的顺序：
先读 nodes/ways
先做道路和标签
再做搜索索引
最后做路径规划和界面排序
先做到“能看见地图”，再做“功能完整”。
地图数据的巨大规模
离线地图文件会非常大
需要避免：
每次启动都全量解析
每次渲染都重新扫描整张图
方案：
分块加载
视口范围加载
轻量索引
缓存已加载区块
渲染性能
Map 是大数据量场景
一次性渲染全部道路很容易卡
做法：
只绘制当前视口内对象
按等级 LOD（Level of Detail）绘制
线路和点按距离过滤
统一坐标系
地图数据使用经纬度
屏幕绘制使用像素
一定要在数据层和渲染层之间做清晰分离
建议定一组转换函数：
lon/lat -> world coordinate
world -> screen
screen -> world
这样后面做缩放和导航定位时不会乱套。
检索和字典表设计
地名搜索最好独立于地图渲染
用 SQLite 做搜索索引，不要每次扫描整个地图
甚至可以先做“地名表”，后面再做“道路名表”“POI 表”
四、我建议的最稳妥开发顺序

建 CMake 工程
初始化 SDL2 窗口和事件循环
加载 zlib / sqlite3 / SDL2_ttf
先实现一个最简单的 PBF 读取器（只获取 nodes 和 ways）
画出一张最小道路图
实现地图拖动和缩放
实现地名搜索和 SQLite 索引
增加导航路径计算
处理自动识别和加载地图目录中的新地图
扩展多格式地图支持
五、一个现实可行的落地原则
arch.md 的关键点非常明确：这不是“从零写一个全能地图引擎”，而是“在你这台 UOS / aarch64 环境下，用最少依赖、最稳妥的架构，先做一个能跑的离线导航程序”。

所以最重要的原则是：

先跑起来
先看得见地图
再做搜索/导航
最后做缓存和性能优化
如果你愿意，我下一步可以直接按这个思路，给你输出：

一个完整的 CMake 目录结构
每个模块的接口定义
PBF 解析的最小数据结构
SDL2 窗口初始化代码骨架
地图目录自动扫描和 SQLite 索引的设计方案