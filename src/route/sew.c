#include "route/sew.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 瓦片缝合缓存：原 route.c 的"瓦片缝合缓存"段整段迁来，行为未改。
 * SewCtx / sew_cb / cmp_pn / cmp_coord_prov 只在本文件用 ⇒ 保持 static；
 * sew_get / tile_prov_count / sew_build 被 route.c 的 router_destroy /
 * router_nearest_k / members_of 调用 ⇒ 导出（声明见 sew.h）。 */

/* ------------------------ 瓦片缝合缓存 ------------------------ */

typedef struct {
    const MapSet *ms;
    SewEnt *v;
    size_t  n, cap;
} SewCtx;

static void sew_cb(const MapSrc *s, uint32_t from, uint32_t edge, void *ud)
{
    SewCtx *c = ud;
    (void)edge;
    int prov = (int)(s - c->ms->srcs);
    const uint8_t *nr = s->nodes.data + (uint64_t)from * NM_NODE_BYTES;
    if (c->n == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 1024;
        c->v = xrealloc(c->v, c->cap * sizeof(SewEnt));
    }
    SewEnt e;
    e.coord = coord_key_e7(nm_get_i32(nr + NF_LAT), nm_get_i32(nr + NF_LON));
    e.prov = (uint32_t)prov;
    e.node = from;
    c->v[c->n++] = e;
}

static int cmp_pn(const void *a, const void *b)
{
    const SewEnt *x = a, *y = b;
    if (x->prov != y->prov) return x->prov < y->prov ? -1 : 1;
    if (x->node != y->node) return x->node < y->node ? -1 : 1;
    return 0;
}

static int cmp_coord_prov(const void *a, const void *b)
{
    const SewEnt *x = a, *y = b;
    if (x->coord != y->coord) return x->coord < y->coord ? -1 : 1;
    if (x->prov != y->prov) return x->prov < y->prov ? -1 : 1;
    return 0;
}

/* 只看瓦片目录，不遍历边：判断这片瓦片被几个省覆盖。
 * 只有 ≥2 个省时才有缝合的可能，省内部的瓦片连索引都不用建——
 * 这是搜索热路径上最大的一笔开销，能省就省。 */
int tile_prov_count(const MapSet *ms, int tx, int ty)
{
    /* 统计"去重后"的省数：被完全包含的副本省映射到它的宿主，同一宿主只算一次。
       北京城区那片瓦片 beijing 和 hebei 都有数据，但 beijing ⊂ hebei，
       有效省数 = 1 ⇒ 根本没有缝合的必要，连索引都不用建。 */
    int seen[32], ns = 0, c = 0;
    for (int i = 0; i < ms->n; i++) {
        const MapSrc *s = &ms->srcs[i];
        if (!s->has_tiles) continue;
        const uint8_t *td = s->tiles.data;
        uint32_t minx = nm_get_u32(td + TF_EDGE_OFF);
        uint32_t miny = nm_get_u32(td + TF_EDGE_CNT);
        uint32_t nx   = nm_get_u32(td + NM_TILE_REC_BYTES + TF_EDGE_OFF);
        uint32_t ny   = nm_get_u32(td + NM_TILE_REC_BYTES + TF_EDGE_CNT);
        if (tx < (int)minx || ty < (int)miny ||
            tx >= (int)(minx + nx) || ty >= (int)(miny + ny))
            continue;
        if ((uint64_t)nx * ny > s->tiles.hdr.rec_count - 2) continue;
        size_t t = ((size_t)(ty - miny) * nx) + (tx - minx);
        const uint8_t *dir = td + (2 + t) * NM_TILE_REC_BYTES;
        if (nm_get_u32(dir + TF_EDGE_CNT) == 0) continue;
        int root = i;
        while (root >= 0 && ms->srcs[root].dup_of >= 0) root = ms->srcs[root].dup_of;
        int dup = 0;
        for (int k = 0; k < ns; k++) if (seen[k] == root) { dup = 1; break; }
        if (!dup && ns < 32) seen[ns++] = root;
        if (++c >= 2 && ns >= 2) return ns;
    }
    return ns;
}

SewSlot *sew_get(Router *r, int tx, int ty)
{
    size_t mask = SEW_SLOTS - 1;
    size_t h = ((size_t)tx * 73856093u + (size_t)ty * 19349663u) & mask;
    for (size_t p = 0; p < 8; p++) {
        size_t j = (h + p) & mask;
        SewSlot *s = &r->sew[j];
        if (s->used && s->tx == tx && s->ty == ty) return s;
        if (!s->used) {
            s->used = 1; s->tx = tx; s->ty = ty;
            s->nprov = -1; s->full = 0; s->v = NULL; s->n = 0;
            return s;
        }
    }
    /* 探测窗口全被占：直接淘汰起点那个槽。局部性好，代价是一次重建。 */
    SewSlot *s = &r->sew[h];
    free(s->v);
    s->used = 1; s->tx = tx; s->ty = ty;
    s->nprov = -1; s->full = 0; s->v = NULL; s->n = 0;
    return s;
}

/* 需要完整成员列表时才调（起终点吸附、多省瓦片的缝合） */
void sew_build(Router *r, SewSlot *s)
{
    if (s->full) return;
    SewCtx c;
    memset(&c, 0, sizeof(c));
    c.ms = r->ms;
    mapset_query_tile(r->ms, s->tx, s->ty, sew_cb, &c);
    if (c.n == 0) {
        free(c.v);
        s->full = 1; s->v = NULL; s->n = 0;
        return;
    }
    /* 同一节点会因有多条出边而重复出现，先按 (prov,node) 去重 */
    qsort(c.v, c.n, sizeof(SewEnt), cmp_pn);
    size_t w = 0;
    for (size_t i = 0; i < c.n; i++)
        if (i == 0 || cmp_pn(&c.v[i], &c.v[w - 1]) != 0) c.v[w++] = c.v[i];
    qsort(c.v, w, sizeof(SewEnt), cmp_coord_prov);
    s->v = c.v; s->n = w; s->full = 1;
}
