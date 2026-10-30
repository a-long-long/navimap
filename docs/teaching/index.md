# NavMap 分课教程 · 总索引

> 本目录是 [`../teaching-plan.md`](../teaching-plan.md) 的**逐课展开版**：每一课单独成文件，统一按
> **① 教学目的 → ② 功能构成 → ③ 怎么实现 → ④ 与其他功能的联系** 四段式详述，并附课堂演示 / 练习 / 验收。
> 所有代码事实均来自当前 `src/` 真实源码。

**课程总览与程序运行流程图见 [`lesson-01-overview.md`](lesson-01-overview.md)（第 1 课）——建议从这里开始读。**

## 课程表（按程序运行流程排序）

| 文件 | 课次 | 主题 |
|---|---|---|
| [lesson-01-overview.md](lesson-01-overview.md) | 1 | 课程总览与程序运行流程（含运行流程图） |
| [lesson-02-coords.md](lesson-02-coords.md) | 2 | 坐标系与大地基准 |
| [lesson-03-platform.md](lesson-03-platform.md) | 3 | 平台抽象层 |
| [lesson-04-pbf.md](lesson-04-pbf.md) | 4 | PBF 流式解析 |
| [lesson-05-binmap.md](lesson-05-binmap.md) | 5 | 自描述二进制与图集 |
| [lesson-06-graph.md](lesson-06-graph.md) | 6 | 建图：PBF→路网 |
| [lesson-07-viewport.md](lesson-07-viewport.md) | 7 | 视口、投影与瓦片键 |
| [lesson-08-raster.md](lesson-08-raster.md) | 8 | 软件光栅与性能旋钮 |
| [lesson-09-pipeline.md](lesson-09-pipeline.md) | 9 | 渲染管线编排 |
| [lesson-10-search.md](lesson-10-search.md) | 10 | 地名搜索 |
| [lesson-11-routing.md](lesson-11-routing.md) | 11 | 跨省路由算法 |
| [lesson-12-navvoice.md](lesson-12-navvoice.md) | 12 | 导航层与语音 |
| [lesson-13-qt.md](lesson-13-qt.md) | 13 | Qt 前端与边界 |
| [lesson-14-gates.md](lesson-14-gates.md) | 14 | 架构纪律与验收门禁 |
| [capstone.md](capstone.md) | ★ | 综合项目 |

## 阅读建议

- 顺序即**程序运行流程**：总览（流程图）→ 底座（坐标/平台）→ 数据生产（PBF→二进制→建图）
  → 每帧渲染（视口→光栅→管线）→ 用户交互（搜索/路由/导航）→ 外壳（Qt）→ 门禁。
- 每课的"④ 与其他功能的联系"标了上游 / 下游，对照第 1 课的流程图一起读，能串起完整链路。
- 环境搭建与构建命令见 [`../teaching-plan.md`](../teaching-plan.md) 的「一、环境搭建」。
