/* audit.c —— 离屏像素审计（共享模块）
 *
 * 把 tools/map_shot.c 的"像素计数 + 断言 + 日志打印"搬到对 RGB 字节序缓冲
 * （C 核心地图缓冲 / SDL surface / QImage::Format_RGB32 同源）的判定上，
 * 让 map_shot 与 navmap_qt --shot 共用同一份逻辑，保证两前端像素判据逐位一致。
 *
 * ★ 计数一律按**字节**比色：p[2]=R, p[1]=G, p[0]=B，alpha 字节忽略。
 *   这样无论缓冲来自 SDL surface（alpha 字节 = 0x00）还是 QImage::Format_RGB32
 *   （alpha 字节 = 0xFF），只要 R/G/B 三字节相同，计数就完全相同 —— 跨两种
 *   缓冲零偏差，也绕开 SDL_MapRGBA / 字节序带来的精确不相等。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "render/audit.h"    /* 自带 ui/window.h：App + PaintStats + 本函数原型 */
#include "render/navdraw.h"  /* NAV_ROUTE_* */
#include "map/coverage.h"    /* mapset_coverage / COV_HAS_ROAD */
#include "map/map_view.h"    /* mv_project */
#include "map/gpx.h"         /* GpxTrack / GpxStore */
#include "route/navsim.h"    /* navsim_progress / navsim_offroute */

/* 按颜色数像素（软件渲染器不做混合，画上去是什么颜色就是什么颜色）。
   搜索框、指令条这类"框画没画出来"的断言都靠它。 */
static long count_color_px(const uint8_t *buf, int stride, int w, int h,
                           int cr, int cg, int cb)
{
    long n = 0;
    for (int y = 0; y < h; y++) {
        const uint8_t *row = buf + (size_t)y * stride;
        for (int x = 0; x < w; x++) {
            const uint8_t *p = row + x * 4;
            if (p[2] == (uint8_t)cr && p[1] == (uint8_t)cg && p[0] == (uint8_t)cb)
                n++;
        }
    }
    return n;
}

/* 数"接近某颜色"的像素（每通道容差 tol）。
   为什么需要：浮层现在是**半透明**的（默认 75%），它画在画面上的颜色是
   "原色 × alpha + 底图色 × (1−alpha)" —— 用原色精确计数会数到 0。 */
static long count_near_color_px(const uint8_t *buf, int stride, int w, int h,
                                int cr, int cg, int cb, int tol)
{
    long n = 0;
    for (int y = 0; y < h; y++) {
        const uint8_t *row = buf + (size_t)y * stride;
        for (int x = 0; x < w; x++) {
            const uint8_t *p = row + x * 4;
            if (abs((int)p[2] - cr) <= tol && abs((int)p[1] - cg) <= tol &&
                abs((int)p[0] - cb) <= tol)
                n++;
        }
    }
    return n;
}

/* 数某个**浮层元素**的像素：按它当前的透明度算出与底图背景混合后的实际颜色。
 * ★ 判据随用户设置走 —— 用户把该浮层调成 30%，这条也不会假红（而不是
 *   反过来把"用户改了设置"判成回归）。 */
static long count_overlay_px(const uint8_t *buf, int stride, int w, int h,
                             int cr, int cg, int cb, int intrinsic_a, int ovl_a)
{
    const int BG[3] = { 244, 241, 234 };      /* paint_background 的底色 */
    /* 有效 alpha = 元素自身底色 alpha × 浮层透明度（两层都是 0~255）。
     * ★ 只按浮层 alpha 算会错：横幅边框 (255,186,64) 是画在**横幅填充**之上的，
     *   不是画在底图上的 ⇒ 按底图混合算出来的期望色根本不存在（实测数到 0）。
     *   所以这里数的是**填充**（它才直接压在底图上）。 */
    const double k = (intrinsic_a / 255.0) * (ovl_a / 255.0);
    int er = (int)(cr * k + BG[0] * (1.0 - k) + 0.5);
    int eg = (int)(cg * k + BG[1] * (1.0 - k) + 0.5);
    int eb = (int)(cb * k + BG[2] * (1.0 - k) + 0.5);
    return count_near_color_px(buf, stride, w, h, er, eg, eb, 8);
}

/* 在 C 核心地图缓冲上做全套像素计数 + 断言 + 日志打印。
 * 返回 0=全部 ASSERT-OK；1=有 ASSERT-FAIL。打印行与 map_shot.c 逐字一致。 */
int app_offscreen_audit(const App *a, const uint8_t *buf, int w, int h, int stride,
                        const PaintStats *st, const AuditCtx *c)
{
    /* GPX 可见性判定（与 map_shot 同口径，用 a->view 投影）。
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
