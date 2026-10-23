# navimap 项目长期约定

> 只留结论与"别再试"；推导看 PLAN.md（渲染 §19-32 / 算路 §33-50 / 转向限制 §14·§33 / 地标 §48-52 / 图集 §53+）。

## 目录 / 固定动作

- `sources/` 只放原始 `*.osm.pbf`；`products/` 全可删、`./scripts/sync_maps.sh` 确定性重建。
- ★★ **实验参数不能只活在命令行上**（header 不存地标坐标）：30 地标在
  `lmbuild_cli.c` 的 `k_lm[]`，`--nlm=N` 取前 N。
- 验收 `./scripts/regress.sh`（**47 项**）；编译**零警告**；改渲染补
  `python3 scripts/bmpdiff.py 旧 新`（要素色→背景色=丢要素）。
- ★ **平台差异只许出现在 `src/utils/plat.{h,c}`**：业务代码零 `#ifdef _WIN32`。
  字体链/临时目录/mmap/目录枚举/进程派生/时钟/rename 全走 nm_*。新平台分支先在
  `tools/plat_selftest` 补契约再动业务（回归 46/47，正反双向、有齿已实证）。
  ⚠️ 负向断言写"失败 Outcome 为真"，**别把成功断言取反**（踩过：8 项假 FAIL）。
  ⚠️ Win 映射句柄按基址登记、不随手关；`mallopt` 守卫不满足**整段消失不留空函数**
  （unused static ⇒ 零警告 FAIL）。本机无 mingw，Windows 分支未实机编译。
- ⚠️ 本机 `rm` 是 safe-delete 改名 ⇒ 真删 `/bin/rm`；别用 `mv` 搬
  `products/graph/<省>`（跨盘退化），用 `cp -p`→`diff -rq`→`/bin/rm -rf`。
- 全量重编译：`LD_LIBRARY_PATH=/home/along/.local/usr/lib/aarch64-linux-gnu cmake --build build -j8`

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
