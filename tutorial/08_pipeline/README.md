# 第 8 课 · 渲染管线编排

> 📖 配套读源码讲义：[`docs/teaching/08_pipeline.md`](../../docs/teaching/08_pipeline.md)（读源码式讲解，讲清楚“为什么这么写”；与本科小程序同号对应）


**累加内容**：看懂一帧怎么从"图集 + 视口"变成"像素缓冲"。本课实现 `paint_frame` 的多阶段编排，
并用 `TICK` 给每个阶段计时——这是定位帧时间瓶颈的唯一手段。

**运行**：
```bash
gcc -std=c11 -O2 -o tut main.c && ./tut
```

**你会看到**：stdout 打印 6 个阶段的耗时（`stage_bg / areas / roads / labels / overlay / chrome`），
`out.bmp` 是分层叠出来的图：米白底 + 两块淡色面 + 三条灰路 + 五个地名锚点 + 一条亮蓝路线 +
顶部标题栏/右栏。

**关键概念（项目里一致）**：
1. **`paint_frame` 的顺序就是图层顺序**：背景 → 视口 bbox → `mapset_set_view`（进省加载/离省卸载）
   → 分阶段跨省统一层级（先所有省的面、再所有省的路）→ 标注 → 叠加层（GPX/路线/车头）
   → 昼夜蒙版 → 面板/chrome。后画的盖先画的。
2. **★ `app_render_to_buf` 是唯一渲染入口**：Qt 把缓冲包成 `QImage::Format_RGB32`，离屏通道额外
   `write_bmp24` + `app_offscreen_audit`。C 核心一行不动——所以"无头出图 + GUI"共用同一渲染。
3. **分阶段计时**：`TICK(ms_bg)` 等是定位帧时间瓶颈的唯一手段——没有它就会去优化"投影贵"
   这种猜错的方向（真实瓶颈常是线要素 bbox 缓存缺失）。

**生产最终形态**：`src/ui/window.c` 的 `paint_frame`（约 9 步）、`app_render_to_buf`；
`src/ui/offscreen_shot.c`（离屏单一可信源）；`src/ui/chrome.c`（自绘 chrome）。

**练习**：在 `paint_frame` 里新加一个 `stage_my`（如画一圈经纬网），用 TICK 看它耗时落在哪。
