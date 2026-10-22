#include "render/navdraw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "render/paint.h"
#include "route/maneuver.h"

#include "map/coverage.h"

/* 段的两端点 + 颜色。0 = 未走（蓝），1 = 已走过（灰） */
typedef struct { int x0, y0, x1, y1, done; } RSeg;

int paint_route(SDL_Renderer *r, const MapView *v,
                const int32_t *lat_e7, const int32_t *lon_e7, uint32_t n,
                int width, int done_seg, double done_t)
{
    if (!r || !v || n < 2) return 0;
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
        for (uint32_t i = 0; i < ns; i++) {
            int x0 = sg[i].x0, y0 = sg[i].y0, x1 = sg[i].x1, y1 = sg[i].y1;
            if ((x0 < -margin && x1 < -margin) ||
                (x0 > W + margin && x1 > W + margin) ||
                (y0 < -margin && y1 < -margin) ||
                (y0 > H + margin && y1 > H + margin))
                continue;
            if (pass == 0) {
                SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
            } else if (sg[i].done) {
                /* 已走过：压暗成灰蓝，和前方未走的蓝一眼能分开 */
                SDL_SetRenderDrawColor(r, 130, 138, 150, 255);
            } else {
                SDL_SetRenderDrawColor(r, NAV_ROUTE_R, NAV_ROUTE_G,
                                       NAV_ROUTE_B, 255);
            }
            paint_thick_line(r, x0, y0, x1, y1, w);
            if (pass == 1 && !sg[i].done) drawn++;
        }
    }
    free(sg);
    free(xy);
    return drawn;
}

/* 三角形填充：SDL 没有多边形填充，软件渲染下直接按重心坐标逐点画。
   车头只有 30×30 像素，这点循环比引一套多边形光栅化划算。 */
static void fill_triangle(SDL_Renderer *r, int x0, int y0, int x1, int y1,
                          int x2, int y2)
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
                SDL_RenderDrawPoint(r, x, y);
        }
    }
}

void paint_nav_car(SDL_Renderer *r, const MapView *v,
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

    SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
    fill_triangle(r, wtx, wty, w1x, w1y, w2x, w2y);
    SDL_SetRenderDrawColor(r, 20, 90, 210, 255);
    fill_triangle(r, tipx, tipy, b1x, b1y, b2x, b2y);
}

void paint_nav_marker(SDL_Renderer *r, const MapView *v,
                      double lat, double lon, int is_start, Font *f)
{
    int x, y;
    mv_project(v, lat, lon, &x, &y);
    if (x < -50 || y < -50 || x > v->w + 50 || y > v->h + 50) return;

    /* 白底方块 + 彩色芯：不管压在什么颜色的路/建筑上都看得见 */
    SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
    SDL_Rect out = { x - 8, y - 8, 17, 17 };
    SDL_RenderFillRect(r, &out);
    SDL_SetRenderDrawColor(r, 40, 40, 40, 255);
    SDL_RenderDrawRect(r, &out);

    SDL_SetRenderDrawColor(r, is_start ? 40 : 226, is_start ? 170 : 52,
                           is_start ? 90 : 52, 255);
    SDL_Rect in = { x - 5, y - 5, 11, 11 };
    SDL_RenderFillRect(r, &in);

    if (f) {
        int tw = font_text_w(f, is_start ? "起点" : "终点");
        font_draw(f, r, x + 12, y - 7, is_start ? "起点" : "终点",
                  30, 30, 40, 1);
        (void)tw;
    }
}

/* 底部导航条：左边大字号转向 + 小字号"多远/进哪条路"，右边剩余里程与 ETA。
   高度固定，宽度按内容取（上限 760），屏幕窄时自动收窄。 */
void paint_nav_banner(SDL_Renderer *r, const MapView *v, Font *f, Font *fbig,
                      int line_h, int line_h_big, const NavBanner *b)
{
    if (!r || !f || !b) return;
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

    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 18, 26, 42, 232);
    SDL_Rect box = { bx, by, bw, bh };
    SDL_RenderFillRect(r, &box);
    SDL_SetRenderDrawColor(r, 90, 160, 255, 255);
    SDL_RenderDrawRect(r, &box);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);

    int ty = by + 10;
    font_draw(fb, r, bx + 20, ty, turn, 255, 255, 255, 0);
    font_draw(f,  r, bx + 20, ty + lhb + 4, sub, 198, 210, 228, 0);

    int rx = bx + bw - 20;
    if (*rem) font_draw(f, r, rx - w_rem, ty + 2, rem, 150, 220, 160, 0);
    if (*eta) font_draw(f, r, rx - w_eta, ty + 2 + line_h + 2, eta, 150, 220, 160, 0);
}

/* 搜索框：顶栏居中。输入框一行（大字号）+ 结果列表。
   宽度按最长的一行结果收，上限 560，屏幕窄时自动让位。 */
void paint_search_box(SDL_Renderer *r, const MapView *v, Font *f, Font *fbig,
                      int line_h, int line_h_big, const SearchBoxView *sb)
{
    if (!r || !f || !sb || !sb->active) return;
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

    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 20, 28, 44, 236);
    SDL_Rect box = { bx, by, bw, bh };
    SDL_RenderFillRect(r, &box);
    SDL_SetRenderDrawColor(r, 110, 170, 255, 255);
    SDL_RenderDrawRect(r, &box);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);

    /* 输入框：白底 + 已提交文本（黑）+ 组合中文本（灰，带下划线感） */
    SDL_SetRenderDrawColor(r, 250, 250, 250, 255);
    SDL_Rect ib = { bx + 12, by + 8, bw - 24, lhb + 6 };
    SDL_RenderFillRect(r, &ib);
    int tw = font_text_w(fb, text);
    font_draw(fb, r, ib.x + 6, ib.y + 3, text, 20, 24, 34, 0);
    if (*editing)
        font_draw(fb, r, ib.x + 6 + tw, ib.y + 3, editing, 90, 120, 190, 0);
    /* 光标 */
    SDL_SetRenderDrawColor(r, 40, 90, 190, 255);
    SDL_Rect cur = { ib.x + 6 + tw + font_text_w(fb, editing) + 1, ib.y + 4,
                     2, lhb };
    SDL_RenderFillRect(r, &cur);

    for (int i = 0; i < show; i++) {
        int ry = by + box_h + i * row_h;
        if (i == sb->sel) {
            SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(r, 44, 92, 190, 235);
            SDL_Rect hr = { bx + 6, ry - 2, bw - 12, row_h };
            SDL_RenderFillRect(r, &hr);
            SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
        }
        int fg_r = 236, fg_g = 240, fg_b = 248;
        font_draw(f, r, bx + 20, ry, rows[i], (uint8_t)fg_r, (uint8_t)fg_g,
                  (uint8_t)fg_b, 0);
        if (dists[i][0]) {
            int wd = font_text_w(f, dists[i]);
            font_draw(f, r, bx + bw - 16 - wd, ry, dists[i], 160, 200, 255, 0);
        }
    }
    if (sb->n_hits > show) {
        char more[48];
        snprintf(more, sizeof(more), "…还有 %d 条", sb->n_hits - show);
        font_draw(f, r, bx + 20, by + box_h + show * row_h, more,
                  150, 165, 190, 0);
    }
}

void paint_search_pin(SDL_Renderer *r, const MapView *v,
                      double lat, double lon, int selected, Font *f,
                      const char *label)
{
    int x, y;
    mv_project(v, lat, lon, &x, &y);
    if (x < -60 || y < -60 || x > v->w + 60 || y > v->h + 60) return;

    SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
    SDL_Rect out = { x - 7, y - 7, 15, 15 };
    SDL_RenderFillRect(r, &out);
    SDL_SetRenderDrawColor(r, 40, 44, 56, 255);
    SDL_RenderDrawRect(r, &out);
    SDL_SetRenderDrawColor(r, selected ? 30 : 150, selected ? 110 : 158,
                           selected ? 235 : 175, 255);
    SDL_Rect in = { x - 4, y - 4, 9, 9 };
    SDL_RenderFillRect(r, &in);
    if (f && label && *label)
        font_draw(f, r, x + 11, y - 7, label, 30, 34, 48, 1);
}

/* 缩放按钮：右下角竖排两个方块。Z+ 在上、Z- 在下。
   ★ 命中判定必须和绘制共用这一份几何，否则"画在这、点在那"最难查。 */
#define ZB_SIZE 36
#define ZB_MARGIN 14

static void zoom_button_rects(const MapView *v, SDL_Rect *in, SDL_Rect *out)
{
    int x = v->w - ZB_MARGIN - ZB_SIZE;
    int yb = v->h - ZB_MARGIN - ZB_SIZE;         /* 下面那个（缩小） */
    int yt = yb - ZB_SIZE - 8;                   /* 上面那个（放大） */
    if (in)  *in  = (SDL_Rect){ x, yt, ZB_SIZE, ZB_SIZE };
    if (out) *out = (SDL_Rect){ x, yb, ZB_SIZE, ZB_SIZE };
}

void paint_zoom_buttons(SDL_Renderer *r, const MapView *v, Font *f)
{
    if (!r) return;
    SDL_Rect ri, ro;
    zoom_button_rects(v, &ri, &ro);

    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_Rect *rs[2] = { &ri, &ro };
    for (int i = 0; i < 2; i++) {
        SDL_SetRenderDrawColor(r, 250, 250, 250, 235);
        SDL_RenderFillRect(r, rs[i]);
        SDL_SetRenderDrawColor(r, 60, 70, 90, 255);
        SDL_RenderDrawRect(r, rs[i]);
    }
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);

    if (f) {
        int w1 = font_text_w(f, "+"), w2 = font_text_w(f, "-");
        font_draw(f, r, ri.x + (ZB_SIZE - w1) / 2, ri.y + 6, "+", 30, 40, 60, 0);
        font_draw(f, r, ro.x + (ZB_SIZE - w2) / 2, ro.y + 6, "-", 30, 40, 60, 0);
    }
}

int zoom_button_hit(const MapView *v, int px, int py)
{
    SDL_Rect ri, ro;
    zoom_button_rects(v, &ri, &ro);
    if (px >= ri.x && px <= ri.x + ri.w && py >= ri.y && py <= ri.y + ri.h) return 1;
    if (px >= ro.x && px <= ro.x + ro.w && py >= ro.y && py <= ro.y + ro.h) return -1;
    return 0;
}

/* 面板底色用的是半透明深蓝：必须先开 BLENDMODE_BLEND，画完一定要关回去，
   否则后面画的路面/标注会带着 alpha，颜色全变（软件渲染器不吃 alpha
   叠加之外的任何"半透明"暗示）。 */
void paint_nav_panel(SDL_Renderer *r, const RouteResult *rr,
                     const ManeuverList *ml, Font *f, int line_h,
                     int x, int y, int max_lines, const char *status)
{
    if (!r || !f) return;
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

    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 24, 32, 48, 225);
    SDL_Rect box = { x, y, bw, bh };
    SDL_RenderFillRect(r, &box);
    SDL_SetRenderDrawColor(r, 235, 200, 90, 255);
    SDL_RenderDrawRect(r, &box);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);

    for (int i = 0; i < n_lines; i++) {
        int ly = y + 6 + i * (line_h + 4);
        if (i == 0) font_draw(f, r, x + 10, ly, lines[i], 255, 214, 102, 0);
        else        font_draw(f, r, x + 10, ly, lines[i], 238, 240, 245, 0);
    }
    (void)rr;
}

void paint_nomap_banner(SDL_Renderer *r, const MapView *v, Font *f, int line_h,
                        int cov_state)
{
    if (!r || !v) return;
    if (cov_state != COV_NONE && cov_state != COV_BBOX_ONLY) return;

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

    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 48, 34, 8, 232);
    SDL_Rect box = { bx, by, bw, bh };
    SDL_RenderFillRect(r, &box);
    SDL_SetRenderDrawColor(r, 255, 186, 64, 255);
    SDL_RenderDrawRect(r, &box);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);

    if (!f) return;
    for (int i = 0; i < n; i++) {
        int ly = by + 7 + i * (line_h + 4);
        if (i == 0) font_draw(f, r, bx + 12, ly, lines[0], 255, 208, 96, 0);
        else        font_draw(f, r, bx + 12, ly, lines[i], 238, 240, 245, 0);
    }
}
