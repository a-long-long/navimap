# navimap 项目长期约定

> 只留结论与"别再试"；推导看 PLAN.md（渲染 §19-32 / 算路 §33-50 / 转向限制 §14·§33 / 地标 §48-52 / 图集 §53+）。

## 目录 / 固定动作

- `sources/` 只放原始 `*.osm.pbf`；`products/` 全可删、`./scripts/sync_maps.sh` 确定性重建。
- ★★ **实验参数不能只活在命令行上**（header 不存地标坐标）：30 地标在
  `lmbuild_cli.c` 的 `k_lm[]`，`--nlm=N` 取前 N。
- 验收 `./scripts/regress.sh`（**54 项**：基线 47 + 去 SDL 化 4 + 字体 2 + UI 1）；编译**零警告**；
  改渲染补 `python3 scripts/bmpdiff.py 旧 新`（要素色→背景色=丢要素）。
  Windows 本机构建用 `./scripts/build_win.sh`（见下面的 gcc 静默失败条目）。
  ★ Qt 前端 UI 交互自检：`navmap_qt --ui-test`（16 项：搜索按钮→点击→输入框出现→Esc 收起
  + **展开搜索框后右栏按钮坐标/像素不变** + 按钮 tooltip + **无顶栏工具条**
  + 搜索按钮浮在地图上 + 右栏浮层
  + **右下角坐标显示有像素 & 鼠标移动后随之更新**），已进 regress.sh；
  **负向对照必须能失败**。
  ★★ **`--shot` 截图路径必须显式刷新状态栏**（主循环不启动 ⇒ 状态栏空白），
  否则**截图在撒谎**（此前交付过的截图底部坐标一直是空的）。
  ★★ **顶栏控件切换会改变顶栏高度 ⇒ 画布(centralWidget)整体移动 ⇒ 浮层跟着挪**：
  搜索按钮 36px 换成 QLineEdit 24px 会让画布上移 12px。
  ⇒ **根治办法：不要用顶栏**（2026-10-24 第三轮已取消顶栏 QToolBar），
  搜索按钮也做成**浮在画布左上角的浮层**（与右上角的操作按钮对称）——
  浮层是画布的子控件、不参与布局 ⇒ 画布尺寸恒定 ⇒ 坐标恒定。
- ★ **平台差异只许出现在 `src/utils/plat.{h,c}`**：业务代码零 `#ifdef _WIN32`。
  字体链/临时目录/mmap/目录枚举/进程派生/时钟/rename 全走 nm_*。新平台分支先在
  `tools/plat_selftest` 补契约再动业务（回归 46/47，正反双向、有齿已实证）。
  ⚠️ 负向断言写"失败 Outcome 为真"，**别把成功断言取反**（踩过：8 项假 FAIL）。
  ⚠️ Win 映射句柄按基址登记、不随手关；`mallopt` 守卫不满足**整段消失不留空函数**
  （unused static ⇒ 零警告 FAIL）。本机无 mingw，Windows 分支未实机编译。
- ★★ **SDL 只许出现在两个适配层**（方案2，2026-10-24 落地）：
  `navimap_sdl`(window_sdl.c) 与 `map_shot`。`navimap_core` 与 `navmap_qt` **零 SDL**。
  `App` 里不许出现 SDL 句柄，也不许放 `Raster ras`（帧缓冲归各前端自持）。
  渲染入口统一 `app_render_to_buf(px,w,h,stride,r/g/b/amask)`，像素格式**从 surface 读、
  不写死**。`core_no_sdl` 门禁钉住这条边界（含负向对照）。
- ★★ **两套字体系统互不相干**（2026-10-24 踩过）：
  地图文字 = C 侧 stb_truetype（`render/font.c`），Qt 控件文字 = Qt 自己的字体系统。
  ⇒ Qt 前端必须 `src/ui_qt/qt_font.cpp` 把 `nm_font_candidates()` 的字体
  **注册进 Qt**（`addApplicationFont`）+ `setFamilies` 设中英回退链，否则控件中文是方框。
  ★ 判"字体能否画中文"**别用 Qt 的字体度量**（offscreen 插件下所有族宽度都一样，
  会把每个族都判成"没有中文"）⇒ 查字体文件自己的 cmap（C 侧 `font_probe_glyph`）。
  ★★ **同理：图标符号（＋ ◎ ☰ 🔍）也不能用字体字符** —— `inFont()` 全 false，
  界面上是豆腐块 ⇒ 一律用 `src/ui_qt/qt_icons.cpp` 的 `QPainter` 自绘矢量。
  ★★ `drawPixmap` 在 DPR≠1 下会把图标画到目标矩形**一角**（含显式给源矩形的重载）
  ⇒ 直接在 `paintEvent` 里画路径，别经 QPixmap 中转。
  门禁 `font_cjk` / `font_cjk_neg` / `qt_ui_test`。
- ★★★ **Windows(MSYS2) 上 gcc 会静默失败**（退出码 1、stderr 零输出）：
  ninja 走 cmd.exe ⇒ 拿到的是 MSYS 转换后的 Windows 形式 PATH；
  且 Defender（MsMpEng）会扫描并**杀掉**新起的 cc1plus.exe。
  ⇒ 构建一律用 `./scripts/build_win.sh`（受控 PATH + `build_launcher.bat` 绕开 cmd.exe
  + `cc_retry.sh` 对空 stderr 重试）。★ `CMAKE_C_COMPILER_LAUNCHER` 是**初始化变量**，
  对已有 build/ 不生效 ⇒ 必须用 target 属性 `C_COMPILER_LAUNCHER`。
  ★ `.bat` 必须纯 ASCII（cmd.exe 会把中文注释当命令执行）。
- ⚠️ 本机 `rm` 是 safe-delete 改名 ⇒ 真删 `/bin/rm`；别用 `mv` 搬
  `products/graph/<省>`（跨盘退化），用 `cp -p`→`diff -rq`→`/bin/rm -rf`。
- 全量重编译：`LD_LIBRARY_PATH=/home/along/.local/usr/lib/aarch64-linux-gnu cmake --build build -j8`
  （Windows 本机：`/d/msys64/ucrt64/bin/cmake.exe --build build -j8`，
  gcc 16.2.0 在 `/d/msys64/ucrt64/bin/`，Qt6 6.11.2 在 `/d/Qt/6.11.2/mingw_64`）

## 判据纪律

- 渲染用 `map_shot` **第二帧**；算路用 `route_cli --repeat=N` **丢第 1 次**取中位。
  负载能污染耗时一个数量级 ⇒ **布尔判据用展开数/里程/像素**，时间只兜底取多轮最小。
  A/B **≥4 轮每轮列出**。
- ★★★ **手挑几条路线下结论必翻车**（已踩 4 次）：样本须覆盖形状且成对配对。
  归因给的是"占多少"不是"删掉能省多少"。等价性判据全量逐点比 + 定向边界 +
  **负向对照必须能失败**。
- 怀疑未初始化内存**先查对象生命周期**（悬垂 ASan/valgrind 查不到）；
  两阶段行为不同先 dump 计数（622 vs 311 直接破案）。

## 已达成水平

- 渲染 1280×800：**z9 116 / z11 56 / z13 42 / z15 31 ms**（LTO 已开）。
- 算路 50 条（**发货：30 地标 + w=1.25**）：中位 123 / p90 753 / 最慢 1503 ms，展开 10.43 M；
  基线（无表 1.4+自适应）196/3131/8913 ⇒ 快 1.6×、p90 快 4.2×、质量 −3.21%。
- 热态固定成本 39~65 ms（首次 1 s 级是 3.8 GB 图 mmap 冷页）⇒ 别抠启动。
  图 5228 万节点，**六成出度 0** ⇒ 基数按"有出边"2250 万算；全图 CH 放不下。

## 别再试

- 渲染：简化几何（三种已否）/ 去描边 / 面 tile 索引 / 顶点内联 / 软件预取 /
  缓存上一条目 / 合并折线 / NMBIN v3。贵的是**间接寻址**不是 stride；
  SDL 每次绘制 ~100 ns ⇒ 只有"不画"才省。自写像素后退化段必须画 1 px；surface RGB888。
- 算路：rev.bin v2（已回退）、PQ 叉数≠4（D=4 13.2 ms 最优，PQEnt 必须 16 B）、
  状态表改布局（三连败）。
- 地标：>30 个无收益、加大 penalty 无效、与双向无关、不是转向限制
  （剩余候选：u 侧格内取 max）。加 w 救不了尾部（1.8→3.0 最多再降 1.9×）。

## 关键定论

- ★ 地标下界**不是严格下界**（用户拍板接受）：是"加速启发式"，`--exact` 开表不承诺最优，
  真值须 `NAVIMAP_LM=0`。第 45 项判据写成上界（开表分钟≤无表+1）。
  ★★ 判据不该在 bug 修好那天反而报错。
- w 最优值随地标紧度下走：10 地标 1.3、30 地标 **1.25**；改地标数必须连带重定 w。
  `heur_w_hi` 也兜底 1.25 ⇒ 关掉撞顶升级，同时 `two_pass=(try_cap!=0)&&(heur_w_hi!=w0)`
  去空转第一遍。
- ⚠️ `RouteOpts` 写死 `.heur_w=DEFAULT` 会把兜底顶掉（三处踩过）；一律指名初始化；
  热路径严禁 `getenv`。拆函数改共享状态必须恢复。
- ★ 销毁 renderer 前必须 `font_drop_textures()`（font+font_big），每条路径都要
  （修过：指针判据被地址复用击败）。**注释写了约定 ≠ 所有路径落实。**
  ⚠️ 但 stb_truetype 后端 `font_drop_textures` 只清 CPU 字形缓存，
  **不能再每帧调**（会毁掉 CJK 缓存）。
- ★★★ **自写光栅必须与 SDL 软渲染器逐像素等价**（已实证 3 个真 bug）：
  ① alpha 混色是 `(s*a)/255 + (d*(255-a))/255` **两次除法**（不是一次除）；
  ② `raster_rect` 角点只能画一次（4 边重叠 ⇒ 混两次）；
  ③ 裁剪要按 SDL `SDL_rect_impl.h` **逐字重写**（水平/垂直短路 + 早退 + 差值方向），
     手写 Cohen-Sutherland 与 SDL 差 1 px 的比例达 34%。
  门禁：`blend_equiv` / `clip_equiv`（regress.sh 内，负向对照已实证能失败）。
- ★ **判"提取成功"要看退出码，不是看输出行数**（否则假绿）：
  `nm -u <PE exe>` **不列 DLL 导入符号**（SDL_* 只在 `objdump -p` 导入表里）⇒
  两个提取器取并集；`2>&1` 会让 stderr 混进 stdout 凑出行数 ⇒ 判定假绿，
  必须 stderr 单独重定向 + 用 `pclose` 的退出码。
  MinGW 的 `popen` 走 cmd.exe：带引号路径要加 `call`，`2>/dev/null` 是无效重定向。
- 图集：`MapSrc.loaded`，**省份下标恒定绝不重排 srcs[]**；`const MapSet*` 传
  `mapset_ensure_loaded` 要强转（零警告门禁）。实测加载 3/卸载 5 省。
- PBF 显示层 `--pbf-display=<dir>[:LRU]`：省粒度解码 + 内存 LRU，8 省逐字节 IDENTICAL；
  polyrefs 即 nodes.bin 下标；下标空间须复刻 graph_build；解码 0.65/1.6/4.4 s ⇒ 需后台线程。
- 转向限制：only_* 存**允许集**（没查到不禁）；验收靠 `restr_ab` 定向 + 双向 +
  必须失败的负向对照；副本省 1190 条死数据，`seam_blocked` 承重。
  地名 kind≤12 画、13 只进搜索；改 kind 表连带改 `places_build --selftest` 与 `kind_max_for_zoom`。
- 多省不变量：多省画面 == 只加载包含方；整省包含的省（dup_of>=0）面/路/线全跳；面/线不去重。
  `DEFAULT_MAX_EXPAND` 1200 万；`SEW_SLOTS`=16384；瓦片回退带 2e-3 > 查表误差（85° 3.6e-4）；
  `NMBIN_VERSION=3` 改格式必须顶。

## 阶段3：route.c 拆分定式（2026-10-24，1856 → 928 行）

- route 模块现状：`src/route/` 下 **route.c（只剩搜索本体）+ st/sew/restr/cost/lmdist/result**（新拆）
  + 既有的 lmtable/revidx/navsim/maneuver；全部编进 `libnavmap_route.a`（**不单独成 lib**）。
  剩余核心：`members_of` / `expand_dir` / `plan_search` / `router_plan` —— **最后才动**。
- ★ `route.h` 是**公共**头（被 render/ui 包含）⇒ 完整 `Router` / `RestrIdx` / `Sew*` 只能放
  私有头 **`route_priv.h`**（仅 route 模块的 .c 用）。lmtable.c/revidx.c 只用 Router 不完整类型。
- ★★ 新模块头（`restr.h`/`sew.h`）一律 **`#include "route/route_priv.h"`**，
  **不要把私有类型搬进模块头**：struct Router 要用 SewSlot ⇒ route_priv.h 必须反过来 include
  sew.h ⇒ 谁先被 include 都会"类型未定义"（guard 让后半段变空）。类型留在 route_priv.h 不动。
- ★ 公共头已声明的函数出来不必新建 .h：`lmdist.c` / `result.c` 就是如此（route.h 里已有声明）。
- ★ 被跨文件调用的函数必须**去掉 static**（否则 static/non-static 冲突）；
  只在本文件用的（sort_kv / sew_cb / cmp_pn / lm_prov_of）保留 static。
- 跨 `.c` 共用的宏（MAX_MEMBER）下沉到 `route_priv.h`；先把所有使用点 grep 一遍再决定。

### 拆大型 .c 的搬迁脚本纪律（C）

- 锚点：唯一子串 + `assert count==1`；**右边界用下一个段落标题**，别用"找下一个 `}`"
  （会拿到内层函数的花括号）。
- 断言要双向：该消失的（旧定义）+ 该保留的（调用点、类型用法）都写；
  断言在写盘**之前** ⇒ 脚本崩了工作区是干净的（安全）。
- ⚠️ 脚本**漏写 write()** 这类错拦不住（cost.c 那次文件没生成却在被调用）⇒
  每次跑完必须 `ls` 新文件、`cmake --build` 零警告才算过。

### 行为等价判据（路由）

- 探针必须用 `--graph=products/graph`（多省父目录）：单省配不上 8 省 lm.bin ⇒ 退回直线 w=1.40、不可复现。
- 确定性字段优先级：**展开节点数 > 预计分钟 > 总长**；`耗时 ms` 随负载波动，只兜底不作判据。
  基准值：120294 / 531738 / 789703 / 546391 / 5135（route_cross/hard/far_dalian/lm_exact/navsim_bj）。
- 每块要有**专属判据**：lmdist→lmdist_cli 三距离逐字节（23136.791/21932.828/27700.450 秒）；
  result→route_check 自检行「通过（3917 段，跨省次数 1）」；restr→restr_ab 正向 OK + 负向 FAIL。
- **regress.sh 基线 = 52 通过 / 2 失败**（两项本机环境 glitch：plat_selftest 字体偏好、
  core_no_sdl_sdl 找不到 build/navimap 给 objdump）。见到 52/54 就是没回归。
