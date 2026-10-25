#include "render/trackdraw.h"

#include <stdlib.h>
#include <math.h>

/* 轨迹线宽：与 paint_route 同思路（远了细一点）。 */
static int track_width(const MapView *v)
{
    return v->zoom >= 13.0 ? 4 : (v->zoom >= 11.0 ? 3 : 2);
}

int paint_tracks(Raster *rs, const MapView *v, const GpxStore *g)
{
    if (!rs || !v || !g || g->n_trk <= 0) return 0;
    int drawn_total = 0;
    int w = track_width(v);
    int W = v->w, H = v->h;
    const int margin = 64;   /* 端点允许在视口外一点，避免边缘的轨迹断掉 */

    for (int t = 0; t < g->n_trk; t++) {
        const GpxTrack *tk = &g->trk[t];
        if (tk->n < 2) continue;

        /* 投影一次，两遍复用（白描边 + 彩芯）。 */
        int *xy = malloc((size_t)tk->n * 2 * sizeof(int));
        if (!xy) continue;
        for (uint32_t i = 0; i < tk->n; i++)
            mv_project(v, tk->lat_e7[i] / 1e7, tk->lon_e7[i] / 1e7,
                       &xy[2 * i], &xy[2 * i + 1]);

        uint32_t white = raster_rgb(rs, 255, 255, 255);
        uint32_t col   = raster_rgb(rs, tk->r, tk->g, tk->b);

        for (int pass = 0; pass < 2; pass++) {
            int lw = pass == 0 ? w + 2 : w;
            uint32_t c = pass == 0 ? white : col;
            for (uint32_t i = 0; i + 1 < tk->n; i++) {
                int x0 = xy[2 * i],     y0 = xy[2 * i + 1];
                int x1 = xy[2 * i + 2], y1 = xy[2 * i + 3];
                /* 两端都在视口外（含 margin）⇒ 整段跳过。判断写成一个表达式，
                 * 避免逐段调 raster_clip_line 的开销（轨迹点数可能上万）。 */
                if ((x0 < -margin && x1 < -margin) ||
                    (x0 > W + margin && x1 > W + margin) ||
                    (y0 < -margin && y1 < -margin) ||
                    (y0 > H + margin && y1 > H + margin))
                    continue;
                raster_thick_line(rs, x0, y0, x1, y1, lw, c);
                if (pass == 1) drawn_total++;
            }
        }
        free(xy);
    }
    return drawn_total;
}

/* 实心圆（照 labels.c 的 fill_disc：逐扫描线一条 hspan）。 */
static void fill_disc(Raster *rs, int cx, int cy, int R, uint32_t c)
{
    if (R <= 0) { raster_point(rs, cx, cy, c); return; }
    for (int dy = -R; dy <= R; dy++) {
        int y = cy + dy;
        if ((unsigned)y >= (unsigned)rs->h) continue;
        int dx = (int)sqrtf((float)(R * R - dy * dy));
        raster_hspan(rs, y, cx - dx, cx + dx + 1, c);
    }
}

int paint_favorites(Raster *rs, const MapView *v, const GpxStore *g, Font *f)
{
    if (!rs || !v || !g || g->n_wpt <= 0) return 0;
    uint32_t white = raster_rgb(rs, 255, 255, 255);
    uint32_t dark  = raster_rgb(rs, 60, 50, 20);
    uint32_t gold  = raster_rgb(rs, 245, 190, 40);   /* 收藏 = 金色，和 POI 蓝圆区分 */
    int n = 0;
    for (int i = 0; i < g->n_wpt; i++) {
        const GpxWpt *w = &g->wpt[i];
        int x, y;
        mv_project(v, w->lat, w->lon, &x, &y);
        if (x < -16 || y < -16 || x > v->w + 16 || y > v->h + 16) continue;
        fill_disc(rs, x, y, 6, dark);
        fill_disc(rs, x, y, 5, white);
        fill_disc(rs, x, y, 3, gold);
        n++;
        if (!f || !w->name[0]) continue;
        int tw = font_text_w(f, w->name);
        int tx = x + 9, ty = y - 7;
        if (tx < 0 || ty < 0 || tx + tw >= v->w || ty >= v->h) continue;
        font_draw(f, rs, tx, ty, w->name, 70, 60, 20, 1);
    }
    return n;
}
