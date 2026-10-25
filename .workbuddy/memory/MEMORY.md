# navimap 项目长期约定

> 只留结论与"别再试"；推导看 PLAN.md（渲染 §19-32 / 算路 §33-50 / 地标 §48-52 / 图集 §53+）、
> PROGRESS.md、`.workbuddy/memory/2026-*.md`。

## 固定动作

- `sources/` 只放原始 `*.osm.pbf`；`products/` 全可删、`./scripts/sync_maps.sh` 重建。
- 验收 `./scripts/regress.sh`（66 项；本机 = 42 通过 / 24 失败，全为环境缺失：7 省 PBF /
  `places_auto.db` / `lm.bin` / objdump 路径）；编译零警告（-Werror）。改渲染补
  `python3 scripts/bmpdiff.py 旧 新`（要素色→背景色=丢要素）。
- `navmap_qt --ui-test`（33 项）已进 regress.sh。完成即 commit + push origin main；
  三前端（navmap / navmap_qt / map_shot）链接一致。Windows 构建用 `./scripts/build_win.sh`。

## 构建环境（Windows/MSYS2）

- ★★★ **编译期 PATH 绝不能放 Qt bin**（`/d/Qt/6.11.2/mingw_64/bin`）：cc1plus 会加载
  Qt 自带旧 libstdc++ ⇒ 退出码 1、stderr 全空。特征：**C 能编过、C++ 全灭**。
  Qt bin 只在运行期加（regress.sh `qt_run`）。另两因：ninja 走 cmd.exe 拿到转换后 PATH；
  Defender 扫杀 cc1plus。`CMAKE_C_COMPILER_LAUNCHER` 是初始化变量、对已有 build/ 无效
  ⇒ 用 target 属性。`.bat` 必须纯 ASCII。
- 平台差异只许在 `src/utils/plat.{h,c}`，业务代码零 `#ifdef _WIN32`；负向断言写
  "失败 Outcome 为真"，别把成功断言取反（踩过 8 项假 FAIL）。

## Qt 前端（2026-10-25 实证）

- ★★ **checkable 按钮 `setChecked()` 会发 `toggled`**：toggled 又连回本函数 ⇒ 无限递归，
  每层建/删控件 ⇒ 栈耗尽 ⇒ **在 `RtlAllocateHeap` 里 SIGSEGV**（与 Defender 杀进程、
  堆损坏表现一模一样，极易误判）。同步勾选态必须 `QSignalBlocker`。
- ★ `isVisible()` 对"父窗口还没 show() 的子控件"恒 false ⇒ 浮层贴位用 `!isHidden()`
  （`--shot-*` 正是这个时序）。顶栏控件切换会改顶栏高度 ⇒ 画布整体移动 ⇒ 浮层跟着挪
  ⇒ **不要顶栏**（已取消 QToolBar），按钮一律画布子浮层。
- 图标符号（＋◎☰🔍）不能用字体字符（`inFont()` 全 false）⇒ `qt_icons.cpp` 自绘；
  `drawPixmap` 在 DPR≠1 下画到矩形一角 ⇒ 直接 paintEvent 画路径。控件中文要把
  `nm_font_candidates()` 字体 `addApplicationFont` 注册进 Qt；判"能否画中文"别用 Qt
  字体度量（offscreen 下所有族宽度一样）⇒ 查字体文件 cmap。

## SDL / 渲染边界

- SDL 只许出现在 `navimap_sdl`(window_sdl.c) 与 `map_shot`；`navimap_core` 与 `navmap_qt`
  **零 SDL**。渲染入口 `app_render_to_buf`，像素格式从 surface 读。
- 自写光栅必须与 SDL 软渲染逐像素等价（实证 3 个真 bug：alpha 两次除法 /
  `raster_rect` 角点只画一次 / 裁剪逐字复刻 `SDL_rect_impl.h`）。门禁 `blend_equiv` / `clip_equiv`。
- `--shot` 截图路径必须显式刷新状态栏（主循环不启动 ⇒ 状态栏空白），否则**截图在撒谎**。
- 销毁 renderer 前必须 `font_drop_textures()`；stb_truetype 后端只清 CPU 字形缓存，
  **不能每帧调**（毁 CJK 缓存）。

## 判据纪律

- 布尔判据用展开数/里程/像素，时间只兜底取多轮最小；渲染取 map_shot 第二帧、算路丢第 1 次。
- ★★★ 手挑样本下结论必翻车（已踩 4 次）：样本须覆盖形状且成对配对；负向对照必须能失败。
- ★ 判"成功"要看**退出码**不是输出行数：`dmap_selftest` 打完 `DMAP-OK` 就 return，
  栈上 `MapSet` 被后台 worker 回调命中 ⇒ rc=139 却显示"通过"。收尾必须 `mapset_free(&ms)`。
- 等价性判据套路："参数从模块读 + 另一条路径独立复算"；跟外部公布值比用外部口径
  （ENU 111320 vs 模块真实 M/N，中纬度差 0.4%）。

## 设置项与 C/C++

- ★★★ `app_init` **逐字段读设置**（不走 `app_apply_settings`）⇒ 新设置项三处都改：
  `SETTINGS_SCHEMA` + `app_init` + `app_persist_settings`。漏 `app_init`：启动不生效、
  退出还写回默认值。C++ 也 include 的 C 头必须 `extern "C"`；注释里别写 `src/x/*.cpp`
  （`/*` 触发 -Wcomment，-Werror 下 FAIL）。

## 坐标系（`src/utils/datum.c`）

- CGCS2000 平移 0；北京1954 = EPSG:4214 默认 TOWGS84（塔里木油气参数，北京实测偏 60~95 m）；
  西安1980 = 公开文献全国 3 参数（−104.57,−45.94,+5.30，8.2/4.2 m）。文案与参数表同文件维护。
- ★★★ "EPSG 没登记" ≠ "没有参数"（XA80 平移填 0 只换椭球 = 没转换，水平差 0.019 m）；
  两条独立证据互印才落值；分区参数已否决（选错代价 15.8~27 m > 全国最大 14.7 m）。
- 三扩展入口已做实：重合点解 7 参数（**必须列均衡**：旋转 µrad/尺度 ppm，否则条件数
  ~1e13、平移解被污染到毫米以上）；图幅改正量双线性 clamp 不外推；局部偏移是**替换**不是叠加。

## 别再试

- 渲染：简化几何（三种已否）/去描边/面 tile 索引/顶点内联/软件预取/缓存上一条目/合并折线/
  NMBIN v3。贵的是**间接寻址**不是 stride；SDL 每次绘制 ~100 ns ⇒ 只有"不画"才省。
- 算路：rev.bin v2 / PQ 叉数≠4（D=4 13.2 ms，PQEnt 16 B）/ 状态表改布局（三连败）。
  `RouteOpts` 一律指名初始化（写死 `.heur_w=DEFAULT` 会顶掉兜底，三处踩过）。
- 地标：>30 无收益、加大 penalty 无效、加 w 救不了尾部；w 随地标紧度下走
  （10→1.3、30→**1.25**），改地标数必须连带重定 w。
- 一次 `nmbin_open` 全部 disp tile 会耗尽 Windows 映射句柄（~168 个后全失败）⇒ 逐 tile 开→拼→关。
- 新增 `Buf` 必须同步设 `.esz`（memset 清零 ⇒ 重 PBF 阶段无声崩溃）。sqlite rowid 会复用：
  删 track 必须同时删 tpoint 子表。**`sqlite3_open` 不建父目录** ⇒ 配置目录不存在时
  打不开只返回 NULL、调用方照常报成功 ⇒ 用户数据静默不落盘；开库前先 `nm_mkdir_p`。
  本机 `rm` 是 safe-delete 改名 ⇒ 真删 `/bin/rm`。

## 已达成水平

- 渲染 1280×800：z9 116 / z11 56 / z13 42 / z15 31 ms。算路 50 条（30 地标 w=1.25）：
  中位 123 / p90 753 / 最慢 1503 ms，展开 10.43 M；比无表快 1.6×、p90 快 4.2×、质量 −3.21%。
- 图 5228 万节点、六成出度 0 ⇒ 按"有出边"2250 万算基数；全图 CH 放不下。
  热态固定成本 39~65 ms ⇒ 别抠启动。
