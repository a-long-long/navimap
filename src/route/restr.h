#ifndef NAVIMAP_ROUTE_RESTR_H
#define NAVIMAP_ROUTE_RESTR_H

#include "route/route_priv.h"   /* 完整 Router / RestrIdx / MapSrc / st_find */

/* 转向限制子系统（原 route.c 的"转向限制"段，整体迁到 restr.c）。
 * 这些函数仍接收 Router*，但只是惰性查 r->ri / r->ms / r->opt / r->st，
 * 不持有状态，是纯函数式查询；route.c 的搜索核心直接调用它们。 */

/* NAVIMAP_RESTR_DEBUG=1 时打印 Miss 诊断（restr.c 定义，route.c 核心也用）。 */
int restr_debug(void);

/* 把某省的限制记录解析成三张排序索引表（kind 0/1/2）。惰性：首次用到才建。 */
void restr_build(const MapSrc *s, RestrIdx *ri);

/* 在 kv 升序表里找 key 对应的区间起点（二分下界）。 */
size_t kv_lower(const uint64_t *kv, size_t n, uint64_t key);

/* kind=0：{via, from}→to 命中即禁。 */
int restr_blocked(Router *r, uint32_t prov, uint32_t via_node,
                  uint32_t from_edge, uint32_t to_edge);

/* kind=1（only_*）：{via, from} 有 only_* 且 to 不在允许集里 ⇒ 禁。 */
int restr_blocked_only(Router *r, uint32_t prov, uint32_t via_node,
                       uint32_t from_edge, uint32_t to_edge);

/* kind=2（via=way 两段）：prev→from→to 命中即禁。 */
int restr_blocked2(Router *r, uint32_t prov, uint32_t via_node,
                   uint32_t prev_edge, uint32_t from_edge, uint32_t to_edge);

/* 反向搜索保守版：只看 (via, from, to) 命中即禁（prev 未知）。 */
int restr_blocked2_rev(Router *r, uint32_t prov, uint32_t via_node,
                       uint32_t from_edge, uint32_t to_edge);

/* 双向会合点处的转向补查（正向最后一条边 × 反向第一条边）。 */
int seam_blocked(Router *r, uint64_t k);

#endif
