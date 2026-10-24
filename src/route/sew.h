#ifndef NAVIMAP_ROUTE_SEW_H
#define NAVIMAP_ROUTE_SEW_H

/* 瓦片缝合缓存（原 route.c 的"瓦片缝合缓存"段，整体迁到 sew.c）。
 * SewEnt / SewSlot / SEW_SLOTS 仍定义在 route_priv.h（struct Router 要用它们，
 * 反过来 sew.h 若定义它们会与 route_priv.h 形成包含环），这里只放对外接口。
 * 与 restr.h 同构：需要完整 Router 定义 ⇒ 包含 route_priv.h。 */

#include "route/route_priv.h"   /* 完整 Router / SewSlot / SewEnt / SEW_SLOTS */

/* 取/建这片瓦片的缓存槽（哈希 + 线性探测；探测窗口满则淘汰起点槽）。 */
SewSlot *sew_get(Router *r, int tx, int ty);

/* 只看瓦片目录，不遍历边：判断这片瓦片被几个省覆盖（去重后）。
 * <2 个省就没有缝合的可能，连索引都不用建——这是搜索热路径上最大的一笔开销。 */
int tile_prov_count(const MapSet *ms, int tx, int ty);

/* 需要完整成员列表时才调（起终点吸附、多省瓦片的缝合）。 */
void sew_build(Router *r, SewSlot *s);

#endif
