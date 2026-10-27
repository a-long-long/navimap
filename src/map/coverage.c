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

/* ===========================================================================
 * 行政区反查（省一级）
 *
 * 表里 id 必须与 products/graph/<目录名> 以及 Geofabrik 中国路径的叶子名一致
 * （asia/china/<id>），这样反查结果能**直接**拿去预选下载项，不用再做一次映射。
 * 港澳台按规范写全称。
 * ========================================================================= */
static const struct { const char *id; const char *cn; } PROV_CN[] = {
    { "beijing",        "北京"     }, { "tianjin",     "天津"     },
    { "hebei",          "河北"     }, { "shanxi",      "山西"     },
    { "inner-mongolia", "内蒙古"   }, { "liaoning",    "辽宁"     },
    { "jilin",          "吉林"     }, { "heilongjiang","黑龙江"   },
    { "shanghai",       "上海"     }, { "jiangsu",     "江苏"     },
    { "zhejiang",       "浙江"     }, { "anhui",       "安徽"     },
    { "fujian",         "福建"     }, { "jiangxi",     "江西"     },
    { "shandong",       "山东"     }, { "henan",       "河南"     },
    { "hubei",          "湖北"     }, { "hunan",       "湖南"     },
    { "guangdong",      "广东"     }, { "guangxi",     "广西"     },
    { "hainan",         "海南"     }, { "chongqing",   "重庆"     },
    { "sichuan",        "四川"     }, { "guizhou",     "贵州"     },
    { "yunnan",         "云南"     }, { "tibet",       "西藏"     },
    { "shaanxi",        "陕西"     }, { "gansu",       "甘肃"     },
    { "qinghai",        "青海"     }, { "ningxia",     "宁夏"     },
    { "xinjiang",       "新疆"     }, { "hong-kong",   "中国香港" },
    { "macau",          "中国澳门" }, { "taiwan",      "中国台湾" },
    /* 本机出现过的市级目录也收进来：不在表里就会退化成显示拼音 */
    { "shijiazhuang",   "石家庄"   },
};

static const char *prov_cn(const char *id)
{
    if (!id || !*id) return "";
    for (size_t i = 0; i < sizeof(PROV_CN) / sizeof(PROV_CN[0]); i++)
        if (strcmp(PROV_CN[i].id, id) == 0) return PROV_CN[i].cn;
    return "";   /* 未知目录名：调用方决定退化显示 */
}

/* ---------------------------------------------------------------------------
 * 全国省会 / 首府坐标表 —— 用来在没有该省数据时**推断"大致是哪个省"**。
 *
 * 为什么需要它：本机只装有 8 个省的数据，省 bbox 也只覆盖这 8 个。用户在
 * 湖北（未下载）时，"距最近的已加载省（河南）152 km"这种回答等于答非所问，
 * 还会把人引去下错省。有了一张 34 点的省会表，就能给出"湖北（按最近省会
 * 推断）"，并且把建议下载项指到 hubei 而不是 henan。
 *
 * 局限（必须如实告知，不要当行政区划依据）：
 *   · 用"最近省会"近似省域，省界附近会偏到邻省（廊坊离北京比离石家庄近）；
 *   · 用它判国别是**不可靠**的 —— 乌兰巴托距呼和浩特约 875 km，而西藏阿里
 *     距拉萨 1100 km。所以国别仍由 COV_INLAND_KM 这个保守阈值把关，宁可
 *     说"可能境外"也不硬猜。
 * ------------------------------------------------------------------------- */
static const struct { const char *id; const char *cn; double lat, lon; } CAPITAL[] = {
    { "beijing",        "北京",     39.905, 116.391 },
    { "tianjin",        "天津",     39.085, 117.199 },
    { "hebei",          "河北",     38.042, 114.515 },
    { "shanxi",         "山西",     37.870, 112.549 },
    { "inner-mongolia", "内蒙古",   40.842, 111.749 },
    { "liaoning",       "辽宁",     41.805, 123.431 },
    { "jilin",          "吉林",     43.817, 125.324 },
    { "heilongjiang",   "黑龙江",   45.757, 126.642 },
    { "shanghai",       "上海",     31.230, 121.474 },
    { "jiangsu",        "江苏",     32.060, 118.797 },
    { "zhejiang",       "浙江",     30.274, 120.155 },
    { "anhui",          "安徽",     31.820, 117.227 },
    { "fujian",         "福建",     26.074, 119.297 },
    { "jiangxi",        "江西",     28.682, 115.858 },
    { "shandong",       "山东",     36.651, 117.120 },
    { "henan",          "河南",     34.747, 113.625 },
    { "hubei",          "湖北",     30.593, 114.306 },
    { "hunan",          "湖南",     28.228, 112.939 },
    { "guangdong",      "广东",     23.129, 113.264 },
    { "guangxi",        "广西",     22.817, 108.367 },
    { "hainan",         "海南",     20.044, 110.199 },
    { "chongqing",      "重庆",     29.563, 106.551 },
    { "sichuan",        "四川",     30.573, 104.066 },
    { "guizhou",        "贵州",     26.647, 106.630 },
    { "yunnan",         "云南",     25.038, 102.718 },
    { "tibet",          "西藏",     29.652,  91.132 },
    { "shaanxi",        "陕西",     34.341, 108.940 },
    { "gansu",          "甘肃",     36.061, 103.834 },
    { "qinghai",        "青海",     36.617, 101.778 },
    { "ningxia",        "宁夏",     38.487, 106.231 },
    { "xinjiang",       "新疆",     43.826,  87.617 },
    { "hong-kong",      "中国香港", 22.320, 114.173 },
    { "macau",          "中国澳门", 22.199, 113.544 },
    { "taiwan",         "中国台湾", 25.033, 121.565 },
};

/* 找最近的省会。返回其下标，*dist_km 写球面距离。 */
static int nearest_capital(double lat, double lon, double *dist_km)
{
    int    bi = -1;
    double best = 1e18;
    for (size_t i = 0; i < sizeof(CAPITAL) / sizeof(CAPITAL[0]); i++) {
        double d = nm_haversine_m(lat, lon, CAPITAL[i].lat, CAPITAL[i].lon) / 1000.0;
        if (d < best) { best = d; bi = (int)i; }
    }
    if (dist_km) *dist_km = (bi >= 0) ? best : -1.0;
    return bi;
}

/* 点到经纬度矩形的最近距离(km)：先把点 clamp 进矩形，再算球面距离。
 * 省一级尺度上与精确算法差 <1%，而这里只用于回答"离最近的省有多远"。 */
static double bbox_dist_km(const int32_t bbox[4], int32_t lat_e7, int32_t lon_e7)
{
    int32_t clat = lat_e7, clon = lon_e7;
    if (clat < bbox[0]) clat = bbox[0];
    if (clat > bbox[2]) clat = bbox[2];
    if (clon < bbox[1]) clon = bbox[1];
    if (clon > bbox[3]) clon = bbox[3];
    if (clat == lat_e7 && clon == lon_e7) return 0.0;
    return nm_haversine_m(lat_e7 / 1e7, lon_e7 / 1e7,
                        clat / 1e7,   clon / 1e7) / 1000.0;
}

int coverage_locate(const MapSet *ms, double lat, double lon, CovPlace *out)
{
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    out->nearest_km = -1.0;
    if (!ms || ms->n == 0) return -1;

    int32_t lat_e7 = (int32_t)(lat * 1e7);
    int32_t lon_e7 = (int32_t)(lon * 1e7);

    /* ---- 落在哪个省的 bbox 里 ---- */
    int hit = -1;
    for (int i = 0; i < ms->n; i++) {
        const MapSrc *s = &ms->srcs[i];
        if (lat_e7 < s->bbox[0] || lon_e7 < s->bbox[1] ||
            lat_e7 > s->bbox[2] || lon_e7 > s->bbox[3])
            continue;
        if (hit < 0) hit = i;
    }

    if (hit >= 0) {
        /* bbox 重叠时挑真正含该坐标的那个（北京 ⊂ 河北），与 mapset_coverage 同源 */
        int detail = mapset_detail_prov(ms, lat_e7, lon_e7, hit);
        if (detail < 0 || detail >= ms->n) detail = hit;
        const char *id = ms->srcs[detail].name;
        const char *cn = prov_cn(id);
        snprintf(out->prov_id, sizeof(out->prov_id), "%s", id);
        snprintf(out->prov,    sizeof(out->prov),    "%s", (cn && *cn) ? cn : id);
        /* 目录在我们的省表里 ⇒ 一定在中国境内 */
        snprintf(out->country, sizeof(out->country), "%s", (cn && *cn) ? "中国" : "");
        out->nearest_km = 0.0;
        snprintf(out->dl_name,  sizeof(out->dl_name),  "%s", id);
        snprintf(out->dl_label, sizeof(out->dl_label), "%s", (cn && *cn) ? cn : id);
        return detail;
    }

    /* ---- 一个 bbox 都没盖住：先用省会表推断"大致是哪个省"，再报距离 ----
     *
     * 为什么不用"最近的已加载省"：那样在湖北会报"距河南 152 km"，还把人引去
     * 下河南（实测踩到）。省会表能给出"湖北（推断）"，建议下载项也就对了。
     * 两个距离都记下来：nearest_km 用于文案（离该省/最近已知点的距离），
     * 国别仍用"到最近**已加载**省的距离"来把守 —— 那是唯一有实证依据的量。 */
    double nearest_loaded_km = 1e18;
    for (int i = 0; i < ms->n; i++) {
        double d = bbox_dist_km(ms->srcs[i].bbox, lat_e7, lon_e7);
        if (d < nearest_loaded_km) nearest_loaded_km = d;
    }

    double cap_km = -1.0;
    int ci = nearest_capital(lat, lon, &cap_km);
    if (ci >= 0) {
        out->prov_guessed = 1;
        out->nearest_km   = cap_km;
        snprintf(out->prov_id, sizeof(out->prov_id), "%s", CAPITAL[ci].id);
        snprintf(out->prov,    sizeof(out->prov),    "%s", CAPITAL[ci].cn);
        snprintf(out->dl_name,  sizeof(out->dl_name),  "%s", CAPITAL[ci].id);
        snprintf(out->dl_label, sizeof(out->dl_label), "%s", CAPITAL[ci].cn);
    }
    if (nearest_loaded_km < 1e17)
        out->nearest_loaded_km = nearest_loaded_km;

    /* 国别：只有"离某个已加载省足够近"才敢说中国境内（该省没下载而已）。
     * 光凭"最近省在中国"会把蒙古乌兰巴托（距河北 764 km）报成中国，实测踩过。 */
    if (nearest_loaded_km <= COV_INLAND_KM)
        snprintf(out->country, sizeof(out->country), "%s", "中国");

    return -1;
}

void coverage_hint(const MapSet *ms, double lat, double lon, int cov,
                   const CovPlace *pl, char *out, size_t cap)
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
        snprintf(out, cap, "有地图数据（%s）。",
                 (pl && pl->prov[0]) ? pl->prov : "—");
        return;
    }

    /* ---- "这是哪儿"：国家 / 省 / 市 / 区，查到几层报几层 ----
     * 用"逐段追加 + 空段跳过"拼，不要写成 %s%s%s 的定长模板：某个字段为空时
     * 会在句尾留下多余空格（实测输出过"中国 河北 。"这种带空格句号）。 */
    char where[224];
    size_t wl = 0;
    where[0] = '\0';
    {
        const char *parts[4] = {
            (pl && pl->country[0])  ? pl->country  : "",
            (pl && pl->prov[0])     ? pl->prov     : "",
            (pl && pl->city[0])     ? pl->city     : "",
            (pl && pl->district[0]) ? pl->district : "",
        };
        for (int i = 0; i < 4; i++) {
            if (!parts[i][0]) continue;
            int n = snprintf(where + wl, sizeof(where) - wl, "%s%s",
                             wl ? " · " : "", parts[i]);
            if (n > 0) wl += (size_t)n;
        }
    }

    const char *country = (pl && pl->country[0]) ? pl->country : "";
    const char *prov    = (pl && pl->prov[0])    ? pl->prov    : "";

    if (pl && pl->nearest_km > 1.0) {
        /* 在已加载省之外。四种说法，各有各的把握程度 —— 不要硬凑成一句：
         *   ① 境内（离已加载省近）且推断出省 ⇒ 直接说省，并注明"推断"
         *   ② 境外/太远但推断出省         ⇒ 报"可能境外"，省只作参考
         *   ③ 无省表可用                 ⇒ 只能说不确定
         */
        const char *how = (pl->prov_guessed && prov[0])
                              ? "（按最近省会推断）" : "";
        if (*country && prov[0])
            snprintf(where, sizeof(where),
                     "%s境内、但该省未下载；大致在 %s%s，距 %.0f km",
                     country, prov, how, pl->nearest_km);
        else if (*country)
            snprintf(where, sizeof(where),
                     "%s境内、但不在已加载的省份里（距最近的省约 %.0f km）",
                     country, pl->nearest_loaded_km);
        else if (prov[0])
            snprintf(where, sizeof(where),
                     "可能在境外（已加载省份之外）；最近的省会参考 %s%s，距 %.0f km",
                     prov, how, pl->nearest_km);
        else
            snprintf(where, sizeof(where), "无法判断（尚未加载任何省份数据）");
    } else if (!where[0]) {
        snprintf(where, sizeof(where), "未知区域");
    }

    const char *what =
        (cov == COV_NONE)      ? "这里没有地图数据" :
        (cov == COV_BBOX_ONLY) ? "这里在数据范围内，但附近没有路网"
                                 "（多半是水域、荒漠或尚未采集）" :
                                 "无法判断（已加载的省都没有空间索引）";

    /* ---- "该怎么办"：COV_NONE 才该下载；COV_BBOX_ONLY 是"数据有了、这块没路"，
     *      此时建议下载**已经加载的省**毫无意义（实测输出过"建议下载：河北"，
     *      而河北明明已加载）。这条分支必须分开写。 ---- */
    char dl[288];
    if (cov == COV_BBOX_ONLY) {
        if (prov[0])
            snprintf(dl, sizeof(dl),
                     "%s的数据已加载，此处确实没有路网（水域/未采集）；"
                     "换个位置或继续浏览即可", prov);
        else
            snprintf(dl, sizeof(dl),
                     "该省数据已加载，此处确实没有路网；换个位置或继续浏览即可");
    } else if (pl && pl->dl_label[0]) {
        snprintf(dl, sizeof(dl),
                 "建议下载：%s（『地图 → 下载全球地图』会直接定位到这一项）",
                 pl->dl_label);
    } else {
        snprintf(dl, sizeof(dl),
                 "建议下载：在『地图 → 下载全球地图』里按国家/省份选择");
    }

    snprintf(out, cap,
             "%s —— %s。\n"
             "位置 %.5f, %.5f    已加载 %d 个省：%s\n"
             "%s\n"
             "（『地图 → 下载全球地图』下载后会自动转换并加载，无需手工跑脚本。）\n"
             "数据来源 Geofabrik https://download.geofabrik.de/ ，"
             "自选范围可用 BBBike https://extract.bbbike.org/ 。",
             what, where, lat, lon, ms->n, provs[0] ? provs : "（无）", dl);
}
