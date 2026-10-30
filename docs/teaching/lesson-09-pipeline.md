# 第 9 课：渲染管线编排

> 模块：`src/ui/window.c`（`paint_frame`/`app_render_to_buf`）、`src/render/navdraw.{c,h}`、`src/ui/audit.{c,h}`、`src/ui/chrome.{c,h}`
> 前置：第 8 课 `Raster`/`raster_*` 原语、第 10 课搜索结果（被 chrome 消费）。

## 一、教学目的

看懂一帧如何从"图集 + 视口"变成"像素缓冲"，以及 Qt 与离屏两套前端如何共用同一入口。学完后学生应能：
- 口述一帧的 9 步渲染顺序（底图 → 要素 → 标注 → 覆盖层）；
- 解释"为什么 `app_render_to_buf` 是唯一渲染入口"——Qt 与离屏都经它；
- 说明 `paint_route` 的"投影一次、两遍复用（白描边 + 彩芯）"与 `done_seg` 切分"已走/未走"；
- 读懂 `app_offscreen_audit` 的"按字节比色"断言，以及 `chrome` 在最上层浮绘（含无图区提示）。

## 二、功能构成

| 关键函数 / 结构体 | 文件:行号区间 | 职责 |
|---|---|---|
| `paint_frame` | `window.c:1169-1433` | 一帧的 9 步编排（底图→要素→标注→覆盖层→夜间→chrome） |
| `app_render_to_buf` | `window.c:1435-1452` | 绑定像素缓冲后调 `paint_frame`，Qt 与离屏共用唯一入口 |
| `PaintStats` | `paint.h:51-67` | 分阶段计时/计数（含 `areas_dotted` 等确定性量） |
| `paint_route` / `RSeg` | `navdraw.c:49-123` / `navdraw.c:15` | 路线折线：白描边 + 蓝芯/(已走灰) 两遍 |
| `paint_nav_car` / `fill_triangle` | `navdraw.c:151-178` / `navdraw.c:127-149` | 车头三角箭头（蓝芯白边），按航向旋转 |
| `paint_nomap_banner` + `nfill/nrect/ntext` | `navdraw.c:548-631` / `navdraw.c:29-47` | 无图区提示横幅（"你为什么看不到东西"） |
| `app_offscreen_audit` | `audit.c:79-217`（声明 `audit.h:43-44`） | 像素计数 + 断言共享模块（被 `offscreen_shot` 复用） |
| `paint_chrome` / `draw_nomap` | `chrome.c:597-615` / `chrome.c:383-402` | nav-desktop 风格界面层（11 类元素）+ 无图卡片 |

> 说明：`paint_frame` 实际位于 `src/ui/window.c`（非 `render/paint.c`）；`render/paint.c` 提供的是被 `paint_frame` 调用的要素绘制函数（`paint_areas/roads/lines/contours` 等）。本课聚焦编排主流程与覆盖层、审计、chrome。

## 三、完整实现代码

### 1. `paint_frame` 的 9 步流程（`window.c:1169-1433`）

```c
static void paint_frame(Raster *rs, App *a, PaintStats *st)
{
    /* 统计在这里清零一次，且只在这里清。
       曾经放在 paint_roads 入口，结果把它之前 paint_areas 累加的 areas_drawn
       抹成 0；也曾一个字段都不清，于是 zoom<15 不画路名时 road_labels_drawn
       是调用方栈上的垃圾（54.92 亿个标签）。两边都踩过，所以规则是：
       一帧一次、画任何东西之前、全部字段清。路名收集同理：多省循环时
       各省依次追加，reset 必须只在这一处做。 */
    memset(st, 0, sizeof(*st));
    road_labels_reset();
    paint_roadpix_begin();

    /* 分阶段计时：帧时间的瓶颈只能这样定位（本机 perf 装不全）。
     * 走平台层的单调时钟（clock_gettime / QueryPerformanceCounter），
     * 不在业务代码里直接调 POSIX 时钟。 */
    uint64_t ts0, ts1;
#define TICK(fld) (ts1 = nm_now_ms(), \
                   st->fld += (double)(ts1 - ts0), \
                   ts0 = ts1)

    ts0 = nm_now_ms();
    paint_background(rs, a->view.w, a->view.h);
    TICK(ms_bg);

    double vb[4];
    mv_visible_bbox(&a->view, vb);
    int32_t evb[4] = {
        (int32_t)(vb[0] * 1e7), (int32_t)(vb[1] * 1e7),
        (int32_t)(vb[2] * 1e7), (int32_t)(vb[3] * 1e7)
    };
    mapset_set_view(&a->ms, evb);   /* 进省才加载、离省卸载（见 mapset.c） */

    /* ★ 整省被别的省完全包含的（dup_of >= 0，加载时按 bbox 包含 + 抽样
       ≥99% 命中判定，实测北京/天津 ⊂ 河北 100%）——渲染直接跳过，不去重、
       不扫描、不落笔。三条理由：
         ① 路由早就这么干（route.c 里 dup_of 的省不参与展开），渲染留着
            它只是把同一条路扫两遍再靠去重判掉一次，8 省 z9 一帧 104 万
            条里有 48 万条是这种纯浪费；
         ② 判据是"包含方数据是超集"：实测同一视口下 z13/z15 两省画面
            逐像素相同，z11 河北反而比北京多画（差异全是"背景→要素"方向，
            即河北更全），所以跳过不会丢东西；
         ③ 顺带让分级口径统一：以前同一条路由"图集里排前面的省"说了算，
            跨省界会出现同等级路两种颜色。 */
    /* 跨省去重：只有 ≥2 个数据源同时盖住视口才启用。单省走 NULL 快路径，
       连一次哈希都不做。
       为什么必要：各省 pbf 按多边形裁切，飞地会被邻省整块包进去（实测北京、
       天津的节点 id 100% 同时存在于河北），同一条路/同一个建筑要连画两遍。 */
    int hits = 0;
    for (int i = 0; i < a->ms.n; i++) {
        if (!a->ms.srcs[i].loaded) continue;   /* 视口外已卸载的省不计 */
        if (bbox_hits(a->ms.srcs[i].bbox, vb)) hits++;
    }
    DedupSet *dd = NULL;
    if (hits > 1) { dedup_begin(&a->dd); dd = &a->dd; }

    /* 分阶段遍历所有省：面 → 路 → 线 → 标注。如果按省整段画完，
       省 A 的铁路会压在省 B 的路上面，层级就乱了。 */
    for (int i = 0; i < a->ms.n; i++) {
        MapSrc *s = &a->ms.srcs[i];
        if (!bbox_hits(s->bbox, vb)) continue;   /* 视口外的省整省跳过 */
        if (s->dup_of >= 0) continue;            /* 整省被别省包了，见下方说明 */
        if (!s->loaded) continue;                /* 该省已卸载（set_view 判定视口外） */
        if (s->has_features)
            paint_areas(rs, &a->view, &s->nodes, &s->areas, &s->polyrefs,
                       &s->area_bbox, st);
    }
    TICK(ms_areas);
    /* 各省的 tiles.bin（含被跳过的省也算了？不：被跳过的省不画，它的格子
       不算"有人画过"，所以只收真会画的省）。给 paint_roads 用来按格子
       门控去重。 */
    const NMBinFile *peer[64];
    int n_peer = 0;
    for (int i = 0; i < a->ms.n && n_peer < 64; i++) {
        MapSrc *s = &a->ms.srcs[i];
        if (!bbox_hits(s->bbox, vb)) continue;
        if (s->dup_of >= 0) continue;
        if (!s->loaded) continue;
        if (s->has_tiles) peer[n_peer++] = &s->tiles;
    }
    for (int i = 0; i < a->ms.n; i++) {
        MapSrc *s = &a->ms.srcs[i];
        if (!bbox_hits(s->bbox, vb)) continue;
        if (s->dup_of >= 0) continue;
        if (!s->loaded) continue;
        paint_roads(rs, &a->view, &s->nodes, &s->edges,
                    s->has_tiles ? &s->tiles : NULL,
                    s->has_tiles ? &s->edgelist : NULL,
                    s->np.ready ? &s->np : NULL, dd,
                    a->max_class, st, peer, n_peer);
    }
    TICK(ms_roads);
    for (int i = 0; i < a->ms.n; i++) {
        MapSrc *s = &a->ms.srcs[i];
        if (!bbox_hits(s->bbox, vb)) continue;
        if (s->dup_of >= 0) continue;
        if (!s->loaded) continue;
        if (s->has_features)
            paint_lines(rs, &a->view, &s->nodes, &s->lines, &s->polyrefs,
                       &s->line_bbox, st);
    }
    /* 等高线：独立于线/面要素，叠加在路网之上、地名之下。受 show_contours 门控。 */
    if (a->show_contours) {
        for (int i = 0; i < a->ms.n; i++) {
            MapSrc *s = &a->ms.srcs[i];
            if (!bbox_hits(s->bbox, vb)) continue;
            if (s->dup_of >= 0) continue;
            if (!s->loaded) continue;
            if (s->has_contours)
                paint_contours(rs, &a->view, &s->contours, &s->contour_polyrefs,
                               &s->contour_pts, &s->contour_bbox, st, a->font);
        }
    }
    TICK(ms_lines);
    if (a->font) {
        /* 地名库是全局一份（places.db 跨省通用），只需查一次 */
        paint_place_labels(rs, &a->view, &a->places, a->font, st,
                           a->show_poi, a->show_labels);
        paint_road_labels(rs, &a->view, a->font, st);
    }
    TICK(ms_labels);
#undef TICK

    /* 路线叠加在最上面：路面已经画完，压在路网上才看得清走向。
       模拟导航开着时按车头位置切成"已走过（灰）/ 未走（蓝）"两截。 */
    /* GPX 轨迹 / 收藏点（地图叠加层）：画在导航路线**下面**——路线是
       "当前这次出行"，轨迹是"历史记录"，后者被前者压住是合理的层级。
       轨迹条数为 0 时连遍历都不做（没有 GPX 的用户零开销）。 */
    if (a->gpx.n_trk > 0) paint_tracks(rs, &a->view, &a->gpx);
    if (a->gpx.n_wpt > 0) paint_favorites(rs, &a->view, &a->gpx, a->font);

    if (a->have_rr && a->rr.n >= 2) {
        int ds = -1;
        double dt = 0;
        if (a->sim_on && a->sim.seg + 1 < a->rr.n) {
            ds = (int)a->sim.seg;
            double lm = a->rr.seg_m ? (double)a->rr.seg_m[a->sim.seg] : 0.0;
            dt = lm > 0 ? a->sim.seg_pos / lm : 0.0;
        }
        paint_route(rs, &a->view, a->rr.lat_e7, a->rr.lon_e7, a->rr.n, 0, ds, dt);
    }
    if (a->sim_on)
        paint_nav_car(rs, &a->view, a->sim.car_lat, a->sim.car_lon,
                      a->sim.heading_deg);
    if (a->pick_a.have)
        paint_nav_marker(rs, &a->view,
                         a->pick_a.snapped ? a->pick_a.slat : a->pick_a.lat,
                         a->pick_a.snapped ? a->pick_a.slon : a->pick_a.lon,
                         1, a->font);
    if (a->pick_b.have)
        paint_nav_marker(rs, &a->view,
                         a->pick_b.snapped ? a->pick_b.slat : a->pick_b.lat,
                         a->pick_b.snapped ? a->pick_b.slon : a->pick_b.lon,
                         0, a->font);

    int cx, cy;
    mv_project(&a->view, a->view.lat, a->view.lon, &cx, &cy);
    paint_marker(rs, cx, cy, 4, 30, 90, 220);

    /* 解码中的省份：画"省名 解码中… NN%"占位（后台线程解码期间 has_features=0） */
    paint_dmap_overlay(rs, a);

    /* 自动昼夜：每帧用当前视口中心 + 本地时间推算昼/夜，跨过日出/日落时自动切换并重绘 */
    app_update_daynight(a);
    /* 夜间蒙版：只压暗地图，不压下面要画的面板/文字/chrome */
    if (a->day_night) paint_night_tint(rs, a->view.w, a->view.h);

    /* Qt 前端接管后这些自绘 UI 全部由真控件承担，不再画 */
    if (a->show_panel && a->font && !a->qt_ui)
        paint_nav_panel(rs, a->have_rr ? &a->rr : NULL,
                        a->man.n ? &a->man : NULL, a->font,
                        a->font_size + 8, 12, 12, 6,
                        a->nav_msg[0] ? a->nav_msg : NULL,
                        app_ovl_a(a, OVL_NAVPANEL));

    /* 底部指令条：模拟导航时才出现（静止看路线时用左上角面板就够了） */
    if (a->sim_on && a->font && !a->qt_ui) {
        static char b_turn[64], b_dist[64], b_road[96], b_rem[64], b_eta[64];
        NavBanner b;
        memset(&b, 0, sizeof(b));
        const char *b_lanes = NULL;
        int b_turn_dir = -3;
        double to_go = 0;
        int mi = navsim_next_maneuver(&a->sim, &a->man, &to_go, NULL);
        if (mi >= 0) {
            const Maneuver *m = &a->man.v[mi];
            snprintf(b_turn, sizeof(b_turn), "%s", maneuver_kind_text(m));
            char d[48];
            fmt_distance(to_go, d, sizeof(d));
            snprintf(b_dist, sizeof(b_dist), "%s后", d);
            if (m->road[0]) snprintf(b_road, sizeof(b_road), "进入%s", m->road);
            else            b_road[0] = '\0';

            /* 车道指示：取"驶入该路口那条边"的 turn:lanes。
               m->idx 是动作发生的路口点下标；驶入边 = edge_idx[idx-1]，
               它属于 pts[idx] 那个省（route.h: edge_idx[i] 在 pts[i+1].prov 内）。 */
            if (m->idx >= 1 && m->idx < a->rr.n) {
                uint32_t seg = m->idx - 1;
                int prov = a->rr.pts[m->idx].prov;
                const char *ls = mapset_edge_lanes(&a->ms, prov, a->rr.edge_idx[seg]);
                if (ls && *ls) b_lanes = ls;
            }
            switch (m->type) {
            case MAN_SLIGHT_LEFT: case MAN_LEFT: case MAN_SHARP_LEFT: b_turn_dir = -1; break;
            case MAN_SLIGHT_RIGHT:case MAN_RIGHT:case MAN_SHARP_RIGHT:b_turn_dir = 1;  break;
            case MAN_STRAIGHT:                                       b_turn_dir = 0;  break;
            case MAN_UTURN:                                          b_turn_dir = -1; break;
            default:                                                 b_turn_dir = -3; break;
            }
        } else {
            snprintf(b_turn, sizeof(b_turn), "沿当前道路行驶");
            b_dist[0] = '\0';
            b_road[0] = '\0';
        }
        char d[48];
        fmt_distance(navsim_remaining_m(&a->sim), d, sizeof(d));
        snprintf(b_rem, sizeof(b_rem), "剩余 %s", d);
        int mins = (int)(navsim_remaining_s(&a->sim) / 60.0 + 0.5);
        if (mins >= 60) snprintf(b_eta, sizeof(b_eta), "约 %d 小时 %d 分",
                                 mins / 60, mins % 60);
        else            snprintf(b_eta, sizeof(b_eta), "约 %d 分钟", mins);
        b.turn = b_turn; b.dist = b_dist; b.road = b_road;
        b.remain = b_rem; b.eta = b_eta;
        b.lanes = b_lanes; b.turn_dir = b_turn_dir;
        paint_nav_banner(rs, &a->view, a->font, a->font_big,
                         a->font_size + 8, app_big_size(a->font_size) + 8, &b,
                         app_ovl_a(a, OVL_NAVBANNER));
    }

    /* 搜索结果图钉（路网之上才看得见）。SDL 自绘搜索框只在无 Qt 前端时画。 */
    if (a->su.active && a->font) {
        int show = a->su.n_hits < 8 ? a->su.n_hits : 8;
        for (int i = 0; i < show; i++)
            paint_search_pin(rs, &a->view, a->su.hits[i].lat, a->su.hits[i].lon,
                             i == a->su.sel, a->font,
                             i == a->su.sel ? a->su.hits[i].name : NULL);
        if (!a->qt_ui) {
        SearchBoxView sb;
        memset(&sb, 0, sizeof(sb));
        sb.active = 1;
        sb.text = a->su.text;
        sb.editing = a->su.editing;
        sb.hits = a->su.hits;
        sb.n_hits = a->su.n_hits;
        sb.sel = a->su.sel;
        sb.show_max = 8;
        sb.elapsed_ms = (int)a->su.elapsed_ms;
        paint_search_box(rs, &a->view, a->font, a->font_big,
                         a->font_size + 8, app_big_size(a->font_size) + 8, &sb,
                         app_ovl_a(a, OVL_SEARCH));
        }
    }

    /* 无地图区域提示画在最上层：它解释的是"你为什么看不到东西"，
       被面板或按钮压住就失去意义了。 */
    app_update_coverage(a);
    if (a->cov_state == COV_NONE || a->cov_state == COV_BBOX_ONLY)
        paint_nomap_banner(rs, &a->view, a->font, a->font_size + 8, a->cov_state,
                           &a->cov_place, app_ovl_a(a, OVL_NOMAP));

    /* nav-desktop 风格 chrome（自绘标题栏 / 顶栏 / 右栏 / 导航栏 /
       步骤面板 / 主菜单 / 无图卡片 / 比例尺 / 指北针 / 昼夜）叠在最上层；
       Qt 前端时由真控件承担，跳过。 */
    if (!a->qt_ui)
        paint_chrome(rs, a);
}
```

### 2. `app_render_to_buf` —— 唯一共享渲染入口（`window.c:1435-1452`）

```c
int app_render_to_buf(App *a, void *px, int w, int h, size_t stride,
                     uint32_t rmask, uint32_t gmask, uint32_t bmask,
                     uint32_t amask, PaintStats *st)
{
    if (w <= 0 || h <= 0 || !px) return -1;
    a->view.w = w;
    a->view.h = h;

    /* 绑定到调用方持有的 32bpp 像素缓冲：Qt 前端在自己的 QImage 缓冲上、
     * 离屏审计在 src/ui/offscreen_shot.c 的 malloc 缓冲上。
     * 字形走 stb_truetype 的 CPU 缓存（见 font.c），与渲染目标生命周期无关，
     * 这里不再每帧 drop 纹理。 */
    Raster rs;
    if (raster_bind_mem(&rs, px, w, h, stride, rmask, gmask, bmask, amask) != 0)
        return -1;
    paint_frame(&rs, a, st);
    return 0;
}
```

> 关键纪律：按"跨省统一层级"遍历（先所有省的面、再所有省的路），否则层级会乱；`dup_of>=0` 整省包含的省直接跳过（见 `paint_frame` 第 1229 行）。

### 3. `navdraw` 路线绘制（`navdraw.c`）

`RSeg`（段的两端点 + 是否已走过）与 `paint_route`（投影一次、两遍复用；按 `done_seg`/`done_t` 切分"已走/未走"）：

```c
/* 段的两端点 + 颜色。0 = 未走（蓝），1 = 已走过（灰） */
typedef struct { int x0, y0, x1, y1, done; } RSeg;

int paint_route(Raster *rs, const MapView *v,
                const int32_t *lat_e7, const int32_t *lon_e7, uint32_t n,
                int width, int done_seg, double done_t)
{
    if (!rs || !v || n < 2) return 0;
    if (width <= 0) width = v->zoom >= 13.0 ? 6 : (v->zoom >= 11.0 ? 4 : 3);

    /* 投影一次，两遍复用（白描边 + 彩芯）。逐段重复投影会把三角函数
       算两遍，长途路线几万段时不划算。 */
    int *xy = malloc((size_t)n * 2 * sizeof(int));
    if (!xy) return 0;
    for (uint32_t i = 0; i < n; i++)
        mv_project(v, (double)lat_e7[i] / 1e7, (double)lon_e7[i] / 1e7,
                   &xy[2 * i], &xy[2 * i + 1]);

    /* 车头所在的那段要切成"已走/未走"两截，所以段数最多 n（多一截） */
    RSeg *sg = malloc((size_t)n * sizeof(RSeg));
    if (!sg) { free(xy); return 0; }
    uint32_t ns = 0;

    int split = (done_seg >= 0 && (uint32_t)done_seg + 1 < n);
    double tt = done_t;
    if (tt < 0) tt = 0; else if (tt > 1) tt = 1;
    int cx = 0, cy = 0;
    if (split) {
        uint32_t k = (uint32_t)done_seg;
        double la = lat_e7[k] / 1e7 + (lat_e7[k + 1] / 1e7 - lat_e7[k] / 1e7) * tt;
        double lo = lon_e7[k] / 1e7 + (lon_e7[k + 1] / 1e7 - lon_e7[k] / 1e7) * tt;
        mv_project(v, la, lo, &cx, &cy);
    }

    for (uint32_t i = 0; i + 1 < n; i++) {
        int x0 = xy[2 * i], y0 = xy[2 * i + 1];
        int x1 = xy[2 * i + 2], y1 = xy[2 * i + 3];
        if (!split) {
            sg[ns++] = (RSeg){ x0, y0, x1, y1, done_seg < 0 ? 0 : (i < (uint32_t)done_seg) };
        } else if (i < (uint32_t)done_seg) {
            sg[ns++] = (RSeg){ x0, y0, x1, y1, 1 };
        } else if (i == (uint32_t)done_seg) {
            sg[ns++] = (RSeg){ x0, y0, cx, cy, 1 };      /* 走过的那一截 */
            sg[ns++] = (RSeg){ cx, cy, x1, y1, 0 };      /* 剩下那一截 */
        } else {
            sg[ns++] = (RSeg){ x0, y0, x1, y1, 0 };
        }
    }

    int W = v->w, H = v->h;
    const int margin = 64;      /* 端点允许在视口外一点，避免边缘的路断掉 */
    int drawn = 0;

    for (int pass = 0; pass < 2; pass++) {
        int w = pass == 0 ? width + 4 : width;
        uint32_t col;
        for (uint32_t i = 0; i < ns; i++) {
            int x0 = sg[i].x0, y0 = sg[i].y0, x1 = sg[i].x1, y1 = sg[i].y1;
            if ((x0 < -margin && x1 < -margin) ||
                (x0 > W + margin && x1 > W + margin) ||
                (y0 < -margin && y1 < -margin) ||
                (y0 > H + margin && y1 > H + margin))
                continue;
            if (pass == 0) {
                col = raster_rgb(rs, 255, 255, 255);
            } else if (sg[i].done) {
                col = raster_rgb(rs, 130, 138, 150);
            } else {
                col = raster_rgb(rs, NAV_ROUTE_R, NAV_ROUTE_G, NAV_ROUTE_B);
            }
            paint_thick_line(rs, x0, y0, x1, y1, w, col);
            if (pass == 1 && !sg[i].done) drawn++;
        }
    }
    free(sg);
    free(xy);
    return drawn;
}
```

车头三角箭头（按航向旋转，蓝芯白边）：

```c
/* 三角形填充：直接按重心坐标逐点画（车头只有 30×30 像素，这点循环比引一套
   多边形光栅化划算）。color 为已打包的 32 位像素值。 */
static void fill_triangle(Raster *rs, int x0, int y0, int x1, int y1,
                          int x2, int y2, uint32_t color)
{
    int mnx = x0, mxx = x0, mny = y0, mxy = y0;
    int xs[3] = { x0, x1, x2 }, ys[3] = { y0, y1, y2 };
    for (int i = 1; i < 3; i++) {
        if (xs[i] < mnx) mnx = xs[i];
        if (xs[i] > mxx) mxx = xs[i];
        if (ys[i] < mny) mny = ys[i];
        if (ys[i] > mxy) mxy = ys[i];
    }
    double area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if (fabs(area) < 1e-6) return;
    for (int y = mny; y <= mxy; y++) {
        for (int x = mnx; x <= mxx; x++) {
            double w0 = ((x1 - x) * (y2 - y) - (x2 - x) * (y1 - y)) / area;
            double w1 = ((x2 - x) * (y0 - y) - (x0 - x) * (y2 - y)) / area;
            double w2 = 1.0 - w0 - w1;
            if (w0 >= -0.001 && w1 >= -0.001 && w2 >= -0.001)
                raster_point(rs, x, y, color);
        }
    }
}

void paint_nav_car(Raster *rs, const MapView *v,
                   double lat, double lon, double heading_deg)
{
    int x, y;
    mv_project(v, lat, lon, &x, &y);
    if (x < -40 || y < -40 || x > v->w + 40 || y > v->h + 40) return;

    /* 屏幕坐标下：航向 0 = 正北 = -y。右手方向 = (cos h, sin h)。 */
    double h = heading_deg * M_PI / 180.0;
    double dx =  sin(h), dy = -cos(h);          /* 车头指向 */
    double px =  cos(h), py =  sin(h);          /* 右手 */

    int tipx = (int)llround(x + dx * 17), tipy = (int)llround(y + dy * 17);
    int b1x  = (int)llround(x - dx * 9 + px * 9), b1y = (int)llround(y - dy * 9 + py * 9);
    int b2x  = (int)llround(x - dx * 9 - px * 9), b2y = (int)llround(y - dy * 9 - py * 9);

    /* 白边：三个顶点沿"背离重心"的方向外扩，比逐边算法简单且不会自交 */
    int gcx = (tipx + b1x + b2x) / 3, gcy = (tipy + b1y + b2y) / 3;
    int wtx = (int)llround(tipx + (tipx - gcx) * 0.28);
    int wty = (int)llround(tipy + (tipy - gcy) * 0.28);
    int w1x = (int)llround(b1x + (b1x - gcx) * 0.28);
    int w1y = (int)llround(b1y + (b1y - gcy) * 0.28);
    int w2x = (int)llround(b2x + (b2x - gcx) * 0.28);
    int w2y = (int)llround(b2y + (b2y - gcy) * 0.28);

    fill_triangle(rs, wtx, wty, w1x, w1y, w2x, w2y, raster_rgb(rs, 255, 255, 255));
    fill_triangle(rs, tipx, tipy, b1x, b1y, b2x, b2y, raster_rgb(rs, 20, 90, 210));
}
```

无图区提示横幅 `paint_nomap_banner` 及其"只淡底板、前景保持清晰"的浮层助手：

```c
static uint8_t g_ovl_a = 255;
static uint8_t am(uint8_t a) { return (uint8_t)(((unsigned)a * g_ovl_a + 127u) / 255u); }
/* 前景（文字 / 图标）：0 ⇒ 隐藏；否则原样不透明。 */
static uint8_t fg(uint8_t a) { return (g_ovl_a == 0) ? 0 : a; }
static void nfill(Raster *rs, int x, int y, int w, int h,
                  uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    raster_fill_rect(rs, x, y, w, h, r, g, b, am(a));
}
static void nrect(Raster *rs, int x, int y, int w, int h,
                  uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    raster_rect(rs, x, y, w, h, r, g, b, am(a));
}
static void ntext(Font *f, Raster *rs, int x, int y, const char *t,
                  uint8_t r, uint8_t g, uint8_t b, int shadow)
{
    font_draw_a(f, rs, x, y, t, r, g, b, shadow, fg(255));
}
```

```c
void paint_nomap_banner(Raster *rs, const MapView *v, Font *f, int line_h,
                        int cov_state, const CovPlace *pl, uint8_t ovl_a)
{
    if (!rs || !v) return;
    if (cov_state != COV_NONE && cov_state != COV_BBOX_ONLY) return;
    g_ovl_a = ovl_a;   /* 顶部无图提示条整体半透明 */

    /* 四行：状态 / 这是哪儿 / 该下哪块 / 怎么装。
     * 文案与 coverage_hint() 同源（都读 CovPlace），避免两处说法漂移；横幅这里
     * 只是压短——C 核心自绘 + 离屏审计都走这条路径。 */
    const char *country  = (pl && pl->country[0])  ? pl->country  : "";
    const char *prov     = (pl && pl->prov[0])     ? pl->prov     : "";
    const char *city     = (pl && pl->city[0])     ? pl->city     : "";
    const char *district = (pl && pl->district[0]) ? pl->district : "";

    char l0[160], l1[256], l2[192], l3[192];

    snprintf(l0, sizeof(l0), "%s",
             (cov_state == COV_NONE)
                 ? "该区域没有地图数据"
                 : "附近没有路网数据（水域 / 尚未采集的区域）");

    /* "这是哪儿"：查到几层报几层；在已加载省之外时补上离最近省有多远 */
    if (pl && pl->nearest_km > 1.0) {
        snprintf(l1, sizeof(l1), "位置：%s%s%s，距最近的省（%s）约 %.0f km",
                 country, *country ? " · " : "", "已加载范围之外",
                 *prov ? prov : "—", pl->nearest_km);
    } else if (*country || *prov || *city || *district) {
        snprintf(l1, sizeof(l1), "位置：%s%s%s%s%s%s%s",
                 country,  *country  ? " · " : "",
                 prov,     *prov     ? " · " : "",
                 city,     *city     ? " · " : "",
                 district);
    } else {
        snprintf(l1, sizeof(l1), "位置：无法判断（未加载任何省份数据）");
    }

    if (pl && pl->dl_label[0])
        snprintf(l2, sizeof(l2), "建议下载：%s", pl->dl_label);
    else
        snprintf(l2, sizeof(l2), "建议下载：在『地图 → 下载全球地图』里按国家/省份选择");

    /* ★ 不写"按 X 键"：D 已被昼夜切换占用、Ctrl+D 是加收藏，横幅不该引导用户
     *   去按一个会触发别的事的键。统一指向菜单项（Qt 前端里那是真入口）。 */
    snprintf(l3, sizeof(l3), "『地图 → 下载全球地图』可直接定位到该地区");

    const char *lines[4] = { l0, l1, l2, l3 };
    int n = 4;

    /* ★ 行距与高度都按 paint_nav_banner 的同一套口径算（那里是被视觉验收过的）：
     *   基线从 line_h 起（不是从 7 起，否则首行会顶到边框外），
     *   每行步进 line_h + 6（不是 +4，否则中文行会互相压住）。
     *   历史坑：初版照抄了旧的 3 行版 `y+7 + i*(line_h+4)`，4 行时行距不够、
     *   首行还超出底板 —— 截图里能直接看到文字压在边框上。 */
    int wmax = 0;
    if (f) for (int i = 0; i < n; i++) {
        int tw = font_text_w(f, lines[i]);
        if (tw > wmax) wmax = tw;
    } else {
        wmax = 520;
    }
    int row_step = line_h + 6;
    int bw = wmax + 24;
    if (bw > v->w - 40) bw = v->w - 40;
    if (bw < 200) bw = 200;
    /* ★ 首行基线的算法：font_draw_a 的 y 是**基线**，字形往上长一个字高。
     *   line_h 传进来是 font_size + 8 ⇒ 字高 ≈ line_h - 8。
     *   所以基线至少要 by + (line_h - 8) 才不让首行顶出底板；
     *   取 by + line_h 再多留几像素余量。实测 by+10 会把首行裁掉上半截。 */
    int top_pad = line_h;                  /* 首行基线相对底板顶的偏移 */
    int bh = top_pad + (n - 1) * row_step + 12;
    int bx = (v->w - bw) / 2;
    int by = 12;

    nfill(rs, bx, by, bw, bh, 48, 34, 8, 232);
    nrect(rs, bx, by, bw, bh, 255, 186, 64, 255);

    if (!f) return;
    for (int i = 0; i < n; i++) {
        int ly = by + top_pad + i * row_step;   /* 基线 */
        if (i == 0) ntext(f, rs, bx + 12, ly, lines[0], 255, 208, 96, 0);
        else        ntext(f, rs, bx + 12, ly, lines[i], 238, 240, 245, 0);
    }
}
```

### 4. `audit` —— 像素计数 + 断言（被 `offscreen_shot` 复用）（`audit.c:79-217`）

```c
/* 在 C 核心地图缓冲上做全套像素计数 + 断言 + 日志打印。
 * 返回 0=全部 ASSERT-OK；1=有 ASSERT-FAIL。 */
int app_offscreen_audit(const App *a, const uint8_t *buf, int w, int h, int stride,
                        const PaintStats *st, const AuditCtx *c)
{
    /* GPX 可见性判定（用 a->view 投影）。
     * 软件渲染不混色，画上去就是原值 ⇒ 可以精确数像素。
     * ★ 判据与"有几条**看得见**的轨迹"挂钩，而不是"大于 0"：轨迹整段落在
     *   视口外时画面本来就该没有轨迹像素（合法情况，不能算失败）；
     *   反过来，看得见却一个像素都没有 ⇒ 渲染层真的坏了。 */
    int gpx_trk_drawn = 0, gpx_trk_visible = 0, gpx_fav_visible = 0;
    for (int i = 0; i < a->gpx.n_trk; i++) {
        const GpxTrack *t = &a->gpx.trk[i];
        if (t->n < 2) continue;                 /* <2 点画不出线段 */
        gpx_trk_drawn++;
        /* 与 paint_tracks 的裁剪口径一致：任一端点落进"视口 + 64 margin"
         * 就算可见（整段都在外面才会被跳过）。 */
        for (uint32_t k = 0; k + 1 < t->n; k++) {
            int x0, y0, x1, y1;
            mv_project(&a->view, t->lat_e7[k] / 1e7, t->lon_e7[k] / 1e7, &x0, &y0);
            mv_project(&a->view, t->lat_e7[k + 1] / 1e7, t->lon_e7[k + 1] / 1e7, &x1, &y1);
            if (!((x0 < -64 && x1 < -64) || (x0 > w + 64 && x1 > w + 64) ||
                  (y0 < -64 && y1 < -64) || (y0 > h + 64 && y1 > h + 64))) {
                gpx_trk_visible++;
                break;
            }
        }
    }
    for (int i = 0; i < a->gpx.n_wpt; i++) {
        /* 与 paint_favorites 自己的裁剪口径一致（视口外 16px 不计） */
        int x, y;
        mv_project(&a->view, a->gpx.wpt[i].lat, a->gpx.wpt[i].lon, &x, &y);
        if (x >= -16 && y >= -16 && x <= a->view.w + 16 && y <= a->view.h + 16)
            gpx_fav_visible++;
    }

    int fail = 0;
    /* 无地图区域时"画不出路"是预期结果，不能再当失败；
       这时该断言的是"提示横幅画出来了没有"——否则会出现
       "既没地图、也没提示"这种最差情况却显示 ASSERT-OK。
       横幅边框是琥珀色 (255,186,64)，数它的像素即可。 */
    char cov_prov[64];
    double flat, flon;
    app_focus_point(a, &flat, &flon);      /* 与界面上的提示用同一个点 */
    int cov = mapset_coverage(&a->ms, flat, flon, 3000,
                              cov_prov, sizeof(cov_prov));
    printf("coverage       %d  省=%s\n", cov, cov_prov[0] ? cov_prov : "-");
    if (cov == COV_HAS_ROAD) {
        if (st->drawn == 0) {
            printf("ASSERT-FAIL    一帧都没画出来（视口里没有路？）\n");
            fail = 1;
        }
        long bp = count_overlay_px(buf, stride, w, h, 48, 34, 8, 232,
                                       app_ovl_a(a, OVL_NOMAP));
        /* 阈值 2000：横幅改 75% 半透明后判的是"混合后近色（容差 8）"，会撞上
         * 密集路网/注记的深色像素 —— 实测 8 省 z13 假阳性 222~525 px（判 200
         * 全假红，截图目视确认根本没有横幅、cov=COV_HAS_ROAD）；真横幅
         * r_nomap 实测 10341 px。2000 与两侧各隔 4 倍以上。 */
        if (bp > 2000) {
            printf("ASSERT-FAIL    有地图却画出了无地图提示横幅（%ld px）\n", bp);
            fail = 1;
        }
    } else {
        long bp = count_overlay_px(buf, stride, w, h, 48, 34, 8, 232,
                                       app_ovl_a(a, OVL_NOMAP));
        printf("nomap_banner_px %ld\n", bp);
        if (bp <= 200) {
            printf("ASSERT-FAIL    无地图区域却没有提示横幅（用户会以为程序坏了）\n");
            fail = 1;
        }
    }

    if (c->have_search) {
        /* 输入框是 (250,250,250) 的白底：框没画出来时会是 0。
           几何算对了但忘了调 paint_search_box 的话，只有这条能抓住。 */
        /* 数边框 (110,170,255) 而不是白底 (250,250,250)：
         * 白底混完是 249，离背景色 244 太近，容差一放宽就把背景也算进来了。 */
        long boxpix = count_overlay_px(buf, stride, w, h, 20, 28, 44, 236,
                                       app_ovl_a(a, OVL_SEARCH));
        printf("search_box_px  %ld\n", boxpix);
        if (boxpix <= 200) { printf("ASSERT-FAIL    画面里没有搜索框\n"); fail = 1; }
    }

    if (c->have_route) {
        long npix = count_color_px(buf, stride, w, h,
                                   NAV_ROUTE_R, NAV_ROUTE_G, NAV_ROUTE_B);
        printf("route_pixels   %ld\n", npix);
        /* 路线画没画上去，只能靠像素数断言：几何算对了但投影/裁剪写错时，
           里程打印全对、画面上却一条线都没有（这类"数据对、画面错"的回归
           用眼睛最难发现）。 */
        if (npix <= 0) { printf("ASSERT-FAIL    画面里没有路线像素\n"); fail = 1; }        if (c->have_sim) {
            long dpix = count_color_px(buf, stride, w, h, 130, 138, 150);
            printf("done_pixels    %ld（已走过那段的灰色像素）\n", dpix);
            double prog = navsim_progress(&a->sim);
            /* 走过一段之后画面上必须同时有灰和蓝：全是蓝说明"已走过"没画，
               全是灰说明切分点写反了。 */
            if (prog > 0.05 && prog < 0.95) {
                if (dpix <= 0) { printf("ASSERT-FAIL    已走过那段没画成灰色\n"); fail = 1; }
                if (npix <= 0) { printf("ASSERT-FAIL    未走那段没有蓝色像素\n"); fail = 1; }
            }
            if (c->deviate_m > 60.0 && !navsim_offroute(&a->sim, 0)) {
                printf("ASSERT-FAIL    注入 %.0f m 偏移却没判偏航（实测 %.1f m）\n",
                       c->deviate_m, a->sim.off_m);
                fail = 1;
            }
        }
        double tol = a->rr.total_m * 0.001 + 1.0;
        if (a->man.n < 2 || fabs(a->man.sum_m - a->rr.total_m) > tol) {
            printf("ASSERT-FAIL    指令里程和 %.1f m != 总里程 %.1f m\n",
                   a->man.sum_m, a->rr.total_m);
            fail = 1;
        }
    }

    /* GPX 轨迹色（调色板四色轮换，见 map/gpx.c 的 kPalette）+ 收藏金。 */
    if (c->n_gpx_in > 0) {
        long trk_px = 0;
        static const int pal[][3] = {
            { 232, 62, 158 }, { 0, 178, 178 }, { 150, 92, 220 }, { 110, 190, 60 }
        };
        for (int k = 0; k < 4; k++)
            trk_px += count_color_px(buf, stride, w, h, pal[k][0], pal[k][1], pal[k][2]);
        long fav_px = count_color_px(buf, stride, w, h, 245, 190, 40);   /* 收藏金芯 */
        printf("gpx_trk_px     %ld（%d 条可画 / %d 条可见）\n",
               trk_px, gpx_trk_drawn, gpx_trk_visible);
        printf("gpx_fav_px     %ld（%d 个可见收藏）\n", fav_px, gpx_fav_visible);
        if (gpx_trk_visible > 0 && trk_px <= 0) {
            printf("ASSERT-FAIL    %d 条轨迹在视口内，画面里却没有轨迹像素\n",
                   gpx_trk_visible);
            fail = 1;
        }
        if (gpx_fav_visible > 0 && fav_px <= 0) {
            printf("ASSERT-FAIL    %d 个收藏在视口内，画面里却没有金色像素\n",
                   gpx_fav_visible);
            fail = 1;
        }
    }
    if (fail) return 1;
    printf("ASSERT-OK\n");
    return 0;
}
```

> `audit.c` 里的 `count_color_px` / `count_near_color_px` / `count_overlay_px` 是纯逐字节比色（`p[2]=R, p[1]=G, p[0]=B`，忽略 alpha），与 `QImage::Format_RGB32` 同源。这些辅助函数已完整存在于 `audit.c:25-75`，为控篇幅此处不再重列，可在 `src/ui/audit.c:25-75` 查阅。

### 5. `chrome` —— 自绘界面层（含无图卡片）（`chrome.c`）

`paint_chrome` 依次画 11 类元素；`draw_nomap` 是"图集完全没加载"时的居中提示卡片：

```c
static void draw_nomap(Raster *rs, App *a, const CLay *L)
{
    ovl_begin(a, OVL_NOMAP);
    (void)L;
    if (a->ms.n != 0) return;     /* 仅图集完全没加载时显示 */
    int w = 400, h = 116, x = (L->w - w)/2, y = (L->h - h)/2;
    rfill(rs, x, y, w, h, 252, 253, 255, 250);
    rstroke(rs, x, y, w, h, 230, 120, 90, 255);
    if (!a->font) return;
    rtext(a->font, rs, x + 20, y + 16, "未加载地图数据", 200, 70, 50, 0);
    /* 图集为空 ⇒ 没有任何省 bbox，反查无从谈起（coverage_locate 会返回空）。
     * 所以这里只能给"去哪儿下"的通用指引；具体到某省的定位由浏览到无图区时
     * 的横幅 / Qt 弹窗负责（那条路径才有坐标可反查）。 */
    rtext(a->font, rs, x + 20, y + 44,
              "打开『地图 → 下载全球地图』选国家/省份", 90, 96, 110, 0);
    rtext(a->font, rs, x + 20, y + 66,
              "下载后会自动转换并加载，无需手工跑脚本", 90, 96, 110, 0);
    rtext(a->font, rs, x + 20, y + 88,
              "也可以把 *.osm.pbf 放进 sources/ 让它自愈重建", 90, 96, 110, 0);
}
```

```c
void paint_chrome(Raster *rs, App *a)
{
    if (!rs) return;
    CLay L; layout(a, &L);

    draw_titlebar(rs, a, &L);
    draw_topbar(rs, a, &L);
    draw_rcol(rs, a, &L);
    draw_navtop(rs, a, &L);
    draw_navbottom(rs, a, &L);
    draw_steps(rs, a, &L);
    draw_alts(rs, a, &L);
    draw_menu(rs, a, &L);
    draw_settings_panel(rs, a);
    draw_tf_panel(rs, a);
    draw_nomap(rs, a, &L);
    draw_scale(rs, a, &L);
    draw_compass(rs, a, &L);
}
```

> 注意：`paint_chrome` 内部各 `draw_*` 函数的具体实现（标题栏/顶栏/右栏/菜单等）较长，为控篇幅此处只列 `paint_chrome` 主流程与"无图区提示"最相关的 `draw_nomap` 完整体；其余绘制函数可在 `src/ui/chrome.c:404-596`（`draw_scale`/`draw_compass`/`draw_tf_panel` 等）查阅。`chrome_hit`/`chrome_act` 命中测试与动作派发同理，签名见 `chrome.h:56-60`。

## 四、与其他功能之间的联系

- **上游**：第 8 课 `Raster`/`raster_*`（绘制媒介）+ `PaintStats`（回归判据源，如 `areas_dotted`）；第 10 课搜索结果经 `a->su` 被 `paint_search_box`/`paint_search_pin` 消费。
- **下游 · 第 13 课（Qt 前端）**：`app_render_to_buf` 是 Qt `MapCanvas` 与第 14 课 `offscreen_shot_main` 共用的唯一渲染入口。`qt_ui=1` 时 `paint_frame` 跳过 C 自绘 chrome/搜索框/面板，改由真 Qt 控件承担。
- **下游 · 第 14 课（架构门禁）**：`PaintStats`/`areas_dotted`/路线像素断言经 `app_offscreen_audit` 守像素等价；`offscreen_shot` 复用 `audit` 模块做验收。

## 五、演示、练习与验收

- **演示**：离屏出一张帧图并跑审计（Qt 前端与离屏共用 `app_render_to_buf`）：
  `./build_rel/navmap_qt --shot-offscreen --graph=products/graph/beijing --lat=39.9 --lon=116.4 --zoom=11 --w=1280 --h=800 --out=frame.bmp`
  看 stdout 的分阶段耗时 `ms_bg/ms_areas/ms_roads/ms_lines/ms_labels`，以及 `coverage` / `route_pixels` / `nomap_banner_px` / `ASSERT-OK`。
- **练习**：在 `paint_frame` 里新加一个 `TICK(ms_mystage)`，包住某段代码，看耗时出现在哪（复用既有的 `TICK` 宏）。
- **练习**：把一条可见路线画进无图区（或反之），跑 `app_offscreen_audit`，观察"有地图却画出无图横幅"或"无图区却没提示横幅"的 `ASSERT-FAIL` 被抓出。
- **验收**：能口述一帧的 9 步渲染顺序，并说明"为什么 Qt 前端和离屏共用 `app_render_to_buf`"——因为像素绑定 + `paint_frame` 编排是两套前端唯一的共同点，分开写会两处漂移、破坏像素等价。
