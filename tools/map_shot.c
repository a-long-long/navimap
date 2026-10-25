/* map_shot —— 不弹窗口，把一帧路网渲成图片落盘。
 *
 * 存在理由：本机没有 xvfb-run、也没有 valgrind，GUI 只能由人在 Wayland 会话里
 * 肉眼看。渲染对不对（有没有路、投影歪不歪、LOD 生不生效）必须能无人值守验证，
 * 所以做一个离屏渲染通道：出来的图可以直接打开看，也可以做像素级断言。
 *
 * 用法：
 *   ./map_shot --graph=products/graph/beijing --out=/tmp/shot.bmp \
 *              --lat=39.90 --lon=116.40 --zoom=12 --w=1280 --h=800 \
 *              [--max-class=4] [--font=<路径.ttf|none>] [--font-size=14]
 *              [--from=lat,lon --to=lat,lon] [--fit-route]
 *              [--sim [--sim-sec=<模拟秒>] [--sim-deviate=<米>]]
 *
 * --from/--to：进程内直接算一条路线并叠加（不用先跑 route_cli 导 GeoJSON），
 *   这样"算路 + 画路线 + 指令"整条链都能无人值守断言。给 --lat/--lon 时
 *   按给定中心渲染，否则自动缩放到整条路线。
 * --sim：把模拟导航那一层也画出来（车头箭头、已走过变灰、底部指令条），
 *   --sim-sec 先推进一段路，--sim-deviate 把定位点拉开若干米模拟走错路。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>

#include "ui/window.h"
#include <time.h>
#include "render/paint.h"
#include "render/navdraw.h"
#include "map/binfmt.h"
#include "map/coverage.h"
#include "utils/plat.h"
#include "tool_common.h"


static int parse_ll(const char *s, double *lat, double *lon)
{
    char *sp = strchr(s, ',');
    if (!sp) return -1;
    char buf[128];
    size_t n = (size_t)(sp - s);
    if (n >= sizeof(buf)) return -1;
    memcpy(buf, s, n); buf[n] = '\0';
    *lat = atof(buf);
    *lon = atof(sp + 1);
    return 0;
}

/* 数屏幕上有多少个"路线蓝"像素：断言路线真的画上去了。
   软件渲染器不做 alpha 混合，画上去的颜色就是原值，可以精确比。 */
static long count_route_pixels(SDL_Surface *s)
{
    if (s->format->BitsPerPixel != 32) return -1;
    Uint32 want = SDL_MapRGBA(s->format, NAV_ROUTE_R, NAV_ROUTE_G, NAV_ROUTE_B, 255);
    long n = 0;
    for (int y = 0; y < s->h; y++) {
        Uint32 *row = (Uint32 *)((Uint8 *)s->pixels + (size_t)y * s->pitch);
        for (int x = 0; x < s->w; x++)
            if (row[x] == want) n++;
    }
    return n;
}

/* 数"已走过"的灰像素（NAV_ROUTE_DONE 那套颜色）。模拟导航推进一半之后，
   画面上必须同时有灰（走过）和蓝（没走）——只断言蓝的话，"已走过那段"
   画没画对根本发现不了（颜色写错、切分位置写错都是蓝的照样全在）。 */
static long count_done_pixels(SDL_Surface *s)
{
    if (s->format->BitsPerPixel != 32) return -1;
    Uint32 want = SDL_MapRGBA(s->format, 130, 138, 150, 255);
    long n = 0;
    for (int y = 0; y < s->h; y++) {
        Uint32 *row = (Uint32 *)((Uint8 *)s->pixels + (size_t)y * s->pitch);
        for (int x = 0; x < s->w; x++)
            if (row[x] == want) n++;
    }
    return n;
}

/* 按颜色数像素（软件渲染器不做混合，画上去是什么颜色就是什么颜色）。
   搜索框、指令条这类"框画没画出来"的断言都靠它。 */
static long count_color_pixels(SDL_Surface *s, int cr, int cg, int cb)
{
    if (s->format->BitsPerPixel != 32) return -1;
    Uint32 want = SDL_MapRGBA(s->format, (Uint8)cr, (Uint8)cg, (Uint8)cb, 255);
    long n = 0;
    for (int y = 0; y < s->h; y++) {
        Uint32 *row = (Uint32 *)((Uint8 *)s->pixels + (size_t)y * s->pitch);
        for (int x = 0; x < s->w; x++)
            if (row[x] == want) n++;
    }
    return n;
}

/* 把一帧渲进调用方的 SDL_Surface。
 *
 * ★ 像素格式全部从 surface 自己的 format 里读（Rmask/Gmask/Bmask/Amask），
 *   不写死：写死就会在"某台机器上红蓝互换"，而这种 bug 只在别人机器上复现。
 *   stride 以像素计（pitch 是字节，32bpp 时 pitch/4 == w）。
 *   注意没有 SDL_MUSTLOCK 的检查：这里只用软件 surface（可写内存），硬件
 *   surface 走不到这条路。 */
static int render_surface(App *a, SDL_Surface *surf, PaintStats *st)
{
    if (!surf || !surf->pixels) return -1;
    if (surf->format->BitsPerPixel != 32 || surf->format->BytesPerPixel != 4)
        return -1;
    if (surf->pitch % 4) return -1;
    return app_render_to_buf(a, surf->pixels, surf->w, surf->h,
                              (size_t)(surf->pitch / 4),
                              surf->format->Rmask, surf->format->Gmask,
                              surf->format->Bmask, surf->format->Amask, st);
}

/* 极简 GeoJSON LineString 读取：只认 "coordinates" 后面的 [[lon,lat],...]。
 * 用途是把 route_cli 算出的路线叠到地图上做视觉验收，不值得引第三方 JSON 库。 */
static double *read_geojson_line(const char *path, size_t *out_n)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "打不开 %s\n", path); return NULL; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz <= 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    buf[got] = '\0';
    fclose(f);

    char *p = strstr(buf, "\"coordinates\"");
    if (!p) { fprintf(stderr, "%s 里没有 coordinates\n", path); free(buf); return NULL; }
    p = strchr(p, '[');
    if (!p) { free(buf); return NULL; }

    double *pts = NULL;
    size_t n = 0, cap = 0;
    char *q = p;
    while (q && *q) {
        q = strchr(q, '[');
        if (!q) break;
        double lon_v, lat_v;
        int ok = (sscanf(q + 1, " %lf , %lf", &lon_v, &lat_v) == 2);
        char *close = strchr(q + 1, ']');
        if (!close) break;
        if (ok) {
            if (n + 2 > cap) {
                cap = cap ? cap * 2 : 1024;
                double *t = realloc(pts, cap * sizeof(double));
                if (!t) { free(pts); free(buf); return NULL; }
                pts = t;
            }
            pts[n++] = lon_v;      /* GeoJSON 顺序：[经度, 纬度] */
            pts[n++] = lat_v;
        }
        q = close + 1;
        while (*q == ' ' || *q == '\n' || *q == '\r' || *q == '\t') q++;
        if (*q == ']') break;
    }
    free(buf);
    *out_n = n / 2;
    return pts;
}

int main(int argc, char **argv)
{
    const char *graph = "products/graph/beijing";
    const char *pbf_display = NULL;   /* PBF 显示层模式 */
    int pbf_lru = 3;
    /* 默认输出：临时目录 + 固定名（曾经写死 /tmp/...，Windows 上没有 /tmp）。
     * 本机 TMPDIR 未设置时 nm_temp_dir() 就是 "/tmp"，默认值与旧版一致。 */
    char outbuf[4096];
    const char *out = outbuf;
    if (nm_path_join(outbuf, sizeof(outbuf), nm_temp_dir(),
                     "navimap_shot.bmp") != 0) {
        fprintf(stderr, "临时目录路径过长：%s\n", nm_temp_dir());
        return 1;
    }
    double lat = 0, lon = 0;
    int have_center = 0;
    double zoom = 12;
    int w = 1280, h = 800, max_class = 4;
    const char *font = NULL;   /* NULL = 默认字体（宋体） */
    int font_size = 0;         /* <=0 = 默认字号 */
    int resize_font = 0;       /* >0：第二帧前改字号，验证运行期调字号路径 */
    const char *route = NULL;  /* route_cli 导出的 GeoJSON，叠加成红线 */
    const char *from_s = NULL, *to_s = NULL;   /* 进程内算路（阶段 9） */
    int fit_route = 0;
    const char *q_s = NULL;      /* 搜索关键词：画搜索框 + 结果钉子（阶段 7） */
    int sim = 0;                 /* 开模拟导航（画车头 + 已走/未走双色 + 指令条） */
    double sim_sec = 0;          /* 渲染前先推进多少模拟秒 */
    double deviate_m = 0;        /* >0：把定位点朝右拉开这么多米（模拟走错路） */
    int zoom_given = 0;
    const char *dmap_demo = NULL;   /* --dmap-demo=<省>:<pct>：强制某省进入"解码中"占位态，用于截图验证 overlay */
    int dmap_demo_pct = 0;

    /* 启动自愈（同 navimap）：源 pbf 在、产物没了/坏了就重建。默认开。 */
    AutoBuildOpt ab;
    tc_ab_init(&ab);

    for (int i = 1; i < argc; i++) {
        const char *v;
        if ((v = tc_arg_val(argv[i], "--from=")))     from_s = v;
        else if ((v = tc_arg_val(argv[i], "--to=")))  to_s = v;
        else if (strcmp(argv[i], "--fit-route") == 0) fit_route = 1;
        else if ((v = tc_arg_val(argv[i], "--q=")))      q_s = v;
        else if (strcmp(argv[i], "--sim") == 0)       sim = 1;
        else if ((v = tc_arg_val(argv[i], "--sim-sec=")))     sim_sec = atof(v);
        else if ((v = tc_arg_val(argv[i], "--sim-deviate="))) deviate_m = atof(v);
        else if (strncmp(argv[i], "--graph=", 8) == 0) graph = argv[i] + 8;
        else if (strncmp(argv[i], "--out=", 6) == 0) out = argv[i] + 6;
        else if (strncmp(argv[i], "--lat=", 6) == 0) { lat = atof(argv[i] + 6); have_center |= 1; }
        else if (strncmp(argv[i], "--lon=", 6) == 0) { lon = atof(argv[i] + 6); have_center |= 2; }
        else if (strncmp(argv[i], "--zoom=", 7) == 0) {
            zoom = atof(argv[i] + 7);
            zoom_given = 1;
        }
        /* 注意 n 是"字面量长度"，不是长度+1：
           strncmp(s, "--w=", 5) 会连字符串结尾的 '\0' 一起比，永远不相等。 */
        else if (strncmp(argv[i], "--w=", 4) == 0) w = atoi(argv[i] + 4);
        else if (strncmp(argv[i], "--h=", 4) == 0) h = atoi(argv[i] + 4);
        else if (strncmp(argv[i], "--max-class=", 12) == 0) max_class = atoi(argv[i] + 12);
        else if (strncmp(argv[i], "--font=", 7) == 0) font = argv[i] + 7;
        else if (strncmp(argv[i], "--font-size=", 12) == 0) font_size = atoi(argv[i] + 12);
        else if (strncmp(argv[i], "--resize-font=", 14) == 0) resize_font = atoi(argv[i] + 14);
        else if (strncmp(argv[i], "--route=", 8) == 0) route = argv[i] + 8;
        else if (strcmp(argv[i], "--no-autobuild") == 0) { ab.enable = 0; }
        else if (strncmp(argv[i], "--sources=", 10) == 0) ab.src_dir = argv[i] + 10;
        else if (strncmp(argv[i], "--pbf-display=", 14) == 0) {
            pbf_display = argv[i] + 14;
            const char *colon = strrchr(pbf_display, ':');
            if (colon && colon[1] >= '0' && colon[1] <= '9') {
                pbf_lru = atoi(colon + 1);
                size_t dlen = (size_t)(colon - pbf_display);
                static char dirbuf[4096];
                if (dlen < sizeof(dirbuf)) {
                    memcpy(dirbuf, pbf_display, dlen);
                    dirbuf[dlen] = '\0';
                    pbf_display = dirbuf;
                }
            }
        }
        else if (strncmp(argv[i], "--dmap-demo=", 12) == 0) {
            dmap_demo = argv[i] + 12;
            const char *colon = strrchr(dmap_demo, ':');
            if (colon && colon[1] >= '0' && colon[1] <= '9') {
                dmap_demo_pct = atoi(colon + 1);
                size_t n = (size_t)(colon - dmap_demo);
                static char dbuf[128];
                if (n < sizeof(dbuf)) {
                    memcpy(dbuf, dmap_demo, n);
                    dbuf[n] = '\0';
                    dmap_demo = dbuf;
                }
            }
            if (!pbf_display) pbf_display = "sources";  /* 占位需要 PBF 显示层模式 */
        }
        else { fprintf(stderr, "未知参数 %s\n", argv[i]); return 2; }
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init 失败: %s\n", SDL_GetError());
        return 1;
    }

    App a;
    if (app_init(&a, graph, w, h, font, font_size, &ab) != 0) { SDL_Quit(); return 1; }
    if (pbf_display) {
        if (mapset_enable_pbf_display(&a.ms, pbf_display, pbf_lru) != 0) {
            fprintf(stderr, "提示: PBF 显示层不可用（%s），改用 nmbin 产物\n", pbf_display);
        } else {
            printf("显示层: PBF 运行时解码（%s, LRU %d）\n", pbf_display, pbf_lru);
            /* map_shot 是一次性离屏截图：没有"逐帧渐进"可言，直接同步解码
             * 各省（与交互前端走后台线程不同），保证出图即是对的。 */
            for (int i = 0; i < a.ms.n; i++)
                mapset_ensure_display(&a.ms, i);
        }
    }
    if (have_center == 3) { a.view.lat = lat; a.view.lon = lon; }
    a.view.zoom = zoom;
    a.view.w = w; a.view.h = h;
    a.max_class = max_class;

    /* --dmap-demo：强制某省进入"解码中"占位态，用于截图验证 overlay。
     * 把视图居中到该省并标出进度%，不真正解码（只是渲染层占位）。 */
    if (dmap_demo) {
        int prov = -1;
        for (int i = 0; i < a.ms.n; i++)
            if (strcmp(a.ms.srcs[i].name, dmap_demo) == 0) { prov = i; break; }
        if (prov < 0) {
            fprintf(stderr, "提示: 找不到省份 %s 供 --dmap-demo\n", dmap_demo);
        } else {
            MapSrc *s = &a.ms.srcs[prov];
            s->pbf_display_mode = 1;
            s->loaded = 1;
            s->has_features = 0;          /* 显示层"还没好" */
            s->dmap_inflight = 1;
            s->dmap_progress = dmap_demo_pct;
            a.view.lat = ((double)s->bbox[0] + s->bbox[2]) / 2e7;
            a.view.lon = ((double)s->bbox[1] + s->bbox[3]) / 2e7;
            a.view.zoom = 8;
            int32_t eb[4] = { s->bbox[0], s->bbox[1], s->bbox[2], s->bbox[3] };
            mapset_set_view(&a.ms, eb);
            printf("dmap-demo      %s 进度 %d%%（强制解码中占位）\n", s->name, dmap_demo_pct);
        }
    }

    /* 进程内算路（阶段 9）：起终点 → 路线 + 转向指令，随后由 paint_frame
       直接叠到画面上，不用先导出 GeoJSON。 */
    int have_route = 0;
    if (from_s || to_s) {
        if (!from_s || !to_s) {
            printf("ASSERT-FAIL    --from 与 --to 必须成对给\n");
            return 1;
        }
        double alat, alon, blat, blon;
        if (parse_ll(from_s, &alat, &alon) != 0 ||
            parse_ll(to_s, &blat, &blon) != 0) {
            printf("ASSERT-FAIL    起终点格式应为 lat,lon\n");
            return 1;
        }
        if (app_nav_plan_sync(&a, alat, alon, blat, blon) != 0) {
            printf("ASSERT-FAIL    算路失败：%s\n",
                   a.nav_msg[0] ? a.nav_msg : "(未知)");
            return 1;
        }
        have_route = 1;
        printf("route          %.2f km  约 %.0f 分  %u 段  接缝 %u  展开 %llu  %llu ms\n",
               a.rr.total_m / 1000.0, a.rr.total_s / 60.0, a.rr.n - 1,
               a.rr.n_seams, (unsigned long long)a.rr.expanded,
               (unsigned long long)a.rr.elapsed_ms);
        printf("snap           起点 %.1f m  终点 %.1f m\n",
               a.pick_a.snap_m, a.pick_b.snap_m);
        printf("maneuvers      %d 条（里程和 %.1f m / 总里程 %.1f m）\n",
               a.man.n, a.man.sum_m, a.rr.total_m);
        if (a.have_alts) {
            printf("alts           %d 方案\n", a.ralts.n);
            for (int k = 0; k < a.ralts.n; k++) {
                if (a.ralts.err[k] == 0)
                    printf("  [%d] %-8s %.2f km  约 %.0f 分  收费 %.2f km  非铺装 %.2f km\n",
                           k, a.ralts.tag[k], a.ralts.total_m[k] / 1000.0,
                           a.ralts.total_s[k] / 60.0, a.ralts.toll_m[k] / 1000.0,
                           a.ralts.unpaved_m[k] / 1000.0);
                else
                    printf("  [%d] %-8s (不可行 err=%d)\n", k, a.ralts.tag[k], a.ralts.err[k]);
            }
        }
        if (have_center != 3 || fit_route) {
            app_nav_fit(&a);
            printf("fit-route      zoom %.2f 中心 %.5f,%.5f\n",
                   a.view.zoom, a.view.lat, a.view.lon);
        }
    }

    /* 搜索（阶段 7）：离屏也能查 + 画。断言命中、含关键词、排序单调、够快，
       再加一条像素断言（输入框的白底画出来了没有）。 */
    int have_search = 0;
    if (q_s) {
        int n = app_search_query(&a, q_s);
        a.su.active = 1;                       /* 让 paint_frame 把框画出来 */
        snprintf(a.su.text, sizeof(a.su.text), "%s", q_s);
        printf("search         「%s」%d 条  %llu ms\n", q_s, n,
               (unsigned long long)a.su.elapsed_ms);
        for (int i = 0; i < n && i < 6; i++)
            printf("  %2d  %-22s kind %2d w %2d  %.1f km\n", i,
                   a.su.hits[i].name, a.su.hits[i].kind, a.su.hits[i].weight,
                   a.su.hits[i].dist_m / 1000.0);
        have_search = 1;
        if (n > 0) {
            /* 没指定中心就跳到第一条结果，否则搜的东西多半在画面外 */
            if (have_center != 3 && !fit_route) {
                a.view.lat = a.su.hits[0].lat;
                a.view.lon = a.su.hits[0].lon;
                if (!zoom_given) a.view.zoom = 14.0;
            }
            for (int i = 0; i < n; i++)
                if (!strstr(a.su.hits[i].name, q_s)) {
                    printf("ASSERT-FAIL    结果「%s」不含关键词\n", a.su.hits[i].name);
                    return 1;
                }
            for (int i = 1; i < n; i++)
                if (a.su.hits[i].score < a.su.hits[i - 1].score - 1e-9) {
                    printf("ASSERT-FAIL    搜索结果排序不是升序\n");
                    return 1;
                }
            if (a.su.elapsed_ms > 100) {
                printf("ASSERT-FAIL    搜索 %llu ms 超过 100 ms\n",
                       (unsigned long long)a.su.elapsed_ms);
                return 1;
            }
        } else {
            printf("ASSERT-FAIL    搜索「%s」没有结果\n", q_s);
            return 1;
        }
    }

    /* 模拟导航：离屏也要能跑，不然"车头 / 已走过那段 / 指令条"只能靠人看。
       推进用固定小步长分块（和 GUI 里按帧推进同一条路径），步长太大就会
       跨过路口，画面上看不出"走到哪了"。 */
    int have_sim = 0;
    if (sim) {
        if (!have_route) {
            printf("ASSERT-FAIL    --sim 要配 --from/--to（先有路线才能模拟）\n");
            return 1;
        }
        if (navsim_start(&a.sim, &a.rr) != 0) {
            printf("ASSERT-FAIL    navsim_start 失败\n");
            return 1;
        }
        a.sim_on = 1;
        a.sim_follow = 1;
        a.sim_speed_idx = 3;
        a.sim.speed_mul = 1;
        double left = sim_sec;
        while (left > 0) {
            double dt = left > 0.05 ? 0.05 : left;
            if (navsim_advance(&a.sim, dt)) break;
            left -= dt;
        }
        /* 车头即定位点 */
        navsim_locate(&a.sim, a.sim.car_lat, a.sim.car_lon);
        if (deviate_m > 0) {
            double ola, olo;
            navsim_offset_point(a.sim.car_lat, a.sim.car_lon, a.sim.heading_deg,
                                deviate_m, &ola, &olo);
            navsim_locate(&a.sim, ola, olo);
        }
        have_sim = 1;
        if (have_center != 3 && !fit_route) {
            a.view.lat = a.sim.car_lat;      /* 跟随：以车头为中心 */
            a.view.lon = a.sim.car_lon;
            if (!zoom_given) a.view.zoom = 15.5;
        }
        double to_go = 0;
        int mi = navsim_next_maneuver(&a.sim, &a.man, &to_go, NULL);
        printf("sim            进度 %.3f  剩余 %.0f m / %.0f s  车头 %.5f,%.5f 航向 %.0f°\n",
               navsim_progress(&a.sim), navsim_remaining_m(&a.sim),
               navsim_remaining_s(&a.sim), a.sim.car_lat, a.sim.car_lon,
               a.sim.heading_deg);
        printf("sim 指令       %s（%.0f m 后）%s\n",
               mi >= 0 ? maneuver_kind_text(&a.man.v[mi]) : "(已过完)",
               to_go, navsim_offroute(&a.sim, 0) ? "  【已偏航】" : "");
        /* 车道指示诊断：复刻 window.c 取"下个转向驶入路口的边"的 turn:lanes，
         * 用来确认车道数据确实喂进了底部指令条（b_lanes）。 */
        if (mi >= 0 && mi < a.man.n) {
            int midx = a.man.v[mi].idx;
            if (midx >= 1 && midx < a.rr.n) {
                int seg = midx - 1;
                int prov = a.rr.pts[midx].prov;
                const char *ls = mapset_edge_lanes(&a.ms, prov, a.rr.edge_idx[seg]);
                printf("sim 车道       %s\n", (ls && *ls) ? ls : "(该边无 turn:lanes)");
            }
        }
    }

    SDL_Surface *surf = SDL_CreateRGBSurface(0, w, h, 32, 0, 0, 0, 0);
    if (!surf) { fprintf(stderr, "创建 surface 失败: %s\n", SDL_GetError()); return 1; }

    PaintStats st;
    clock_t t0 = clock();
    if (render_surface(&a, surf, &st) != 0) {
        fprintf(stderr, "渲染失败\n");
        return 1;
    }
    /* 第二帧：热身后的稳态成本（首帧含 bbox 懒建等一次性成本） */
    if (resize_font > 0) {
        int got = app_set_font_size(&a, resize_font);
        printf("resized_font   %d pt\n", got);
        if (got != resize_font) { printf("ASSERT-FAIL    运行期改字号失败\n"); return 1; }
    }
    clock_t t1 = clock();
    render_surface(&a, surf, &st);
    double warm_ms = (double)(clock() - t1) / CLOCKS_PER_SEC * 1000;

    /* 路线叠加：把 route_cli 算出的路径画成红线，一眼看出路线贴不贴路网 */
    size_t nroute = 0;
    if (route) {
        double *rp = read_geojson_line(route, &nroute);
        if (!rp) { printf("ASSERT-FAIL    读不了路线 %s\n", route); return 1; }
        SDL_Renderer *rr = SDL_CreateSoftwareRenderer(surf);
        if (!rr) { printf("ASSERT-FAIL    建不了软渲染器\n"); return 1; }
        SDL_SetRenderDrawColor(rr, 225, 40, 40, 255);
        int drawn_seg = 0;
        for (size_t i = 0; i + 1 < nroute; i++) {
            int x0, y0, x1, y1;
            mv_project(&a.view, rp[2 * i + 1], rp[2 * i], &x0, &y0);
            mv_project(&a.view, rp[2 * i + 3], rp[2 * i + 2], &x1, &y1);
            /* 两端都在视野外的段直接跳过（长途路径绝大多数段都在外面） */
            if ((x0 < -50 && x1 < -50) || (x0 > w + 50 && x1 > w + 50) ||
                (y0 < -50 && y1 < -50) || (y0 > h + 50 && y1 > h + 50))
                continue;
            for (int d = -1; d <= 1; d++) {          /* 3px 粗 */
                SDL_RenderDrawLine(rr, x0 + d, y0, x1 + d, y1);
                SDL_RenderDrawLine(rr, x0, y0 + d, x1, y1 + d);
            }
            drawn_seg++;
        }
        SDL_DestroyRenderer(rr);
        free(rp);
        printf("route          %s（%zu 点，本帧可见 %d 段）\n", route, nroute, drawn_seg);
    }
    printf("render_ms       %.0f  (第2帧 %.0f)\n",
           (double)(clock() - t0) / CLOCKS_PER_SEC * 1000, warm_ms);
    printf("  分解(第2帧)   背景 %.0f  面 %.0f  路 %.0f  线 %.0f  标注 %.0f ms\n",
           st.ms_bg, st.ms_areas, st.ms_roads, st.ms_lines, st.ms_labels);
    if (SDL_SaveBMP(surf, out) != 0) {
        fprintf(stderr, "保存失败: %s\n", SDL_GetError());
        return 1;
    }

    printf("out            %s\n", out);
    printf("font           %s  %d pt\n",
           a.font_used[0] ? a.font_used : "(无)", a.font_size);
    printf("center         %.5f %.5f zoom %.2f\n", a.view.lat, a.view.lon, a.view.zoom);
    {   /* 图集体检：全体 bbox / 视口 / 每省指纹与可选件。多省加载
           （--graph= 父目录）出问题时先看这里：bbox 解析、np/features
           有没有就绪，一眼定位是"没加载"还是"没裁剪"还是"没画"。 */
        double vb[4];
        mv_visible_bbox(&a.view, vb);
        printf("diag bbox_all  %d %d %d %d\n",
               a.ms.bbox[0], a.ms.bbox[1], a.ms.bbox[2], a.ms.bbox[3]);
        printf("diag viewport  %.4f %.4f %.4f %.4f\n", vb[0], vb[1], vb[2], vb[3]);
        for (int i = 0; i < a.ms.n; i++) {
            MapSrc *s = &a.ms.srcs[i];
            printf("diag src %-12s bbox %d %d %d %d feat %d tiles %d np %d\n",
                   s->name, s->bbox[0], s->bbox[1], s->bbox[2], s->bbox[3],
                   s->has_features, s->has_tiles, s->np.ready);
        }
    }
    printf("edges_visited  %llu\n", (unsigned long long)st.visited);
    printf("edges_drawn    %llu\n", (unsigned long long)st.drawn);
    printf("areas_drawn    %llu\n", (unsigned long long)st.areas_drawn);
    printf("lines_drawn    %llu\n", (unsigned long long)st.lines_drawn);
    printf("dedup_skipped  %llu\n", (unsigned long long)st.dedup_skipped);
    printf("place_labels   %llu\n", (unsigned long long)st.labels_drawn);
    printf("poi_icons      %llu\n", (unsigned long long)st.poi_icons);
    printf("road_labels    %llu\n", (unsigned long long)st.road_labels_drawn);

    int fail = 0;
    /* 无地图区域时"画不出路"是预期结果，不能再当失败；
       这时该断言的是"提示横幅画出来了没有"——否则会出现
       "既没地图、也没提示"这种最差情况却显示 ASSERT-OK。
       横幅边框是琥珀色 (255,186,64)，数它的像素即可。 */
    char cov_prov[64];
    double flat, flon;
    app_focus_point(&a, &flat, &flon);      /* 与界面上的提示用同一个点 */
    int cov = mapset_coverage(&a.ms, flat, flon, 3000,
                              cov_prov, sizeof(cov_prov));
    printf("coverage       %d  省=%s\n", cov, cov_prov[0] ? cov_prov : "-");
    if (cov == COV_HAS_ROAD) {
        if (st.drawn == 0) {
            printf("ASSERT-FAIL    一帧都没画出来（视口里没有路？）\n");
            fail = 1;
        }
        long bp = count_color_pixels(surf, 255, 186, 64);
        if (bp > 0) {
            printf("ASSERT-FAIL    有地图却画出了无地图提示横幅（%ld px）\n", bp);
            fail = 1;
        }
    } else {
        long bp = count_color_pixels(surf, 255, 186, 64);
        printf("nomap_banner_px %ld\n", bp);
        if (bp <= 0) {
            printf("ASSERT-FAIL    无地图区域却没有提示横幅（用户会以为程序坏了）\n");
            fail = 1;
        }
    }

    if (have_search) {
        /* 输入框是 (250,250,250) 的白底：框没画出来时会是 0。
           几何算对了但忘了调 paint_search_box 的话，只有这条能抓住。 */
        long boxpix = count_color_pixels(surf, 250, 250, 250);
        printf("search_box_px  %ld\n", boxpix);
        if (boxpix <= 0) { printf("ASSERT-FAIL    画面里没有搜索框\n"); fail = 1; }
    }

    if (have_route) {
        long npix = count_route_pixels(surf);
        printf("route_pixels   %ld\n", npix);
        /* 路线画没画上去，只能靠像素数断言：几何算对了但投影/裁剪写错时，
           里程打印全对、画面上却一条线都没有（这类"数据对、画面错"的回归
           用眼睛最难发现）。 */
        if (npix <= 0) { printf("ASSERT-FAIL    画面里没有路线像素\n"); fail = 1; }
        if (have_sim) {
            long dpix = count_done_pixels(surf);
            printf("done_pixels    %ld（已走过那段的灰色像素）\n", dpix);
            double prog = navsim_progress(&a.sim);
            /* 走过一段之后画面上必须同时有灰和蓝：全是蓝说明"已走过"没画，
               全是灰说明切分点写反了。 */
            if (prog > 0.05 && prog < 0.95) {
                if (dpix <= 0) { printf("ASSERT-FAIL    已走过那段没画成灰色\n"); fail = 1; }
                if (npix <= 0) { printf("ASSERT-FAIL    未走那段没有蓝色像素\n"); fail = 1; }
            }
            if (deviate_m > 60.0 && !navsim_offroute(&a.sim, 0)) {
                printf("ASSERT-FAIL    注入 %.0f m 偏移却没判偏航（实测 %.1f m）\n",
                       deviate_m, a.sim.off_m);
                fail = 1;
            }
        }
        double tol = a.rr.total_m * 0.001 + 1.0;
        if (a.man.n < 2 || fabs(a.man.sum_m - a.rr.total_m) > tol) {
            printf("ASSERT-FAIL    指令里程和 %.1f m != 总里程 %.1f m\n",
                   a.man.sum_m, a.rr.total_m);
            fail = 1;
        }
    }
    if (fail) return 1;
    printf("ASSERT-OK\n");
    return 0;
}
