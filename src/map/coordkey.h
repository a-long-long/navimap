#ifndef NAVIMAP_COORDKEY_H
#define NAVIMAP_COORDKEY_H

#include <stdint.h>

/* ---------------------------------------------------------------------------
 * 路由节点的跨省主键：坐标即 id
 *
 * 学自 OsmAnd（OsmAnd-core/src/Routing/RoutePlannerContext.cpp:132）：
 *     uint64_t id = (x31 << 31) | y31;
 * 它不给节点编全局号，而是把整数坐标本身当主键。查找时对所有数据源
 * 按 tile 查询、把同一坐标上的分段串起来——跨省接边不需要任何
 * "本省下标 → 全局下标"的映射表。
 *
 * 我们用 e7 度（1e-7 度，OSM PBF 原生精度）直接打包。跨省一致性有保证：
 * 各省 pbf 是同一份 OSM 数据按多边形裁出来的，同一节点在两个省里的
 * e7 坐标逐字节相同（实测北京 ⊂ 河北 100% 节点 id 重合、渲染 md5 相同）。
 *
 * 为什么不用节点下标或 OSM node id 做键：
 *   - 下标：只在单个省内有意义，跨省要维护映射表，加一个省要重建全部；
 *   - OSM node id：可以做，但路由展开时真正需要的是"这个坐标上有哪些
 *     分段"，坐标键配合 tile 索引一次就能查全，id 键反而要多一层
 *     id→坐标的转换。
 * ------------------------------------------------------------------------- */

static inline uint64_t coord_key_e7(int32_t lat_e7, int32_t lon_e7)
{
    return ((uint64_t)(uint32_t)lat_e7 << 32) | (uint32_t)lon_e7;
}

static inline int32_t coord_key_lat(uint64_t k) { return (int32_t)(uint32_t)(k >> 32); }
static inline int32_t coord_key_lon(uint64_t k) { return (int32_t)(uint32_t)(k & 0xFFFFFFFFu); }

#endif
