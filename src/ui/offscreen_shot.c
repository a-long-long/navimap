/* offscreen_shot —— 离屏像素审计通道（skedaddle --shot-offscreen 的实现）
 *
 * 存在理由：本机没有 xvfb-run、也没有 valgrind，GUI 只能由人在 Wayland 会话里
 * 肉眼看。渲染对不对（有没有路、投影歪不歪、LOD 生不生效、浮层透不透明、
 * 轨迹画没画上去）必须能无人值守验证，所以做一个离屏渲染通道：出来的图可以
 * 直接打开看，也可以做像素级断言。
 *
 * ★ 本文件是这条通道的**单一可信源**：skedaddle --shot-offscreen 调
 *   offscreen_shot_main()，regress.sh 的离屏判据（shot / app_route / gpx_render /
 *   ovl_shot）全部走它。旧 map_shot 壳已于 #88 删除。
 *
 * ★ 零 SDL 依赖：早期版本用 SDL_CreateRGBSurface + SDL_SaveBMP，现在改成
 *   malloc 一块 0x00RRGGBB 内存缓冲 + 内建 24 位 BMP 写出（Qt 前端本就不链 SDL，
 *   这条路径必须零 SDL）。
 *
 * 用法：
 *   skedaddle --shot-offscreen --graph=products/graph/beijing --out=/tmp/shot.bmp \
 *             --lat=39.90 --lon=116.40 --zoom=12 --w=1280 --h=800 \
 *             [--from=lat,lon --to=lat,lon --fit-route] \
 *             [--q=<查询词>] [--sim --sim-sec=<秒> --sim-deviate=<米>] \
 *             [--gpx=<文件> ...] [--ovl-alpha=<key>=<pct> ...] \
 *             [--shot-offscreen]  ← 仅 skedaddle 多传一次，本函数忽略
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <stdint.h>

#include "ui/window.h"
#include "map/autobuild.h"   /* AutoBuildOpt（app_init 的 ab 参数；不依赖 tool_common） */
#include "map/gpx.h"         /* --gpx：导入轨迹/收藏后渲染（只进内存） */
#include "ui/overlay.h"      /* --ovl-alpha：逐浮层透明度 */
#include "map/binfmt.h"
#include "map/coverage.h"    /* mapset_coverage / COV_HAS_ROAD */
#include "map/map_view.h"    /* mv_project / mv_visible_bbox */
#include "map/mapset.h"      /* mapset_enable_pbf_display */
#include "utils/plat.h"
#include "render/paint.h"
#include "render/navdraw.h"  /* NAV_ROUTE_* */
#include "ui/audit.h"      /* 共享离屏像素审计（app_offscreen_audit） */
#include "route/navsim.h"    /* 模拟导航 */
#include "route/maneuver.h"  /* maneuver_kind_text */

/* 把一帧渲进调用方持有的内存缓冲（0x00RRGGBB，与 SDL surface / QImage 同源）。
 * 这里不再用 SDL，缓冲自己管（与 MapCanvas::m_px 完全一致的内存布局）。 */
static int render_buffer(App *a, uint8_t *px, int w, int h, PaintStats *st)
{
    if (!px) return -1;
    return app_render_to_buf(a, px, w, h, (size_t)w,
                             0x00FF0000u, 0x0000FF00u, 0x000000FFu, 0x00000000u, st);
}

/* 内建 24 位 BMP 写出：BGR、行 4 字节对齐、自下而上存储（与 ovl_px.py 口径一致）。
   不依赖任何外部库。返回 0=成功，-1=失败。 */
static int write_bmp24(const char *path, const uint8_t *px, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "打不开 %s\n", path); return -1; }
    int stride = (w * 3 + 3) & ~3;
    uint32_t imgsize = (uint32_t)stride * (uint32_t)h;
    uint32_t filesize = 54 + imgsize;
    uint8_t hdr[54];
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    hdr[2] = (uint8_t)(filesize & 0xff); hdr[3] = (uint8_t)((filesize >> 8) & 0xff);
    hdr[4] = (uint8_t)((filesize >> 16) & 0xff); hdr[5] = (uint8_t)((filesize >> 24) & 0xff);
    hdr[10] = 54;                       /* 像素数据偏移 */
    hdr[14] = 40;                      /* BITMAPINFOHEADER 大小 */
    hdr[18] = (uint8_t)(w & 0xff); hdr[19] = (uint8_t)((w >> 8) & 0xff);
    hdr[20] = (uint8_t)((w >> 16) & 0xff); hdr[21] = (uint8_t)((w >> 24) & 0xff);
    hdr[22] = (uint8_t)(h & 0xff); hdr[23] = (uint8_t)((h >> 8) & 0xff);
    hdr[24] = (uint8_t)((h >> 16) & 0xff); hdr[25] = (uint8_t)((h >> 24) & 0xff);
    hdr[26] = 1; hdr[27] = 0;          /* planes */
    hdr[28] = 24; hdr[29] = 0;         /* bpp */
    hdr[34] = (uint8_t)(imgsize & 0xff); hdr[35] = (uint8_t)((imgsize >> 8) & 0xff);
    hdr[36] = (uint8_t)((imgsize >> 16) & 0xff); hdr[37] = (uint8_t)((imgsize >> 24) & 0xff);
    if (fwrite(hdr, 1, 54, f) != (size_t)54) { fclose(f); return -1; }
    uint8_t *row = (uint8_t *)malloc((size_t)stride);
    if (!row) { fclose(f); return -1; }
    for (int y = h - 1; y >= 0; y--) {              /* 自下而上 */
        const uint8_t *src = px + (size_t)y * w * 4;
        for (int x = 0; x < w; x++) {
            row[x * 3 + 0] = src[x * 4 + 0];       /* B */
            row[x * 3 + 1] = src[x * 4 + 1];       /* G */
            row[x * 3 + 2] = src[x * 4 + 2];       /* R */
        }
        for (int p = w * 3; p < stride; p++) row[p] = 0;
        if (fwrite(row, 1, (size_t)stride, f) != (size_t)stride) { free(row); fclose(f); return -1; }
    }
    free(row);
    if (fclose(f) != 0) return -1;
    return 0;
}

/* 前缀匹配取参数值：arg_val("--from=39,116", "--from=") -> "39,116"；不匹配返回 NULL。
   替代 tool_common 的 tc_arg_val（避免 skedaddle 链接 tool_common）。 */
static const char *arg_val(const char *a, const char *pfx)
{
    size_t n = strlen(pfx);
    if (strncmp(a, pfx, n) == 0) return a + n;
    return NULL;
}

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

/* 把画面上一个像素设成给定颜色（0x00RRGGBB）。用于 --route GeoJSON 叠加。 */
static void set_px(uint8_t *px, int w, int x, int y, int r, int g, int b)
{
    if (x < 0 || y < 0 || x >= w) return;
    uint8_t *p = px + ((size_t)y * w + x) * 4;
    p[2] = (uint8_t)r; p[1] = (uint8_t)g; p[0] = (uint8_t)b;
}
/* 3px 粗线（与旧 SDL 软渲染同口径）。 */
static void draw_line(uint8_t *px, int w, int x0, int y0, int x1, int y1, int r, int g, int b)
{
    int dx = abs(x1 - x0), dy = abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        for (int d = -1; d <= 1; d++) {
            if (dx >= dy) set_px(px, w, x0, y0 + d, r, g, b);
            else          set_px(px, w, x0 + d, y0, r, g, b);
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

/* 极简 GeoJSON LineString 读取：只认 "coordinates" 后面的 [[lon,lat],...]。
 * 用途是把 skedaddle_route_cli 算出的路线叠到地图上做视觉验收，不值得引第三方 JSON 库。 */
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

int offscreen_shot_main(int argc, char **argv)
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
    const char *route = NULL;  /* skedaddle_route_cli 导出的 GeoJSON，叠加成红线 */
    const char *from_s = NULL, *to_s = NULL;   /* 进程内算路（阶段 9） */
    int fit_route = 0;
    const char *q_s = NULL;      /* 搜索关键词：画搜索框 + 结果钉子（阶段 7） */
    int sim = 0;                 /* 开模拟导航（画车头 + 已走/未走双色 + 指令条） */
    double sim_sec = 0;          /* 渲染前先推进多少模拟秒 */
    double deviate_m = 0;        /* >0：把定位点朝右拉开这么多米（模拟走错路） */
    int zoom_given = 0;
    const char *dmap_demo = NULL;   /* --dmap-demo=<省>:<pct>：强制某省进入"解码中"占位态，用于截图验证 overlay */
    int dmap_demo_pct = 0;
    const char *gpx_in[8] = {0};    /* --gpx=<文件>：导入轨迹/收藏后渲染（可多次） */
    int settings_ovl = 0;            /* --settings-ovl：出图前打开浮层透明度子面板 */
    int open_tf = 0;                 /* --panel=tf：出图前打开轨迹/收藏管理面板（QA 验收用） */
    int open_menu = 0;               /* --menu：出图前打开主菜单浮层（QA 验收用） */
    struct { NmOverlay id; int pct; } ovl_set[OVL_COUNT] = {{OVL_TITLE,0}};
    int n_ovl = 0;                   /* --ovl-alpha=<key>=<pct>：逐浮层透明度 */
    int n_gpx_in = 0;

    /* 启动自愈（同 navimap）：源 pbf 在、产物没了/坏了就重建。默认开。 */
    AutoBuildOpt ab;
    memset(&ab, 0, sizeof(ab));
    ab.enable = 1;

    for (int i = 1; i < argc; i++) {
        const char *v;
        if ((v = arg_val(argv[i], "--from=")))     from_s = v;
        else if ((v = arg_val(argv[i], "--to=")))  to_s = v;
        else if (strcmp(argv[i], "--fit-route") == 0) fit_route = 1;
        else if ((v = arg_val(argv[i], "--q=")))      q_s = v;
        else if (strcmp(argv[i], "--sim") == 0)       sim = 1;
        else if ((v = arg_val(argv[i], "--sim-sec=")))     sim_sec = atof(v);
        else if ((v = arg_val(argv[i], "--sim-deviate="))) deviate_m = atof(v);
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
        else if (strncmp(argv[i], "--gpx=", 6) == 0) {
            if (n_gpx_in < (int)(sizeof(gpx_in) / sizeof(gpx_in[0])))
                gpx_in[n_gpx_in++] = argv[i] + 6;
            else fprintf(stderr, "提示: --gpx 最多 %d 个，忽略 %s\n",
                         (int)(sizeof(gpx_in) / sizeof(gpx_in[0])), argv[i] + 6);
        }
        /* --ovl-alpha=<key>=<pct>：单独设某个浮层的透明度（key 见 ui/overlay.h，
         * 如 alpha_title / alpha_topbar），可重复（最多 OVL_COUNT 个）。
         * 给无人值守截图的像素判据用：同一视角下 100% 与 75% 两张图，
         * 浮层底板像素必须不同，且差值正好等于"底色按 pct 与地图色混合"。 */
        else if (strcmp(argv[i], "--settings-ovl") == 0) {
            /* 把「浮层透明度」子面板打开再出图：给"每个浮层都能单独设"留个
             * 可视证据（面板本身也按 OVL_SETTINGS 的透明度画）。 */
            settings_ovl = 1;
        }
        else if (strcmp(argv[i], "--panel=tf") == 0) {
            /* 把「轨迹/收藏」管理面板打开再出图：给"管理面板补回来了"留个
             * 可视证据（配合 --gpx 可看到面板里真的列出轨迹/收藏）。 */
            open_tf = 1;
        }
        else if (strcmp(argv[i], "--menu") == 0) {
            /* 把主菜单浮层打开再出图：给"新增的菜单项（导入/导出 GPX、
             * 轨迹收藏）"留个可视证据。 */
            open_menu = 1;
        }
        else if (strncmp(argv[i], "--ovl-alpha=", 12) == 0) {
            if (n_ovl >= (int)(sizeof(ovl_set) / sizeof(ovl_set[0]))) {
                fprintf(stderr, "提示: --ovl-alpha 最多 %d 个\n",
                        (int)(sizeof(ovl_set) / sizeof(ovl_set[0])));
            } else {
                char spec[256];
                snprintf(spec, sizeof(spec), "%s", argv[i] + 12);
                char *eq = strchr(spec, '=');
                if (!eq) { fprintf(stderr, "--ovl-alpha 格式: --ovl-alpha=<key>=<pct>\n"); return 2; }
                *eq = '\0';
                int id = ovl_by_key(spec);
                if (id < 0) { fprintf(stderr, "未知浮层 %s\n", spec); return 2; }
                ovl_set[n_ovl].id = (NmOverlay)id;
                ovl_set[n_ovl].pct = atoi(eq + 1);
                n_ovl++;
            }
        }
        /* --shot-offscreen：仅 skedaddle 调本函数时多传一次，忽略即可。 */
        else if (strcmp(argv[i], "--shot-offscreen") == 0) { /* no-op */ }
        else { fprintf(stderr, "未知参数 %s\n", argv[i]); return 2; }
    }

    App a;
    if (app_init(&a, graph, w, h, font, font_size, &ab) != 0) return 1;
    a.qt_ui = 0;   /* 离屏审计通道：让 C 核心把 chrome（无图横幅/搜索框/标题栏）
                      画进缓冲 —— 审计的计数源必须是 C 核心缓冲，不能换成
                      Qt 控件，否则像素判据失去意义。 */
    if (pbf_display) {
        if (mapset_enable_pbf_display(&a.ms, pbf_display, pbf_lru) != 0) {
            fprintf(stderr, "提示: PBF 显示层不可用（%s），改用 nmbin 产物\n", pbf_display);
        } else {
            printf("显示层: PBF 运行时解码（%s, LRU %d）\n", pbf_display, pbf_lru);
            /* 离屏截图：没有"逐帧渐进"可言，直接同步解码各省。 */
            for (int i = 0; i < a.ms.n; i++)
                mapset_ensure_display(&a.ms, i);
        }
    }
    if (have_center == 3) { a.view.lat = lat; a.view.lon = lon; }
    a.view.zoom = zoom;
    a.view.w = w; a.view.h = h;
    a.max_class = max_class;
    if (settings_ovl) { a.settings_open = 1; a.settings_ovl = 1; }
    if (open_tf) { a.tf_open = 1; }
    if (open_menu) { a.menu_open = 1; }
    /* --ovl-alpha：默认 75% 已在 app_init 里读好，这里再覆盖 */
    for (int i = 0; i < n_ovl; i++)
        app_ovl_alpha_set(&a, ovl_set[i].id, ovl_set[i].pct);

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
            if (midx >= 1 && midx < (int)a.rr.n) {
                int seg = midx - 1;
                int prov = a.rr.pts[midx].prov;
                const char *ls = mapset_edge_lanes(&a.ms, prov, a.rr.edge_idx[seg]);
                printf("sim 车道       %s\n", (ls && *ls) ? ls : "(该边无 turn:lanes)");
            }
        }
    }

    /* GPX 导入：放在**所有会改视图的选项之后**（--from/--to 的 fit-route、
       --q 的跳转、--sim 的跟随都会挪中心），否则轨迹可能被挪出画面。
       只进内存不落库（见文件头说明）。 */
    for (int i = 0; i < n_gpx_in; i++) {
        int n = gpx_parse_file(gpx_in[i], &a.gpx);
        if (n < 0) {
            printf("ASSERT-FAIL    GPX %s 解析失败（rc=%d）\n", gpx_in[i], n);
            return 1;
        }
        printf("gpx            %s -> %d 条轨迹\n", gpx_in[i], n);
    }
    /* 轨迹/收藏的"可见性"判定交给共享审计模块（render/audit.c），
       它读 a.gpx 自己算，保证与 skedaddle --shot-offscreen 同口径。 */

    uint8_t *buf = (uint8_t *)malloc((size_t)w * h * 4);
    if (!buf) { fprintf(stderr, "内存不足\n"); return 1; }

    PaintStats st;
    clock_t t0 = clock();
    if (render_buffer(&a, buf, w, h, &st) != 0) {
        fprintf(stderr, "渲染失败\n");
        free(buf);
        return 1;
    }
    /* 第二帧：热身后的稳态成本（首帧含 bbox 懒建等一次性成本） */
    if (resize_font > 0) {
        int got = app_set_font_size(&a, resize_font);
        printf("resized_font   %d pt\n", got);
        if (got != resize_font) { printf("ASSERT-FAIL    运行期改字号失败\n"); free(buf); return 1; }
    }
    clock_t t1 = clock();
    render_buffer(&a, buf, w, h, &st);
    double warm_ms = (double)(clock() - t1) / CLOCKS_PER_SEC * 1000;

    /* 路线叠加：把 skedaddle_route_cli 算出的路径画成红线，一眼看出路线贴不贴路网 */
    size_t nroute = 0;
    if (route) {
        double *rp = read_geojson_line(route, &nroute);
        if (!rp) { printf("ASSERT-FAIL    读不了路线 %s\n", route); free(buf); return 1; }
        int drawn_seg = 0;
        for (size_t i = 0; i + 1 < nroute; i++) {
            int x0, y0, x1, y1;
            mv_project(&a.view, rp[2 * i + 1], rp[2 * i], &x0, &y0);
            mv_project(&a.view, rp[2 * i + 3], rp[2 * i + 2], &x1, &y1);
            /* 两端都在视野外的段直接跳过（长途路径绝大多数段都在外面） */
            if ((x0 < -50 && x1 < -50) || (x0 > w + 50 && x1 > w + 50) ||
                (y0 < -50 && y1 < -50) || (y0 > h + 50 && y1 > h + 50))
                continue;
            draw_line(buf, w, x0, y0, x1, y1, 225, 40, 40);
            drawn_seg++;
        }
        free(rp);
        printf("route          %s（%zu 点，本帧可见 %d 段）\n", route, nroute, drawn_seg);
    }
    printf("render_ms       %.0f  (第2帧 %.0f)\n",
           (double)(clock() - t0) / CLOCKS_PER_SEC * 1000, warm_ms);
    printf("  分解(第2帧)   背景 %.0f  面 %.0f  路 %.0f  线 %.0f  标注 %.0f ms\n",
           st.ms_bg, st.ms_areas, st.ms_roads, st.ms_lines, st.ms_labels);
    if (write_bmp24(out, buf, w, h) != 0) {
        fprintf(stderr, "保存失败: %s\n", out);
        free(buf);
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
    /* ★ 面要素 LOD 档位的判据锚点（确定性量，不用时间）：
     *   走"小面画点"快速路径的面数。AREA_DOT_PX 定档 4 时这个数会变，
     *   它直接反映"多少面被简化成单个像素"，与机器负载无关。 */
    printf("areas_dotted   %llu\n", (unsigned long long)st.areas_dotted);
    printf("lines_drawn    %llu\n", (unsigned long long)st.lines_drawn);
    printf("dedup_skipped  %llu\n", (unsigned long long)st.dedup_skipped);
    printf("place_labels   %llu\n", (unsigned long long)st.labels_drawn);
    printf("poi_icons      %llu\n", (unsigned long long)st.poi_icons);
    printf("road_labels    %llu\n", (unsigned long long)st.road_labels_drawn);

    /* 像素计数 + 断言 + 日志：交给共享审计模块（render/audit.c）。
       skedaddle --shot-offscreen 也走同一份逻辑，保证两前端判据逐位一致。 */
    AuditCtx actx;
    actx.have_search = have_search;
    actx.have_route  = have_route;
    actx.have_sim    = have_sim;
    actx.deviate_m   = deviate_m;
    actx.n_gpx_in    = n_gpx_in;
    int rc = app_offscreen_audit(&a, buf, w, h, w * 4, &st, &actx);
    free(buf);
    return rc;
}
