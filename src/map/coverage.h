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

/* radius_m：细判的搜索半径（默认 3000 m，够覆盖 Z14 一个瓦片约 2.4 km）。
 * prov_out：命中的省名（没有就填空串）。返回上面四个取值之一。 */
int  mapset_coverage(const MapSet *ms, double lat, double lon, double radius_m,
                     char *prov_out, size_t cap);

/* 生成面向用户的提示文案（含去哪下载、下载后怎么办）。
 * 这个函数刻意不碰 SDL，好让 CLI 能直接断言它的输出。 */
void coverage_hint(const MapSet *ms, double lat, double lon, int cov,
                   const char *prov, char *out, size_t cap);

#endif
