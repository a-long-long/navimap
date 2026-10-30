# 第 12 课 · Qt 前端与边界原则

> 📖 配套读源码讲义：[`docs/teaching/12_qt.md`](../../docs/teaching/12_qt.md)（读源码式讲解，讲清楚“为什么这么写”；与本科小程序同号对应）


**累加内容**：遗留 C 系统如何渐进式 Qt 化——C 渲染核心一行不动，只把像素缓冲包成 `QImage`。
本课用 Qt 的 `QImage` 直接"接住"一块 `0xFFRRGGBB` 的 C 缓冲（与 `offscreen_shot.c` 同约定），
并存成 PNG 验证"Qt 只是缓冲的搬运工"。

**运行（无头验证版 main.cpp）**：
```bash
g++ -std=c++17 \
  -I/d/Qt/6.11.2/mingw_64/include -I/d/Qt/6.11.2/mingw_64/include/QtGui -I/d/Qt/6.11.2/mingw_64/include/QtCore \
  -I/d/Qt/6.11.2/mingw_64/mkspecs/win32-g++ \
  main.cpp -o tut12.exe \
  -L/d/Qt/6.11.2/mingw_64/lib -lQt6Gui -lQt6Core -luser32 -lkernel32 -lshell32 -lgdi32
./tut12.exe     # 生成 out.png
```
真实 GUI 版见 `main_windows.cpp`（`QLabel::setPixmap(QPixmap::fromImage(img))` 放进 `QMainWindow`），
用 `CMakeLists.txt` 里的 `tut12_win` 目标构建。

**你会看到**：`out.png` 是一张红绿渐变 + 红对角线——和前几课 C 缓冲画的东西一致，说明 Qt 没有重绘，
只是把同一块缓冲显示出来。

**关键原则（项目铁律）**：
1. **边界原则**：C 渲染核心一行不动，只换"缓冲归属 + 叠加层由 QPainter 画"。`qt_ui=1` 时 `paint_frame`
   跳过 C 自绘 chrome/搜索框/面板（由 Qt 真控件承担），但地图渲染保留。
2. **像素格式**：缓冲必须是 `0xFFRRGGBB` 的 `Format_RGB32`——`RGBA8888`→黑屏、`RGBX8888`→红蓝互换，
   用 RGB32 最稳。
3. **浮层透明度语义**：只淡底板、文字/图标保持清晰（`IconButton` 自绘底板按 pct 算 alpha；
   `QGraphicsOpacityEffect` 已否——它会把文字一起淡掉）。

**生产最终形态**：`src/ui_qt/map_canvas.cpp`（`m_px` 自持 `uint32_t` 缓冲，`app_render_to_buf` 画进它再包成
`QImage::Format_RGB32`）、`src/ui_qt/qt_main.cpp`（唯一 GUI 进程入口）。

**练习**：把 `main_windows.cpp` 里的 `px` 换成第 8 课 `paint_frame` 的输出，让真实窗口显示一张完整地图帧。
