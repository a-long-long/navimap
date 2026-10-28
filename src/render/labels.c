#include "render/labels.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/* 地名标注                                                             */
/* ------------------------------------------------------------------ */

int label_cache_init(LabelCache *lc, const char *db_path)
{
    memset(lc, 0, sizeof(*lc));
    if (sqlite3_open_v2(db_path, &lc->db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        fprintf(stderr, "地名库打不开 %s（无地名标注）\n", db_path);
        lc->db = NULL;
        return -1;
    }
    return 0;
}

void label_cache_free(LabelCache *lc)
{
    if (lc->db) sqlite3_close(lc->db);
    free(lc->items);
    memset(lc, 0, sizeof(*lc));
}

/* ===========================================================================
 * 最近的上级行政区（"这是哪儿"）
 *
 * 地名库里没有省界/市界多边形，只有带坐标的点 ⇒ 只能报"最近的那个市/区"。
 * 这在城区完全够用；在省界、市界附近可能报成邻省邻市的，调用方要按"大致
 * 位置"来用（头文件里已就此写明）。
 *
 * 性能：place 表有 place_latlon(lat,lon) 索引，所以先用 lat/lon 的矩形粗筛
 * （能走索引），再在候选集里按真实平方距离排序。半径按级放大，一找到就停 ——
 * 城区第一级 0.08°(≈9 km) 就命中，荒漠才会一路放大到 6°。
 * 只在"无图区提示"这条路径上调用（位置移动 450 m 才重算一次），不在渲染热路径。
 * ========================================================================= */
static int nearest_admin_point(sqlite3 *db, double lat, double lon,
                               int kind_lo, int kind_hi, char *out, size_t cap)
{
    if (out && cap) out[0] = '\0';
    if (!db) return 0;

    /* 经度方向按 cos(lat) 压缩后再比距离，否则中纬度会把东西方向拉长 30% */
    double clat = cos(lat * 3.14159265358979 / 180.0);
    double kx = clat * clat;
    if (kx < 0.01) kx = 0.01;

    /* 半径只放大到约 67 km（0.6°）：再大就会在无人区报出几百公里外的市名，
     * 那种"反查结果"比不报更有害。城区第一级 0.08°(≈9 km) 基本就命中了。 */
    static const double RADII[] = { 0.08, 0.25, 0.60 };
    const char *sql =
        "SELECT name FROM place"
        " WHERE kind BETWEEN ?1 AND ?2"
        "   AND lat BETWEEN ?3 AND ?4 AND lon BETWEEN ?5 AND ?6"
        " ORDER BY (lat-?7)*(lat-?7) + (lon-?8)*(lon-?8)*?9"
        " LIMIT 1";

    for (size_t r = 0; r < sizeof(RADII) / sizeof(RADII[0]); r++) {
        double d = RADII[r];
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
        sqlite3_bind_int(st, 1, kind_lo);
        sqlite3_bind_int(st, 2, kind_hi);
        sqlite3_bind_double(st, 3, lat - d);
        sqlite3_bind_double(st, 4, lat + d);
        sqlite3_bind_double(st, 5, lon - d);
        sqlite3_bind_double(st, 6, lon + d);
        sqlite3_bind_double(st, 7, lat);
        sqlite3_bind_double(st, 8, lon);
        sqlite3_bind_double(st, 9, kx);

        int rc = sqlite3_step(st);
        int got = 0;
        if (rc == SQLITE_ROW) {
            const unsigned char *nm = sqlite3_column_text(st, 0);
            if (nm && out && cap) {
                snprintf(out, cap, "%s", (const char *)nm);
                got = 1;
            }
        }
        sqlite3_finalize(st);
        if (got) return 1;
    }
    return 0;
}

int label_nearest_admin(LabelCache *lc, double lat, double lon,
                        char *city, size_t ccap,
                        char *district, size_t dcap)
{
    if (city && ccap)     city[0]     = '\0';
    if (district && dcap) district[0] = '\0';
    if (!lc || !lc->db) return 0;
    if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) return 0;

    /* 市级：kind 1 = 省会/直辖市城区，2 = 地级市 */
    int n = nearest_admin_point(lc->db, lat, lon, 1, 2, city, ccap);
    /* 区县级：kind 3 = 县 / 镇 */
    n += nearest_admin_point(lc->db, lat, lon, 3, 3, district, dcap);
    return n;
}

/* zoom → 地名等级上限。kind 语义按抽取约定：数值小 = 行政级别高。
 * 2026-10-24 调整：把 POI（kind 11）的可见起点从 z15 下探到 z13，
 * 让"POI 图标"在城区尺度就能看到（此前只在 z15 才画到，基本看不见）。 */
static int kind_max_for_zoom(double zoom)
{
    if (zoom < 6)  return 1;
    if (zoom < 8)  return 2;
    if (zoom < 10) return 3;
    if (zoom < 11) return 4;
    if (zoom < 12) return 6;
    if (zoom < 13) return 9;
    if (zoom < 14) return 11;     /* POI(kind 11) 自 z13 起可见 */
    return 12;
}

/* 视口变化超过 20% 才重查 SQL；否则平移缩放只重投影 */
static int bbox_moved_far(const double *a, const double *b)
{
    double dw = (a[2] - a[0]) + (a[3] - a[1]);
    if (dw <= 1e-12) return 1;
    double d = 0;
    for (int i = 0; i < 4; i++) {
        double x = (a[i] - b[i]) / dw;
        if (x < 0) x = -x;
        if (x > d) d = x;
    }
    return d > 0.2;
}

static int refresh_items(LabelCache *lc, const MapView *v)
{
    double bb[4];
    mv_visible_bbox(v, bb);
    int kind_max = kind_max_for_zoom(v->zoom);

    lc->loaded = 1;
    lc->cached_kindmax = kind_max;
    memcpy(lc->cached_bb, bb, sizeof(bb));
    if (!lc->db) { lc->n_items = 0; return 0; }

    /* 外扩 30%：小幅平移不重查 */
    double dla = (bb[2] - bb[0]) * 0.3 + 1e-6;
    double dlo = (bb[3] - bb[1]) * 0.3 + 1e-6;

    const char *sql =
        "SELECT name,lat,lon,kind,weight FROM place "
        "WHERE lat BETWEEN ?1 AND ?2 AND lon BETWEEN ?3 AND ?4 AND kind<=?5 "
        "ORDER BY weight DESC LIMIT 400";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(lc->db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_double(st, 1, bb[0] - dla);
    sqlite3_bind_double(st, 2, bb[2] + dla);
    sqlite3_bind_double(st, 3, bb[1] - dlo);
    sqlite3_bind_double(st, 4, bb[3] + dlo);
    sqlite3_bind_int(st, 5, kind_max);

    lc->n_items = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (lc->n_items >= lc->cap_items) {
            int ncap = lc->cap_items ? lc->cap_items * 2 : 256;
            LabelItem *ni = realloc(lc->items, (size_t)ncap * sizeof(LabelItem));
            if (!ni) break;
            lc->items = ni; lc->cap_items = ncap;
        }
        LabelItem *it = &lc->items[lc->n_items];
        const unsigned char *nm = sqlite3_column_text(st, 0);
        snprintf(it->name, sizeof(it->name), "%s", nm ? (const char *)nm : "");
        it->lat    = sqlite3_column_double(st, 1);
        it->lon    = sqlite3_column_double(st, 2);
        it->kind   = sqlite3_column_int(st, 3);
        it->weight = sqlite3_column_int(st, 4);
        lc->n_items++;
    }
    sqlite3_finalize(st);
    return 0;
}

/* 网格避让：每格最多一个标签（先到先得，SQL 已按 weight 排序）。
 * 格子尺寸按字号自适应 —— 固定 110x34 的话，字号调大后标签会互相压字，
 * 调小又会稀到浪费空间。以单个汉字宽度为单位估算。 */
#define GRID_COLS 64
#define GRID_ROWS 40

static uint8_t g_grid[GRID_COLS * GRID_ROWS];
static int g_grid_w = 110, g_grid_h = 34;

static void grid_for_font(Font *font)
{
    int cw = font ? font_text_w(font, "\xe4\xb8\xad") : 14;   /* "中" */
    if (cw <= 0) cw = 14;
    g_grid_w = cw * 8;   if (g_grid_w < 60) g_grid_w = 60;
    g_grid_h = (int)(cw * 2.4); if (g_grid_h < 22) g_grid_h = 22;
}

/* ------------------------------------------------------------------ */
/* POI / 地名图标（矢量标记，按类别着色 + 形状区分）                     */
/* ------------------------------------------------------------------ */

typedef enum { SH_DISC, SH_TRI, SH_DIAMOND } PoiShape;

/* kind 语义见 places_build.c：数值小 = 行政级别高。按类别给颜色与形状。 */
static void poi_style(int kind, uint8_t *r, uint8_t *g, uint8_t *b, PoiShape *sh)
{
    if      (kind <= 2) { *r = 214; *g = 48;  *b = 48;  *sh = SH_DISC; }   /* 首都/直辖市 红 */
    else if (kind <= 4) { *r = 232; *g = 128; *b = 24;  *sh = SH_DISC; }   /* 市/镇 橙 */
    else if (kind <= 7) { *r = 236; *g = 192; *b = 32;  *sh = SH_DISC; }   /* 村/屯 金 */
    else if (kind == 9) { *r = 72;  *g = 182; *b = 96;  *sh = SH_DISC; }   /* 街区 绿 */
    else if (kind == 10){ *r = 40;  *g = 162; *b = 172; *sh = SH_TRI; }     /* 自然地物 青三角 */
    else if (kind == 11){ *r = 40;  *g = 120; *b = 232; *sh = SH_DISC; }   /* POI 蓝圆 */
    else                { *r = 150; *g = 150; *b = 160; *sh = SH_DIAMOND; }  /* 其他地物 灰菱 */
}

/* 实心圆（逐扫描线填充一条 hspan）。 */
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

/* 凸多边形扫描线填充（三角 / 菱形）。 */
static void fill_poly(Raster *rs, const int *px, const int *py, int n, uint32_t c)
{
    int ymin = py[0], ymax = py[0];
    for (int i = 1; i < n; i++) {
        if (py[i] < ymin) ymin = py[i];
        if (py[i] > ymax) ymax = py[i];
    }
    if (ymin < 0) ymin = 0;
    if (ymax >= rs->h) ymax = rs->h - 1;
    for (int y = ymin; y <= ymax; y++) {
        double yc = y + 0.5;
        int xs[8]; int nx = 0;
        for (int i = 0; i < n; i++) {
            int j = (i + 1) % n;
            if ((py[i] <= yc && py[j] > yc) || (py[j] <= yc && py[i] > yc)) {
                xs[nx++] = (int)(px[i] +
                           (yc - py[i]) / (double)(py[j] - py[i]) * (px[j] - px[i]));
            }
        }
        for (int a = 0; a + 1 < nx; a++)
            for (int b = a + 1; b < nx; b++)
                if (xs[b] < xs[a]) { int t = xs[a]; xs[a] = xs[b]; xs[b] = t; }
        for (int a = 0; a + 1 < nx; a += 2)
            raster_hspan(rs, y, xs[a], xs[a + 1] + 1, c);
    }
}

static void shape_fill(Raster *rs, int x, int y, int t, PoiShape sh, uint32_t c)
{
    if (sh == SH_DISC) { fill_disc(rs, x, y, t, c); return; }
    if (sh == SH_TRI) {
        int px[3] = { x, x - t + 1, x + t - 1 };
        int py[3] = { y - t, y + t, y + t };
        fill_poly(rs, px, py, 3, c);
        return;
    }
    int px[4] = { x, x + t, x, x - t };
    int py[4] = { y - t, y, y + t, y };
    fill_poly(rs, px, py, 4, c);
}

/* 画一个 POI 标记：深灰描边 + 白底 + 类别色芯，紧贴 (x,y) 坐标点。 */
static void draw_poi_marker(Raster *rs, int x, int y, int kind)
{
    uint8_t r, g, b; PoiShape sh;
    poi_style(kind, &r, &g, &b, &sh);
    uint32_t dark  = raster_rgb(rs, 40, 42, 48);
    uint32_t white = raster_rgb(rs, 255, 255, 255);
    uint32_t col   = raster_rgb(rs, r, g, b);
    int R = 6;
    shape_fill(rs, x, y, R + 1, sh, dark);
    shape_fill(rs, x, y, R,     sh, white);
    shape_fill(rs, x, y, R - 2, sh, col);
}

void paint_place_labels(Raster *rs, const MapView *v,
                        LabelCache *lc, Font *font, PaintStats *st,
                        int show_poi, int show_labels)
{
    if (!lc || !font) return;
    double bb[4];
    mv_visible_bbox(v, bb);
    int kind_max = kind_max_for_zoom(v->zoom);

    if (!lc->loaded || lc->cached_kindmax != kind_max ||
        bbox_moved_far(lc->cached_bb, bb)) {
        if (refresh_items(lc, v) != 0) return;
    }

    memset(g_grid, 0, sizeof(g_grid));
    grid_for_font(font);

    int drawn = 0, icons = 0;
    for (int i = 0; i < lc->n_items; i++) {
        LabelItem *it = &lc->items[i];
        int x, y;
        mv_project(v, it->lat, it->lon, &x, &y);
        if (x < 0 || y < 0 || x >= v->w || y >= v->h) continue;

        int gx = x / g_grid_w, gy = y / g_grid_h;
        if (gx >= GRID_COLS || gy >= GRID_ROWS) continue;
        if (g_grid[gy * GRID_COLS + gx]) continue;
        g_grid[gy * GRID_COLS + gx] = 1;

        /* 图标与标签独立开关，但共用一个避让格（任一个显示就占格，避免重叠） */
        if (show_poi)   { draw_poi_marker(rs, x, y, it->kind); icons++; }
        if (show_labels){
            font_draw(font, rs, x + 10, y - 8, it->name, 46, 48, 56, 1);
            drawn++;
        }
    }
    if (st) { st->labels_drawn = drawn; st->poi_icons = icons; }
}

/* ------------------------------------------------------------------ */
/* 路名标注：消费 paint_roads 收集的屏幕内有名边                          */
/* ------------------------------------------------------------------ */

void paint_road_labels(Raster *rs, const MapView *v, Font *font,
                       PaintStats *st)
{
    if (!font || v->zoom < 15.0) return;

    const RoadLabel *lbl; int n;
    road_labels_get(&lbl, &n);

    memset(g_grid, 0, sizeof(g_grid));
    grid_for_font(font);

    /* 同名只标一次（哈希位图）。注意 name_id 只在本省 names.bin 内唯一，
       不同省的同名路（如"建设路"）各自带着自己的池，哈希到同一位会互相
       去重掉——这是可接受的：同屏出现两条同名路，标一条反而更干净。 */
    static uint8_t seen[1 << 14];
    memset(seen, 0, sizeof(seen));

    int drawn = 0;
    for (int i = 0; i < n; i++) {
        uint32_t id = lbl[i].name_id;
        if (id == 0 || (id & 0xFFFF) < 0x1000) { /* 简易哈希位 */ }
        uint32_t bit = (id * 2654435761u) & 0xFFFF;
        uint32_t byte = bit >> 3, mask = 1u << (bit & 7);
        if (seen[byte] & mask) continue;

        const NamePool *np = lbl[i].np;
        const char *s = (np && np->ready && id <= np->n)
                        ? np->blob + np->off[id] : NULL;
        if (!s || !*s) continue;

        int tw = font_text_w(font, s);
        int x = lbl[i].x - tw / 2, y = lbl[i].y - 7;
        if (x < 0 || y < 0 || x + tw >= v->w || y >= v->h) continue;
        int gx = lbl[i].x / g_grid_w, gy = lbl[i].y / g_grid_h;
        if (gx >= GRID_COLS || gy >= GRID_ROWS) continue;
        if (g_grid[gy * GRID_COLS + gx]) continue;
        g_grid[gy * GRID_COLS + gx] = 1;
        seen[byte] |= mask;

        /* 路名用深蓝灰，与地名黑灰区分 */
        font_draw(font, rs, x, y, s, 70, 80, 110, 1);
        drawn++;
    }
    if (st) st->road_labels_drawn = drawn;
}
