# 第 7 课 · 软件光栅与性能旋钮

> 📖 配套读源码讲义：[`docs/teaching/07_raster.md`](../../docs/teaching/07_raster.md)（读源码式讲解，讲清楚“为什么这么写”；与本科小程序同号对应）


**累加内容**：自写光栅的动机，以及"画质 ↔ 帧时间"如何用确定性量来权衡。本课实现 `Raster`
（像素缓冲 + 裁剪矩形）、`raster_line`（Cohen-Sutherland 裁剪 + Bresenham，int64 中间量防溢出）、
`raster_px_rgba`（alpha 混合），以及一个面 LOD 旋钮 `AREA_DOT_PX`。

**运行**：
```bash
gcc -std=c11 -O2 -o tut main.c && ./tut
```

**你会看到**：一根从画布外穿入的红线（被裁剪）、一块半透明蓝色矩形（alpha 混合）、一个绿色大面
（填充）和一个绿色小面（只点一个像素）。

**关键概念（项目里一致）**：
1. **为什么自写光栅**：SDL 软渲染每次 `DrawLine` 有固定税；自写后这笔税消失，且与 SDL
   **逐像素等价**是硬要求（`raster_clip_line` 逐字移植，`tools/clip_equiv.c` 用 20 万随机线段门禁）。
2. **★ `AREA_DOT_PX` 旋钮（定档 4）**：面外接框小于阈值走"只点一个像素"快速路径。档位 4 时
   `z9 −21%`、净损失面积 3.46%——这是"画质↔帧时间"权衡的实证范例。本课为演示把它放大到 30。
3. **线要素 bbox 缓存**：线无瓦片索引，每帧扫 10 万条线、每条最多 4096 点全投影，占 z14 一帧 75%；
   `line_bbox` 懒填缓存消除它。

**生产最终形态**：`src/render/raster.c`（`Raster`/`raster_line`/`raster_fill_rect`/`raster_px_rgba`）、
`src/render/paint.c`（`AREA_DOT_PX`、`paint_areas`/`paint_roads`/`paint_lines`）。

**练习**：把 `AREA_DOT_PX` 改小（如 10）再改大（如 80），重跑，观察小面从"单点"变成"填充"的临界点。
