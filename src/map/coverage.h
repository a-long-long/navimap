#ifndef NAVIMAP_COVERAGE_H
#define NAVIMAP_COVERAGE_H

#include "map/mapset.h"

/* ---------------------------------------------------------------------------
 * "这里有没有地图" —— 浏览和定位都要回答这个问题。
 *
 * 分两级，因为 bbox 是矩形，而省界是不规则的：河北省的 bbox 里既有渤海、
 * 也包着北京天津。所以"落在某个省的 bbox 内"只说明"可能有"，不代表真有路。
 *
 *   COV_NONE      不在任何已加载省份的 bbox 内 —— 确定没有，该提示下载
 *   COV_BBOX_ONLY 在某个省的 bbox 内，但附近确实没有路网（水域/未采集）
 *   COV_HAS_ROAD  附近有路 —— 有图
 *   COV_UNKNOWN   省都没有空间索引，细判做不了，只能靠粗判
 * ------------------------------------------------------------------------- */

enum { COV_NONE = 0, COV_BBOX_ONLY = 1, COV_HAS_ROAD = 2, COV_UNKNOWN = -1 };

/* ---------------------------------------------------------------------------
 * 反查"关注点在哪" —— 只回答到**国家 / 省级**这一层。
 *
 * 为什么只到省：市/区要靠地名库（places_auto.db），而那在 render 层
 * （LabelCache）；coverage 属 map 层，反向依赖 render 会破坏模块方向。
 * 所以市/区由上层用 label_nearest_admin() 补齐后再拼成一句话。
 * ------------------------------------------------------------------------- */
#define COV_PROV_MAX 40

/* 判断"离某个已加载省多近，才敢认为它仍在中国境内"的阈值(km)。
 * 本机没有国界数据，只有省 bbox：距最近省超过一个省的尺度时，最可能的情况是
 * 用户已经跑到境外（实测蒙古乌兰巴托距河北 bbox 764 km），此时不该断言国别。
 * 取 150 km ≈ 中国相邻两省中心到省界的典型距离。 */
#define COV_INLAND_KM 150.0

typedef struct {
    char   country[24];            /* "中国"；判断不出来（多半在境外）时为空串 */
    char   prov[COV_PROV_MAX];     /* 中文省名 "河北"；无则空 */
    char   prov_id[COV_PROV_MAX];  /* 拼音目录名 "hebei"（= 已加载省名 / 下载叶子名） */
    char   city[64];               /* 地级市 "廊坊市"；由上层用地名库补（见 label_nearest_admin） */
    char   district[64];           /* 区/县 "广阳区"；同上 */
    int    prov_guessed;           /* 1 = prov 是"按最近省会推断"的，不是 bbox 命中的 */
    double nearest_km;             /* prov_guessed ? 到最近省会的距离 : 到最近已加载省的距离；
                                    * 落在已加载省内为 0，无法算为 -1 */
    double nearest_loaded_km;      /* 到最近**已加载**省 bbox 的距离（判国别用，无实证时为 -1） */
    char   dl_name[COV_PROV_MAX];  /* 建议下载的 Geofabrik 叶子名（如 "hebei"）；无建议为空 */
    char   dl_label[COV_PROV_MAX]; /* 建议下载的显示名（如 "河北"）；无建议为空 */
} CovPlace;

/* 反查 (lat, lon) 落在哪个省 / 离哪个已加载省最近，并给出**该下载哪一块**。
 * 返回命中的省下标（ms->srcs 的下标），一个都没盖住时返回 -1。
 * out 一定被写满（无信息时各字段置空串、nearest_km=-1），调用方无需预清。 */
int  coverage_locate(const MapSet *ms, double lat, double lon, CovPlace *out);

/* radius_m：细判的搜索半径（默认 3000 m，够覆盖 Z14 一个瓦片约 2.4 km）。
 * prov_out：命中的省名（没有就填空串）。返回上面四个取值之一。 */
int  mapset_coverage(const MapSet *ms, double lat, double lon, double radius_m,
                     char *prov_out, size_t cap);

/* 生成面向用户的提示文案（含"在哪 / 该下哪块 / 怎么装"）。
 * pl 可为 NULL（只有省信息时的退化路径）。
 * 这个函数刻意不碰 SDL / Qt，好让 CLI 能直接断言它的输出。 */
void coverage_hint(const MapSet *ms, double lat, double lon, int cov,
                   const CovPlace *pl, char *out, size_t cap);

#endif
