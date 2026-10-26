# 功能已实现、但 UI 按钮缺失的审计（C 自绘前端 / SDL / map_shot）

> 审计时间：2026-10-26。范围：`src/render/chrome.c`（layout/draw/hit/act）、
> `src/ui/window.c`、`src/ui/window_sdl.c`（键位）、`src/ui_qt/`（对照）、
> `src/settings/settings.c`（SETTINGS_SCHEMA）、`src/ui/overlay.h`。
> 判定标准：功能逻辑存在 + chrome 的 draw/hit/act 三件套是否接上按钮/快捷键。

## 类别 1 — 功能完整，但 C 前端完全无按钮、无快捷键（仅 CLI 可触发）

### GPX 导入 / 导出
- 实现：`app_gpx_import` / `app_gpx_export`（`src/ui/window.c:1304–1357`），解析与 sqlite 持久化齐全。
- 按钮：**无**。C 前端 chrome 无 draw/hit/act 项；`window_sdl.c` 键位表中也没有 gpx 键。
- 唯一入口：命令行 `--gpx=<file>` / `--export-gpx=<file>`（`src/main.c:41-43, 79-130`）。
- Qt 前端反而有完整按钮（`src/ui_qt/main_window.cpp` import/export 工具条）。
- 结论：C 前端用户无法在界面里导入/导出 GPX，只能记命令行参数。

## 类别 2 — 功能在、地图上画出来了，但 C 前端没有「管理面板」（能加不能管）

### 轨迹 / 收藏
- 渲染：`paint_tracks` / `paint_favorites` 已在地图上绘制（`src/ui/window.c:1119-1120`）。
- 面板：`paint_chrome`（`chrome.c:447-458`）**不调用**任何 track/favorites 面板；
  `ui/overlay.h` 把 `OVL_TRACK` 标为 `fe = OVL_FE_QT`（仅 Qt 画）。
- 入口残缺：收藏可用 `Ctrl+d` 加（`window_sdl.c:152`），轨迹可 `--gpx` 导入；
  但 C 前端**没有查看列表、删除、显隐切换**的面板或按钮，右栏也无显隐开关。
- Qt 前端有完整 `trackPanel`（`main_window.cpp:1156` 等）。
- 结论：轨迹/收藏在 C 前端是"只能加、不能管"。

## 类别 3 — 功能有入口，但只是「设置面板一行 / 快捷键」，无独立按钮（弱入口）

- **语音播报 `voice_on`**：V 键（`window_sdl.c:200`）+ 设置面板行（`SETTINGS_SCHEMA[15]`）。右栏无喇叭按钮。
- **POI 图标 `show_poi` / 地名标注 `show_labels`**：仅设置面板行（`SETTINGS_SCHEMA[13][14]`），无快捷开关。
- **坐标系 `datum`**（WGS84/CGCS2000/北京54/西安80）：仅设置面板行（`SETTINGS_SCHEMA[19]`），无快捷切换。

## 类别 4 — 按钮在，但点了没效果（flag 写入后无人消费）

### 顶栏「浏览 / 导航」页签
- 命中：`CH_MODE_BROWSE` / `CH_MODE_NAV`（`chrome.c:482-483` → `chrome_act` 写 `a->mode`）。
- 全局搜索 `a->mode` 的读取点，确认它**仅**：① 从设置读（`window.c:66,272`）、② 写回设置（`window.c:289`）、
  ③ 被这两个按钮赋值（`chrome.c:526,529`）。**渲染与逻辑层没有任何地方读 `a->mode` 产生行为差异。**
- 结论：页签高亮会变，但浏览模式与导航模式在 C 前端无任何功能区别 —— 是"按钮在但无效"的真死开关。

## 类别 5 — 已确认按钮齐全（排除项，避免误报）

三件套完整（draw + hit + act）：
- 右栏 ＋/−/定位/昼夜；顶栏 ☰/搜索/浏览·导航（按钮本身在，仅 mode 无效，见类别 4）；
  标题栏 关闭/最小化/最大化（`window_sdl.c:76-77` 接管 min/max，非死按钮）；
- 主菜单 7 项、设置面板（含浮层透明度子面板）、多方案对比面板（命中切方案）、
  步骤列表、导航栏、比例尺、指北针、无图卡片；
- 夜间蒙版（昼夜按钮）、跟随车头（菜单项4 + t 键）、搜索（☰/搜索框/s 键）、
  多方案（1-4 键）、LOD（0-4 键）均齐全。

## 修复优先级建议（如需补全 C 前端）
1. 「导航」页签的 mode 要么接上行为、要么从 UI 移除（类别 4，最该修，纯死开关）。
2. GPX 导入/导出按钮 + 轨迹/收藏管理面板（类别 1、2，功能已就绪，只差 chrome 接线）。
3. 语音/POI/标注/坐标系的快捷开关（类别 3，可选，设置面板已能改）。

---

## 修复记录（2026-10-26 续：已全部落地）

### 类别 4 — 「浏览/导航」页签（已修，接通真实行为）
- `a->mode` 之前只被写入、从不读取；`draw_topbar` 页签高亮还看的是 `have_rr`，两处脱节。
- 语义接通：`have_rr` 置 1 时同时 `a->mode = 1`（导航）、`nav_drop_route` 置 0 时 `a->mode = 0`（浏览）。
  落点：`window.c` 的 `app_nav_poll` / `app_nav_select_alt` / `app_nav_plan_sync` / `nav_drop_route`。
- 渲染门禁：导航 chrome（navtop / steps / alts / navbottom）改为 `if (!have_rr || mode==0) return;`，
  即浏览模式下隐藏导航层、保留地图与路线；页签高亮改为看 `a->mode`（与 `have_rr` 组合）。
- 文件：`src/ui/window.c`、`src/render/chrome.c`（draw_topbar / draw_navtop / draw_steps / draw_alts / draw_navbottom）。

### 类别 1 — GPX 导入 / 导出（已修，菜单入口）
- 主菜单新增「导入 GPX」「导出 GPX」两项（菜单 10 项，索引 6/7）。
- C 端无文件对话框 ⇒ 走"配置目录"约定：导入扫描 `<config>/gpx/*.gpx`、导出写 `<config>/export.gpx`；
  进度/结果打 stderr（C 端从终端启动可见）。新增 `app_gpx_import_dir` / `app_gpx_export_default`
  （`window.c`，用 `opendir`/`nm_path_join`/`nm_mkdir_p`/`nm_config_dir`）；菜单动作在 `chrome_act` 派发。
- 文件：`src/ui/window.c`、`src/render/chrome.c`（MENU_ITEMS + CH_MENU_ITEM switch）。

### 类别 2 — 轨迹 / 收藏管理面板（已修，C 端也画）
- 把 `OVL_TRACK` 的 `ovl_frontends` 从 `OVL_FE_QT` 改为 `OVL_FE_C | OVL_FE_QT`（`src/ui/overlay.c`），
  让 C 端真正拥有该面板、且浮层透明度子面板能给其调 alpha。
- 新增 `draw_tf_panel` / `tf_layout` / `tf_hit` / `tf_act`（`chrome.c`）：面板分「收藏」「轨迹」两节，
  每行一个 × 删除单条、每节一个「清空」删全部；布局由 `tf_layout` 统一算（绘制/命中共用，不会错位）。
- App 新增 `tf_open / tf_row / tf_btn` 三字段（`window.h`）；`paint_chrome` 加绘制、`chrome_hit`
  优先命中、`chrome_act` 的 `CH_TF` 分支消费。主菜单「轨迹 / 收藏」项（索引 8）打开面板。
- 文件：`src/render/chrome.c`、`src/render/chrome.h`（CH_TF）、`src/ui/window.h`、`src/ui/overlay.c`。

### 类别 3 — 语音/POI/标注/坐标系（保持原样）
- 经评估：设置面板已有行 + 语音有 V 键，属于"弱入口"而非"完全缺失"，本次不动。
  若后续要更强入口，可仿照右栏按钮加快捷开关（注意 overlay 透明度语义："只淡底板、文字清晰"）。

### 验收（改完须过）
- `./scripts/regress.sh --build`：零警告门禁 + 78 项回归。
- 重点回归：`gpx_render`（map_shot --gpx 的像素判据，与本面板无关、不应受影响）、
  `qt_ovl_text`（浮层透明度语义）、其余导航/渲染项。

