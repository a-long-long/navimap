#include "render/labels.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* 路名池（每省一份）                                                    */
/* ------------------------------------------------------------------ */

int name_pool_open(NamePool *np, const char *names_path)
{
    memset(np, 0, sizeof(*np));

    NMBinFile nf;
    if (nmbin_open(names_path, &nf) != 0) return -1;

    const uint8_t *d = nf.data;
    uint32_t cnt = nm_get_u32(d);
    size_t offs_bytes = (size_t)(cnt + 1) * 4;
    if (nf.data_len < 4 + offs_bytes) { nmbin_close(&nf); return -1; }

    np->n = cnt;
    np->off = calloc(cnt + 1, sizeof(uint32_t));
    np->blob = malloc(nf.data_len - 4 - offs_bytes + 1);
    if (!np->off || !np->blob) {
        name_pool_close(np);
        nmbin_close(&nf);
        return -1;
    }
    for (uint32_t i = 0; i <= cnt; i++)
        np->off[i] = nm_get_u32(d + 4 + (size_t)i * 4);
    memcpy(np->blob, d + 4 + offs_bytes, nf.data_len - 4 - offs_bytes);
    np->blob[nf.data_len - 4 - offs_bytes] = '\0';

    nmbin_close(&nf);
    np->ready = 1;
    return 0;
}

void name_pool_close(NamePool *np)
{
    if (!np) return;
    free(np->off);
    free(np->blob);
    memset(np, 0, sizeof(*np));   /* 失败路径的指针都已置空，重复调用安全 */
}

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

/* zoom → 地名等级上限。kind 语义按抽取约定：数值小 = 行政级别高。 */
static int kind_max_for_zoom(double zoom)
{
    if (zoom < 6)  return 1;
    if (zoom < 8)  return 2;
    if (zoom < 10) return 3;
    if (zoom < 12) return 4;
    if (zoom < 13) return 6;
    if (zoom < 14) return 9;
    if (zoom < 15) return 10;
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

void paint_place_labels(Raster *rs, const MapView *v,
                        LabelCache *lc, Font *font, PaintStats *st)
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

    int drawn = 0;
    for (int i = 0; i < lc->n_items; i++) {
        LabelItem *it = &lc->items[i];
        int x, y;
        mv_project(v, it->lat, it->lon, &x, &y);
        if (x < 0 || y < 0 || x >= v->w || y >= v->h) continue;

        int gx = x / g_grid_w, gy = y / g_grid_h;
        if (gx >= GRID_COLS || gy >= GRID_ROWS) continue;
        if (g_grid[gy * GRID_COLS + gx]) continue;
        g_grid[gy * GRID_COLS + gx] = 1;

        int tw = font_text_w(font, it->name);
        font_draw(font, rs, x - tw / 2, y - 8, it->name, 60, 60, 65, 1);
        drawn++;
    }
    if (st) st->labels_drawn = drawn;
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
