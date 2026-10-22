#include "render/paint.h"

#include "map/binfmt.h"
#include "map/coordkey.h"
#include "map/road_tags.h"
#include "utils/geo.h"
#include "render/labels.h"
#include "render/raster.h"

/* ------------------------------------------------------------------ */
/* 路名收集（paint_roads 顺带记录，labels.c 消费）                       */
/* ------------------------------------------------------------------ */

#define ROAD_LABEL_CAP 4096
static RoadLabel g_rl[ROAD_LABEL_CAP];
static int       g_rl_n;

void road_labels_reset(void)          { g_rl_n = 0; }
void road_labels_get(const RoadLabel **out, int *n) { *out = g_rl; *n = g_rl_n; }
void road_labels_add(const NamePool *np, uint32_t name_id, int x, int y)
{
    if (g_rl_n >= ROAD_LABEL_CAP) return;
    g_rl[g_rl_n].np = np;
    g_rl[g_rl_n].name_id = name_id;
    g_rl[g_rl_n].x = x;
    g_rl[g_rl_n].y = y;
    g_rl_n++;
}

/* 路的"退化成单像素"去重：每帧开头复位一次。
   ★ 必须在 paint_frame 里调（不是 paint_roads 里）：多省时 paint_roads
     每省调一次；而 g_last_px 跨帧不清会出 holes——背景每帧重刷，上一帧
     写过的像素这帧是空的，若拿上一帧的状态去判"已写过"就会漏画。 */
static int g_last_px = -1, g_last_tag = -1;
void paint_roadpix_begin(void) { g_last_px = -1; g_last_tag = -1; }

/* 路面配色：高等级路更醒目，低等级路压暗 */
typedef struct { uint8_t r, g, b; uint8_t width; } Style;

static const Style kStyle[5] = {
    { 214,  94,  40, 4 },   /* 0 高速/国道：橙红 */
    { 240, 176,  64, 3 },   /* 1 主干：橙黄 */
    { 252, 226, 150, 2 },   /* 2 次干：浅黄 */
    { 150, 150, 150, 1 },   /* 3 支路/小路：灰 */
    { 190, 190, 190, 1 },   /* 4 其他 */
};

/* ------------------------------------------------------------------ */
/* 跨省去重集合（64 位指纹 + 开放寻址 + 世代标记，槽 = 1 条 cache line） */
/* ------------------------------------------------------------------ */

static uint64_t dd_mix(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

static int dd_alloc(DedupSet *d, size_t cap)
{
    DdSlot *m = calloc(cap, sizeof(DdSlot));
    if (!m) return -1;
    d->slot = m;
    d->cap = cap;
    d->mask = cap - 1;
    d->used = 0;
    return 0;
}

void dedup_init(DedupSet *d)
{
    memset(d, 0, sizeof(*d));
    /* 起步 64K 槽（1 MB），不够按需翻倍，够用后不缩回 */
    if (dd_alloc(d, 1u << 16) != 0) memset(d, 0, sizeof(*d));
    d->cur = 1;
}

void dedup_free(DedupSet *d)
{
    if (!d || !d->slot) return;
    free(d->slot);
    memset(d, 0, sizeof(*d));
}

/* 扩容：把当前世代还活着的条目重新散列到更大的表。
   分配失败就退回旧表（表会偏满，去重率略降，不会错）。 */
static void dd_grow(DedupSet *d)
{
    size_t ocap = d->cap;
    DdSlot *os = d->slot;
    uint32_t cur = d->cur;

    if (dd_alloc(d, ocap * 2) != 0) {
        /* 分不到更大的表：退回旧的，并把 used 顶到满载让调用方别再试 */
        d->slot = os;
        d->cap = ocap; d->mask = ocap - 1; d->used = ocap; d->cur = cur;
        return;
    }
    d->cur = cur;
    for (size_t i = 0; i < ocap; i++) {
        if (os[i].gen != cur) continue;
        size_t j = (size_t)(os[i].key & d->mask);
        while (d->slot[j].gen == cur) j = (j + 1) & d->mask;
        d->slot[j].key = os[i].key;
        d->slot[j].gen = cur;
        d->used++;
    }
    free(os);
}

void dedup_begin(DedupSet *d)
{
    if (!d || !d->slot) return;
    d->cur++;
    /* 世代回绕：整表清零并重新从 1 开始，避免和"从未使用"的 gen=0 混淆 */
    if (d->cur == 0 || d->cur == UINT32_MAX) {
        memset(d->slot, 0, d->cap * sizeof(DdSlot));
        d->cur = 1;
    }
    d->used = 0;
}

int dedup_seen(DedupSet *d, uint64_t hi, uint64_t lo)
{
    if (!d || !d->slot) return 0;       /* 未启用（单省）：一律当首次 */
    uint64_t key = dd_mix(hi ^ dd_mix(lo));
    size_t i = (size_t)(key & d->mask);
    for (;;) {
        if (d->slot[i].gen != d->cur) { /* 空槽：登记并返回"首次" */
            d->slot[i].key = key;
            d->slot[i].gen = d->cur;
            d->used++;
            if (d->used * 4 >= d->cap * 3) dd_grow(d);
            return 0;
        }
        if (d->slot[i].key == key) return 1;    /* 画过了 */
        i = (i + 1) & d->mask;
    }
}

void paint_background(SDL_Renderer *r, int w, int h)
{
    SDL_SetRenderDrawColor(r, 244, 241, 234, 255);
    SDL_Rect rc = { 0, 0, w, h };
    SDL_RenderFillRect(r, &rc);
}

/* 粗线：SDL_RenderDrawLine 只有 1 像素宽，这里用竖向偏移画 n 条近似加粗。
   横向/近水平的线加粗效果差，但路网里绝大多数段是斜的，观感够用。
   路线叠加（navdraw.c）也用同一份，别在别处另写。 */
void paint_thick_line(SDL_Renderer *r, int x0, int y0, int x1, int y1, int width)
{
    if (width <= 1) { SDL_RenderDrawLine(r, x0, y0, x1, y1); return; }
    int half = width / 2;
    for (int d = -half; d <= half; d++) {
        if (width % 2 == 0 && d == half) continue;
        SDL_RenderDrawLine(r, x0, y0 + d, x1, y1 + d);
    }
    /* 近垂直的线补横向偏移 */
    if (abs(x1 - x0) < abs(y1 - y0)) {
        for (int d = -half; d <= half; d++) {
            if (width % 2 == 0 && d == half) continue;
            SDL_RenderDrawLine(r, x0 + d, y0, x1 + d, y1);
        }
    }
}

/* ------------------------------------------------------------------ */
/* 「把连续边合并成折线」试过了，没采纳（留档，省得下次又想到它）      */
/*                                                                     */
/* 设想：除了岔路口，多数节点的后继边唯一，可以把首尾相接的边接成一条   */
/*   折线，用一次 SDL_RenderDrawLines 画完，顺带把顶点投影减半。        */
/*   实现过一版（node → 唯一出/入边的开放寻址表 + 两遍行走），实测：    */
/*     - 合并率很好：z9 273770 条边 → 44435 条折线，平均每条 6.16 边    */
/*     - 但更慢：z9 路网 81 → 113 ms，z11 43 → 54 ms（+30 ms）          */
/*   原因（用临时诊断开关把两项成本分开量出来的）：                     */
/*     ①  机器代价：为了找"后继"，每条边要做 2 次哈希插入 + 若干次查询， */
/*        随机探表几乎每次都是一次访存缺失，摊到每条边 ~110 ns，超过收益 */
/*     ②  收益原本就被高估：以为省的是"绘制调用次数"，可 DrawLines 相对  */
/*        DrawLine 并不便宜（真正贵的是逐像素光栅化，像素一个都没少）；   */
/*        真正能省的只有"接点被重复投影那次"，而查表版投影上线后          */
/*        投影本身只剩 9 ms（路网 80 ms 的 11%），减半也才 ~4.5 ms。      */
/*   ⇒ 除非能不查表就找后继（例如边记录里带 way id + 段序号），别再做。  */
/* ------------------------------------------------------------------ */

/* 视口覆盖的 Z14 瓦片范围（±1 格 margin）。clamp 进目录范围。 */
typedef struct {
    int      tx_lo, tx_hi, ty_lo, ty_hi;
    uint32_t minx, miny, nx;
} TileRange;

static int tile_range(const NMBinFile *tiles, const MapView *v, TileRange *tr)
{
    /* tiles.bin payload：rec0 = {minx,miny}，rec1 = {nx,ny}，其后 ntile 条目录 */
    uint32_t minx = nm_get_u32(tiles->data + TF_EDGE_OFF);
    uint32_t miny = nm_get_u32(tiles->data + TF_EDGE_CNT);
    uint32_t nx   = nm_get_u32(tiles->data + NM_TILE_REC_BYTES + TF_EDGE_OFF);
    uint32_t ny   = nm_get_u32(tiles->data + NM_TILE_REC_BYTES + TF_EDGE_CNT);
    uint64_t ntile_dir = tiles->hdr.rec_count - 2;
    if (nx == 0 || ny == 0 || (uint64_t)nx * ny > ntile_dir)
        return 0;                               /* 目录不完整，放弃 */

    double bb[4];
    mv_visible_bbox(v, bb);
    double wxa, wya, wxb, wyb;
    nm_mercator(bb[0], bb[1], &wxa, &wya);
    nm_mercator(bb[2], bb[3], &wxb, &wyb);

    double Z = (double)(1u << NM_TILE_Z);
    int tx_lo = (int)( (wxa < wxb ? wxa : wxb) * Z) - 1;
    int tx_hi = (int)( (wxa > wxb ? wxa : wxb) * Z) + 1;
    int ty_lo = (int)( (wya < wyb ? wya : wyb) * Z) - 1;
    int ty_hi = (int)( (wya > wyb ? wya : wyb) * Z) + 1;
    if (tx_lo < (int)minx) tx_lo = (int)minx;
    if (ty_lo < (int)miny) ty_lo = (int)miny;
    if (tx_hi > (int)(minx + nx - 1)) tx_hi = (int)(minx + nx - 1);
    if (ty_hi > (int)(miny + ny - 1)) ty_hi = (int)(miny + ny - 1);

    tr->tx_lo = tx_lo; tr->tx_hi = tx_hi;
    tr->ty_lo = ty_lo; tr->ty_hi = ty_hi;
    tr->minx = minx; tr->miny = miny; tr->nx = nx;
    return 1;
}

/* 某省的索引里，(tx,ty) 这一格有多少条边——用来判"这格有几个省有数据"。
   0 = 该省不覆盖这格（或索引不可用）。 */
static uint32_t tile_cnt_at(const NMBinFile *tiles, int tx, int ty)
{
    if (!tiles || tiles->hdr.rec_count < 2) return 0;
    uint32_t minx = nm_get_u32(tiles->data + TF_EDGE_OFF);
    uint32_t miny = nm_get_u32(tiles->data + TF_EDGE_CNT);
    uint32_t nx   = nm_get_u32(tiles->data + NM_TILE_REC_BYTES + TF_EDGE_OFF);
    uint32_t ny   = nm_get_u32(tiles->data + NM_TILE_REC_BYTES + TF_EDGE_CNT);
    if (nx == 0 || ny == 0) return 0;
    if (tx < (int)minx || ty < (int)miny ||
        tx >= (int)(minx + nx) || ty >= (int)(miny + ny)) return 0;
    if ((uint64_t)nx * ny > tiles->hdr.rec_count - 2) return 0;
    size_t t = ((size_t)(ty - miny) * nx) + (tx - minx);
    return nm_get_u32(tiles->data + (2 + t) * NM_TILE_REC_BYTES + TF_EDGE_CNT);
}

static void tile_dir(const NMBinFile *tiles, const TileRange *tr, int tx, int ty,
                     uint32_t *off, uint32_t *cnt)
{
    size_t t = ((size_t)(ty - (int)tr->miny) * tr->nx) + (tx - (int)tr->minx);
    const uint8_t *dir = tiles->data + (2 + t) * NM_TILE_REC_BYTES;
    *off = nm_get_u32(dir + TF_EDGE_OFF);
    *cnt = nm_get_u32(dir + TF_EDGE_CNT);
}

/* tile 驱动路径：只遍历视口覆盖的 tile（±1 格 margin）。 */
static void paint_roads_tiled(SDL_Renderer *r, Raster *rs, const MapView *v,
                              const NMBinFile *nodes, const NMBinFile *edges,
                              const NMBinFile *tiles, const NMBinFile *edgelist,
                              const NamePool *np, DedupSet *dd,
                              int max_class, PaintStats *st,
                              const NMBinFile *const *peer, int n_peer)
{
    (void)r;
    TileRange tr;
    if (!tile_range(tiles, v, &tr)) return;

    /* 5 个等级的颜色一次打包好：原来每条边一次 SDL_SetRenderDrawColor，
       z9 一帧 77 万次纯状态切换（实测 3.9 ms，不值但它白占着循环）。 */
    uint32_t ccls[5];
    for (int k = 0; k < 5; k++)
        ccls[k] = raster_rgb(rs, kStyle[k].r, kStyle[k].g, kStyle[k].b);

    double zoom = v->zoom;
    int W = v->w, H = v->h;
    const int margin = 64;
    uint64_t nn = nodes->hdr.rec_count, ne = edges->hdr.rec_count;

    MvProj Pstore, *P = &Pstore;
    mv_proj_begin(v, P);

    /* 按 zoom 自动降 LOD，与用户手动的 max_class 取交集。
       z12 视口覆盖全省：245096 条边全画换来 123 ms，而三级小路在屏幕上
       密得看不清、又细成 1 像素，纯属视觉噪音 + 白画。地图常识就是
       "越远越只看主干"。手动调高不会突破这里的上限——按 0-4 键时看到
       的提示要与这个表一致。 */
    int eff_max = max_class;
    if (zoom < 9.0)        { if (eff_max > 0) eff_max = 0; }
    else if (zoom < 11.5)  { if (eff_max > 1) eff_max = 1; }
    else if (zoom < 12.5)  { if (eff_max > 2) eff_max = 2; }
    else if (zoom < 14.0)  { if (eff_max > 3) eff_max = 3; }


    /* ---- edgelist v2：条目自带坐标/等级/标志 ----
       整条路径只顺序扫 edgelist，nodes.bin 一次都不读、edges.bin 只在
       z≥15 取路名时读（那时视口里就几百条边）。这就是 PLAN.md §21 里
       "预取和缓存都压不下去的随机访存"的唯一根治法：不访问。 */
    if (edgelist->hdr.rec_size == NM_XL2_BYTES) {
        for (int ty = tr.ty_lo; ty <= tr.ty_hi; ty++) {
            for (int tx = tr.tx_lo; tx <= tr.tx_hi; tx++) {
                uint32_t off, cnt;
                tile_dir(tiles, &tr, tx, ty, &off, &cnt);
                /* 只有"这一格有 ≥2 个省"才可能出跨省重复，才查去重表 */
                DedupSet *ddt = dd;
                if (ddt && n_peer > 0) {
                    int cov = 0;
                    for (int p = 0; p < n_peer && cov < 2; p++)
                        if (tile_cnt_at(peer[p], tx, ty)) cov++;
                    if (cov < 2) ddt = NULL;
                }
                const uint8_t *xl = edgelist->data +
                                    (size_t)off * NM_XL2_BYTES;
                for (uint32_t k = 0; k < cnt; k++, xl += NM_XL2_BYTES) {
                    uint8_t cls = xl[XL2_CLS];
                    if (cls > eff_max) continue;
                    if (nm_get_u16(xl + XL2_FLAGS) & EF_REVERSE) continue;

                    int32_t fla = nm_get_i32(xl + XL2_FROM_LAT);
                    int32_t flo = nm_get_i32(xl + XL2_FROM_LON);
                    int32_t tla = nm_get_i32(xl + XL2_TO_LAT);
                    int32_t tlo = nm_get_i32(xl + XL2_TO_LON);

                    int x0, y0, x1, y1;
                    mv_proj_e7(P, fla, flo, &x0, &y0);
                    mv_proj_e7(P, tla, tlo, &x1, &y1);
                    if ((x0 < -margin && x1 < -margin) || (x0 > W + margin && x1 > W + margin) ||
                        (y0 < -margin && y1 < -margin) || (y0 > H + margin && y1 > H + margin))
                        continue;

                    if (ddt) {
                        /* 身份 = 两端 e7 坐标的有序对。
                           ★ 不能排序成 (min,max)：双向道路的回程边会因此被判成
                             重复，整条路会消失。
                           ★ 比 v1 用 OSM node id 更稳：跨省同一路口即便两个
                             pbf 给的 id 体系不同，坐标也一致（这是 coordkey.h
                             里整套跨省缝合的前提）。 */
                        if (dedup_seen(dd, coord_key_e7(fla, flo),
                                           coord_key_e7(tla, tlo))) {
                            st->dedup_skipped++;
                            continue;
                        }
                    }

                    const Style *s = &kStyle[cls > 4 ? 4 : cls];
                    int width = (zoom < 12.5) ? 1 : s->width;
                    st->drawn++;
                    /* ★临时：与上一条落在同一像素且同等级 → 跳过落笔。
                       只认"紧邻的前一条"：这样中间不可能插进别的等级把它改写，
                       是构造上等价的（用逐像素记录表试过，非退化的线穿过像素时
                       不会更新记录表，会被误判，实测 16 个像素换了赢家）。 */
                    /* 退化成一个像素的段：若紧邻的上一条落在同一像素且同等级，
                       再画一次是纯浪费（SDL 每次 DrawLine 的固定开销 ~100ns，
                       z9 一帧 59 万条里 67% 是这种段）。只认"紧邻的前一条"是
                       刻意的——中间不可能插进别的等级把它改写，所以像素结果
                       与逐条画完全一致（用逐像素记录表试过：非退化的线穿过
                       像素时不会更新记录表，会被误判，16 个像素换了赢家）。 */
                    if (width == 1 && x0 == x1 && y0 == y1 &&
                        (unsigned)x0 < (unsigned)W && (unsigned)y0 < (unsigned)H) {
                        int px = y0 * W + x0, tag = cls > 4 ? 4 : cls;
                        if (px == g_last_px && tag == g_last_tag) continue;
                        g_last_px = px; g_last_tag = tag;
                    } else {
                        g_last_px = -1;
                    }
                    raster_thick_line(rs, x0, y0, x1, y1, width,
                                      ccls[cls > 4 ? 4 : cls]);

                    if (cls <= 2 && zoom >= 15.0 &&
                        x0 >= 0 && x0 < W && y0 >= 0 && y0 < H) {
                        uint32_t eidx = nm_get_u32(xl + XL2_EDGE);
                        if ((uint64_t)eidx < ne) {
                            uint32_t nid = nm_get_u32(edges->data +
                                              (uint64_t)eidx * NM_EDGE_BYTES +
                                              EF_NAME_ID);
                            if (nid) road_labels_add(np, nid,
                                                     (x0 + x1) / 2, (y0 + y1) / 2);
                        }
                    }
                }
            }
        }
        return;
    }

    /* ---- edgelist v1：条目只有两个下标，坐标/等级都要去 nodes.bin /
       edges.bin 随机读（PLAN.md §21 量到它占河北 z9 路网的一大半）。
       当前所有 v3 产物写的都是 v2 记录，这条路径只在手工搭的旧记录目录、
       或将来再改记录布局时的过渡期才会走到；别删掉，它同时是
       "v2 快路径画错了没有"的对照。 ---- */
    for (int ty = tr.ty_lo; ty <= tr.ty_hi; ty++) {
        for (int tx = tr.tx_lo; tx <= tr.tx_hi; tx++) {
            uint32_t off, cnt;
            tile_dir(tiles, &tr, tx, ty, &off, &cnt);
            for (uint32_t k = 0; k < cnt; k++) {
                const uint8_t *xl = edgelist->data + ((size_t)off + k) * NM_XL_BYTES;
                uint32_t from = nm_get_u32(xl + XL_NODE);
                uint32_t eidx = nm_get_u32(xl + XL_EDGE);
                if ((uint64_t)from >= nn || (uint64_t)eidx >= ne) continue;

                const uint8_t *nr = nodes->data + (uint64_t)from * NM_NODE_BYTES;
                const uint8_t *er = edges->data + (uint64_t)eidx * NM_EDGE_BYTES;
                uint8_t cls = er[EF_CLASS];
                if (cls > eff_max) continue;
                if (nm_get_u16(er + EF_FLAGS) & EF_REVERSE) continue;

                uint32_t to = nm_get_u32(er + EF_TO);
                if ((uint64_t)to >= nn) continue;
                const uint8_t *tnr = nodes->data + (uint64_t)to * NM_NODE_BYTES;

                int x0, y0, x1, y1;
                mv_proj_e7(P, nm_get_i32(nr + NF_LAT), nm_get_i32(nr + NF_LON), &x0, &y0);
                mv_proj_e7(P, nm_get_i32(tnr + NF_LAT), nm_get_i32(tnr + NF_LON), &x1, &y1);
                if ((x0 < -margin && x1 < -margin) || (x0 > W + margin && x1 > W + margin) ||
                    (y0 < -margin && y1 < -margin) || (y0 > H + margin && y1 > H + margin))
                    continue;

                if (dd) {
                    /* 身份 = 两端 e7 坐标的有序对（与 v2 路径同一套键）。
                       ★ 不能排序成 (min,max)：双向道路的回程边会因此被判成
                         重复，整条路会消失。
                       ★ 必须用和 v2 相同的键：否则"一半省 v1、一半省 v2"的
                         混合图集里两套键互不相认，跨省重复路会被画两遍。 */
                    if (dedup_seen(dd, coord_key_e7(nm_get_i32(nr + NF_LAT),
                                                    nm_get_i32(nr + NF_LON)),
                                       coord_key_e7(nm_get_i32(tnr + NF_LAT),
                                                    nm_get_i32(tnr + NF_LON)))) {
                        st->dedup_skipped++;
                        continue;
                    }
                }

                const Style *s = &kStyle[cls > 4 ? 4 : cls];
                int width = (zoom < 12.5) ? 1 : s->width;
                raster_thick_line(rs, x0, y0, x1, y1, width,
                                  ccls[cls > 4 ? 4 : cls]);
                st->drawn++;

                uint32_t nid = nm_get_u32(er + EF_NAME_ID);
                if (nid && cls <= 2 && zoom >= 15.0 &&
                    x0 >= 0 && x0 < W && y0 >= 0 && y0 < H)
                    road_labels_add(np, nid, (x0 + x1) / 2, (y0 + y1) / 2);
            }
        }
    }
}

void paint_roads(SDL_Renderer *r, Raster *rs, const MapView *v,
                 const NMBinFile *nodes, const NMBinFile *edges,
                 const NMBinFile *tiles, const NMBinFile *edgelist,
                 const NamePool *np, DedupSet *dd,
                 int max_class, PaintStats *st,
                 const NMBinFile *const *peer_tiles, int n_peer)
{
    /* ★ 不要在这里 memset 和 reset：多省时 paint_roads 每省调用一次，
       一清就把前一个省的统计/标签收集抹掉。统一由 paint_frame 开头做。 */
    st->max_class = max_class;
    if (!rs) return;

    uint64_t nn = nodes->hdr.rec_count;
    uint64_t ne = edges->hdr.rec_count;
    if (nn == 0 || ne == 0) return;

    if (tiles && edgelist) {
        paint_roads_tiled(r, rs, v, nodes, edges, tiles, edgelist, np, dd,
                          max_class, st, peer_tiles, n_peer);
        return;
    }

    const uint8_t *nd = nodes->data;
    const uint8_t *ed = edges->data;

    int W = v->w, H = v->h;
    const int margin = 64;   /* 允许端点在视口外一点，避免边缘的路突然断掉 */

    /* 投影快照：本 pass 内视口不变，循环里用 mv_proj() 免去每次的缓存键比对 */
    MvProj Pstore, *P = &Pstore;
    mv_proj_begin(v, P);

    /* 先把所有节点投影一遍太贵（145 万次三角/对数），这里改成“边命中再投影”：
       先按经纬度做粗筛（视口 bbox，线性近似足够），命中后再精算像素。 */
    double bb[4];
    mv_visible_bbox(v, bb);
    double pad_lat = (bb[2] - bb[0]) * 0.1 + 1e-6;
    double pad_lon = (bb[3] - bb[1]) * 0.1 + 1e-6;
    double la0 = bb[0] - pad_lat, la1 = bb[2] + pad_lat;
    double lo0 = bb[1] - pad_lon, lo1 = bb[3] + pad_lon;

    for (uint64_t i = 0; i < nn; i++) {
        const uint8_t *nr = nd + i * NM_NODE_BYTES;
        int32_t la = nm_get_i32(nr + NF_LAT);
        int32_t lo = nm_get_i32(nr + NF_LON);

        double dla = (double)la / 1e7, dlo = (double)lo / 1e7;
        int in_a = (dla >= la0 && dla <= la1 && dlo >= lo0 && dlo <= lo1);

        uint32_t b = nm_get_u32(nr + NF_EDGE_BEGIN);
        uint16_t c = nm_get_u16(nr + NF_EDGE_COUNT);
        for (uint32_t k = 0; k < c; k++) {
            uint64_t e = (uint64_t)b + k;
            if (e >= ne) break;
            st->visited++;
            const uint8_t *er = ed + e * NM_EDGE_BYTES;
            uint8_t cls = er[EF_CLASS];
            if (cls > max_class) continue;
            /* EF_REVERSE = 双向路的回程边，几何与去程重复，画一次就够 */
            uint16_t fl = nm_get_u16(er + EF_FLAGS);
            if (fl & EF_REVERSE) continue;      /* 双向路的回程边，几何重复 */

            uint32_t to = nm_get_u32(er + EF_TO);
            if (to >= nn) continue;
            const uint8_t *tr = nd + (uint64_t)to * NM_NODE_BYTES;
            int32_t la2 = nm_get_i32(tr + NF_LAT);
            int32_t lo2 = nm_get_i32(tr + NF_LON);

            double dla2 = (double)la2 / 1e7, dlo2 = (double)lo2 / 1e7;
            int in_b = (dla2 >= la0 && dla2 <= la1 && dlo2 >= lo0 && dlo2 <= lo1);
            if (!in_a && !in_b) continue;

            int x0, y0, x1, y1;
            mv_proj_e7(P, la, lo, &x0, &y0);
            mv_proj_e7(P, la2, lo2, &x1, &y1);
            if ((x0 < -margin && x1 < -margin) || (x0 > W + margin && x1 > W + margin) ||
                (y0 < -margin && y1 < -margin) || (y0 > H + margin && y1 > H + margin))
                continue;

            if (dd) {
                /* 同上：有序对、不排序；键与 tiled 两条路径一致（e7 坐标） */
                if (dedup_seen(dd, coord_key_e7(la, lo), coord_key_e7(la2, lo2))) {
                    st->dedup_skipped++;
                    continue;
                }
            }

            const Style *s = &kStyle[cls > 4 ? 4 : cls];
            /* 缩小到全省/全市时，粗线把画面糊成色块且每条要画 5 次；
               直接退化为 1 像素，帧时间降一个数量级 */
            int width = (v->zoom < 12.5) ? 1 : s->width;
            raster_thick_line(rs, x0, y0, x1, y1, width,
                              raster_rgb(rs, s->r, s->g, s->b));
            st->drawn++;

            /* 顺带收集路名锚点：主干以上、有名、屏幕内 */
            uint32_t nid = nm_get_u32(er + EF_NAME_ID);
            if (nid && cls <= 2 && v->zoom >= 15.0 &&
                x0 >= 0 && x0 < W && y0 >= 0 && y0 < H)
                road_labels_add(np, nid, (x0 + x1) / 2, (y0 + y1) / 2);
        }
    }
}

void paint_marker(SDL_Renderer *r, int x, int y, int radius,
                  uint8_t rr, uint8_t gg, uint8_t bb)
{
    SDL_SetRenderDrawColor(r, rr, gg, bb, 255);
    SDL_Rect rc = { x - radius, y - radius, radius * 2 + 1, radius * 2 + 1 };
    SDL_RenderFillRect(r, &rc);
    SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
    SDL_RenderDrawRect(r, &rc);
}

/* ------------------------------------------------------------------ */
/* 面要素：建筑 / 水面 / 绿地                                           */
/* ------------------------------------------------------------------ */

typedef struct { uint8_t fr, fg, fb, br, bg, bb; } AreaStyle;

static const AreaStyle kAreaStyle[AK_OTHER + 1] = {
    { 0, 0, 0, 0, 0, 0 },
    { 219, 213, 201, 178, 171, 158 },   /* 建筑：米灰 + 深描边（背景 244,241,234，要有对比） */
    { 186, 216, 238, 158, 196, 226 },   /* 水面 */
    { 208, 232, 202, 178, 214, 170 },   /* 绿地 */
    { 232, 229, 218, 210, 206, 196 },   /* 其他区域 */
};

/* ------------------------------------------------------------------ */
/* 描边：连续折线攒起来一次 DrawLines（第六刀）                        */
/*                                                                     */
/* 原来每个多边形/每条线的边框是"一条边一次 SDL_RenderDrawLine"。        */
/* 实测 z9 北京一帧面要素 89 ms 里**描边占 50 ms**（比填充还多一倍），   */
/* 而且里面大量边在低 zoom 下首尾投影到同一个像素（零长边）。           */
/*                                                                     */
/* 两处改动，都不改变最终像素（同一-cmd 对比 md5 完全相同）：           */
/*   ① 合并连续重复像素点：零长边本来也不产生可见像素                   */
/*   ② 整环一次 SDL_RenderDrawLines：一次绘制命令，而不是 n 次          */
/* 实测：z9 面 91→57 ms、z13 面 60→43 ms。这是一笔纯赚。               */
/* ------------------------------------------------------------------ */

/* 现在逐段直写像素，不再需要攒点缓冲（原来攒点是为了凑一次 DrawLines）。
   ★ 逐段画与 SDL_RenderDrawLines 像素等价：折线的中间接点在本段被当作
     终点、在下一段被当作起点，SDL 只画一次、我们画两次，落在同一个像素、
     同一个颜色上；最后一点 SDL 由末尾那次 DrawPoint 补上，而我们的每段
     都画满两端，所以恒等。（用 1500 条随机折线逐像素验证过 0 差异。） */
static int g_sp_x, g_sp_y, g_sp_have;

static void stroke_begin(void) { g_sp_have = 0; }

static void stroke_push(Raster *rs, int x, int y, uint32_t c)
{
    if (!g_sp_have) { g_sp_x = x; g_sp_y = y; g_sp_have = 1; return; }
    if (g_sp_x == x && g_sp_y == y) return;       /* 零长边：跳过 */
    raster_line(rs, g_sp_x, g_sp_y, x, y, c);
    g_sp_x = x; g_sp_y = y;
}

static void stroke_flush(void) { g_sp_have = 0; }

/* 闭合环：末尾补回首点。 */
static void stroke_ring(Raster *rs, const int *px, const int *py, int n, uint32_t c)
{
    stroke_begin();
    for (int k = 0; k < n; k++) stroke_push(rs, px[k], py[k], c);
    stroke_push(rs, px[0], py[0], c);
    stroke_flush();
}

/* 多边形填充：扫描线 even-odd。
 * 为什么不用 SDL_RenderGeometry：本机 SDL 2.30 的软件渲染器没有实现
 * geometry 命令，调用被静默丢弃（颜色直方图里根本没有填充色才抓到的）。
 * 自绘扫描线顺带支持自交多边形与内环，比三角化更稳。 */

static void fill_polygon(Raster *rs, const int *px, const int *py, int n,
                         uint32_t color)
{
    int ymin = py[0], ymax = py[0];
    for (int i = 1; i < n; i++) {
        if (py[i] < ymin) ymin = py[i];
        if (py[i] > ymax) ymax = py[i];
    }
    if (ymin < 0) ymin = 0;
    if (ymax > 8191) ymax = 8191;   /* 防御：超大面裁掉 */
    if (ymin > ymax) return;

    /* ---- 活动边表（AET）----
     * 原写法是"每一行都扫一遍全部 n 条边"：z9 一帧 267K 行 × 平均几十条边，
     * 光这一项就 25 ms（判别实验量出来的）。改成按 ylo 排序后逐行增量维护
     * 活动集合，每行只算真正与这一行相交的那几条。
     * ★ 交点公式、半开区间 [ylo,yhi)、插入排序、跨度画法一个字都没改，
     *   所以画出来的像素必须完全相同 —— 这不是"画面换速度"的取舍，是纯赚。
     *   （同一轮里试过的"按像素抽稀顶点"会削掉小面的凸角，实测 z9 丢 10% 的
     *   绿地面积，转移方向是"要素色 → 背景色"，已否决。） */
    struct Edge { int ylo, yhi, y1, y2, x1, x2; } e[256];
    int ne = 0;
    for (int i = 0; i < n && ne < 256; i++) {
        int j = (i + 1) % n;
        int y1 = py[i], y2 = py[j];
        if (y1 == y2) continue;                    /* 与原实现一致：水平边不算相交 */
        e[ne].y1 = y1; e[ne].y2 = y2; e[ne].x1 = px[i]; e[ne].x2 = px[j];
        e[ne].ylo = y1 < y2 ? y1 : y2;
        e[ne].yhi = y1 < y2 ? y2 : y1;
        ne++;
    }
    for (int a = 1; a < ne; a++) {                 /* 按 ylo 插入排序（边数很少） */
        struct Edge t = e[a];
        int b = a - 1;
        while (b >= 0 && e[b].ylo > t.ylo) { e[b + 1] = e[b]; b--; }
        e[b + 1] = t;
    }

    static int xs[128], act[256];
    int na = 0, next = 0;
    for (int y = ymin; y <= ymax; y++) {
        double yc = y + 0.5;
        while (next < ne && e[next].ylo <= yc) act[na++] = next++;
        int w = 0;
        for (int q = 0; q < na; q++)                /* 淘汰已经走完的边 */
            if (e[act[q]].yhi > yc) act[w++] = act[q];
        na = w;
        int nx = 0;
        for (int q = 0; q < na && nx < 126; q++) {
            int id = act[q];
            double t = (yc - e[id].y1) / (double)(e[id].y2 - e[id].y1);
            xs[nx++] = (int)(e[id].x1 + t * (e[id].x2 - e[id].x1) + 0.5);
        }
        /* 插入排序（每行交点很少） */
        for (int a = 1; a < nx; a++) {
            int v = xs[a], b = a - 1;
            while (b >= 0 && xs[b] > v) { xs[b + 1] = xs[b]; b--; }
            xs[b + 1] = v;
        }
        for (int a = 0; a + 1 < nx; a += 2) {
            int x0 = xs[a], x1 = xs[a + 1];
            if (x1 > x0) raster_hspan(rs, y, x0, x1, color);
        }
    }
}

/* 取一个环的顶点 → 投影。返回投影后的点数（<0 = 索引越界）。
 */
#define RING_CAP 8192

static int project_ring(const MvProj *P, const NMBinFile *nodes,
                        const NMBinFile *polyrefs, uint32_t off, uint32_t cnt,
                        uint64_t nn, int *px, int *py, int cap)
{
    static int32_t  rla[RING_CAP], rlo[RING_CAP];
    if (cnt > RING_CAP) cnt = RING_CAP;
    uint32_t m = 0;
    for (uint32_t k = 0; k < cnt; k++) {
        uint32_t idx = nm_get_u32(polyrefs->data + ((uint64_t)off + k) * 4);
        if ((uint64_t)idx >= nn) return -1;
        const uint8_t *nr = nodes->data + (uint64_t)idx * NM_NODE_BYTES;
        rla[m] = nm_get_i32(nr + NF_LAT);
        rlo[m] = nm_get_i32(nr + NF_LON);
        m++;
    }
    int n = 0;
    for (uint32_t k = 0; k < m && n < cap; k++) {
        mv_proj_e7(P, rla[k], rlo[k], &px[n], &py[n]);
        n++;
    }
    return n;
}

int paint_areas(SDL_Renderer *r, Raster *rs, const MapView *v,
                const NMBinFile *nodes, const NMBinFile *areas,
                const NMBinFile *polyrefs,
                 int32_t **bbox_cache, PaintStats *st)
{
    (void)r;                       /* 热点走 rs；SDL 渲染器在此函数里已无用 */
    uint64_t na = areas->hdr.rec_count;
    uint64_t np = polyrefs->hdr.rec_count;
    uint64_t nn = nodes->hdr.rec_count;
    if (!na || !np || !nn) return 0;
    if (!rs) return 0;

    /* 颜色在这里一次性打包好（5 种面类型 × 填充/描边），别放到每个面的循环里：
       原来每面两次 SDL_SetRenderDrawColor，z9 一帧是 20 多万次纯状态切换。 */
    uint32_t afill[AK_OTHER + 1], astroke[AK_OTHER + 1];
    for (int k = 0; k <= AK_OTHER; k++) {
        afill[k]   = raster_rgb(rs, kAreaStyle[k].fr, kAreaStyle[k].fg, kAreaStyle[k].fb);
        astroke[k] = raster_rgb(rs, kAreaStyle[k].br, kAreaStyle[k].bg, kAreaStyle[k].bb);
    }

    /* 懒初始化每面 bbox（进程内一次），避免每帧扫 20 万个面的全部顶点 */
    if (bbox_cache && !*bbox_cache) {
        int32_t *bb = calloc((size_t)na * 4, sizeof(int32_t));
        if (!bb) return -1;
        for (uint64_t i = 0; i < na; i++) {
            const uint8_t *ar = areas->data + i * NM_AREA_BYTES;
            uint32_t off = nm_get_u32(ar + AF_REF_OFF);
            uint32_t cnt = nm_get_u32(ar + AF_REF_CNT);
            int32_t la0 = 90 * 10000000, la1 = -90 * 10000000;
            int32_t lo0 = 180 * 10000000, lo1 = -180 * 10000000;
            for (uint32_t k = 0; k < cnt; k++) {
                uint32_t idx = nm_get_u32(polyrefs->data + ((uint64_t)off + k) * 4);
                if ((uint64_t)idx >= nn) continue;
                const uint8_t *nr = nodes->data + (uint64_t)idx * NM_NODE_BYTES;
                int32_t la = nm_get_i32(nr + NF_LAT), lo = nm_get_i32(nr + NF_LON);
                if (la < la0) la0 = la;
                if (la > la1) la1 = la;
                if (lo < lo0) lo0 = lo;
                if (lo > lo1) lo1 = lo;
            }
            bb[i * 4] = la0; bb[i * 4 + 1] = lo0; bb[i * 4 + 2] = la1; bb[i * 4 + 3] = lo1;
        }
        *bbox_cache = bb;
    }

    /* 本 pass 的视口不变 → 取一次投影快照，循环里用 mv_proj（PLAN.md §30） */
    MvProj P;
    mv_proj_begin(v, &P);

    double bbl[4];
    mv_visible_bbox(v, bbl);
    int32_t vla0 = (int32_t)(bbl[0] * 1e7), vla1 = (int32_t)(bbl[2] * 1e7);
    int32_t vlo0 = (int32_t)(bbl[1] * 1e7), vlo1 = (int32_t)(bbl[3] * 1e7);
    double zoom = v->zoom;

    /* ★ 不要在这里清零：多省加载时是按省循环调用本函数的，一清零就把上一个省
       累加的结果抹掉（表现为"图上明明有建筑/水面，统计却报 0"——全 8 省加载时
       最后一个不覆盖视口的省会把计数清成 0）。一帧的清零只在 paint_frame 开头
       做一次，见那里的注释。 */
    for (uint64_t i = 0; i < na; i++) {
        const uint8_t *ar = areas->data + i * NM_AREA_BYTES;
        uint8_t kind = ar[AF_KIND];
        if (kind == AK_NONE || kind > AK_OTHER) continue;
        /* LOD：低 zoom 只画大片水面/绿地，建筑到中高 zoom 才出现 */
        if (kind == AK_BUILDING && zoom < 13.0) continue;
        if (zoom < 8.0) continue;
        const int32_t *bb = *bbox_cache + i * 4;
        if (bb[0] > vla1 || bb[2] < vla0 || bb[1] > vlo1 || bb[3] < vlo0) continue;

        uint32_t off = nm_get_u32(ar + AF_REF_OFF);
        uint32_t cnt = nm_get_u32(ar + AF_REF_CNT);
        if (cnt < 4) continue;

        /* ★ 面要素不做跨省去重：填充/描边都是不透明色，同一条要素被两省各画
           一遍落在同样的像素上；去重只是白花哈希钱（实测 8 省 z9 省 22 ms、
           逐像素 0 差异，见 PLAN.md §23）。 */

        /* ---- 小面快速路径（低 zoom 的面开销主要就在这里）----
         * 实测面的屏幕尺寸分布（北京城内 1280×800）：
         *   z9 : 61618 个面里 42952 个（70%）最大边 < 2 px
         *   z11: 36237 个面里 10106 个（28%）
         * 这些面填充出来最多一个像素，却要走"投影全部 cnt 个顶点 → 扫描线
         * 逐行对 n 条边求交 → 再描一遍 n 条边"这一整套，成本和大面同量级。
         * 用 geo bbox 的两个角点投影估出屏幕尺寸（2 次投影，不必投影全部顶点），
         * 够小就只点一个像素：颜色贡献保留，开销从 O(cnt×行) 降到 O(1)。
         * 估的是外接框上界，只会挑出确实很小的面，不会误伤大面。 */
        if (*bbox_cache) {
            int xa, ya, xb, yb;
            mv_proj_e7(&P, bb[0], bb[1], &xa, &ya);
            mv_proj_e7(&P, bb[2], bb[3], &xb, &yb);
            int aw = xa > xb ? xa - xb : xb - xa;
            int ah = ya > yb ? ya - yb : yb - ya;
            if ((aw > ah ? aw : ah) < AREA_DOT_PX) {
                raster_point(rs, (xa + xb) / 2, (ya + yb) / 2, afill[kind]);
                st->areas_drawn++;
                st->areas_dotted++;
                continue;
            }
        }

        /* 复杂面（顶点很多，如公园/湖泊边界）：填充上限 256 顶点，超出退化为描边 */
        if (cnt > 256) {
            static int bx[RING_CAP], by[RING_CAP];
            int bn = project_ring(&P, nodes, polyrefs, off, cnt, nn, bx, by, RING_CAP);
            stroke_begin();
            for (int k = 0; k < bn; k++) stroke_push(rs, bx[k], by[k], astroke[kind]);
            stroke_flush();
            st->areas_drawn++;
            continue;
        }

        int px[256], py[256];
        int n = project_ring(&P, nodes, polyrefs, off, cnt, nn, px, py, 256);
        if (n < 3) continue;
        /* 闭合 way 首尾同点，去掉重复尾点 */
        if (n > 3 && px[0] == px[n - 1] && py[0] == py[n - 1]) n--;

        fill_polygon(rs, px, py, n, afill[kind]);
        stroke_ring(rs, px, py, n, astroke[kind]);   /* 闭合：末尾自动补回首点 */
        st->areas_drawn++;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* 线要素：步行道 / 铁路 / 水系线                                        */
/* ------------------------------------------------------------------ */

int paint_lines(SDL_Renderer *r, Raster *rs, const MapView *v,
                const NMBinFile *nodes, const NMBinFile *lines,
                const NMBinFile *polyrefs,
                int32_t **bbox_cache, PaintStats *st)
{
    (void)r;
    uint64_t nl = lines->hdr.rec_count;
    uint64_t np = polyrefs->hdr.rec_count;
    uint64_t nn = nodes->hdr.rec_count;
    if (!nl || !np || !nn) return 0;
    if (!rs) return 0;

    /* 懒初始化每条线的 bbox（进程内一次）。
       没有它就得为每条线投影全部顶点才能知道它可不可见——线要素没有 tile
       索引，10 万条线 × 平均几十个顶点的投影是 z14 一帧 248 ms 的主要来源。 */
    if (bbox_cache && !*bbox_cache) {
        int32_t *bb = calloc((size_t)nl * 4, sizeof(int32_t));
        if (!bb) return -1;
        for (uint64_t i = 0; i < nl; i++) {
            const uint8_t *lr = lines->data + i * NM_LINE_BYTES;
            uint32_t off = nm_get_u32(lr + LF_REF_OFF);
            uint32_t cnt = nm_get_u32(lr + LF_REF_CNT);
            int32_t a0 = 90 * 10000000, a1 = -90 * 10000000;
            int32_t o0 = 180 * 10000000, o1 = -180 * 10000000;
            for (uint32_t k = 0; k < cnt; k++) {
                uint32_t idx = nm_get_u32(polyrefs->data + ((uint64_t)off + k) * 4);
                if ((uint64_t)idx >= nn) continue;
                const uint8_t *nr = nodes->data + (uint64_t)idx * NM_NODE_BYTES;
                int32_t la = nm_get_i32(nr + NF_LAT), lo = nm_get_i32(nr + NF_LON);
                if (la < a0) a0 = la;
                if (la > a1) a1 = la;
                if (lo < o0) o0 = lo;
                if (lo > o1) o1 = lo;
            }
            bb[i * 4] = a0; bb[i * 4 + 1] = o0; bb[i * 4 + 2] = a1; bb[i * 4 + 3] = o1;
        }
        *bbox_cache = bb;
    }
    int32_t *bb = bbox_cache ? *bbox_cache : NULL;

    /* 同上：本 pass 视口不变，取一次投影快照 */
    MvProj P;
    mv_proj_begin(v, &P);

    double bbl[4];
    mv_visible_bbox(v, bbl);
    double la0 = bbl[0] - 0.02, la1 = bbl[2] + 0.02;
    double lo0 = bbl[1] - 0.02, lo1 = bbl[3] + 0.02;
    int32_t vla0 = (int32_t)(la0 * 1e7), vla1 = (int32_t)(la1 * 1e7);
    int32_t vlo0 = (int32_t)(lo0 * 1e7), vlo1 = (int32_t)(lo1 * 1e7);
    double zoom = v->zoom;

    /* 同上：不清零，多省循环时跨省累加。 */
    for (uint64_t i = 0; i < nl; i++) {
        /* bbox 预筛：线没有 tile 索引，这是唯一能避免"为每条线投影全部顶点"
           的手段。放在去重之前——bbox 测试比哈希便宜得多。 */
        if (bb) {
            if (bb[i * 4 + 2] < vla0 || bb[i * 4] > vla1 ||
                bb[i * 4 + 3] < vlo0 || bb[i * 4 + 1] > vlo1)
                continue;
        }
        const uint8_t *lr = lines->data + i * NM_LINE_BYTES;
        uint8_t cls = lr[LF_CLASS];
        /* LOD：步行道细节多、密度大，中高 zoom 才显示 */
        if (cls == RC_PATH && zoom < 14.0) continue;
        if (zoom < 8.0) continue;

        /* ★ 线要素同样不去重（理由同面要素，见 PLAN.md §23）。 */

        uint32_t off = nm_get_u32(lr + LF_REF_OFF);
        uint32_t cnt = nm_get_u32(lr + LF_REF_CNT);
        if ((uint64_t)off + cnt > np) continue;

        int vis = 0;
        int px[4096], py[4096];
        int n = 0;
        for (uint32_t k = 0; k < cnt && n < 4096; k++) {
            uint32_t idx = nm_get_u32(polyrefs->data + ((uint64_t)off + k) * 4);
            if ((uint64_t)idx >= nn) continue;
            const uint8_t *nr = nodes->data + (uint64_t)idx * NM_NODE_BYTES;
            int32_t ila = nm_get_i32(nr + NF_LAT), ilo = nm_get_i32(nr + NF_LON);
            double la = (double)ila / 1e7;
            double lo = (double)ilo / 1e7;
            if (la >= la0 && la <= la1 && lo >= lo0 && lo <= lo1) vis = 1;
            int x, y;
            mv_proj_e7(&P, ila, ilo, &x, &y);
            px[n] = x; py[n] = y; n++;
        }
        if (!vis || n < 2) continue;

        uint8_t cr, cg, cb, width;
        switch (cls) {
        case RC_RAIL:  cr = 120; cg = 120; cb = 132; width = (zoom < 12.5) ? 1 : 2; break;
        case RC_WATER: cr = 150; cg = 195; cb = 230; width = (zoom < 12.5) ? 1 : 2; break;
        default:       cr = 202; cg = 199; cb = 190; width = 1; break;  /* RC_PATH */
        }
        uint32_t col = raster_rgb(rs, cr, cg, cb);
        if (width <= 1) {
            stroke_begin();
            for (int k = 0; k < n; k++) stroke_push(rs, px[k], py[k], col);
            stroke_flush();
        } else {
            for (int k = 1; k < n; k++)
                raster_thick_line(rs, px[k - 1], py[k - 1], px[k], py[k],
                                  width, col);
        }
        st->lines_drawn++;
    }
    return 0;
}
