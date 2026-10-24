#ifndef NAVIMAP_ROUTE_COST_H
#define NAVIMAP_ROUTE_COST_H

#include "route/route_priv.h"   /* 完整 Router / MapSrc / MapSet / 常量 / H_SPEED_KMH */

/* 代价 / 启发式 / 地标表辅助（原 route.c 的"代价与启发式"段 + 生命周期辅助，
 * 整体迁到 cost.c）。route.c 核心搜索也调用这些，故在此声明。 */

/* 道路等级 → 兜底车速（km/h），edge_cost 在缺 speed 字段时用。 */
uint8_t fallback_speed(uint8_t cls);

/* 单边代价：mode_time=1 返回秒，否则返回米；meters 回传米（用于 navsim 自检）。 */
double edge_cost(const Router *r, const uint8_t *er, double *meters);

/* 启发式下界：大圆距离，与地标下界取 max（admissible 且 consistent）。 */
double heur(const Router *r, int dir, int32_t lat_e7, int32_t lon_e7);

/* 节点坐标键（coord_key_e7）。 */
uint64_t node_coord(const MapSrc *s, uint32_t node);

/* 地标表总开关（NAVIMAP_LM=0 关）。 */
int lm_enabled(void);

/* 诊断旋钮：在地表 penalty 上再加 N 米（默认 0）。 */
uint32_t lm_penalty_add_m(void);

/* 图目录 = 省目录的上一级（lm.bin / rev.bin 全图只有一份，写在父目录）。 */
int graph_root_dir(const MapSet *ms, char *out, size_t cap);

#endif
