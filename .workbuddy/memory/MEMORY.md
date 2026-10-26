# navimap 项目长期约定

> 只留结论与"别再试"；推导看 PLAN.md（渲染 §19-32 / 算路 §33-50 / 地标 §48-52 / 图集 §53+）、
> PROGRESS.md、`.workbuddy/memory/2026-*.md`。

## 固定动作

- `sources/` 只放原始 `*.osm.pbf`；`products/` 全可删、`./scripts/sync_maps.sh` 重建。
- 验收 `./scripts/regress.sh`：跨平台门禁，本机(Windows)与 Linux 主机**环境互补**——
  - Linux 主机（数据齐全）：**75 通过 / 3 失败**，3 项是环境缺失（lanes.bin / disp.idx / objdump 路径）。
  - 本机(Windows/MSYS2)：仅 beijing 1 省 graph（sources/ 只有 beijing.osm.pbf，无 7 省源）⇒
    补**运行期** PATH（`/d/Qt/6.11.2/mingw_64/bin` + `/d/msys64/ucrt64/bin` + `QT_QPA_PLATFORM_PLUGIN_PATH`
    指 Qt plugins）后 **55 通过 / 24 失败**，24 全为数据缺口（多省 graph / places_auto.db /
    liaoning/rev.bin / lm.bin）+ 1 项 disp_runtime 边角环境，**零代码回归**。
  - 全量对齐 75/3 需在那台有完整数据的 Linux 机器跑。编译零警告（-Werror）。改渲染补
  `python3 scripts/bmpdiff.py 旧 新`（要素色→背景色=丢要素）。
- `navmap_qt --ui-test`（33 项）已进 regress.sh。完成即 commit + push origin main；
  **前端只有 navmap_qt**（原 navimap/map_shot 已删，见「SDL / 渲染边界」）。
  Windows 构建用 `./scripts/build_win.sh`。
- ★★ **删掉"实现所在的文件"时，必须 grep 该文件提供的符号在别处的声明**
  （本项目实例：删 `window_sdl.c` ⇒ `window.h` 的 `app_run` 成了悬空声明；
  无人调用 ⇒ 链接不报错，但它是"声明了却永远不存在"的假 API）。

## 构建环境（Windows/MSYS2）

- ★★★ **编译期 PATH 绝不能放 Qt bin**（`/d/Qt/6.11.2/mingw_64/bin`）：cc1plus 会加载
  Qt 自带旧 libstdc++ ⇒ 退出码 1、stderr 全空。特征：**C 能编过、C++ 全灭**。
  Qt bin 只在运行期加（regress.sh `qt_run`）。另两因：ninja 走 cmd.exe 拿到转换后 PATH；
  Defender 扫杀 cc1plus。`CMAKE_C_COMPILER_LAUNCHER` 是初始化变量、对已有 build/ 无效
  ⇒ 用 target 属性。`.bat` 必须纯 ASCII。
- 平台差异只许在 `src/utils/plat.{h,c}`，业务代码零 `#ifdef _WIN32`；负向断言写
  "失败 Outcome 为真"，别把成功断言取反（踩过 8 项假 FAIL）。

## 本机(Windows/MSYS2)运行 regress / navmap_qt 的环境

- ★ 跑 `regress.sh` 或 `navmap_qt --ui-test` / `--shot-offscreen` **前必须 export**（否则 Qt6*.dll /
  libprotobuf-c-1.dll 找不到 ⇒ rc=127 假失败）：
  export PATH="/d/Qt/6.11.2/mingw_64/bin:/d/msys64/ucrt64/bin:$PATH"
  export QT_QPA_PLATFORM=offscreen
  export QT_QPA_PLATFORM_PLUGIN_PATH="/d/Qt/6.11.2/mingw_64/plugins"
  （libprotobuf-c-1.dll 在 **ucrt64**/bin，不是 mingw64；regress.sh 的 qt_run 只加 Qt bin，
   本机需自己补 ucrt64 bin + QT 插件路径。MSYS2 下 POSIX 风格 PATH 对 Windows loader 未必生效，
   用绝对路径更稳。）
- ★ 数据缺口是**真·环境限制**：本机 products 仅 beijing 1 省（sources/ 仅 beijing.osm.pbf），
  多省 graph / places_auto.db / rev.bin / lm.bin 无法生成 ⇒ 相关 regress 用例必 FAIL，非代码。
  全量 75/3 对齐在那台有完整数据的 Linux 机器跑。

## Qt 前端（2026-10-25 实证）

- ★★ **checkable 按钮 `setChecked()` 会发 `toggled`**：toggled 又连回本函数 ⇒ 无限递归，
  每层建/删控件 ⇒ 栈耗尽 ⇒ **在 `RtlAllocateHeap` 里 SIGSEGV**（与 Defender 杀进程、
  堆损坏表现一模一样，极易误判）。同步勾选态必须 `QSignalBlocker`。
- ★ `isVisible()` 对"父窗口还没 show() 的子控件"恒 false ⇒ 浮层贴位用 `!isHidden()`
  （`--shot-*` 正是这个时序）。顶栏控件切换会改顶栏高度 ⇒ 画布整体移动 ⇒ 浮层跟着挪
  ⇒ **不要顶栏**（已取消 QToolBar），按钮一律画布子浮层。
- ★★ **`QWidget::setWindowOpacity()` 只对顶层窗口有效**：对画布子控件浮层调它**完全没
  效果也不报错**。子控件要淡只能用 `QGraphicsOpacityEffect` —— ★ 但**它会把文字和
  图标一起淡掉**，与"只淡底板、文字保持清晰"冲突 ⇒ 实测**弃用**，改用：
  ① `IconButton` 自绘底板时按系数算 alpha（图标/文字的 pen 不乘系数）；
  ② 面板/输入框用 QSS `rgba(r,g,b,alpha)` 背景；③ `QScrollArea` 必须
  `viewport()->setAttribute(Qt::WA_TranslucentBackground, true)`，否则中间压一块实心。
- ★★ **浮层透明度的语义 = 只淡底板，文字/图标保持清晰**（J 先生 2026-10-25 定）：
  底板/描边按 pct 线性缩放 alpha；前景（文字/图标）**不参与线性淡出** ——
  pct=0 ⇒ 整个浮层隐藏（否则文字孤零零浮在底图上）；pct>0 ⇒ 前景保持完全不透明。
  ★ 初版做成"整块一起淡"被 J 先生**直接否决**：浅色底板上的文字淡下去就糊了。
  C 侧实现：`ak()` 给底板、`afg()` 给前景（`chrome.c` / `navdraw.c` 各自的 `am()`/`fg()`）。
- ★ `qobject_cast` 要求目标类型有 `Q_OBJECT` 宏 ⇒ 自绘 QWidget 子类（如 `IconButton`，
  不参与 moc）**必须用 `dynamic_cast`**，否则 `static_assert` 编译失败。
- ★ **改 `.hpp` 后 ninja 不自动重编** ⇒ 注入的负向对照 bug 没进 exe ⇒ **负向对照假通过**。
  必须 `touch` 对应的 `.cpp` 再重编。
- 图标符号（＋◎☰🔍）不能用字体字符（`inFont()` 全 false）⇒ `qt_icons.cpp` 自绘；
  `drawPixmap` 在 DPR≠1 下画到矩形一角 ⇒ 直接 paintEvent 画路径。控件中文要把
  `nm_font_candidates()` 字体 `addApplicationFont` 注册进 Qt；判"能否画中文"别用 Qt
  字体度量（offscreen 下所有族宽度一样）⇒ 查字体文件 cmap。

## SDL / 渲染边界

- ★ **SDL GUI 壳已彻底移除**（2026-10-26 #88）：`navmap_sdl`/`window_sdl.c`/`navimap`/
  `map_shot` 全部删除。交互前端唯一是 `navmap_qt`；离屏出图唯一是
  `src/ui/offscreen_shot.c`（`offscreen_shot_main`，**零 SDL**：malloc 缓冲 + 内建
  24 位 BMP writer）。SDL 现只出现在 `blend_equiv`/`clip_equiv`（去 SDL 化门禁对照体）。
- ★★ **审计计数源走 C 核心地图缓冲**：`offscreen_shot_main` 设 `a.qt_ui=0` ⇒ C 核心把
  chrome/横幅/搜索框全画进缓冲 ⇒ 与旧 map_shot 逐字节一致。**⇒ 不可删
  `chrome.c`/`paint.c`/`navdraw.c`**（window.c:1258 调 `paint_chrome`），删了既编译不过
  也破 74/4 像素基线。共享审计模块 = `src/render/audit.{c,h}`。
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
- ★★ **浮层半透明后，`count_color_pixels`（精确配色计数）判据全失效**：原来数 255,186,64
  改 75% 后数到 0 ⇒ 9 个用例一起假红。改数**混合后**的颜色：有效 alpha = 元素自身
  alpha/255 × 浮层 alpha/255，对背景 (244,241,234) 混合 + 容差 8（见
  `tools/map_shot.c` 的 `count_overlay_pixels`）。
  ★ 数哪个元素要看它的**alpha 基准**：无图横幅的边框画在横幅**填充**之上、不是压在
  底图上 ⇒ 按底图混合的期望色根本不存在，必须数填充。
- ★ 判半透明比率：合成是线性的 ⇒ `|M−H| / |O−H| == pct/100`（M 该 pct / O 100% / H 0%
  三张图）。**这个比值与元素底色的固有 alpha 无关**（右栏底色 245 不是 255）。
  脚本 `scripts/ovl_px.py`。★ 读 BMP 按绝对坐标取像素：`h>0` 是自下而上存储，必须
  `rows.reverse()`，否则取样全是背景色。
  ★★ **判"底板按 pct 淡"要取矩形内所有变化像素的中位数**，不能取单点：底板像素占多数，
  单点容易采到文字/图标上。
  ★★★ **判"前景没被淡"不能用"比值 ≈ 1"**：浮层未覆盖的边角像素（多出的地图行）恰好
  凑出 ≈1 的比值（实测标题栏矩形里凑出 **424 个假前景**）⇒ 必须用**逐像素完全相同**
  （`M == O 且 O != H`）。
  ★★ **中文 14px 抗锯齿没有 coverage=1 的纯色核** ⇒ "逐像素相同"对文字数不到（Qt 搜索
  按钮「搜索」只数到 3 个）⇒ 文字改用**口径 b**：相对底图的偏离量 `|e75|/|e100| > (1+pct/100)/2`。
  ★ 口径 b **必须框准前景区域才有判别力**（`--fgrect`）：不框时底板边缘给 129 个纯噪声
  （注入前后 129 vs 130，零判别力）；框准文字区后 111 vs 39。口径 b 只在指定 `--fgrect`
  时启用，否则在 C 右栏是纯噪声。

## 语音播报（2026-10-25）

- 分层：`src/route/voice.c` 只决定"该说什么"（纯逻辑、零平台依赖），
  发声在 `utils/plat.c` 的 `nm_tts_speak()`；**接线只有一份** ——
  `src/ui/voice_wire.c` 的 `nav_voice_frame()`，生产（`window.c`）与
  `tools/voice_e2e.c` **共用**。★ 接线若长在 `App` 里，自测够不着只能复刻
  ⇒ 判据必假绿（实测踩过：把发声循环改成 `i<0`，两个自测照样全绿）。
- ★ 官方 `say()` 是**所有**播报的唯一出口 ⇒ 15 秒同文本冷却对
  限速 / 到达 / 告警 / 转向一视同仁 ⇒ 四类都要过同一个 `say_dup()`。
- ★ 官方 `VoiceRouter.update()` 顺序 = speedLimit → arrival → alarm → turn（转向放最后）。
- ★★ **顺序耦合**：到达那一刻既要置 `sim_paused=1`（模拟停下）又要播
  "您已到达目的地附近，导航结束" ⇒ ① **先播报后置位**；② 收尾语**单独判**、
  不受 paused 挡（否则永远播不出来 —— 实测真 bug，GUI 里从来没响过这句）。
- ★ 判"窗口真的播了什么"：设 `NAVIMAP_TTS_DRYRUN=<文件>`，`nm_tts_speak`
  会**同步**把文本写进该文件 ⇒ 直接断言真实发声，不必复刻接线。
- ★ 负向对照的注入点必须**可观测**：锚到 `arrived_said` 先置位的地方 =
  永远命中不了 = 假绿。注入写法别让变量变成"未使用"（-Werror ⇒ 编译失败 ⇒
  跑的仍是旧 exe ⇒ 同样假绿）。用 `if (n < 0)` 而不是 `i < 0`。
- ✅ 缺口已补（`0179e51`）：把"到达帧例外"收归 `voice_wire.c` 成为唯一裁决点，
  `window.c` 置位顺序不再是 load-bearing；`tools/voice_e2e.c` A8 + `scripts/voice_neg.sh` #9/#10
  负向对照盖住两道闸（常规播报 + 收尾语都不受 paused 影响）。

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
- 浮层透明度：**`QGraphicsOpacityEffect` 已否**（会把文字/图标一起淡掉，与"只淡底板、
  文字清晰"冲突）；**"整块元素一起淡"已否**（J 先生否决：浅底板上文字淡下去就糊）。
  Qt 唯一可行解 = IconButton 自绘底板按系数算 alpha + QSS `rgba()` + viewport 透明。
- 新增 `Buf` 必须同步设 `.esz`（memset 清零 ⇒ 重 PBF 阶段无声崩溃）。sqlite rowid 会复用：
  删 track 必须同时删 tpoint 子表。**`sqlite3_open` 不建父目录** ⇒ 配置目录不存在时
  打不开只返回 NULL、调用方照常报成功 ⇒ 用户数据静默不落盘；开库前先 `nm_mkdir_p`。
  本机 `rm` 是 safe-delete 改名 ⇒ 真删 `/bin/rm`。

## 已达成水平

- 渲染 1280×800：z9 116 / z11 56 / z13 42 / z15 31 ms。算路 50 条（30 地标 w=1.25）：
  中位 123 / p90 753 / 最慢 1503 ms，展开 10.43 M；比无表快 1.6×、p90 快 4.2×、质量 −3.21%。
- 图 5228 万节点、六成出度 0 ⇒ 按"有出边"2250 万算基数；全图 CH 放不下。
  热态固定成本 39~65 ms ⇒ 别抠启动。
