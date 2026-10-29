# navimap 项目长期约定

> 只留结论与"别再试"；推导看 PLAN.md（渲染 §19-32 / 算路 §33-50 / 地标 §48-52 / 图集 §53+）、
> PROGRESS.md、`.workbuddy/memory/2026-*.md`。

## 固定动作

- `sources/` 只放原始 `*.osm.pbf`；`products/` 全可删、`./scripts/sync_maps.sh` 重建。
- 验收 `./scripts/regress.sh`：Linux 主机（数据齐全）**77 通过 / 3 失败**（3 项环境缺失：
  lanes.bin / disp.idx / objdump 路径）。Windows/MSYS2 副机 products 仅 beijing 1 省 ⇒
  多省 graph / places_auto.db / liaoning/rev.bin / lm.bin 缺失，相关用例必 FAIL、**非代码回归**；
  全量 77/3 只能在数据齐全的 Linux 跑。编译零警告（-Werror）。改渲染补
  `python3 scripts/bmpdiff.py 旧 新`（要素色→背景色=丢要素）。
- `navmap_qt --ui-test`（33 项）已进 regress.sh。完成即 commit + push origin main。
  **前端只有 navmap_qt**（原 navimap/map_shot 已删）。
- ★★ **删文件前必须 grep 它提供的符号在别处的声明**（删 `window_sdl.c` ⇒ `window.h` 的
  `app_run` 成悬空声明，链接不报错但是假 API）。★ 同理**删 include 前必须 grep 该头的全部符号**
  （只 grep `label_` 漏掉 `NamePool`，差点把承重依赖当死代码删掉 ⇒ 编译才证伪）。

## 构建环境

- ★ **本机 Linux（UOS aarch64 / Wayland）**：`scripts/build_release.sh` 是 Windows/MSYS2 专用，本机跑不了。
  本机 Release 走 CMake 原生 Qt5（**本机只有 Qt5 dev 在 `~/.local/qt5-dev/usr`，无 Qt6 cmake 模块**）：
  `source env.sh && cmake -S . -B build_rel -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/home/along/.local/qt5-dev/usr -DQt5_DIR=/home/along/.local/qt5-dev/usr/lib/aarch64-linux-gnu/cmake/Qt5 && cmake --build build_rel -j$(nproc)`
- 本机无头自检：`export QT_QPA_PLATFORM=offscreen QT_QPA_PLATFORM_PLUGIN_PATH=/usr/lib/aarch64-linux-gnu/qt5/plugins`
  后跑 `--shot-offscreen` / `--ui-test`。GUI：桌面终端直接 `./build_rel/navmap_qt --graph=products/graph/beijing`（已装 qtwayland5）。
  ★ 交互 GUI **不认** `--lat/--lon`（仅离屏认）；启动视图来自 `~/.config/navimap/settings.ini` 的
  `view_lat/view_lon`（默认北京 39.9/116.4）。本机数据齐全：`products/graph/` 有 **7 省**
  （beijing/hebei/henan/liaoning/shaanxi/shandong/shanxi）+ `lm.bin` + `places_auto.db`，`sources/` 有 8 省 pbf（含 tianjin）。
- ★★★ **Windows/MSYS2 编译期 PATH 绝不能放 Qt bin**（`/d/Qt/6.11.2/mingw_64/bin`）：cc1plus 加载旧
  libstdc++ ⇒ 退出码 1、stderr 全空。特征：**C 能编过、C++ 全灭**。Qt bin 只在运行期加。另两因：
  ninja 走 cmd.exe 拿到转换后 PATH；Defender 扫杀 cc1plus。`CMAKE_C_COMPILER_LAUNCHER` 对已有 build/ 无效
  ⇒ 用 target 属性。`.bat` 必须纯 ASCII。运行期须 export `PATH="/d/Qt/6.11.2/mingw_64/bin:/d/msys64/ucrt64/bin:$PATH"`
  + `QT_QPA_PLATFORM=offscreen` + `QT_QPA_PLATFORM_PLUGIN_PATH="/d/Qt/6.11.2/mingw_64/plugins"`
  （libprotobuf-c-1.dll 在 **ucrt64**/bin；用绝对路径更稳）。
- 平台差异只许在 `src/utils/plat.{h,c}`，业务代码零 `#ifdef _WIN32`；负向断言写"失败 Outcome 为真"，
  别把成功断言取反（踩过 8 项假 FAIL）。

## Qt 前端（2026-10-25 实证）

- ★★ **checkable 按钮 `setChecked()` 会发 `toggled`** ⇒ toggled 连回本函数即无限递归 → 栈耗尽 →
  **在 `RtlAllocateHeap` 里 SIGSEGV**（与 Defender 杀进程表现一模一样）。同步勾选态必须 `QSignalBlocker`。
- ★ `isVisible()` 对"父窗口还没 show() 的子控件"恒 false ⇒ 浮层贴位用 `!isHidden()`。顶栏控件切换会改
  顶栏高度 ⇒ 画布整体移动 ⇒ **不要顶栏**（已取消 QToolBar），按钮一律画布子浮层。
- ★★ **`QWidget::setWindowOpacity()` 只对顶层窗口有效**，对画布子控件浮层调它完全没效果也不报错。
  子控件要淡只能用 `QGraphicsOpacityEffect` —— ★ 但**它会把文字和图标一起淡掉** ⇒ 实测**弃用**。
  改用：① `IconButton` 自绘底板按系数算 alpha（图标/文字 pen 不乘系数）；② 面板/输入框用 QSS `rgba()`；
  ③ `QScrollArea` 必须 `viewport()->setAttribute(Qt::WA_TranslucentBackground, true)`。
- ★★ **浮层透明度语义 = 只淡底板，文字/图标保持清晰**（J 先生定）：底板/描边按 pct 线性缩放 alpha；
  前景不参与淡出 —— pct=0 ⇒ 整个浮层隐藏；pct>0 ⇒ 前景完全不透明。★ 初版"整块一起淡"被**直接否决**
  （浅底板上文字淡下去就糊）。C 侧：`ak()` 给底板、`afg()` 给前景。
- ★ `qobject_cast` 要求目标类型有 `Q_OBJECT` ⇒ 自绘子类（如 `IconButton`）**必须用 `dynamic_cast`**。
- ★ **改 `.hpp` 后 ninja 不自动重编** ⇒ 注入的负向对照 bug 没进 exe ⇒ **负向对照假通过**。必须 `touch` 对应 `.cpp`。
- ★ 新增 C++ 代码用 C 库数学函数（如 `floor`）**必须 `#include <cmath>` 并用 `std::floor`**（等高线功能漏头，
  Windows/Qt6 靠隐式头侥幸编过，Qt5/GCC8 编不过；已修 `e43278e`）。
- 图标符号（＋◎☰🔍）不能用字体字符（`inFont()` 全 false）⇒ `qt_icons.cpp` 自绘；`drawPixmap` 在 DPR≠1 下
  画到矩形一角 ⇒ 直接 paintEvent 画路径。控件中文要把 `nm_font_candidates()` 字体注册进 Qt；判"能否画中文"
  别用 Qt 字体度量（offscreen 下所有族宽度一样）⇒ 查字体文件 cmap。

## 渲染边界（SDL 已彻底移除，2026-10-26 #88）

- `navmap_sdl`/`window_sdl.c`/`navimap`/`map_shot` 全删。交互前端唯一是 `navmap_qt`；离屏出图唯一是
  `src/ui/offscreen_shot.c`（**零 SDL**：malloc 缓冲 + 内建 24 位 BMP writer）。SDL 现只出现在
  `blend_equiv`/`clip_equiv`（去 SDL 化门禁对照体）。
- ★★ **审计计数源走 C 核心地图缓冲**：`offscreen_shot_main` 设 `a.qt_ui=0` ⇒ C 核心把 chrome/横幅/搜索框
  全画进缓冲 ⇒ 与旧 map_shot 逐字节一致。**⇒ 不可删 `chrome.c`/`paint.c`/`navdraw.c`**。共享审计模块 =
  `src/render/audit.{c,h}`。
- 自写光栅必须与 SDL 软渲染逐像素等价（实证 3 个真 bug：alpha 两次除法 / `raster_rect` 角点只画一次 /
  裁剪逐字复刻 `SDL_rect_impl.h`）。门禁 `blend_equiv` / `clip_equiv`。
- `--shot` 截图路径必须显式刷新状态栏（主循环不启动 ⇒ 状态栏空白），否则**截图在撒谎**。
- 销毁 renderer 前必须 `font_drop_textures()`；stb_truetype 后端只清 CPU 字形缓存，**不能每帧调**（毁 CJK 缓存）。

## 判据纪律

- 布尔判据用展开数/里程/像素，时间只兜底取多轮最小；渲染取第二帧、算路丢第 1 次。
- ★★★ 手挑样本下结论必翻车（踩 4 次）：样本须覆盖形状且成对配对；**负向对照必须能失败**。
- ★ 判"成功"看**退出码**不是输出行数（`dmap_selftest` 打完 DMAP-OK 就 return，栈上 MapSet 被 worker 命中
  ⇒ rc=139 却显示"通过"）。收尾必须 `mapset_free(&ms)`。
- 等价性判据套路："参数从模块读 + 另一条路径独立复算"；跟外部公布值比用外部口径。
- ★★ **浮层半透明后 `count_color_pixels`（精确配色计数）判据全失效** ⇒ 改数**混合后**的颜色：有效 alpha =
  元素 alpha/255 × 浮层 alpha/255，对背景 (244,241,234) 混合 + 容差 8（`count_overlay_pixels`）。
  ★ 数哪个元素看它的 **alpha 基准**（无图横幅边框画在填充之上 ⇒ 必须数填充）。
- ★ 判半透明比率：合成线性 ⇒ `|M−H| / |O−H| == pct/100`（M 该 pct / O 100% / H 0%）。脚本 `scripts/ovl_px.py`。
  ★ 读 BMP 按绝对坐标取像素：`h>0` 自下而上，必须 `rows.reverse()`。★ 判"底板按 pct 淡"取矩形内所有
  变化像素的**中位数**。
- ★★★ **判"前景没被淡"不能用"比值 ≈ 1"**（浮层未覆盖的边角像素凑出 ≈1，实测 424 个假前景）⇒ 必须**逐像素
  完全相同**（`M == O 且 O != H`）。★★ 中文 14px 抗锯齿无 coverage=1 纯色核 ⇒ "逐像素相同"对文字数不到 ⇒
  文字用**口径 b**：相对底图偏离量 `|e75|/|e100| > (1+pct/100)/2`，且**必须框准前景区域**（`--fgrect`）才有判别力。
- ★★★ **凡是会写回用户配置的实验参数（`--ovl-alpha` 等），跑实验必须隔离配置**
  （`XDG_CONFIG_HOME=/tmp/xxx` + `APPDATA=/tmp/xxx/appdata`）。否则退出时 `app_persist_settings` 把实验值
  写进 `~/.config/navimap/settings.ini` ⇒ **污染用户真实配置** ⇒ 后续回归里"断言默认配置值"的用例全部假红。
  实测踩过：一次 `--ovl-alpha=alpha_search=0` 实验 ⇒ 真实配置被写成 0 ⇒ 之后每次 `qt_ui_test` 报 6 项不符
  （4 项是搜索浮层连锁失败），一度误判成"上游引入的回归"。
  ⇒ 回归里"断言默认配置值"的用例也必须用独立配置目录（`qt_ui_test` 已修）。隔离的判别力验法：
  故意把真实配置污染成坏值 ⇒ 该用例仍 PASS 才算隔离有效。
- ★★ **UI 改版会让像素判据的硬编码坐标失效** ⇒ 判据坐标是"每次 UI 改版都要重测"的活资产。
  改判据先**量新位置**（100% vs 0% 差分包围盒 + 深色像素聚类）再定新坐标，并**随坐标重跑负向对照**
  （注入"文字 pen 也乘系数"⇒ 前景判据必须 FAIL）。

## 语音播报（2026-10-25）

- 分层：`src/route/voice.c` 只决定"说什么"（纯逻辑），发声在 `utils/plat.c` 的 `nm_tts_speak()`；
  **接线只有一份** —— `src/ui/voice_wire.c` 的 `nav_voice_frame()`，生产与 `tools/voice_e2e.c` **共用**。
  ★ 接线若长在 `App` 里，自测够不着只能复刻 ⇒ 判据必假绿。
- ★ 官方 `say()` 是**所有**播报的唯一出口 ⇒ 15 秒同文本冷却对限速/到达/告警/转向一视同仁。
  `VoiceRouter.update()` 顺序 = speedLimit → arrival → alarm → turn。
- ★★ **顺序耦合**：到达那一刻既要置 `sim_paused=1` 又要播收尾语 ⇒ ① **先播报后置位**；② 收尾语**单独判**、
  不受 paused 挡（否则永远播不出来 —— 实测真 bug）。
- ★ 判"窗口真的播了什么"：设 `NAVIMAP_TTS_DRYRUN=<文件>`，`nm_tts_speak` 同步写文本 ⇒ 直接断言真实发声。
- ★ 负向对照注入点必须**可观测**（锚到 `arrived_said` 先置位处 = 永远命中不了 = 假绿）。注入写法别让变量变
  "未使用"（-Werror ⇒ 编不过 ⇒ 跑的仍是旧 exe ⇒ 同样假绿）。用 `if (n < 0)` 而不是 `i < 0`。
- ✅ `0179e51` 把"到达帧例外"收归 `voice_wire.c` 成为唯一裁决点；`voice_e2e.c` A8 + `scripts/voice_neg.sh` #9/#10 盖住两道闸。

## 设置项与 C/C++

- ★★★ `app_init` **逐字段读设置**（不走 `app_apply_settings`）⇒ 新设置项三处都改：`SETTINGS_SCHEMA` +
  `app_init` + `app_persist_settings`。漏 `app_init`：启动不生效、退出还写回默认值。C++ include 的 C 头必须
  `extern "C"`；注释里别写 `src/x/*.cpp`（`/*` 触发 -Wcomment，-Werror 下 FAIL）。

## 分层与模块边界（2026-10-28 R0–R3 立规，PLAN.md §53）

- **终态依赖图是纯 DAG、零环**（`scripts/check_layers.sh` 实测）：
  `utils ← pbf ← map ← route ← render ← ui ← ui_qt`（search/settings 横切）。
- ★★ **`-Wl,--start-group` 已从 CMake 移除，不要再加回来**。理由不是"更干净"，而是 start-group
  让链接器**多轮扫描**，会把新引入的反向依赖**悄悄补上** ⇒ 边界形同虚设。去掉后新反向依赖
  **立刻 undefined-reference** ⇒ 链接器是第二道防线（第一道是 check_layers.sh）。
- ★★ 改依赖方向前**必须查符号归属，别只 grep 函数前缀**：`mapset.h` 那行 `render/labels.h`
  看着像死代码实际承重（`NamePool` 定义在 `render/paint.h`）。只 grep `label_` 会漏 `NamePool`。
- **归属原则**：数据概念归 map、界面/验收归 ui、render 只做"把要素画进 Raster"。
  `chrome.c`/`audit.c` 都在 ui。
- ★ `./scripts/check_layers.sh` = 分层门禁（已进 regress，用例 `layer_dirs`）：解析**真实** include
  （**必须剥块注释与行注释**——本项目注释里常出现 include 路径文本，不剥会大量假 FAIL）→ 比对
  允许方向表 → 违规 exit 非 0 并打印**文件行号**。跨模块改动前先跑它。
- ★ 新的 `run()` 门禁类脚本，**结论标记行必须始终打印**（不受 `--quiet` 影响）——回归靠
  「退出码 + 标记」双判据，标记缺失会被判成假 FAIL（踩过）。
- ★ CMake 换源后**旧 .o 不自动清**：查符号级依赖前先确认 .o 归属（残留 `chrome.c.o` 会让 `nm`
  误以为环还在）。
- 重构验收口径：零警告 + 依赖门禁 + **出图逐字节一致**（`scripts/bmpdiff.py`）+ `--ui-test` 33 项  + regress 全量（基线现 **78 通过 / 3 失败**，3 项是环境缺失）。

## 坐标系（`src/utils/datum.c`）

- CGCS2000 平移 0；北京1954 = EPSG:4214 默认 TOWGS84（塔里木油气参数，北京实测偏 60~95 m）；西安1980 =
  公开文献全国 3 参数（−104.57,−45.94,+5.30，8.2/4.2 m）。文案与参数表同文件维护。
- ★★★ "EPSG 没登记" ≠ "没有参数"（XA80 平移填 0 只换椭球 = 没转换，水平差 0.019 m）；两条独立证据互印才落值；
  分区参数已否决（选错代价 15.8~27 m > 全国最大 14.7 m）。
- 三扩展入口已做实：重合点解 7 参数（**必须列均衡**：旋转 µrad/尺度 ppm，否则条件数 ~1e13）；图幅改正量双线性
  clamp 不外推；局部偏移是**替换**不是叠加。

## 别再试

- 渲染：简化几何（三种已否）/去描边/面 tile 索引/顶点内联/软件预取/缓存上一条目/合并折线/NMBIN v3。
  贵的是**间接寻址**不是 stride；SDL 每次绘制 ~100 ns ⇒ 只有"不画"才省。
- ★ **瓦片缓存/条带复用已试做并全量回退**（2026-10-27）：条带当独立小视口漏画大要素（64634px 差）；只设
  raster clip 是负优化（z9 226ms vs 114ms）；全屏视口+收窄 tile_range 无收益（全局状态污染）。硬障碍：
  `mv_project` 逐要素 lround ⇒ 复用区天然 0.14~0.2% 差 1px；`nm_mercator_y_fast` 查表插值、与 `nm_mercator_inv`
  不互逆 ⇒ 整数像素平移无法精确表达、来回平移累积误差（0.2%→74%）。便宜路线也被否（降 LOD 只管"路"不管
  "面"省 18-25%；同视野降分辨率 1.6x 但投影成本不降）。
- 算路：rev.bin v2 / PQ 叉数≠4（D=4 13.2 ms，PQEnt 16 B）/ 状态表改布局（三连败）。`RouteOpts` 一律指名
  初始化（写死 `.heur_w=DEFAULT` 会顶掉兜底，三处踩过）。
- 地标：>30 无收益、加大 penalty 无效、加 w 救不了尾部；w 随地标紧度下走（10→1.3、30→**1.25**），
  改地标数必须连带重定 w。
- 一次 `nmbin_open` 全部 disp tile 会耗尽 Windows 映射句柄（~168 个后全失败）⇒ 逐 tile 开→拼→关。
- 浮层透明度：**`QGraphicsOpacityEffect` 已否**；**"整块元素一起淡"已否**（J 先生否决）。Qt 唯一可行解 =
  IconButton 自绘底板按系数算 alpha + QSS `rgba()` + viewport 透明。
- 新增 `Buf` 必须同步设 `.esz`（memset 清零 ⇒ 重 PBF 阶段无声崩溃）。sqlite rowid 会复用：删 track 必须同时删
  tpoint 子表。**`sqlite3_open` 不建父目录** ⇒ 目录不存在时返回 NULL、调用方照常报成功 ⇒ 用户数据静默不落盘；
  开库前先 `nm_mkdir_p`。本机 `rm` 是 safe-delete 改名 ⇒ 真删 `/bin/rm`。
- ★ **UI 改版后像素判据的硬编码坐标必然失效** ⇒ 判据坐标是"每次 UI 改版都要重测"的活资产，
  不是一次写死的常量。改判据要**先量新位置**再改，并随坐标更新重跑负向对照。

## 已达成水平

- 渲染 1280×800：**z9 113 / z11 51 / z13 35 / z15 16 ms**（`navmap_qt --shot-offscreen`，8 省北京中心，
  4 轮取最小，2026-10-27）。★★ **旧值 116/56/42/31 是 `map_shot`（SDL 壳，已删）口径，与新值不可相减**
  （同视口差 −3/−5/−7/−15 ms，随 zoom 单调增大，属执行路径差异）。任何"快了 N 倍"结论必须先对齐口径。
- 算路 50 条（30 地标 w=1.25）：中位 123 / p90 753 / 最慢 1503 ms，展开 10.43 M；比无表快 1.6×、p90 快 4.2×、
  质量 −3.21%。
- 图 5228 万节点、六成出度 0 ⇒ 按"有出边"2250 万算基数；全图 CH 放不下。热态固定成本 39~65 ms ⇒ 别抠启动。
