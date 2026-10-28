#include "ui/window.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <dirent.h>             /* 目录扫描：C 前端无文件对话框，GPX 走"配置目录"约定 */

#include "ui/voice_wire.h"
#include "render/paint.h"
#include "render/navdraw.h"
#include "ui/chrome.h"
#include "render/trackdraw.h"
#include "map/binfmt.h"
#include "map/map_view.h"
#include "utils/geo.h"
#include "utils/datum.h"
#include "utils/plat.h"
#include "settings/settings.h"

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

/* 语音播报：per-maneuver 去重位图重置 + 每帧按距离触发（实现见下方
 * app_nav_sim_toggle 之前）。先声明，供 app_nav_poll / app_nav_plan_sync 调用。 */
static void nav_announce_reset(App *a);
static void nav_announce_at(App *a, uint64_t now_ms);

int app_init(App *a, const char *graph_dir, int w, int h,
             const char *font_path, int font_size, const AutoBuildOpt *ab)
{
    memset(a, 0, sizeof(*a));
    /* 设置持久化：先载入配置文件，把值当成默认值填进 App；下面既有的
       CLI 参数 / NAVIMAP_* 环境变量读取会自然覆盖（谁后写谁赢），
       从而满足"命令行与环境变量优先级高于配置文件"。 */
    a->cfg = settings_create();
    if (a->cfg) settings_load(a->cfg, NULL);
    a->max_class = settings_get_int(a->cfg, "max_class", 4);
    a->need_redraw = 1;
    a->show_panel = settings_get_bool(a->cfg, "show_panel", 1);
    a->day_night  = settings_get_bool(a->cfg, "day_night", 0);
    a->show_poi    = settings_get_bool(a->cfg, "show_poi", 1);
    a->show_labels = settings_get_bool(a->cfg, "show_labels", 1);
    a->show_contours = settings_get_bool(a->cfg, "show_contours", 1);
    a->voice_on    = settings_get_bool(a->cfg, "voice_on", 1);
    a->grid_subdiv   = settings_get_bool(a->cfg, "grid_subdiv", 0);
    a->mode        = settings_get_int(a->cfg, "mode", 0);
    /* ★ app_init 是**逐字段**读设置的（不经过 app_apply_settings——那个只给
     *   C 核心自绘设置面板（chrome.c）的「应用/恢复默认」用）。新增设置项必须在这里也读一遍，
     *   否则启动时不生效、下次退出还会把它写回默认值（踩过：datum 启动读不出）。 */
    {
        int dv = settings_get_int(a->cfg, "datum", 0);
        a->datum = (dv >= 0 && dv < DATUM_COUNT) ? dv : DATUM_WGS84;
    }
    /* 浮层透明度：逐浮层从设置读（键缺失 ⇒ 默认 75%）。同样必须在这里读一遍，
     * app_apply_settings 只服务 C 核心自绘设置面板的「应用/恢复默认」，
     * Qt 前端改完直接写字段 + app_persist_settings。 */
    ovl_load_all(a->cfg, a->ovl_alpha);
    a->sim_speed_idx = SIM_SPEED_DEFAULT;
    a->sim_follow = 1;
    /* 互斥量用平台抽象层（不直接碰 pthread / CRITICAL_SECTION）。
       nm_mutex 不依赖任何窗口系统，这里直接建。 */
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

    /* 记下图集根目录与自愈选项，供 app_reload_maps 在"下载新省后"重扫使用
     * （否则 reload 时既不知道去哪扫、也不知道要不要现场重建）。 */
    if (graph_dir)
        snprintf(a->graph_dir, sizeof(a->graph_dir), "%s", graph_dir);
    if (ab) a->ab = *ab;
    else { a->ab.enable = 1; a->ab.src_dir = NULL; a->ab.tool = NULL; }

    /* 字体可选。优先级：命令行 --font > 环境变量 NAVIMAP_FONT > 默认宋体候选链。
       "none"/空串 表示显式关闭文字标注（纯图形模式）。 */
    if (!font_path) font_path = getenv("NAVMAP_FONT");
    int want_font = !(font_path && (font_path[0] == '\0' ||
                                    strcmp(font_path, "none") == 0));
    a->font_size = font_size > 0 ? font_size
                                  : settings_get_int(a->cfg, "font_size", 14);

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

    /* 用户轨迹/收藏库（tracks.db，与 places.db 分开：那是抽取产物、只读）。
       打不开/不存在都照常跑——只是没有历史轨迹可加载（导入时会现场建库）。
       ★ 必须先 gpx_store_init：App 是调用方的裸结构体（未初始化），
       gpx_db_load_all 开头会 gpx_store_free —— 对垃圾指针 free 会直接崩。 */
    gpx_store_init(&a->gpx);
    {
        char dbp[1024];
        int pr = gpx_db_default_path(dbp, sizeof(dbp));
        if (pr == 0) {
            a->gpx_db = gpx_db_open(dbp);
            if (a->gpx_db) gpx_db_load_all(a->gpx_db, &a->gpx);
        }
    }

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
    /* 记住上次退出时的位置（首次运行无 view_* 键则保留上面的图集中心） */
    if (a->cfg && settings_has(a->cfg, "view_lat")) {
        clat = settings_get_dbl(a->cfg, "view_lat", clat);
        clon = settings_get_dbl(a->cfg, "view_lon", clon);
        czoom = settings_get_dbl(a->cfg, "view_zoom", czoom);
    }
    mv_init(&a->view, clat, clon, czoom, w, h);
    dedup_init(&a->dd);
    return 0;
}

int app_reload_maps(App *a)
{
    if (!a || !a->graph_dir[0]) return -1;
    /* 释放旧图集，清零（mapset_load 内部按空结构重新初始化，旧指针不能留着）。
     * 单线程：只在主循环 tick 里被调用，reload 完成前不会再有渲染拿旧 ms。 */
    mapset_free(&a->ms);
    memset(&a->ms, 0, sizeof(a->ms));
    if (mapset_load_repair(&a->ms, a->graph_dir, &a->ab, NULL) != 0) {
        /* 失败：ms 已是空壳，调用方应退出或重建，别拿去渲染 */
        a->total_nodes = a->total_edges = 0;
        return -1;
    }
    mapset_totals(&a->ms, &a->total_nodes, &a->total_edges);
    return 0;
}

void app_shutdown(App *a)
{
    /* 退出前把当前状态（含视图位置）写回配置文件，再释放配置对象。
       必须在字体/视图等字段还有效时做——放在最前面。 */
    if (a->cfg) {
        app_persist_settings(a);
        settings_destroy(a->cfg);
        a->cfg = NULL;
    }
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
    if (a->nav_done) { route_result_set_free(&a->nav_done->alts); free(a->nav_done); a->nav_done = NULL; }
    if (a->have_rr) route_result_free(&a->rr);
    if (a->have_alts) route_result_set_free(&a->ralts);
    maneuver_free(&a->man);
    router_destroy(a->router);
    if (a->nav_mu) { nm_mutex_destroy(a->nav_mu); a->nav_mu = NULL; }
    mapset_free(&a->ms);
    dedup_free(&a->dd);
    label_cache_free(&a->places);
    search_close(a->sdb);
    /* 用户轨迹/收藏：先关库再放内存（库里内容退出前都已同步写入了——
       导入/收藏/删除都是即时写库，不存在"待落盘"状态）。 */
    gpx_db_close(a->gpx_db);
    a->gpx_db = NULL;
    gpx_store_free(&a->gpx);
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
 * 设置持久化：cfg ↔ App 双向同步
 * ------------------------------------------------------------------------- */

void app_apply_settings(App *a)
{
    if (!a || !a->cfg) return;
    a->day_night    = settings_get_bool(a->cfg, "day_night", 0);
    a->show_panel   = settings_get_bool(a->cfg, "show_panel", 1);
    a->show_poi     = settings_get_bool(a->cfg, "show_poi", 1);
    a->show_labels  = settings_get_bool(a->cfg, "show_labels", 1);
    a->show_contours = settings_get_bool(a->cfg, "show_contours", 1);
    a->voice_on     = settings_get_bool(a->cfg, "voice_on", 1);
    a->grid_subdiv   = settings_get_bool(a->cfg, "grid_subdiv", 0);
    a->mode         = settings_get_int(a->cfg, "mode", 0);
    a->max_class    = settings_get_int(a->cfg, "max_class", 4);
    int dv = settings_get_int(a->cfg, "datum", 0);
    a->datum = (dv >= 0 && dv < DATUM_COUNT) ? dv : DATUM_WGS84;
    int fs = settings_get_int(a->cfg, "font_size", 14);
    if (fs != a->font_size) app_set_font_size(a, fs);
    ovl_load_all(a->cfg, a->ovl_alpha);
}

void app_persist_settings(App *a)
{
    if (!a || !a->cfg) return;
    settings_set_bool(a->cfg, "day_night",   a->day_night);
    settings_set_bool(a->cfg, "show_panel",  a->show_panel);
    settings_set_bool(a->cfg, "show_poi",    a->show_poi);
    settings_set_bool(a->cfg, "show_labels", a->show_labels);
    settings_set_bool(a->cfg, "show_contours", a->show_contours);
    settings_set_bool(a->cfg, "voice_on",    a->voice_on);
    settings_set_bool(a->cfg, "grid_subdiv", a->grid_subdiv);
    settings_set_int(a->cfg,  "mode",        a->mode);
    settings_set_int(a->cfg,  "font_size",   a->font_size);
    settings_set_int(a->cfg,  "max_class",   a->max_class);
    settings_set_int(a->cfg,  "datum",       a->datum);
    ovl_store_all(a->cfg, a->ovl_alpha);
    /* 视图位置：记住上次退出时所在（下次启动直接回到这里） */
    settings_set_dbl(a->cfg, "view_lat",  a->view.lat);
    settings_set_dbl(a->cfg, "view_lon",  a->view.lon);
    settings_set_dbl(a->cfg, "view_zoom", a->view.zoom);
    settings_save(a->cfg);
}

/* 改某个浮层的透明度（百分比）。UI（SDL 设置子面板 / Qt 对话框）统一走这里：
 * 改 App + 立即写设置 + 标红重绘；真正落盘仍在 app_persist_settings。 */
void app_ovl_alpha_set(App *a, NmOverlay id, int pct)
{
    if (!a || id < 0 || id >= OVL_COUNT) return;
    int v = ovl_clamp_pct(pct);
    if (a->ovl_alpha[id] == v) return;
    a->ovl_alpha[id] = v;
    if (a->cfg) settings_set_int(a->cfg, ovl_key(id), v);
    a->need_redraw = 1;
}

/* 全部浮层恢复默认（75%）。 */
void app_ovl_alpha_reset(App *a)
{
    if (!a) return;
    for (int i = 0; i < OVL_COUNT; i++) app_ovl_alpha_set(a, (NmOverlay)i,
                                                          OVL_ALPHA_DEFAULT);
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
    a->mode = 0;                          /* 路线没了 ⇒ 回到浏览模式（顶栏页签同步） */
    if (a->have_alts) { route_result_set_free(&a->ralts); a->have_alts = 0; a->sel_alt = 0; }
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
            /* ★ 用 router_route_alts 一次算出多方案（最快/最短/避收费/避非铺装），
               端点先按"最快"找到一对连通的、四个方案复用这组端点，保证可比。 */
            int rc = router_route_alts(a->router, pa.lat, pa.lon, pb.lat, pb.lon,
                                       &res->alts, &f, &t, &da, &db);
            res->err = nav_err_of(rc);
            if (rc != -1) {
                res->a_snap_m = da; res->b_snap_m = db;
                nav_snap_info(&a->ms, f, &res->a_lat, &res->a_lon, &res->a_prov);
                nav_snap_info(&a->ms, t, &res->b_lat, &res->b_lon, &res->b_prov);
            }
        }

        nm_mutex_lock(a->nav_mu);
        if (a->nav_done) { route_result_set_free(&a->nav_done->alts); free(a->nav_done); }
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
        a->ralts = res->alts;              /* 多方案整体搬入 App */
        memset(&res->alts, 0, sizeof(res->alts));
        a->have_alts = 1;
        a->sel_alt = 0;                    /* 默认选中"最快" */
        /* 把选中方案（ralts.rr[0]）的指针搬到 a->rr 当主路线，源槽清零避免双重释放 */
        a->rr = a->ralts.rr[0];
        memset(&a->ralts.rr[0], 0, sizeof(RouteResult));
        a->have_rr = 1;
    a->mode = 1;                         /* 有了路线 ⇒ 导航模式（顶栏页签 + 导航 chrome 显示） */
        maneuver_build(&a->rr, &a->ms, &a->man);
        nav_announce_reset(a);
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
        route_result_set_free(&res->alts);
        const char *m = res->err == -1 ? "起终点之间不连通"
                      : res->err == -2 ? "搜索规模超限（路太远）"
                                       : "起终点附近没有可通行的路";
        snprintf(a->nav_msg, sizeof(a->nav_msg), "%s", m);
    }
    free(res);
    a->need_redraw = 1;
}

/* 切换多方案中正在走的那条。k 必须是可行方案的下标（err[k]==0）。
 * 把该方案从 ralts 整体"搬"进 a->rr 当主路线（源槽清零，避免双重释放），
 * 重算转向指令；若正在模拟则从头重新开始。 */
void app_nav_select_alt(App *a, int k)
{
    if (!a->have_alts || k < 0 || k >= a->ralts.n) return;
    if (a->ralts.err[k] != 0) return;                 /* 不可行方案不能选 */
    if (k == a->sel_alt && a->have_rr) return;        /* 已经是当前选中的 */

    if (a->have_rr) route_result_free(&a->rr);        /* 释放旧主路线 */
    a->rr = a->ralts.rr[k];                           /* 指针整体搬家 */
    memset(&a->ralts.rr[k], 0, sizeof(RouteResult));
    a->sel_alt = k;
    a->have_rr = 1;
    a->mode = 1;                         /* 有了路线 ⇒ 导航模式（顶栏页签 + 导航 chrome 显示） */
    maneuver_build(&a->rr, &a->ms, &a->man);
    nav_announce_reset(a);
    if (a->sim_on) {                                  /* 模拟中切换：路线从头走 */
        navsim_start(&a->sim, &a->rr);
        a->sim.speed_mul = sim_speed_of(a);
        a->sim_on = 1;
        a->sim_paused = 0;
        a->sim_tick = nm_now_ms();
    }
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
    app_persist_settings(a);
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
    /* 注：文本输入由前端自持（Qt 端 QLineEdit 自带 IME，无需额外动作），
       本函数是平台无关的。 */
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

/* 重置播报状态（新路线 / 重新起步时调用）。
 * ★ 现在播报记忆全在 VoiceRouter 里（层级、同文本去抖、限速/到达/告警冷却），
 *   不再需要 per-maneuver 的位图。 */
static void nav_announce_reset(App *a)
{
    voice_router_reset(&a->vr);
    a->ann_finished = 0;
}

/* 每帧调用：按官方 OsmAnd 的规则决定播什么。
 *
 * ★ 抄写来源（详见 src/route/voice.h 的清单）：
 *   · 分级提前量 = 速度 × 秒数（官方 AnnounceTimeDistances.java）
 *     ⇒ 高速上提前得多（120 km/h 时提前 3.8 km），慢路上不啰嗦。
 *   · 话术 = 官方 nav-desktop 的中文桌面实现（VoiceRouter.turnPhrase/action）
 *   · 路口合并 = 官方 turnPhrase 的 "，然后紧接着…"
 *   · 告警两级 = 官方 Alarm.phrase + 官方 Alarms/Navigator 的选取规则
 *
 * ★ 编排（"这一帧先播限速还是先播转向"）也在 route/voice.c 的 voice_tick() 里，
 *   顺序严格照官方 VoiceRouter.update()：speedLimit → arrival → alarm → turn。
 *
 * ★★ 这里只是一层**薄封装**：真正干活的接线在 src/ui/voice_wire.c 的
 *   nav_voice_frame()（生产与无头自测共用同一份）⇒ 接线坏了自测必然变红
 *   （原先接线长在 App 里，自测够不着，只能复刻一遍 ⇒ 判据假绿）。 */
static void nav_announce_at(App *a, uint64_t now_ms)
{
    if (!a->have_rr || !a->sim_on) return;

    NavVoiceCtx c;
    c.vr        = &a->vr;
    c.rr        = &a->rr;
    c.man       = &a->man;
    c.sim       = &a->sim;
    c.voice_on  = a->voice_on;
    c.paused    = a->sim_paused;
    c.ann_finished = &a->ann_finished;
    nav_voice_frame(&c, now_ms);
}

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
        nav_announce_reset(a);
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
    /* ★★ 播报在 sim_paused 置位**之前**调用（历史习惯，仍保留）：
     *   到达那一刻"您已到达目的地附近，导航结束"要能说出来。
     *   ★ 现在更稳的兜底在 src/ui/voice_wire.c 的 nav_voice_frame：
     *     它对"到达帧"一律不抑制（常规到达提示 + 收尾语都不受 paused 影响），
     *     所以本调用的顺序已不是 load-bearing —— 即便先置位再播报，
     *     语音连线也会把到达帧的播报放出来。判据只盖 voice_wire.c
     *     （e2e A8 + 负向对照 #9/#10），无需为 window.c 顺序单独写判据。
     *   顺序：先播报（此时 sim_paused 还是旧的 0）⇒ 再置位。 */
    nav_announce_at(a, now);
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

/* 同步算一次（无窗口、无主循环时用，见 navmap_qt --shot-offscreen --from/--to）。
   与后台线程走同一套吸附 + 结算逻辑，只是不另起线程。 */
int app_nav_plan_sync(App *a, double alat, double alon, double blat, double blon)
{
    if (nav_ensure_router(a) != 0) return -1;
    RNode f, t;
    NavResult res;
    memset(&res, 0, sizeof(res));
    /* 同上：走 router_route_alts，吸附撞上死节点会换候选重试（PLAN.md §51.1）。
       同步路径也给多方案，便于离屏审计一并验证。 */
    int rc = router_route_alts(a->router, alat, alon, blat, blon,
                               &res.alts, &f, &t, &res.a_snap_m, &res.b_snap_m);
    if (rc == -1) {
        snprintf(a->nav_msg, sizeof(a->nav_msg), "起终点附近没有可通行的路");
        return -1;
    }
    nav_snap_info(&a->ms, f, &res.a_lat, &res.a_lon, &res.a_prov);
    nav_snap_info(&a->ms, t, &res.b_lat, &res.b_lon, &res.b_prov);
    if (rc != 0) {
        route_result_set_free(&res.alts);
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
    a->ralts = res.alts;
    memset(&res.alts, 0, sizeof(res.alts));
    a->have_alts = 1;
    a->sel_alt = 0;
    a->rr = a->ralts.rr[0];
    memset(&a->ralts.rr[0], 0, sizeof(RouteResult));
    a->have_rr = 1;
    a->mode = 1;                         /* 有了路线 ⇒ 导航模式（顶栏页签 + 导航 chrome 显示） */
    maneuver_build(&a->rr, &a->ms, &a->man);
    nav_announce_reset(a);
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
    memset(&a->cov_place, 0, sizeof(a->cov_place));
    a->cov_place.nearest_km = -1;

    if (a->cov_state == COV_NONE || a->cov_state == COV_BBOX_ONLY) {
        /* 反查"这是哪儿"：省一级走 map 层（bbox + 省名表），市/区走 render 层
         * （地名库）。★ 只在无图区做——有图时不查库，浏览热路径零开销。 */
        coverage_locate(&a->ms, lat, lon, &a->cov_place);
        if (a->has_places)
            label_nearest_admin(&a->places, lat, lon,
                                a->cov_place.city, sizeof(a->cov_place.city),
                                a->cov_place.district, sizeof(a->cov_place.district));

        coverage_hint(&a->ms, lat, lon, a->cov_state, &a->cov_place,
                      a->cov_hint, sizeof(a->cov_hint));
        /* 完整指引打一次就够，横幅上只放浓缩版 */
        fprintf(stderr, "\n[无地图区域] %s\n", a->cov_hint);
    } else {
        a->cov_hint[0] = '\0';
    }
}

/* 公开入口：见 window.h。内部实现就是 paint_frame 用的那个 app_update_coverage，
 * 但那个是 static 且要求"位置变了才重算"。自检/截图路径是"位置没变也要拿到结果"，
 * 所以这里先把 cov_checked 清掉强制重算一次（成本可接受：只在显式调用时发生）。 */
void app_refresh_coverage(App *a)
{
    if (!a) return;
    a->cov_checked = 0;
    app_update_coverage(a);
}




/* 解码中省份的占位提示：后台线程解码 PBF 显示层期间，该省 has_features=0、
 * 渲染自然空白。这里画一个"省名 解码中… NN%"小药丸，避免用户以为地图坏了。
 * 进度来自 worker 经 pbfmap_decode 进度回调实时更新的 dmap_progress。 */
static void paint_dmap_overlay(Raster *rs, App *a)
{
    if (!a->font) return;
    MvProj P;
    mv_proj_begin(&a->view, &P);
    int W = a->view.w, H = a->view.h;
    for (int i = 0; i < a->ms.n; i++) {
        MapSrc *s = &a->ms.srcs[i];
        if (!s->loaded || !s->pbf_display_mode || s->has_features) continue;
        int prog  = mapset_dmap_progress(&a->ms, i);
        int failed = s->dmap_failed;

        /* 投影省 bbox 中心到屏幕；超出则夹紧到可视区边缘 */
        double cla = ((double)s->bbox[0] + s->bbox[2]) / 2e7;
        double clo = ((double)s->bbox[1] + s->bbox[3]) / 2e7;
        int sx, sy;
        mv_proj(&P, cla, clo, &sx, &sy);

        char buf[128];
        const char *txt = buf;
        if (failed)
            snprintf(buf, sizeof(buf), "%s 解码失败", s->name);
        else
            snprintf(buf, sizeof(buf), "%s 解码中… %d%%", s->name, prog);

        int tw = font_text_w(a->font, txt);
        int pw = tw + 16, ph = 22;
        int x = sx - pw / 2, y = sy - ph / 2;
        if (x < 4) x = 4;
        if (x + pw > W - 4) x = W - pw - 4;
        if (y < 4) y = 4;
        if (y + ph > H - 4) y = H - ph - 4;

        uint8_t r = failed ? 120 : 28, g = failed ? 40 : 34, b = failed ? 40 : 48;
        raster_fill_rect(rs, x, y, pw, ph, r, g, b, 210);
        raster_rect(rs, x, y, pw, ph, 70, 80, 100, 220);
        font_draw(a->font, rs, x + 8, y + 4, txt, 232, 240, 252, 0);
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
    /* 等高线：独立于线/面要素，叠加在路网之上、地名之下。受 show_contours 门控。 */
    if (a->show_contours) {
        for (int i = 0; i < a->ms.n; i++) {
            MapSrc *s = &a->ms.srcs[i];
            if (!bbox_hits(s->bbox, vb)) continue;
            if (s->dup_of >= 0) continue;
            if (!s->loaded) continue;
            if (s->has_contours)
                paint_contours(rs, &a->view, &s->contours, &s->contour_polyrefs,
                               &s->contour_pts, &s->contour_bbox, st);
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

/* 主循环与离屏画布：交互主循环在 navmap_qt（Qt 端），离屏出图在
   src/ui/offscreen_shot.c。C 核心这边两者都不做。 */

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

/* ================================================================== */
/* GPX 轨迹 / 收藏（轨迹落地）                                          */
/* ================================================================== */

int app_gpx_import(App *a, const char *path)
{
    if (!a || !path || !*path) return -1;
    /* 库里还没有就在默认路径建一个（导入是"用户数据"的起点）。 */
    if (!a->gpx_db) {
        char dbp[1024];
        if (gpx_db_default_path(dbp, sizeof(dbp)) == 0)
            a->gpx_db = gpx_db_open(dbp);
    }
    int n0 = a->gpx.n_trk;
    int w0 = a->gpx.n_wpt;
    int rc = gpx_parse_file(path, &a->gpx);
    if (rc < 0) return rc;                 /* -1 读不了 / -2 没解析到点 */
    /* ★ 导入必须**幂等**：同一个文件导入两次，结果要和导入一次一样。
     * 库里轨道按 name 替换、航点按 (name,lat,lon) 查重，内存侧得跟着做，
     * 否则"库里一条、内存两条"，面板上出现同名重复行（实测踩过：
     * 连导两次后面板里 3 个故宫东门）。 */
    for (int i = a->gpx.n_trk - 1; i >= n0; i--) {
        for (int k = 0; k < i; k++)
            if (!strcmp(a->gpx.trk[k].name, a->gpx.trk[i].name)) {
                gpx_store_del_track(&a->gpx, i);
                break;
            }
    }
    for (int i = a->gpx.n_wpt - 1; i >= w0; i--) {
        const GpxWpt *w = &a->gpx.wpt[i];
        int dup = 0;
        for (int k = 0; k < i && !dup; k++) {
            const GpxWpt *o = &a->gpx.wpt[k];
            if (!strcmp(o->name, w->name) &&
                fabs(o->lat - w->lat) < 1e-9 && fabs(o->lon - w->lon) < 1e-9) dup = 1;
        }
        if (!dup && a->gpx_db &&
            gpx_db_find_fav(a->gpx_db, w->name, w->lat, w->lon) > 0) dup = 1;
        if (dup) gpx_store_del_wpt(&a->gpx, i);
    }
    int added = a->gpx.n_trk - n0;
    if (a->gpx_db) {
        for (int i = n0; i < a->gpx.n_trk; i++)
            a->gpx.trk[i].db_id = gpx_db_save_track(a->gpx_db, &a->gpx.trk[i]);
        /* GPX 的 wpt 语义就是航点 ⇒ 进收藏夹 */
        for (int i = w0; i < a->gpx.n_wpt; i++) {
            const GpxWpt *w = &a->gpx.wpt[i];
            a->gpx.wpt[i].db_id = gpx_db_add_fav(a->gpx_db, w->name, w->lat, w->lon);
        }
    }
    a->need_redraw = 1;
    return added;
}

int app_gpx_export(App *a, const char *path)
{
    if (!a || !path || !*path) return -1;
    return gpx_write(&a->gpx, path);
}

int app_fav_add(App *a, double lat, double lon)
{
    if (!a) return -1;
    if (!a->gpx_db) {
        char dbp[1024];
        if (gpx_db_default_path(dbp, sizeof(dbp)) == 0)
            a->gpx_db = gpx_db_open(dbp);
        if (!a->gpx_db) return -1;
    }
    char nm[GPX_NAME_MAX];
    snprintf(nm, sizeof(nm), "收藏 %d", a->gpx.n_wpt + 1);
    int id = gpx_db_add_fav(a->gpx_db, nm, lat, lon);
    if (id < 0) return -1;
    gpx_store_add_wpt(&a->gpx, nm, lat, lon);
    a->gpx.wpt[a->gpx.n_wpt - 1].db_id = id;
    a->need_redraw = 1;
    return 0;
}

int app_fav_del(App *a, int idx)
{
    if (!a || idx < 0 || idx >= a->gpx.n_wpt) return -1;
    gpx_db_del_fav(a->gpx_db, (int)a->gpx.wpt[idx].db_id);
    gpx_store_del_wpt(&a->gpx, idx);
    a->need_redraw = 1;
    return 0;
}

int app_gpx_del_track(App *a, int idx)
{
    if (!a || idx < 0 || idx >= a->gpx.n_trk) return -1;
    gpx_db_del_track(a->gpx_db, (int)a->gpx.trk[idx].db_id);
    gpx_store_del_track(&a->gpx, idx);
    a->need_redraw = 1;
    return 0;
}

/* 扫描目录下的 *.gpx 逐个导入（C 前端没有文件对话框的妥协方案：把文件丢进
 * <config>/gpx/ 再一键导入）。目录不存在 ⇒ 返回 0（不是错误，只是没有可导入的）。
 * 进度/结果打到 stderr（C 前端从终端启动，stderr 可见）。 */
int app_gpx_import_dir(App *a, const char *dir)
{
    if (!a || !dir || !*dir) return -1;
    DIR *d = opendir(dir);
    if (!d) return 0;                      /* 目录不存在 = 0 条，不是失败 */
    int added = 0;
    char path[2048];
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *nm = e->d_name;
        size_t L = strlen(nm);
        if (L < 4u || strcmp(nm + L - 4, ".gpx") != 0) continue;
        if (nm_path_join(path, sizeof(path), dir, nm) != 0) continue;
        int rc = app_gpx_import(a, path);
        if (rc > 0) added += rc;
        else if (rc < 0) fprintf(stderr, "[gpx] 导入失败: %s\n", path);
    }
    closedir(d);
    if (added > 0) fprintf(stderr, "[gpx] 从 %s 导入了 %d 条轨迹\n", dir, added);
    else            fprintf(stderr, "[gpx] %s 下没有可导入的 .gpx 文件\n", dir);
    a->need_redraw = 1;
    return added;
}

/* 导出全部（轨迹 + 收藏）到 <config>/export.gpx。 */
int app_gpx_export_default(App *a)
{
    if (!a) return -1;
    const char *cfg = nm_config_dir();
    char path[2048];
    if (!cfg || nm_path_join(path, sizeof(path), cfg, "export.gpx") != 0) return -1;
    int rc = app_gpx_export(a, path);
    if (rc == 0) fprintf(stderr, "[gpx] 已导出到 %s\n", path);
    else         fprintf(stderr, "[gpx] 导出失败\n");
    return rc;
}
