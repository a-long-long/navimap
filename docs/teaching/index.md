# NavMap 分课教程 · 总索引

> 本目录是 [`../teaching-plan.md`](../teaching-plan.md) 的**逐课展开版**：每一课单独成文件，统一按
> **① 教学目的 → ② 功能构成 → ③ 怎么实现 → ④ 与其他功能的联系** 四段式详述，并附课堂演示 / 练习 / 验收。
> 所有代码事实均来自当前 `src/` 真实源码。
>
> **编号约定（与 `tutorial/` 对齐）**：本目录讲义与 `tutorial/NN_xxx` 小程序**同号对应**——
> `00_overview` ↔ `tutorial/00_minimal`（前者讲"概念总览"、后者是"最小框架动手热身"），
> `01_coords` ↔ `tutorial/01_coords`，……，`13_gates` ↔ `tutorial/13_gates`，
> `14_capstone` ↔ `tutorial/14_capstone`。读源码式讲义看"为什么这么写"，配套小程序看"怎么跑起来"。

**课程总览与程序运行流程图见 [`00_overview.md`](00_overview.md)（第 0 课）——建议从这里开始读。**

## 课程表（按程序运行流程排序，编号 00–14）

| 文件 | 课次 | 主题 | 配套 `tutorial/` |
|---|---|---|---|
| [00_overview.md](00_overview.md) | 0 | 课程总览与程序运行流程（含运行流程图） | `00_minimal` |
| [01_coords.md](01_coords.md) | 1 | 坐标系与大地基准 | `01_coords` |
| [02_platform.md](02_platform.md) | 2 | 平台抽象层 | `02_platform` |
| [03_pbf.md](03_pbf.md) | 3 | PBF 流式解析 | `03_pbf` |
| [04_binary.md](04_binary.md) | 4 | 自描述二进制与图集 | `04_binary` |
| [05_graph.md](05_graph.md) | 5 | 建图：PBF→路网 | `05_graph` |
| [06_viewport.md](06_viewport.md) | 6 | 视口、投影与瓦片键 | `06_viewport` |
| [07_raster.md](07_raster.md) | 7 | 软件光栅与性能旋钮 | `07_raster` |
| [08_pipeline.md](08_pipeline.md) | 8 | 渲染管线编排 | `08_pipeline` |
| [09_search.md](09_search.md) | 9 | 地名搜索 | `09_search` |
| [10_routing.md](10_routing.md) | 10 | 跨省路由算法 | `10_routing` |
| [11_navvoice.md](11_navvoice.md) | 11 | 导航层与语音 | `11_navvoice` |
| [12_qt.md](12_qt.md) | 12 | Qt 前端与边界 | `12_qt` |
| [13_gates.md](13_gates.md) | 13 | 架构纪律与验收门禁 | `13_gates` |
| [14_capstone.md](14_capstone.md) | 14 | 综合项目（改一个点 + 加一条判据） | `14_capstone` |

## 阅读建议

- 顺序即**程序运行流程**：总览（流程图）→ 底座（坐标/平台）→ 数据生产（PBF→二进制→建图）
  → 每帧渲染（视口→光栅→管线）→ 用户交互（搜索/路由/导航）→ 外壳（Qt）→ 门禁 → 综合项目。
- 每课的"④ 与其他功能的联系"标了上游 / 下游，对照第 0 课的流程图一起读，能串起完整链路。
- 每课都可在 `tutorial/NN_xxx/` 找到同名编号、可独立编译运行的小程序，建议"先读源码讲义、再跑小程序"或反之。
- 环境搭建与构建命令见 [`../teaching-plan.md`](../teaching-plan.md) 的「一、环境搭建」。
