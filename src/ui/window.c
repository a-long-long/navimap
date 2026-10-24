#include "ui/window.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "render/paint.h"
#include "render/navdraw.h"
#include "render/chrome.h"
#include "map/binfmt.h"
#include "map/map_view.h"
#include "utils/geo.h"
#include "utils/plat.h"

/* ---- 模拟导航 ----
 * 倍速档：为什么要分档而不是随便填——软件渲染一帧几百毫秒，倍速太大时
 * 一帧就跨过好几个路口，"转弯提示"根本来不及看；1x 又慢得没法验收。
 * 10x 起步，长途（几小时）再往上加。 */
static const int SIM_SPEEDS[] = { 1, 2, 5, 10, 30, 60, 180 };
#define N_SIM_SPEEDS ((int)(sizeof(SIM_SPEEDS) / sizeof(SIM_SPEEDS[0])))
#define SIM_SPEED_DEFAULT 3          /* 10x */

static int sim_speed_of(const App *a)
{
    int i = a->sim_speed_idx;
    if (i < 0) i = 0;
    if (i >= N_SIM_SPEEDS) i = N_SIM_SPEEDS - 1;
    return SIM_SPEEDS[i];
}

/* 指令条大字号 = 标注字号 ×2，上下限兜住（8pt 时 16pt、32pt 时 64pt） */
static int app_big_size(int base)
{
    int s = base * 2;
    if (s < 16) s = 16;
    if (s > 64) s = 64;
    return s;
}

int app_init(App *a, const char *graph_dir, int w, int h,
             const char *font_path, int font_size, const AutoBuildOpt *ab)
{
    memset(a, 0, sizeof(*a));
    a->max_class = 4;
    a->need_redraw = 1;
    a->show_panel = 1;
    a->sim_speed_idx = SIM_SPEED_DEFAULT;
    a->sim_follow = 1;
    /* 互斥量用平台抽象层（不直接碰 pthread / CRITICAL_SECTION）。
       app_init 早于 app_run，但 nm_mutex 不依赖任何窗口系统，这里直接建。 */
    a->nav_mu = nm_mutex_create();     /* 算路线程与主线程之间的那点状态 */
    if (!a->nav_mu) fprintf(stderr, "提示: 建不了互斥量，导航功能不可用\n");

    /* 图集加载。graph_dir 既可以是单个省目录（products/graph/beijing），
       也可以是父目录（products/graph，自动扫描所有含 manifest 的省）。
       加载端校验（nmbin_check）在 mapset 里做：版本不符/文件自洽性不过
       的省会被跳过并警告，不再静默画错图。
       加载前先自愈一次：源 pbf 还在、产物没了/坏了就当场重建，判据与
       scripts/sync_maps.sh 完全一致（见 map/autobuild.h）。 */
    if (mapset_load_repair(&a->ms, graph_dir, ab, NULL) != 0) return -1;
    mapset_totals(&a->ms, &a->total_nodes, &a->total_edges);

    /* 字体可选。优先级：命令行 --font > 环境变量 NAVIMAP_FONT > 默认宋体候选链。
       "none"/空串 表示显式关闭文字标注（纯图形模式）。 */
    if (!font_path) font_path = getenv("NAVMAP_FONT");
    int want_font = !(font_path && (font_path[0] == '\0' ||
                                    strcmp(font_path, "none") == 0));
    a->font_size = font_size > 0 ? font_size : 14;

    if (want_font) {
        if (font_path && *font_path) {
            a->font = font_open(font_path, a->font_size);
            if (a->font)
                snprintf(a->font_used, sizeof(a->font_used), "%s", font_path);
            else
                fprintf(stderr, "提示: 字体 %s 打不开，退回默认字体\n", font_path);
        }
        if (!a->font) {
            a->font = font_open_default(a->font_size);
            if (a->font) {
                const char *dp = font_default_path();
                snprintf(a->font_used, sizeof(a->font_used), "%s (默认)",
                         dp ? dp : "?");
            }
        }
        if (!a->font)
            fprintf(stderr, "提示: 没有任何可用字体，本帧不画文字标注\n");
    } else {
        snprintf(a->font_used, sizeof(a->font_used), "(已关闭)");
    }
    /* 记下实际生效的字体文件：运行期改字号要按同一路径重开（字号是 TTF_Font
       的属性，改它必须重建 Font，字形纹理也全部作废）。 */
    if (a->font && a->font_used[0] && strcmp(a->font_used, "(已关闭)") != 0) {
        /* font_used 可能带 " (默认)" 后缀，去掉再存 */
        snprintf(a->font_path, sizeof(a->font_path), "%s", a->font_used);
        char *sp = strstr(a->font_path, " (默认)");
        if (sp) *sp = '\0';
    } else {
        a->font_path[0] = '\0';
    }
    /* 指令条的大字号（字号 ×2）。单独开一份 Font：TTF_Font 的字号是创建时
       定的，没有"临时放大"这回事。没有字体时保持 NULL，指令条退化为小字。 */
    if (a->font_path[0]) a->font_big = font_open(a->font_path, app_big_size(a->font_size));

    const char *places = getenv("NAVMAP_PLACES");
    if (!places || !*places) places = "products/places_auto.db";
    if (label_cache_init(&a->places, places) == 0) a->has_places = 1;
    /* 搜索库：同一个文件，但**另开一个连接**——LabelCache 可能正拿着语句在
       遍历，共用连接会互相踩游标（sqlite3 连接不是并发安全的）。
       打开失败只提示，不致命（只是搜不了）。 */
    a->sdb = search_open(places);
    if (!a->sdb)
        fprintf(stderr, "提示: 搜索库 %s 打不开，搜索不可用\n", places);

    /* 初始中心：单省用省里第一个节点（沿用旧行为）；多省用全省并集
       bbox 的中心，并把缩放拉远到能一眼看到整组省 */
    double clat = 39.9, clon = 116.4, czoom = 12.0;
    if (a->ms.n == 1 && a->ms.srcs[0].nodes.hdr.rec_count) {
        const uint8_t *r = a->ms.srcs[0].nodes.data;
        clat = nm_get_i32(r + NF_LAT) / 1e7;
        clon = nm_get_i32(r + NF_LON) / 1e7;
    } else if (a->ms.n > 1) {
        /* ★ 用 64 位算：两个 e7 经度相加（10.5 亿 + 12.6 亿）会溢出 int32，
           中心被算到西经去（实测 -98.9°，图上自然什么都没有） */
        clat = ((int64_t)a->ms.bbox[0] + a->ms.bbox[2]) / 2 / 1e7;
        clon = ((int64_t)a->ms.bbox[1] + a->ms.bbox[3]) / 2 / 1e7;
        czoom = 9.0;
    }
    mv_init(&a->view, clat, clon, czoom, w, h);
    dedup_init(&a->dd);
    return 0;
}

void app_shutdown(App *a)
{
    /* 后台线程可能还在跑（用户点了算路又直接 Esc）。Router 不是线程安全的，
       必须等它退出再销毁：nav_busy 由 nav_mu 保护，这里自旋等一下。
       退出路径上最多等几秒，不当成错误。 */
    if (a->nav_mu) {
        nm_mutex_lock(a->nav_mu);
        a->nav_stop = 1;                 /* 排队的请求不再开工 */
        nm_mutex_unlock(a->nav_mu);
        for (int i = 0; i < 3000; i++) { /* 最多等 30 秒：跨省长途 5 秒级 */
            nm_mutex_lock(a->nav_mu);
            int busy = a->nav_busy;
            nm_mutex_unlock(a->nav_mu);
            if (!busy) break;
            nm_sleep_ms(10);
        }
    }
    if (a->nav_done) { route_result_free(&a->nav_done->rr); free(a->nav_done); a->nav_done = NULL; }
    if (a->have_rr) route_result_free(&a->rr);
    maneuver_free(&a->man);
    router_destroy(a->router);
    if (a->nav_mu) { nm_mutex_destroy(a->nav_mu); a->nav_mu = NULL; }
    mapset_free(&a->ms);
    dedup_free(&a->dd);
    label_cache_free(&a->places);
    search_close(a->sdb);
    font_close(a->font_big);
    font_close(a->font);
}

#define FONT_SIZE_MIN 8
#define FONT_SIZE_MAX 32

int app_set_font_size(App *a, int size)
{
    if (size < FONT_SIZE_MIN) size = FONT_SIZE_MIN;
    if (size > FONT_SIZE_MAX) size = FONT_SIZE_MAX;
    if (!a->font_path[0]) {        /* 字体被 --font=none 关掉了 */
        a->font_size = size;       /* 记住，将来重新打开时生效 */
        return 0;
    }
    if (size == a->font_size && a->font) return size;

    font_close(a->font);
    a->font = NULL;
    /* 同一路径重开；路径为空（极端情况）就退回默认链 */
    a->font = a->font_path[0] ? font_open(a->font_path, size)
                              : font_open_default(size);
    if (!a->font) {
        fprintf(stderr, "提示: %d pt 字体打不开，保持原样\n", size);
        a->font = a->font_path[0] ? font_open(a->font_path, a->font_size)
                                  : font_open_default(a->font_size);
        return a->font ? a->font_size : 0;
    }
    a->font_size = size;
    snprintf(a->font_used, sizeof(a->font_used), "%s", a->font_path);
    /* 指令条的大字号跟着一起换，否则改完字号指令条还是旧大小 */
    font_close(a->font_big);
    a->font_big = font_open(a->font_path, app_big_size(size));
    return size;
}

/* ---------------------------------------------------------------------------
 * 导航（阶段 9）
 *
 * 算路放在后台线程：跨省长途几秒级，放主线程会把窗口卡成假死（Wayland 下
 * 连标题栏都不刷新，看着像崩了）。规则：
 *   - Router 只被后台线程碰。它不是线程安全的（反向 CSR 是按省惰性构建的），
 *     主线程只提交请求 + 收割结果，绝不直接调 router_*。
 *   - 结果用 NavResult 外壳整体搬：RouteResult 里的数组指针搬到 App 后就
 *     属于主线程，之后只 free 外壳，不能再 route_result_free。
 *   - 用户在算路期间又点了点：只记 pending，让当前这单跑完再开新的
 *     （中途打断要处理半建状态，不值得）。
 * ------------------------------------------------------------------------- */

static void nav_drop_route(App *a)
{
    if (a->have_rr) { route_result_free(&a->rr); a->have_rr = 0; }
    maneuver_free(&a->man);
    a->nav_msg[0] = '\0';
    /* ★ 模拟导航必须跟着停：NavSim 里存的是 &a->rr 的指针，路线一释放
       它就是悬空的，再 tick / 画指令条就是 use-after-free。
       谁想留着模拟状态（偏航重算），在调 nav_drop_route 之前把
       sim_on 记下来，拿到新路线后自己重新 navsim_start。 */
    a->sim_on = 0;
    a->sim_paused = 0;
}

/* Router 惰性创建：状态表一上来就 30MB，只有真要算路才建。 */
static int nav_ensure_router(App *a)
{
    if (a->router) return 0;
    /* heur_w 留 0：让 router_create 按"有没有 lm.bin"兜底（见 route.h ROUTE_HEUR_W_LM）。
       写死 1.4 会把兜底顶掉，有表时反而更贪心、路线更差。 */
    RouteOpts opt = { .mode_time = 1, .bidir = 1 };
    a->router = router_create(&a->ms, &opt);
    if (!a->router) {
        fprintf(stderr, "路由初始化失败\n");
        return -1;
    }
    return 0;
}

/* 吸附到最近的可路由节点，并回填吸附后的经纬度（画标记用真实路口位置，
   不然标记会浮在路网外，看着像"路线没接到点上"）。
   ★ 只负责**展示用**的吸附。真正算路走 router_route_ll —— 那个会在
     "最近节点悬在小连通块里"时自动换候选重试（PLAN.md §51.1）。 */
static void nav_snap_info(const MapSet *ms, RNode n,
                          double *slat, double *slon, int *prov)
{
    if (n.prov >= (uint32_t)ms->n) return;
    mapset_ensure_loaded((MapSet *)ms, (int)n.prov);   /* 该省可能已被 set_view 卸载 */
    const uint8_t *nr = ms->srcs[n.prov].nodes.data +
                        (uint64_t)n.node * NM_NODE_BYTES;
    if (slat) *slat = nm_get_i32(nr + NF_LAT) / 1e7;
    if (slon) *slon = nm_get_i32(nr + NF_LON) / 1e7;
    if (prov) *prov = (int)n.prov;
}

/* router_route_ll 的返回码 → NavResult.err 的口径（0 / -1 不连通 / -2 超限 / -3 无路） */
static int nav_err_of(int rc)
{
    if (rc == 0)  return 0;
    if (rc == -1) return -3;    /* 附近无路网 */
    if (rc == -2) return -1;    /* 不连通 */
    return -2;                  /* 超限 */
}

static void nav_worker(void *ud)
{
    App *a = (App *)ud;

    for (;;) {
        NavPick pa, pb;
        nm_mutex_lock(a->nav_mu);
        pa = a->req_a;
        pb = a->req_b;
        a->nav_pending = 0;
        nm_mutex_unlock(a->nav_mu);

        NavResult *res = calloc(1, sizeof(NavResult));
        if (!res) { fprintf(stderr, "导航: 内存不足\n"); break; }

        if (!a->router) {
            res->err = -3;
        } else {
            RNode f, t;
            double da = 0, db = 0;
            /* ★ 用 router_route_ll 而不是"吸附两次 + router_plan"：吸附可能落在
               与主干断开的小连通块里（PLAN.md §51.1），那样整条路线判"不连通"。
               它内部会用最近的 4 个候选重试，失败的判定是毫秒级，几乎不要钱。 */
            int rc = router_route_ll(a->router, pa.lat, pa.lon, pb.lat, pb.lon,
                                     &res->rr, &f, &t, &da, &db);
            res->err = nav_err_of(rc);
            if (rc != -1) {
                res->a_snap_m = da; res->b_snap_m = db;
                nav_snap_info(&a->ms, f, &res->a_lat, &res->a_lon, &res->a_prov);
                nav_snap_info(&a->ms, t, &res->b_lat, &res->b_lon, &res->b_prov);
            }
        }

        nm_mutex_lock(a->nav_mu);
        if (a->nav_done) { route_result_free(&a->nav_done->rr); free(a->nav_done); }
        a->nav_done = res;
        a->nav_done_flag = 1;
        int again = a->nav_pending && !a->nav_stop;
        if (!again) a->nav_busy = 0;
        nm_mutex_unlock(a->nav_mu);
        if (!again) break;
    }
}

static void nav_request(App *a)
{
    if (!a->pick_a.have || !a->pick_b.have) return;
    if (nav_ensure_router(a) != 0) {
        snprintf(a->nav_msg, sizeof(a->nav_msg), "路由初始化失败");
        return;
    }
    int start = 0;
    nm_mutex_lock(a->nav_mu);
    a->req_a = a->pick_a;
    a->req_b = a->pick_b;
    if (a->nav_busy) a->nav_pending = 1;    /* 手头这单跑完再算新的 */
    else { a->nav_busy = 1; start = 1; }
    nm_mutex_unlock(a->nav_mu);
    if (!start) return;

    a->nav_t0 = nm_now_ms();
    if (nm_thread_spawn(nav_worker, a) != 0) {
        nm_mutex_lock(a->nav_mu);
        a->nav_busy = 0;
        nm_mutex_unlock(a->nav_mu);
        snprintf(a->nav_msg, sizeof(a->nav_msg), "算路线程起不来");
        return;
    }                                          /* 结果走 nav_done，不等它 join */
}

void app_nav_poll(App *a)
{
    if (!a->nav_done_flag) return;

    NavResult *res = NULL;
    nm_mutex_lock(a->nav_mu);
    if (a->nav_done) { res = a->nav_done; a->nav_done = NULL; }
    a->nav_done_flag = 0;
    nm_mutex_unlock(a->nav_mu);
    if (!res) return;

    int was_sim = a->sim_on;            /* 偏航重算后要接着模拟 */

    if (res->err != -3) {
        a->pick_a.snapped = 1;
        a->pick_a.slat = res->a_lat;  a->pick_a.slon = res->a_lon;
        a->pick_a.snap_m = res->a_snap_m;
        a->pick_b.snapped = 1;
        a->pick_b.slat = res->b_lat;  a->pick_b.slon = res->b_lon;
        a->pick_b.snap_m = res->b_snap_m;
        if (res->a_prov >= 0 && res->a_prov < a->ms.n)
            snprintf(a->pick_a.prov, sizeof(a->pick_a.prov), "%s",
                     a->ms.srcs[res->a_prov].name);
        if (res->b_prov >= 0 && res->b_prov < a->ms.n)
            snprintf(a->pick_b.prov, sizeof(a->pick_b.prov), "%s",
                     a->ms.srcs[res->b_prov].name);
    }

    nav_drop_route(a);
    if (res->err == 0) {
        a->rr = res->rr;                    /* 数组指针整体搬家 */
        a->have_rr = 1;
        memset(&res->rr, 0, sizeof(res->rr));
        maneuver_build(&a->rr, &a->ms, &a->man);
        a->nav_msg[0] = '\0';               /* 面板顶行回到"全程 X 约 Y 分钟" */
        /* 模拟导航开着时，新路线（可能是偏航重算的结果）从头起步：
           重算的起点就是当前位置，所以进度归零是对的。 */
        if (was_sim) {
            navsim_start(&a->sim, &a->rr);
            a->sim.speed_mul = sim_speed_of(a);
            a->sim_on = 1;
            a->sim_paused = 0;
            a->sim_tick = nm_now_ms();
        }
        printf("路线  %.2f km  约 %.0f 分  %u 段  %d 条指令  展开 %llu  %llu ms\n",
               a->rr.total_m / 1000.0, a->rr.total_s / 60.0, a->rr.n - 1,
               a->man.n, (unsigned long long)a->rr.expanded,
               (unsigned long long)a->rr.elapsed_ms);
    } else {
        route_result_free(&res->rr);
        const char *m = res->err == -1 ? "起终点之间不连通"
                      : res->err == -2 ? "搜索规模超限（路太远）"
                                       : "起终点附近没有可通行的路";
        snprintf(a->nav_msg, sizeof(a->nav_msg), "%s", m);
    }
    free(res);
    a->need_redraw = 1;
}

void app_nav_click(App *a, double lat, double lon)
{
    if (!a->pick_a.have || a->pick_b.have) {
        /* 重开一头：清掉终点和旧路线 */
        NavPick empty;
        memset(&empty, 0, sizeof(empty));
        nav_drop_route(a);
        a->pick_a = empty;
        a->pick_b = empty;
        a->pick_a.have = 1;
        a->pick_a.lat = lat;
        a->pick_a.lon = lon;
        snprintf(a->nav_msg, sizeof(a->nav_msg), "起点已设，再点一下设终点");
    } else {
        a->pick_b.have = 1;
        a->pick_b.lat = lat;
        a->pick_b.lon = lon;
        snprintf(a->nav_msg, sizeof(a->nav_msg), "计算中…");
        nav_request(a);
    }
    a->need_redraw = 1;
}

void app_nav_clear(App *a)
{
    NavPick empty;
    memset(&empty, 0, sizeof(empty));
    nav_drop_route(a);
    a->pick_a = empty;
    a->pick_b = empty;
    a->need_redraw = 1;
}

void app_nav_fit(App *a)
{
    if (!a->have_rr || a->rr.n < 2) return;
    int32_t la0 = 2000000000, lo0 = 2000000000, la1 = -2000000000, lo1 = -2000000000;
    for (uint32_t i = 0; i < a->rr.n; i++) {
        if (a->rr.lat_e7[i] < la0) la0 = a->rr.lat_e7[i];
        if (a->rr.lat_e7[i] > la1) la1 = a->rr.lat_e7[i];
        if (a->rr.lon_e7[i] < lo0) lo0 = a->rr.lon_e7[i];
        if (a->rr.lon_e7[i] > lo1) lo1 = a->rr.lon_e7[i];
    }
    mv_fit_bbox(&a->view, la0 / 1e7, lo0 / 1e7, la1 / 1e7, lo1 / 1e7, 60.0);
    mv_clamp_zoom(&a->view, 3.0, 19.0);
    a->need_redraw = 1;
}

void app_nav_toggle_panel(App *a)
{
    a->show_panel = !a->show_panel;
    a->need_redraw = 1;
}

/* ---- 搜索（阶段 7）----
 * 查询是毫秒级（实测均值 1 ms），直接在主线程做，不另开线程。
 * 排序中心用当前视口中心：搜"中关村"时人想看的是**眼前**的中关村，
 * 不是山东那个同名村（库里真有，378 km 外）。 */

int app_search_query(App *a, const char *q)
{
    if (!a->sdb) return 0;
    memset(a->su.hits, 0, sizeof(a->su.hits));
    int n = 0;
    search_query(a->sdb, q, 1, a->view.lat, a->view.lon, 0, 0,
                 (int)(sizeof(a->su.hits) / sizeof(a->su.hits[0])),
                 a->su.hits,
                 (int)(sizeof(a->su.hits) / sizeof(a->su.hits[0])),
                 &n, &a->su.elapsed_ms);
    a->su.n_hits = n;
    if (a->su.sel >= n) a->su.sel = n > 0 ? n - 1 : 0;
    if (a->su.sel < 0) a->su.sel = 0;
    return n;
}

void app_search_open(App *a)
{
    if (!a->sdb) {
        snprintf(a->nav_msg, sizeof(a->nav_msg), "搜索库不可用");
        a->need_redraw = 1;
        return;
    }
    a->su.active = 1;
    a->su.editing[0] = '\0';
    /* 注：SDL 窗口版在 window_sdl.c 的事件处理里负责 SDL_StartTextInput，
       本函数是平台无关的；Qt 前端由 QLineEdit 自带 IME，无需额外动作。 */
    if (a->su.text[0]) app_search_query(a, a->su.text);
    a->need_redraw = 1;
}

void app_search_close(App *a)
{
    if (!a->su.active) return;
    a->su.active = 0;
    a->su.editing[0] = '\0';
    a->need_redraw = 1;
}

void app_search_editing(App *a, const char *utf8)
{
    if (!a->su.active) return;
    snprintf(a->su.editing, sizeof(a->su.editing), "%s", utf8 ? utf8 : "");
    a->need_redraw = 1;
}

void app_search_input(App *a, const char *utf8)
{
    if (!a->su.active || !utf8 || !*utf8) return;
    size_t have = strlen(a->su.text);
    size_t room = sizeof(a->su.text) - have - 1;
    if (room == 0) return;
    /* 追加到 UTF-8 字符边界：截断在多字节字中间会留下半个字 */
    size_t n = strlen(utf8);
    if (n > room) {
        n = room;
        while (n > 0 && ((unsigned char)utf8[n] & 0xC0) == 0x80) n--;
    }
    memcpy(a->su.text + have, utf8, n);
    a->su.text[have + n] = '\0';
    a->su.editing[0] = '\0';       /* 上屏了，组合串清掉 */
    a->su.sel = 0;
    app_search_query(a, a->su.text);
    a->need_redraw = 1;
}

void app_search_backspace(App *a)
{
    if (!a->su.active) return;
    size_t n = strlen(a->su.text);
    if (n == 0) { a->su.editing[0] = '\0'; a->need_redraw = 1; return; }
    /* 退一个 UTF-8 字符，不是退一个字节 */
    size_t i = n - 1;
    while (i > 0 && ((unsigned char)a->su.text[i] & 0xC0) == 0x80) i--;
    a->su.text[i] = '\0';
    a->su.sel = 0;
    app_search_query(a, a->su.text);
    a->need_redraw = 1;
}

void app_search_move(App *a, int dir)
{
    if (!a->su.active || a->su.n_hits <= 0) return;
    a->su.sel += dir;
    if (a->su.sel < 0) a->su.sel = 0;
    if (a->su.sel >= a->su.n_hits) a->su.sel = a->su.n_hits - 1;
    a->need_redraw = 1;
}

void app_search_commit(App *a)
{
    if (!a->su.active || a->su.n_hits <= 0) { app_search_close(a); return; }
    if (a->su.sel < 0 || a->su.sel >= a->su.n_hits) a->su.sel = 0;
    const SearchHit *h = &a->su.hits[a->su.sel];

    /* 定位：跳到该点并拉到街道级 */
    a->view.lat = h->lat;
    a->view.lon = h->lon;
    if (a->view.zoom < 14.0) a->view.zoom = 15.0;
    mv_clamp_zoom(&a->view, 3.0, 19.0);

    /* 第一次搜索 → 当起点；已有起点 → 当终点并立刻算路 */
    NavPick p;
    memset(&p, 0, sizeof(p));
    p.have = 1;
    p.lat = h->lat;
    p.lon = h->lon;
    snprintf(p.prov, sizeof(p.prov), "%s", "搜索结果");
    if (!a->pick_a.have) {
        a->pick_a = p;
        snprintf(a->nav_msg, sizeof(a->nav_msg), "起点：%s，再搜一个当终点",
                 h->name);
    } else {
        a->pick_b = p;
        snprintf(a->nav_msg, sizeof(a->nav_msg), "计算中…");
        nav_request(a);
    }
    app_search_close(a);
    a->need_redraw = 1;
}

/* ---- 模拟导航 ----
 * sim 只在主线程里推进：每帧按真实 dt 走一点，车头即定位点（模拟 GNSS）。
 * 唯一会"偏离路线"的情况是 app_nav_sim_deviate 主动注入，那之后走
 * nav_request 重算，新路线到了再 navsim_start 重新起步。 */

void app_nav_sim_toggle(App *a)
{
    if (!a->have_rr || a->rr.n < 2) {
        snprintf(a->nav_msg, sizeof(a->nav_msg), "先点起点和终点，算条路线再模拟");
        a->need_redraw = 1;
        return;
    }
    if (a->sim_on) {
        a->sim_on = 0;
        a->sim_paused = 0;
    } else {
        if (navsim_start(&a->sim, &a->rr) != 0) return;
        a->sim.speed_mul = sim_speed_of(a);
        a->sim_on = 1;
        a->sim_paused = 0;
        a->sim_follow = 1;
        a->sim_tick = nm_now_ms();
        a->nav_msg[0] = '\0';
        /* 导航视角：z<14 时路口看不清，转向提示无从判断对不对 */
        if (a->view.zoom < 14.0) a->view.zoom = 15.0;
        mv_clamp_zoom(&a->view, 3.0, 19.0);
    }
    a->need_redraw = 1;
}

void app_nav_sim_speed(App *a, int dir)
{
    a->sim_speed_idx += dir;
    if (a->sim_speed_idx < 0) a->sim_speed_idx = 0;
    if (a->sim_speed_idx >= N_SIM_SPEEDS) a->sim_speed_idx = N_SIM_SPEEDS - 1;
    a->sim.speed_mul = sim_speed_of(a);
    a->need_redraw = 1;
}

void app_nav_sim_follow(App *a)
{
    a->sim_follow = !a->sim_follow;
    a->need_redraw = 1;
}

/* 注入一次偏航：把定位点朝车头右手方向拉 300 m。
 * 真机上这是 GNSS 漂了 / 走错路口，这里用一个按键代替，顺便把"偏航 →
 * 自动重算"整条链路跑通（无头版在 navsim_cli 里断言，这里是给人看的）。 */
void app_nav_sim_deviate(App *a)
{
    if (!a->sim_on || !a->have_rr) return;
    double ola, olo;
    navsim_offset_point(a->sim.car_lat, a->sim.car_lon, a->sim.heading_deg,
                        300.0, &ola, &olo);
    navsim_locate(&a->sim, ola, olo);
    if (!navsim_offroute(&a->sim, 0)) {
        snprintf(a->nav_msg, sizeof(a->nav_msg), "注入偏航失败（只偏了 %.0f m）",
                 a->sim.off_m);
        a->need_redraw = 1;
        return;
    }
    /* 从当前位置重新规划：起点换成偏航点，终点不动 */
    memset(&a->pick_a, 0, sizeof(a->pick_a));
    a->pick_a.have = 1;
    a->pick_a.lat = ola;
    a->pick_a.lon = olo;
    a->sim_paused = 1;                 /* 新路线到了再走 */
    snprintf(a->nav_msg, sizeof(a->nav_msg), "已偏航 %.0f m，重新规划…",
             a->sim.off_m);
    nav_request(a);
    a->need_redraw = 1;
}

void app_nav_sim_tick(App *a)
{
    /* have_rr 一起判：正常情况下 sim_on 蕴含 have_rr（drop 时会一起关），
       这里再挡一道，悬空指针就进不了 navsim_advance。 */
    if (!a->sim_on || !a->have_rr || a->sim_paused) return;
    uint64_t now = nm_now_ms();
    double dt = (double)(now - a->sim_tick) / 1000.0;
    a->sim_tick = now;
    if (dt <= 0) return;
    /* 卡帧保护：软件渲染一帧几百毫秒，不夹住的话一次 advance 就跨过几个路口 */
    if (dt > 0.5) dt = 0.5;

    a->sim.speed_mul = sim_speed_of(a);
    int arrived = navsim_advance(&a->sim, dt);
    /* 车头就是定位点：模拟 GNSS 永远给出"我在路上" 的位置 */
    navsim_locate(&a->sim, a->sim.car_lat, a->sim.car_lon);
    if (arrived) {
        a->sim_paused = 1;
        snprintf(a->nav_msg, sizeof(a->nav_msg), "已到达目的地");
    }
    if (a->sim_follow) {
        a->view.lat = a->sim.car_lat;
        a->view.lon = a->sim.car_lon;
    }
    a->need_redraw = 1;
}

/* 同步算一次（无窗口、无主循环时用，见 map_shot --from/--to）。
   与后台线程走同一套吸附 + 结算逻辑，只是不另起线程。 */
int app_nav_plan_sync(App *a, double alat, double alon, double blat, double blon)
{
    if (nav_ensure_router(a) != 0) return -1;
    RNode f, t;
    NavResult res;
    memset(&res, 0, sizeof(res));
    /* 同上：走 router_route_ll，吸附撞上死节点会换候选重试（PLAN.md §51.1） */
    int rc = router_route_ll(a->router, alat, alon, blat, blon,
                             &res.rr, &f, &t, &res.a_snap_m, &res.b_snap_m);
    if (rc == -1) {
        snprintf(a->nav_msg, sizeof(a->nav_msg), "起终点附近没有可通行的路");
        return -1;
    }
    nav_snap_info(&a->ms, f, &res.a_lat, &res.a_lon, &res.a_prov);
    nav_snap_info(&a->ms, t, &res.b_lat, &res.b_lon, &res.b_prov);
    if (rc != 0) {
        route_result_free(&res.rr);
        snprintf(a->nav_msg, sizeof(a->nav_msg), "%s",
                 rc == -2 ? "起终点之间不连通（已试过多组吸附候选）" : "搜索规模超限");
        return -1;
    }
    a->pick_a.have = a->pick_b.have = 1;
    a->pick_a.snapped = a->pick_b.snapped = 1;
    a->pick_a.slat = res.a_lat; a->pick_a.slon = res.a_lon;
    a->pick_b.slat = res.b_lat; a->pick_b.slon = res.b_lon;
    a->pick_a.snap_m = res.a_snap_m; a->pick_b.snap_m = res.b_snap_m;
    a->pick_a.lat = alat; a->pick_a.lon = alon;
    a->pick_b.lat = blat; a->pick_b.lon = blon;
    nav_drop_route(a);
    a->rr = res.rr;
    a->have_rr = 1;
    maneuver_build(&a->rr, &a->ms, &a->man);
    a->nav_msg[0] = '\0';
    return 0;
}

/* 一帧的完整绘制顺序：背景 → 面 → 路 → 线要素 → 标注 → 路线 → 标记 → 面板 */
/* 视口（lat0 lon0 lat1 lon1）与省 bbox（e7）是否相交 */
static int bbox_hits(const int32_t *bb, const double *vb)
{
    return !(bb[0] * 1e-7 > vb[2] || bb[2] * 1e-7 < vb[0] ||
             bb[1] * 1e-7 > vb[3] || bb[3] * 1e-7 < vb[1]);
}

void app_focus_point(const App *a, double *lat, double *lon)
{
    if (a->sim_on) { *lat = a->sim.car_lat; *lon = a->sim.car_lon; }
    else           { *lat = a->view.lat;    *lon = a->view.lon; }
}

/* 关注点有没有地图数据？
 * 移动不足约 450 m 就不重算：细判要查瓦片索引，每帧做一次不划算。 */
static void app_update_coverage(App *a)
{
    double lat, lon;
    app_focus_point(a, &lat, &lon);
    if (a->cov_checked &&
        fabs(lat - a->cov_lat) < 0.004 && fabs(lon - a->cov_lon) < 0.004)
        return;
    a->cov_checked = 1;
    a->cov_lat = lat;
    a->cov_lon = lon;

    char prov[64];
    a->cov_state = mapset_coverage(&a->ms, lat, lon, 3000, prov, sizeof(prov));
    if (a->cov_state == COV_NONE || a->cov_state == COV_BBOX_ONLY) {
        coverage_hint(&a->ms, lat, lon, a->cov_state, prov,
                      a->cov_hint, sizeof(a->cov_hint));
        /* 完整指引（含下载来源）打一次就够，横幅上只放浓缩版 */
        fprintf(stderr, "\n[无地图区域] %s\n", a->cov_hint);
    } else {
        a->cov_hint[0] = '\0';
    }
}



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
    TICK(ms_lines);
    if (a->font) {
        /* 地名库是全局一份（places.db 跨省通用），只需查一次 */
        paint_place_labels(rs, &a->view, &a->places, a->font, st);
        paint_road_labels(rs, &a->view, a->font, st);
    }
    TICK(ms_labels);
#undef TICK

    /* 路线叠加在最上面：路面已经画完，压在路网上才看得清走向。
       模拟导航开着时按车头位置切成"已走过（灰）/ 未走（蓝）"两截。 */
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

    /* 夜间蒙版：只压暗地图，不压下面要画的面板/文字/chrome */
    if (a->day_night) paint_night_tint(rs, a->view.w, a->view.h);

    /* Qt 前端接管后这些自绘 UI 全部由真控件承担，不再画 */
    if (a->show_panel && a->font && !a->qt_ui)
        paint_nav_panel(rs, a->have_rr ? &a->rr : NULL,
                        a->man.n ? &a->man : NULL, a->font,
                        a->font_size + 8, 12, 12, 6,
                        a->nav_msg[0] ? a->nav_msg : NULL);

    /* 底部指令条：模拟导航时才出现（静止看路线时用左上角面板就够了） */
    if (a->sim_on && a->font && !a->qt_ui) {
        static char b_turn[64], b_dist[64], b_road[96], b_rem[64], b_eta[64];
        NavBanner b;
        memset(&b, 0, sizeof(b));
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
        paint_nav_banner(rs, &a->view, a->font, a->font_big,
                         a->font_size + 8, app_big_size(a->font_size) + 8, &b);
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
                         a->font_size + 8, app_big_size(a->font_size) + 8, &sb);
        }
    }

    /* 无地图区域提示画在最上层：它解释的是"你为什么看不到东西"，
       被面板或按钮压住就失去意义了。 */
    app_update_coverage(a);
    if (a->cov_state == COV_NONE || a->cov_state == COV_BBOX_ONLY)
        paint_nomap_banner(rs, &a->view, a->font, a->font_size + 8, a->cov_state);

    /* nav-desktop 风格 chrome（自绘标题栏 / 顶栏 / 右栏 / 导航栏 /
       步骤面板 / 主菜单 / 无图卡片 / 比例尺 / 指北针 / 昼夜）叠在最上层；
       Qt 前端时由真控件承担，跳过。 */
    if (!a->qt_ui)
        paint_chrome(rs, a);
}

int app_render_to_buf(App *a, void *px, int w, int h, size_t stride,
                     uint32_t rmask, uint32_t gmask, uint32_t bmask,
                     uint32_t amask, PaintStats *st)
{
    if (w <= 0 || h <= 0 || !px) return -1;
    a->view.w = w;
    a->view.h = h;

    /* 绑定到调用方持有的 32bpp 像素缓冲：窗口版在 window_sdl.c 的离屏
     * surface 上、Qt 前端在自己的 QImage 缓冲上、map_shot 在 SDL_Surface 上。
     * 字形走 stb_truetype 的 CPU 缓存（见 font.c），与渲染目标生命周期无关，
     * 这里不再每帧 drop 纹理。 */
    Raster rs;
    if (raster_bind_mem(&rs, px, w, h, stride, rmask, gmask, bmask, amask) != 0)
        return -1;
    paint_frame(&rs, a, st);
    return 0;
}

/* SDL 离屏画布 + 主循环见 window_sdl.c（仅 navmap / map_shot 链 SDL）。 */

void update_status(App *a, double frame_ms)
{
    /* 标题栏只留 60 字符给导航信息：nav_msg 最长 191，全放进来会把
       前面的坐标挤掉（gcc 也会报截断）。状态栏不是给人读长句的地方。 */
    char nav[80];
    nav[0] = '\0';
    if (a->sim_on) {
        snprintf(nav, sizeof(nav), "  模拟 %dx 剩 %.1fkm/%.0f分",
                 sim_speed_of(a), navsim_remaining_m(&a->sim) / 1000.0,
                 navsim_remaining_s(&a->sim) / 60.0);
    } else if (a->have_rr) {
        snprintf(nav, sizeof(nav), "  路线 %.1fkm/%.0f分",
                 a->rr.total_m / 1000.0, a->rr.total_s / 60.0);
    } else if (a->nav_msg[0]) {
        snprintf(nav, sizeof(nav), "  %.60s", a->nav_msg);
    }
    snprintf(a->status, sizeof(a->status),
             "lat %.5f lon %.5f zoom %.2f  LOD<=%d  字号 %d  %d省/%llu边  帧 %.0f ms%s",
             a->view.lat, a->view.lon, a->view.zoom, a->max_class,
             a->font ? a->font_size : 0,
             a->ms.n, (unsigned long long)a->total_edges, frame_ms, nav);
}
