#ifndef NAVIMAP_GRAPH_BUILD_H
#define NAVIMAP_GRAPH_BUILD_H

#include <stdint.h>
#include <stddef.h>

/* PBF → 自描述路网二进制。
 * 单文件流程（两遍扫描，因为 way 只给 node 引用，坐标在 DenseNodes 里）：
 *   第 1 遍：挑出可通行的 way，收集它引用的 node id；顺带解析 restriction 关系
 *   排序去重：node id → 内部连续下标
 *   第 2 遍：再扫一次 DenseNodes，把命中的 node 的经纬度回填到下标位置
 *   建边：way 内相邻 node 对 → 有向边（单向路只留正向），再排成 CSR
 */

typedef struct {
    uint64_t ways_total;
    uint64_t ways_kept;
    uint64_t ways_skip;
    uint64_t ways_no_hw;            /* 没有 highway 标签（building/boundary…） */
    uint64_t ways_too_short;        /* 引用节点 < 2 */
    uint64_t nodes;
    uint64_t edges;
    uint64_t dropped_missing_node;  /* way 引用了文件里没有的节点 */
    uint64_t dead_nodes;             /* 因无坐标被整条剔除的节点 */

    uint64_t rt_unknown_ways;       /* highway 取值不在表内 */
    uint64_t rt_unknown_nondrivable;/* 其中命中"已知不可通行"白名单（不报警） */
    uint64_t rt_unknown_drivable;   /* ★ 闸门：必须为 0 */

    uint64_t restr_scanned, restr_written;
    uint64_t restr_skip_via_way, restr_skip_incomplete, restr_skip_unresolved;
    uint64_t restr_via_way_written;  /* 其中由 via=way 展开成的两段限制 */

    /* 线/面要素（只显示、不进路由） */
    uint64_t lines_kept;               /* lines.bin 条数（步行道/铁路/水系线） */
    uint64_t areas_kept;               /* areas.bin 条数（闭合的面） */
    uint64_t areas_skip_open;          /* 打了面标签但 way 不闭合 */
    uint64_t linearea_dropped_missing; /* 引用的节点缺失/被剔除，整条丢弃 */
    uint64_t tiles_written;            /* 空间索引 tile 数（0 = 未生成） */

    uint64_t names;                 /* 路名池条目数 */
    int32_t  bbox[4];               /* lat_min lon_min lat_max lon_max，1e-7 度 */
    double   elapsed_sec;
} GraphBuildStats;

/* sources[0..n-1] 合图（当前实现按单文件逐次调用即可，跨文件去重在下一阶段）。 */
int graph_build(const char *pbf_path, const char *out_dir, GraphBuildStats *st);

#endif
