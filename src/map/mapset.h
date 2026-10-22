#ifndef NAVIMAP_MAPSET_H
#define NAVIMAP_MAPSET_H

#include "map/binfmt.h"
#include "render/labels.h"

/* 多省图集：一次加载 products/graph 下的所有省，渲染时按 bbox 裁剪。
 *
 * 为什么按省分片而不是把 8 省合成一张 CSR：跨文件去重需要 5000 万级
 * osmid→全局下标的外排序，而且路由合并是阶段 8 的事；先让"看到跨省
 * 连续的地图"成立，显示层各省独立 mmap、独立裁剪就够了。相邻省
 * （北京/河北）边界上同一条 OSM way 会两边各画一次，完全重合、
 * 软渲染无 alpha 叠加，视觉无差异。
 *
 * 加载端校验也在这里做（此前欠着）：每个省的 nodes/edges 过
 * nmbin_check（magic/版本/尺寸自洽），不过的省跳过并警告——
 * 宁可少一个省，也不要静默画错图。
 */

typedef struct {
    char      name[64];      /* 目录名（省名） */
    char      dir[4096];     /* 省目录绝对路径：派生文件（rev.bin 等）要按它找 */
    NMBinFile nodes, edges;
    NMBinFile restrictions;             /* 转向限制（可选，阶段 8 路由用） */
    int       has_restr;
    NMBinFile lines, areas, polyrefs;   /* 线/面要素（旧目录可没有） */
    int       has_features;             /* 三者齐备才为 1 */
    NMBinFile tiles, edgelist;          /* 空间索引（可选） */
    int       has_tiles;
    int       loaded;       /* 本省份 mmap 是否已打开；mapset_set_view 按需置 1 / 清 0。
                             * 卸载只关文件、保留 name/dir/bbox/dup_of，省份下标恒定 */
    int       dup_of;       /* >=0：本省数据被 srcs[dup_of] 完全包含（同一份
                             * OSM 数据被多边形裁切时的常见结果，实测
                             * 北京/天津 ⊂ 河北 100%）。路由缝合时跳过本省的
                             * 副本——留着只会让每个路口被展开两次。
                             * 渲染不受影响（那边有按 OSM id 的去重）。 */
    int32_t   bbox[4];      /* e7: lat0 lon0 lat1 lon1，来自 manifest；
                             * 旧目录没有 manifest 时置为全域（不做裁剪） */
    NamePool  np;           /* 该省的路名池（names.bin） */
    int32_t  *area_bbox;    /* 面缓存（paint_areas 懒填，na*4 个 i32） */
    int32_t  *line_bbox;    /* 线缓存（paint_lines 懒填，nl*4 个 i32）。
                             * 线要素没有 tile 索引，缺了它每帧要白投影 10 万条线
                             * 的全部顶点——实测占 z14 一帧的 75%。 */
} MapSrc;

typedef struct {
    MapSrc *srcs;
    int     n, cap;
    int32_t bbox[4];       /* 全体省份的并集（e7） */
} MapSet;

/* graph_dir 可以是单个省目录（含 nodes.bin），也可以是父目录
 * （扫描所有含 manifest.txt 的子目录，跳过 *.tmp.* 与隐藏目录）。
 * 返回 0 = 至少加载了一个省；-1 = 一个都没有。 */
int  mapset_load(MapSet *ms, const char *graph_dir);
void mapset_free(MapSet *ms);

/* 视口驱动的按需加载：只保留与 vb（e7 的 [lat0,lon0,lat1,lon1]）相交（含边距）的
 * 省份打开，其余卸载。省份下标恒定（槽位 = 省 id），迭代函数跳过 !loaded，因此
 * 路由/转向里写进结果的 prov 下标始终有效，不会因重排而悬垂。 */
void mapset_set_view(MapSet *ms, const int32_t vb[4]);

/* 按省下标确保该省已加载（maneuver/snap 访问特定省前调用，避免卸载后悬垂访问）。 */
void mapset_ensure_loaded(MapSet *ms, int prov);

/* 供状态栏/日志用的汇总：总节点数、总边数 */
void mapset_totals(const MapSet *ms, uint64_t *nodes, uint64_t *edges);

/* 该坐标属于哪个省（用于显示）。被完全包含的小省优先：北京 ⊂ 河北，
   路由实际走的是 hebei 那份数据，但显示在北京市内就该说 beijing。
   fallback 是调用方已知的下标（比如路径点所在的省），找不到更具体的就返回它。 */
int mapset_detail_prov(const MapSet *ms, int32_t lat_e7, int32_t lon_e7,
                       int fallback);

/* ---------------------------------------------------------------------------
 * 按 tile 跨省查询（阶段 8 跨省路由的地基）
 *
 * 对每个盖住 (tx,ty) 这个 Z14 瓦片的省，遍历其空间索引里的
 * {from 节点, CSR 边下标} 对，逐条回调。这正是 OsmAnd
 * RoutePlanner::loadTileHeader（RoutePlanner.cpp:250-293）做的事——
 * 它遍历所有 source 收集覆盖该 tile 的 subsection，我们遍历所有省
 * 收集该 tile 的边。路由展开时以 coord_key_e7(节点坐标) 为主键，
 * 同一坐标上来自不同省的分段自然合成一个图节点。
 *
 * 返回回调被调用的总次数；没有索引的省跳过。
 * ------------------------------------------------------------------------- */
typedef void (*MapsetTileFn)(const MapSrc *s, uint32_t from_node, uint32_t edge_idx, void *ud);
int  mapset_query_tile(const MapSet *ms, int tx, int ty, MapsetTileFn fn, void *ud);

#endif
