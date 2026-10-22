/* tile_xcheck —— 验证"坐标即节点主键"的跨省接边是否成立（阶段 8 地基）
 *
 * 给定一个经纬度，取它所在的 Z14 瓦片，用 mapset_query_tile 把所有省
 * 在这片里的边都收上来，按 coord_key_e7(节点坐标) 分组：
 *
 *   1. 统计被 ≥2 个省共享的坐标数（= 将来的跨省缝合点）；
 *   2. ★ 断言：同一坐标在所有省里的 OSM node id 必须一致——这是坐标键
 *      方案正确性的前提，不一致说明数据有问题（比如两省用了不同版本
 *      的提取，同名不同点），必须报错而不是静默接错路；
 *   3. 打几个缝合点样例（坐标 / OSM id / 都有哪些省）。
 *
 * 用法：
 *   ./tile_xcheck --graph=products/graph [--lat=39.72 --lon=117.15]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "map/mapset.h"
#include "map/autobuild.h"
#include "map/coordkey.h"
#include "utils/geo.h"

typedef struct {
    uint64_t key;        /* coord_key_e7 */
    uint32_t prov;
    uint32_t node;
    uint64_t osm_id;
} Ent;

typedef struct {
    Ent    *v;
    size_t  n, cap;
} EntVec;

static void ev_push(EntVec *ev, Ent e)
{
    if (ev->n == ev->cap) {
        ev->cap = ev->cap ? ev->cap * 2 : 4096;
        ev->v = realloc(ev->v, ev->cap * sizeof(Ent));
        if (!ev->v) { fprintf(stderr, "OOM\n"); exit(1); }
    }
    ev->v[ev->n++] = e;
}

typedef struct { const MapSet *ms; EntVec *ev; } Ctx;

/* 省 identity 直接用回调里的 s 指针——mapset_query_tile 一次遍历所有省，
 * 不能在外层按 i 归属（那会把所有省的边都记到同一个省名下） */
static void collect(const MapSrc *s, uint32_t from, uint32_t edge, void *ud)
{
    Ctx *c = ud;
    uint32_t prov = (uint32_t)(s - c->ms->srcs);
    const uint8_t *nr = s->nodes.data + (uint64_t)from * NM_NODE_BYTES;
    Ent e;
    e.key    = coord_key_e7(nm_get_i32(nr + NF_LAT), nm_get_i32(nr + NF_LON));
    e.prov   = prov;
    e.node   = from;
    e.osm_id = nm_get_u64(nr + NF_OSM_ID);
    ev_push(c->ev, e);
    (void)edge;
}

static int ent_cmp(const void *pa, const void *pb)
{
    const Ent *a = pa, *b = pb;
    if (a->key  != b->key)  return a->key  < b->key  ? -1 : 1;
    if (a->prov != b->prov) return a->prov < b->prov ? -1 : 1;
    return 0;
}

int main(int argc, char **argv)
{
    const char *dir = "products/graph";
    double lat = 39.72, lon = 117.15;    /* 平谷—蓟州—宝坻三省交界 */
    AutoBuildOpt ab;
    memset(&ab, 0, sizeof(ab));
    ab.enable = 1;

    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--graph=", 8)) dir = argv[i] + 8;
        else if (!strncmp(argv[i], "--lat=", 6))  lat = atof(argv[i] + 6);
        else if (!strncmp(argv[i], "--lon=", 6))  lon = atof(argv[i] + 6);
        else if (!strcmp(argv[i], "--no-autobuild")) ab.enable = 0;
        else { fprintf(stderr, "未知参数 %s\n", argv[i]); return 2; }
    }

    MapSet ms;
    if (mapset_load_repair(&ms, dir, &ab, NULL) != 0) { fprintf(stderr, "加载 %s 失败\n", dir); return 1; }
    printf("已加载 %d 个省\n", ms.n);

    double wx, wy;
    nm_mercator(lat, lon, &wx, &wy);
    double Z = (double)(1u << NM_TILE_Z);
    int tx = (int)(wx * Z), ty = (int)(wy * Z);
    printf("Z14 tile: x=%d y=%d（中心 %.5f,%.5f）\n\n", tx, ty, lat, lon);

    EntVec ev = {0};
    Ctx ctx = { &ms, &ev };
    mapset_query_tile(&ms, tx, ty, collect, &ctx);

    /* 各省在这片 tile 里贡献了多少条边（按省统计一遍） */
    uint64_t per[64] = {0};
    for (size_t k = 0; k < ev.n; k++)
        if (ev.v[k].prov < sizeof(per)/sizeof(per[0])) per[ev.v[k].prov]++;
    for (int i = 0; i < ms.n; i++)
        printf("  %-12s %6llu 条边\n", ms.srcs[i].name, (unsigned long long)per[i]);
    if (ev.n == 0) { printf("这片没有数据\n"); return 1; }

    qsort(ev.v, ev.n, sizeof(Ent), ent_cmp);

    /* 同一 (坐标,省) 只留一条（一个节点有多条出边会重复出现） */
    size_t un = 0;
    for (size_t i = 0; i < ev.n; i++)
        if (un == 0 || ev.v[un-1].key != ev.v[i].key || ev.v[un-1].prov != ev.v[i].prov)
            ev.v[un++] = ev.v[i];

    /* 按 key 分组：组内省数 ≥2 即缝合点；同时校验 OSM id 一致 */
    size_t shared = 0, id_mismatch = 0, shared_edges_total = 0;
    Ent sample[8]; int nsample = 0;
    size_t i = 0;
    while (i < un) {
        size_t j = i;
        int nprov = 1;
        uint64_t osm = ev.v[i].osm_id;
        int mismatch = 0;
        while (j + 1 < un && ev.v[j+1].key == ev.v[i].key) {
            j++;
            if (ev.v[j].prov != ev.v[j-1].prov) nprov++;
            if (ev.v[j].osm_id != osm) mismatch = 1;
        }
        if (nprov >= 2) {
            shared++;
            shared_edges_total += (j - i + 1);
            if (mismatch) {
                id_mismatch++;
                if (id_mismatch <= 5) {
                    int32_t la = coord_key_lat(ev.v[i].key), lo = coord_key_lon(ev.v[i].key);
                    printf("  ✗ id 不一致: (%.7f,%.7f)", la / 1e7, lo / 1e7);
                    for (size_t k = i; k <= j; k++)
                        printf("  [%s #%u osm=%llu]", ms.srcs[ev.v[k].prov].name,
                               ev.v[k].node, (unsigned long long)ev.v[k].osm_id);
                    printf("\n");
                }
            } else if (nsample < 8) {
                sample[nsample++] = ev.v[i];
            }
        }
        i = j + 1;
    }

    printf("\n共享坐标（≥2 省都有）: %zu 个，涉及 %zu 条边记录\n", shared, shared_edges_total);
    printf("OSM id 不一致的坐标 : %llu 个 %s\n", (unsigned long long)id_mismatch,
           id_mismatch ? "← 必须处理，否则路由会接错" : "（坐标键方案的前提成立）");
    printf("\n缝合点样例：\n");
    for (int k = 0; k < nsample; k++) {
        uint64_t key = sample[k].key;
        /* 找出同组的所有省 */
        printf("  (%.7f, %.7f)  osm=%-11llu 省内下标",
               coord_key_lat(key) / 1e7, coord_key_lon(key) / 1e7,
               (unsigned long long)sample[k].osm_id);
        for (size_t t = 0; t < un; t++) {
            if (ev.v[t].key != key) { if (ev.v[t].key > key) break; continue; }
            printf(" %s:#%u", ms.srcs[ev.v[t].prov].name, ev.v[t].node);
        }
        printf("\n");
    }

    free(ev.v);
    mapset_free(&ms);
    return id_mismatch ? 1 : 0;
}
