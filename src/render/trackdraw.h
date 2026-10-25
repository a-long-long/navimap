#ifndef NAVIMAP_TRACKDRAW_H
#define NAVIMAP_TRACKDRAW_H

#include <stdint.h>
#include "map/gpx.h"
#include "map/map_view.h"
#include "render/raster.h"
#include "render/font.h"

/* GPX 轨迹 / 收藏点的地图叠加层（阶段：轨迹落地）。
 *
 * 只做"画"，不做解析/存取（那是 map/gpx.c）。配色与底图、导航路线都不糊：
 * 路线是蓝 (30,110,235)，所以轨迹用各自的高饱和色（gpx.c 的调色板），
 * 统一加白描边保证在任何底色上都跳出来。
 */

/* 画全部轨迹。视口外的段整段跳过（长途轨迹几万点，全画一遍没意义）。
 * 返回真正画出的段数。 */
int  paint_tracks(Raster *rs, const MapView *v, const GpxStore *g);

/* 画收藏点：白边 + 金芯圆点 + 右侧名称（f 为 NULL 时不画名字）。
 * 返回画出的个数。 */
int  paint_favorites(Raster *rs, const MapView *v, const GpxStore *g, Font *f);

#endif /* NAVIMAP_TRACKDRAW_H */
