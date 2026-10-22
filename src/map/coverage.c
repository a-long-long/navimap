#include "map/coverage.h"

#include <stdio.h>
#include <string.h>

#include "map/coordkey.h"
#include "utils/geo.h"

typedef struct {
    const MapSrc *srcs;   /* 用来把回调拿到的 MapSrc* 换回下标 */
    int           n;      /* 找到几条边 */
    int           prov;   /* 第一条边来自哪个省 */
} CovHit;

static void count_edge(const MapSrc *s, uint32_t from_node, uint32_t edge_idx,
                       void *ud)
{
    (void)from_node; (void)edge_idx;
    CovHit *h = (CovHit *)ud;
    if (h->n == 0) h->prov = (int)(s - h->srcs);
    h->n++;
}

/* 以 (cx,cy) 为中心、半径 r 个瓦片的范围里有没有边。
 * 顺带记下边来自哪个省——省名按"实际提供数据的那个省"报，比按 bbox 猜准
 * （天津地界上 bbox 会先命中北京，报出来就是错的）。 */
static int tiles_have_edge(const MapSet *ms, int cx, int cy, int r, CovHit *h)
{
    h->srcs = ms->srcs;
    h->n = 0;
    h->prov = -1;
    for (int dy = -r; dy <= r && h->n == 0; dy++)
        for (int dx = -r; dx <= r && h->n == 0; dx++)
            mapset_query_tile(ms, cx + dx, cy + dy, count_edge, h);
    return h->n;
}

int mapset_coverage(const MapSet *ms, double lat, double lon, double radius_m,
                    char *prov_out, size_t cap)
{
    if (prov_out && cap) prov_out[0] = '\0';
    if (!ms || ms->n == 0) return COV_NONE;

    int32_t lat_e7 = (int32_t)(lat * 1e7);
    int32_t lon_e7 = (int32_t)(lon * 1e7);

    /* ---- 粗判：落在哪个省的 bbox 内 ---- */
    int hit = -1;
    for (int i = 0; i < ms->n; i++) {
        const MapSrc *s = &ms->srcs[i];
        if (lat_e7 < s->bbox[0] || lon_e7 < s->bbox[1] ||
            lat_e7 > s->bbox[2] || lon_e7 > s->bbox[3])
            continue;
        /* bbox 在卸载后仍保留，所以这里即使省已卸载也能命中；命中后确保打开
         * 再查瓦片/命名，避免"视口外查询"误报无数据（卸载前全省常驻时无此问题）。 */
        mapset_ensure_loaded((MapSet *)ms, i);
        if (hit < 0) hit = i;
    }
    if (hit < 0) return COV_NONE;                 /* 一个省的 bbox 都没盖住 */

    /* bbox 会重叠（天津的 bbox 能盖到北京边上），所以省名不能直接用 bbox 命中的
       那个，要走 mapset_detail_prov：它会在被完全包含的小省里挑真正含有该坐标的
       那个（北京 ⊂ 河北，落在北京市内就该说 beijing，而不是 hebei）。 */
    int detail = mapset_detail_prov(ms, lat_e7, lon_e7, hit);
    if (detail < 0 || detail >= ms->n) detail = hit;
    if (prov_out && cap) snprintf(prov_out, cap, "%s", ms->srcs[detail].name);

    /* 细判：那附近到底有没有路 */
    int have_index = 0;
    for (int i = 0; i < ms->n; i++)
        if (ms->srcs[i].loaded && ms->srcs[i].has_tiles) { have_index = 1; break; }
    if (!have_index) return COV_UNKNOWN;

    double wx, wy;
    nm_mercator(lat, lon, &wx, &wy);
    double Z = (double)(1u << NM_TILE_Z);
    int cx = (int)(wx * Z), cy = (int)(wy * Z);

    /* Z14 一个瓦片在赤道约 2.45 km，往高纬度还要再乘 cos(lat)，
       所以半径按纬度折算一下，别在高纬度只搜了半个瓦片就下结论。 */
    double tile_m = 40075000.0 / Z * cos(lat * 3.14159265358979 / 180.0);
    if (tile_m < 1.0) tile_m = 1.0;
    int r = (int)(radius_m / tile_m);
    if (r < 1) r = 1;
    if (r > 3) r = 3;                             /* 最多 7×7，够了 */

    CovHit h;
    if (tiles_have_edge(ms, cx, cy, r, &h) > 0) {
        if (h.prov >= 0 && h.prov < ms->n && prov_out && cap)
            snprintf(prov_out, cap, "%s", ms->srcs[h.prov].name);
        return COV_HAS_ROAD;
    }
    return COV_BBOX_ONLY;
}

void coverage_hint(const MapSet *ms, double lat, double lon, int cov,
                   const char *prov, char *out, size_t cap)
{
    if (!out || !cap) return;
    out[0] = '\0';

    /* 已加载的省列出来，让人一眼看出自己手上有什么 */
    char provs[512];
    provs[0] = '\0';
    size_t used = 0;
    for (int i = 0; i < ms->n && used < sizeof(provs) - 32; i++) {
        int n = snprintf(provs + used, sizeof(provs) - used, "%s%s",
                         i ? " " : "", ms->srcs[i].name);
        if (n > 0) used += (size_t)n;
    }

    if (cov == COV_HAS_ROAD) {
        snprintf(out, cap, "有地图数据（%s）。", prov && *prov ? prov : "—");
        return;
    }

    const char *what =
        (cov == COV_NONE)      ? "这里不在任何已加载省份的数据范围内" :
        (cov == COV_BBOX_ONLY) ? "这里在某个省的数据范围内，但附近没有路网数据"
                                 "（多半是水域、荒漠或尚未采集的区域）" :
                                 "无法判断（已加载的省都没有空间索引）";

    snprintf(out, cap,
             "%s。\n"
             "位置 %.5f, %.5f    已加载 %d 个省：%s\n"
             "请下载该地区的 OSM 数据（*.osm.pbf）放进 sources/，"
             "然后运行 ./scripts/sync_maps.sh 自动转换。\n"
             "下载来源：\n"
             "  Geofabrik（按省/地区，中国） https://download.geofabrik.de/asia/china/\n"
             "  BBBike（可自己框选范围）     https://extract.bbbike.org/",
             what, lat, lon, ms->n, provs[0] ? provs : "（无）");
}
