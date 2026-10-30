# 第 5 课 · 建图：PBF → 路网（CSR）

> 📖 配套读源码讲义：[`docs/teaching/05_graph.md`](../../docs/teaching/05_graph.md)（读源码式讲解，讲清楚“为什么这么写”；与本科小程序同号对应）


**累加内容**：从"原始 OSM 要素"到"可导航图"。本课用一小撮手写节点 + way 演示建图全过程：
① 节点原始 id → 0..n-1 下标映射；② 每条 way 相邻节点对 → 双向边；③ 边代价 = haversine 米。
最终存成 CSR（压缩稀疏行：offset + edge list），这是 A* 高效遍历的内存布局。

**运行**：
```bash
gcc -std=c11 -O2 -o tut main.c && ./tut
```

**你会看到**：stdout 打出 CSR 邻接表（每个节点的出边 + 代价），`out.bmp` 是 5 节点路网
（蓝方块=节点，红线=边）。

**关键概念（项目里一致）**：
1. **最小导航集**：真实建图只抽 `highway=*`、`railway`、通行标志等，不抽全部要素。
2. **CSR 存储**：邻接表用 `offset[] + edge[]` 压缩，A* 遍历只需 `for(e=off[u];e<off[u+1];e++)`，
   缓存友好、零指针跳跃。
3. **限制（restriction）**：`relation` 里的 `type=restriction` 在建图阶段就抽成 `restrictions.bin`，
   路由时查 `restr_blocked` 判定禁转。

**生产最终形态**：`src/map/road_tags.c`（`road_tags_parse` 把 way 的 key/val 变成 `road_class`/
`drivable`/`oneway`/`speed_kmh`/`lanes`…），`src/map/graph_build.c`（两遍扫描：挑可通行 way →
节点下标映射 → 建 CSR 边 + 限制）。

**练习**：把 `way[][4]` 再加一条路（如 `{7002,7005,7003}`），重跑，确认邻接表与图同步更新。
