#include "render/navdraw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "render/paint.h"
#include "render/raster.h"
#include "route/maneuver.h"

#include "map/coverage.h"

/* 段的两端点 + 颜色。0 = 未走（蓝），1 = 已走过（灰） */
typedef struct { int x0, y0, x1, y1, done; } RSeg;

/* ---- 浮层透明度（本文件里那几个"浮在地图上的面板"共用）----
 * 指令条 / 搜索框 / 导航面板 / 无图提示都由 window.c 画，它们拿不到 App
 * ⇒ 由调用方把该浮层的 alpha 传进来（window.c 用 app_ovl_a()）。
 *
 * ★★ 规则与 chrome.c 一致：**只淡底板，前景（文字 / 图标）保持清晰**。
 *   · 底板、描边（nfill / nrect）：按 alpha 比例淡；
 *   · 文字、图标（ntext / nline）：不参与线性淡出 ——
 *     alpha=0 ⇒ 整个浮层隐藏（前景也不画）；alpha>0 ⇒ 前景完全不透明。
 *   J 先生 2026-10-25 明确要求"只淡底板、文字保持清晰"：前景一起淡的话，
 *   浅色底板上的文字会糊成灰，反而看不清。
 * ★ g_ovl_a 只在下面这几个 paint_* 的入口处设置；其余（路线、车头、图钉）
 *   是地图要素不是浮层，绝不能被它影响。 */
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
static void nline(Raster *rs, int x0, int y0, int x1, int y1,
                  uint8_t r, uint8_t g, uint8_t b)
{
    raster_line_rgba(rs, x0, y0, x1, y1, r, g, b, fg(255));
}

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

void paint_nav_marker(Raster *rs, const MapView *v,
                      double lat, double lon, int is_start, Font *f)
{
    int x, y;
    mv_project(v, lat, lon, &x, &y);
    if (x < -50 || y < -50 || x > v->w + 50 || y > v->h + 50) return;

    /* 白底方块 + 彩色芯：不管压在什么颜色的路/建筑上都看得见 */
    raster_fill_rect(rs, x - 8, y - 8, 17, 17, 255, 255, 255, 255);
    raster_rect(rs, x - 8, y - 8, 17, 17, 40, 40, 40, 255);
    raster_fill_rect(rs, x - 5, y - 5, 11, 11,
                     is_start ? 40 : 226, is_start ? 170 : 52, is_start ? 90 : 52, 255);

    if (f) {
        font_draw(f, rs, x + 12, y - 7, is_start ? "起点" : "终点", 30, 30, 40, 1);
    }
}

/* ---- 路口车道指示 ---- */
static int lane_has(const char *s, size_t n, const char *kw)
{
    size_t kl = strlen(kw);
    if (kl == 0 || n < kl) return 0;
    for (size_t i = 0; i + kl <= n; i++)
        if (memcmp(s + i, kw, kl) == 0) return 1;
    return 0;
}

/* 画一个箭头：dir 0=直行(上) -1=左 +1=右 2=掉头(画成向左下勾) 其它=短竖线。 */
static void draw_lane_arrow(Raster *rs, int cx, int cy, int dir, int R, int G, int B)
{
    uint8_t r = (uint8_t)R, g = (uint8_t)G, b = (uint8_t)B;
    int sh = 9, hd = 5;
    if (dir == 0) {
        raster_line_rgba(rs, cx, cy + sh, cx, cy - sh, r, g, b, 255);
        raster_line_rgba(rs, cx, cy - sh, cx - hd, cy - sh + hd, r, g, b, 255);
        raster_line_rgba(rs, cx, cy - sh, cx + hd, cy - sh + hd, r, g, b, 255);
    } else if (dir < 0) {
        raster_line_rgba(rs, cx, cy + sh, cx, cy - 2, r, g, b, 255);
        raster_line_rgba(rs, cx, cy - 2, cx - (sh + 2), cy - 2, r, g, b, 255);
        raster_line_rgba(rs, cx - (sh + 2), cy - 2, cx - (sh + 2) + hd, cy - 2 - hd, r, g, b, 255);
        raster_line_rgba(rs, cx - (sh + 2), cy - 2, cx - (sh + 2) + hd, cy - 2 + hd, r, g, b, 255);
    } else if (dir == 2) {
        /* 掉头：向上再折回下方，用一个小环近似 */
        raster_line_rgba(rs, cx, cy + sh, cx, cy - 3, r, g, b, 255);
        raster_line_rgba(rs, cx, cy - 3, cx + 3, cy - 3, r, g, b, 255);
        raster_line_rgba(rs, cx + 3, cy - 3, cx + 3, cy + 3, r, g, b, 255);
        raster_line_rgba(rs, cx + 3, cy + 3, cx + 1, cy + 1, r, g, b, 255);
        raster_line_rgba(rs, cx + 3, cy + 3, cx + 5, cy + 1, r, g, b, 255);
    } else if (dir > 0) {
        raster_line_rgba(rs, cx, cy + sh, cx, cy - 2, r, g, b, 255);
        raster_line_rgba(rs, cx, cy - 2, cx + (sh + 2), cy - 2, r, g, b, 255);
        raster_line_rgba(rs, cx + (sh + 2), cy - 2, cx + (sh + 2) - hd, cy - 2 - hd, r, g, b, 255);
        raster_line_rgba(rs, cx + (sh + 2), cy - 2, cx + (sh + 2) - hd, cy - 2 + hd, r, g, b, 255);
    } else {
        raster_line_rgba(rs, cx, cy - 6, cx, cy + 6, r, g, b, 255);
    }
}

/* 在 banner_top 上方画一排车道格，把"能走这条转向"的车道高亮。
   lanes 形如 "left|through|through|right"。turn_dir 见 NavBanner 注释。 */
static void paint_lane_arrows(Raster *rs, const MapView *v,
                              const char *lanes, int turn_dir, int banner_top)
{
    if (!rs || !lanes || !*lanes) return;

    int n = 1;
    for (const char *p = lanes; *p; p++) if (*p == '|') n++;
    if (n <= 1) return;              /* 单车道没什么可指示的 */
    if (n > 8) n = 8;

    const int bw = 30, bh = 44, gap = 5;
    int total = n * bw + (n - 1) * gap;
    int x0 = v->w / 2 - total / 2;
    int y = banner_top - bh - 10;
    if (y < 4) return;

    raster_fill_rect(rs, x0 - 8, y - 6, total + 16, bh + 12, 18, 26, 42, 214);
    raster_rect(rs, x0 - 8, y - 6, total + 16, bh + 12, 90, 160, 255, 255);

    const char *p = lanes;
    for (int i = 0; i < n; i++) {
        const char *bar = p;
        while (*p && *p != '|') p++;
        size_t len = (size_t)(p - bar);
        if (*p == '|') p++;

        int has_l = lane_has(bar, len, "left");
        int has_r = lane_has(bar, len, "right");
        int has_t = lane_has(bar, len, "through") || lane_has(bar, len, "straight");
        int has_u = lane_has(bar, len, "uturn") || lane_has(bar, len, "u_turn")
                 || lane_has(bar, len, "reverse");

        int hot = 0;
        if (turn_dir == 0 && has_t) hot = 1;
        else if (turn_dir == -1 && (has_l || has_u)) hot = 1;
        else if (turn_dir == 1 && has_r) hot = 1;

        int x = x0 + i * (bw + gap);
        if (hot) raster_fill_rect(rs, x, y, bw, bh, 30, 92, 182, 240);
        else     raster_fill_rect(rs, x, y, bw, bh, 44, 52, 68, 240);
        if (hot) raster_rect(rs, x, y, bw, bh, 130, 195, 255, 255);
        else     raster_rect(rs, x, y, bw, bh, 105, 118, 138, 255);

        /* 该格画哪种箭头：多向时优先左>右>直行，全无关键词则画短竖线 */
        int dir;
        if (has_l)      dir = -1;
        else if (has_r) dir = 1;
        else if (has_u) dir = 2;
        else if (has_t) dir = 0;
        else            dir = -99;

        int cx = x + bw / 2, cy = y + bh / 2;
        if (hot) draw_lane_arrow(rs, cx, cy, dir, 236, 246, 255);
        else     draw_lane_arrow(rs, cx, cy, dir, 150, 158, 172);
    }
}

/* 底部导航条：左边大字号转向 + 小字号"多远/进哪条路"，右边剩余里程与 ETA。
   高度固定，宽度按内容取（上限 760），屏幕窄时自动收窄。 */
void paint_nav_banner(Raster *rs, const MapView *v, Font *f, Font *fbig,
                      int line_h, int line_h_big, const NavBanner *b,
                      uint8_t ovl_a)
{
    if (!rs || !f || !b) return;
    g_ovl_a = ovl_a;   /* 整条指令条（含上方车道指示）按该浮层的透明度画 */
    const char *turn = b->turn ? b->turn : "";
    const char *dist = b->dist ? b->dist : "";
    const char *road = b->road ? b->road : "";
    const char *rem  = b->remain ? b->remain : "";
    const char *eta  = b->eta ? b->eta : "";
    if (!*turn && !*dist && !*rem) return;

    Font *fb = fbig ? fbig : f;
    int lhb = fbig ? line_h_big : line_h;

    /* 下面那行小字："300 米后 · 进入建国路" */
    char sub[192];
    if (*dist && *road) snprintf(sub, sizeof(sub), "%s · %s", dist, road);
    else                snprintf(sub, sizeof(sub), "%s%s", dist, road);

    int w_turn = font_text_w(fb, turn);
    int w_sub  = font_text_w(f, sub);
    int w_rem  = font_text_w(f, rem);
    int w_eta  = font_text_w(f, eta);
    int left_w = w_turn > w_sub ? w_turn : w_sub;
    int right_w = w_rem > w_eta ? w_rem : w_eta;
    int gap = 40;

    int bw = 24 + left_w + gap + right_w + 24;
    if (bw > v->w - 40) bw = v->w - 40;
    if (bw > 760) bw = 760;
    int bh = 16 + lhb + 6 + line_h + 12;
    int bx = (v->w - bw) / 2;
    int by = v->h - bh - 16;

    /* 车道指示画在指令条上方（有 turn:lanes 才出现） */
    paint_lane_arrows(rs, v, b->lanes, b->turn_dir, by);

    nfill(rs, bx, by, bw, bh, 18, 26, 42, 232);
    nrect(rs, bx, by, bw, bh, 90, 160, 255, 255);

    int ty = by + 10;
    ntext(fb, rs, bx + 20, ty, turn, 255, 255, 255, 0);
    ntext(f,  rs, bx + 20, ty + lhb + 4, sub, 198, 210, 228, 0);

    int rx = bx + bw - 20;
    if (*rem) ntext(f, rs, rx - w_rem, ty + 2, rem, 150, 220, 160, 0);
    if (*eta) ntext(f, rs, rx - w_eta, ty + 2 + line_h + 2, eta, 150, 220, 160, 0);
}

/* 搜索框：顶栏居中。输入框一行（大字号）+ 结果列表。
   宽度按最长的一行结果收，上限 560，屏幕窄时自动让位。 */
void paint_search_box(Raster *rs, const MapView *v, Font *f, Font *fbig,
                      int line_h, int line_h_big, const SearchBoxView *sb,
                      uint8_t ovl_a)
{
    if (!rs || !f || !sb || !sb->active) return;
    g_ovl_a = ovl_a;   /* 搜索框 + 结果列表整体半透明 */
    const char *text    = sb->text    ? sb->text    : "";
    const char *editing = sb->editing ? sb->editing : "";
    int show = sb->n_hits;
    int maxshow = sb->show_max > 0 ? sb->show_max : 8;
    if (show > maxshow) show = maxshow;

    Font *fb = fbig ? fbig : f;
    int lhb = fbig ? line_h_big : line_h;

    /* 结果行文本："中关村东路        3.3 公里" —— 距离另起一列右对齐 */
    char rows[16][128];
    char dists[16][32];
    int w_row = 0, w_dist = 0;
    for (int i = 0; i < show; i++) {
        char nm[96];
        snprintf(nm, sizeof(nm), "%s", sb->hits[i].name);
        snprintf(rows[i], sizeof(rows[i]), "%s", nm);
        if (sb->hits[i].dist_m >= 0) {
            char d[32];
            fmt_distance(sb->hits[i].dist_m, d, sizeof(d));
            snprintf(dists[i], sizeof(dists[i]), "%s", d);
        } else dists[i][0] = '\0';
        int w1 = font_text_w(f, rows[i]);
        int w2 = font_text_w(f, dists[i]);
        if (w1 > w_row) w_row = w1;
        if (w2 > w_dist) w_dist = w2;
    }

    int box_h = 16 + lhb + 12;                  /* 输入框本体 */
    int row_h = line_h + 6;
    int bx_pad = 14;
    int bw = 24 + w_row + (w_dist ? 20 + w_dist : 0) + 24 + bx_pad;
    int w_text = font_text_w(fb, text) + font_text_w(fb, editing) + 40;
    if (bw < w_text) bw = w_text;
    if (bw > v->w - 80) bw = v->w - 80;
    if (bw > 560) bw = 560;
    int bx = (v->w - bw) / 2, by = 12;
    int bh = box_h + show * row_h + 8;

    nfill(rs, bx, by, bw, bh, 20, 28, 44, 236);
    nrect(rs, bx, by, bw, bh, 110, 170, 255, 255);

    /* 输入框：白底 + 已提交文本（黑）+ 组合中文本（灰，带下划线感） */
    nfill(rs, bx + 12, by + 8, bw - 24, lhb + 6, 250, 250, 250, 255);
    int tw = font_text_w(fb, text);
    ntext(fb, rs, bx + 18, by + 11, text, 20, 24, 34, 0);
    if (*editing)
        ntext(fb, rs, bx + 18 + tw, by + 11, editing, 90, 120, 190, 0);
    /* 光标 */
    nfill(rs, bx + 18 + tw + font_text_w(fb, editing) + 1, by + 12,
          2, lhb, 40, 90, 190, 255);

    for (int i = 0; i < show; i++) {
        int ry = by + box_h + i * row_h;
        if (i == sb->sel)
            nfill(rs, bx + 6, ry - 2, bw - 12, row_h, 44, 92, 190, 235);
        ntext(f, rs, bx + 20, ry, rows[i], 236, 240, 248, 0);
        if (dists[i][0]) {
            int wd = font_text_w(f, dists[i]);
            ntext(f, rs, bx + bw - 16 - wd, ry, dists[i], 160, 200, 255, 0);
        }
    }
    if (sb->n_hits > show) {
        char more[48];
        snprintf(more, sizeof(more), "…还有 %d 条", sb->n_hits - show);
        ntext(f, rs, bx + 20, by + box_h + show * row_h, more, 150, 165, 190, 0);
    }
}

void paint_search_pin(Raster *rs, const MapView *v,
                      double lat, double lon, int selected, Font *f,
                      const char *label)
{
    int x, y;
    mv_project(v, lat, lon, &x, &y);
    if (x < -60 || y < -60 || x > v->w + 60 || y > v->h + 60) return;

    raster_fill_rect(rs, x - 7, y - 7, 15, 15, 255, 255, 255, 255);
    raster_rect(rs, x - 7, y - 7, 15, 15, 40, 44, 56, 255);
    raster_fill_rect(rs, x - 4, y - 4, 9, 9,
                     selected ? 30 : 150, selected ? 110 : 158,
                     selected ? 235 : 175, 255);
    if (f && label && *label)
        font_draw(f, rs, x + 11, y - 7, label, 30, 34, 48, 1);
}

/* 缩放按钮：右下角竖排两个方块。Z+ 在上、Z- 在下。
   ★ 命中判定必须和绘制共用这一份几何，否则"画在这、点在那"最难查。 */
#define ZB_SIZE 36
#define ZB_MARGIN 14

static void zoom_button_rects(const MapView *v, RectI *in, RectI *out)
{
    int x = v->w - ZB_MARGIN - ZB_SIZE;
    int yb = v->h - ZB_MARGIN - ZB_SIZE;         /* 下面那个（缩小） */
    int yt = yb - ZB_SIZE - 8;                   /* 上面那个（放大） */
    if (in)  *in  = (RectI){ x, yt, ZB_SIZE, ZB_SIZE };
    if (out) *out = (RectI){ x, yb, ZB_SIZE, ZB_SIZE };
}

void paint_zoom_buttons(Raster *rs, const MapView *v, Font *f)
{
    if (!rs) return;
    RectI ri, ro;
    zoom_button_rects(v, &ri, &ro);

    RectI *rects[2] = { &ri, &ro };
    for (int i = 0; i < 2; i++) {
        raster_fill_rect(rs, rects[i]->x, rects[i]->y, rects[i]->w, rects[i]->h,
                        250, 250, 250, 235);
        raster_rect(rs, rects[i]->x, rects[i]->y, rects[i]->w, rects[i]->h,
                   60, 70, 90, 255);
    }

    if (f) {
        int w1 = font_text_w(f, "+"), w2 = font_text_w(f, "-");
        font_draw(f, rs, ri.x + (ZB_SIZE - w1) / 2, ri.y + 6, "+", 30, 40, 60, 0);
        font_draw(f, rs, ro.x + (ZB_SIZE - w2) / 2, ro.y + 6, "-", 30, 40, 60, 0);
    }
}

int zoom_button_hit(const MapView *v, int px, int py)
{
    RectI ri, ro;
    zoom_button_rects(v, &ri, &ro);
    if (px >= ri.x && px <= ri.x + ri.w && py >= ri.y && py <= ri.y + ri.h) return 1;
    if (px >= ro.x && px <= ro.x + ro.w && py >= ro.y && py <= ro.y + ro.h) return -1;
    return 0;
}

void paint_nav_panel(Raster *rs, const RouteResult *rr,
                     const ManeuverList *ml, Font *f, int line_h,
                     int x, int y, int max_lines, const char *status,
                     uint8_t ovl_a)
{
    if (!rs || !f) return;
    g_ovl_a = ovl_a;   /* 左上导航信息面板整体半透明 */
    if (max_lines < 1) max_lines = 1;
    if (max_lines > 12) max_lines = 12;

    char lines[12][176];
    int  n_lines = 0;

    char head[176];
    if (status && *status) {
        snprintf(head, sizeof(head), "%s", status);
    } else if (rr && rr->ok) {
        char d[32];
        fmt_distance(rr->total_m, d, sizeof(d));
        int mins = (int)(rr->total_s / 60.0 + 0.5);
        if (mins >= 60)
            snprintf(head, sizeof(head), "全程 %s  约 %d 小时 %d 分",
                     d, mins / 60, mins % 60);
        else
            snprintf(head, sizeof(head), "全程 %s  约 %d 分钟", d, mins);
    } else {
        snprintf(head, sizeof(head), "左键点起点、再点终点；右键清除");
    }
    snprintf(lines[n_lines++], sizeof(lines[0]), "%s", head);

    if (ml) {
        for (int i = 0; i < ml->n && n_lines < max_lines; i++) {
            const Maneuver *m = &ml->v[i];
            if (m->type == MAN_START) continue;      /* 起点行已被标题覆盖 */
            char t[128];
            maneuver_text(m, t, sizeof(t));
            snprintf(lines[n_lines++], sizeof(lines[0]), "%s", t);
        }
    }

    int wmax = 0;
    for (int i = 0; i < n_lines; i++) {
        int tw = font_text_w(f, lines[i]);
        if (tw > wmax) wmax = tw;
    }
    int bw = wmax + 20, bh = n_lines * (line_h + 4) + 12;
    if (bw > 520) bw = 520;

    nfill(rs, x, y, bw, bh, 24, 32, 48, 225);
    nrect(rs, x, y, bw, bh, 235, 200, 90, 255);

    for (int i = 0; i < n_lines; i++) {
        int ly = y + 6 + i * (line_h + 4);
        if (i == 0) ntext(f, rs, x + 10, ly, lines[i], 255, 214, 102, 0);
        else        ntext(f, rs, x + 10, ly, lines[i], 238, 240, 245, 0);
    }
    (void)rr;
}

void paint_nomap_banner(Raster *rs, const MapView *v, Font *f, int line_h,
                        int cov_state, uint8_t ovl_a)
{
    if (!rs || !v) return;
    if (cov_state != COV_NONE && cov_state != COV_BBOX_ONLY) return;
    g_ovl_a = ovl_a;   /* 顶部无图提示条整体半透明 */

    const char *lines[3];
    int n = 3;
    if (cov_state == COV_NONE) {
        lines[0] = "该区域没有地图数据";
        lines[1] = "下载该地区的 OSM 数据（*.osm.pbf）放进 sources/，再运行 ./scripts/sync_maps.sh";
        lines[2] = "https://download.geofabrik.de/asia/china/";
    } else {
        lines[0] = "附近没有路网数据（水域 / 尚未采集的区域）";
        lines[1] = "如需该区域地图，下载 OSM 数据放进 sources/ 后运行 ./scripts/sync_maps.sh";
        lines[2] = "https://extract.bbbike.org/";
    }

    int wmax = 0;
    if (f) for (int i = 0; i < n; i++) {
        int tw = font_text_w(f, lines[i]);
        if (tw > wmax) wmax = tw;
    } else {
        wmax = 520;
    }
    int bw = wmax + 24;
    if (bw > v->w - 40) bw = v->w - 40;
    if (bw < 200) bw = 200;
    int bh = (f ? n * (line_h + 4) : 2 * line_h) + 14;
    int bx = (v->w - bw) / 2;
    int by = 12;

    nfill(rs, bx, by, bw, bh, 48, 34, 8, 232);
    nrect(rs, bx, by, bw, bh, 255, 186, 64, 255);

    if (!f) return;
    for (int i = 0; i < n; i++) {
        int ly = by + 7 + i * (line_h + 4);
        if (i == 0) ntext(f, rs, bx + 12, ly, lines[0], 255, 208, 96, 0);
        else        ntext(f, rs, bx + 12, ly, lines[i], 238, 240, 245, 0);
    }
}
