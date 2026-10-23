#ifndef NAVIMAP_PBFMAP_H
#define NAVIMAP_PBFMAP_H

#include "map/binfmt.h"

/* 从 OSM PBF 直接解码出**显示层**数据（省粒度）。
 *
 * 动机：显示相关产物（lines/areas/polyrefs）只占 nmbin 的 ~200 MB
 * （8 省合计，占 nmbin 总量 4.8%），而 nmbin 整体 4.2 G 里 94% 是路由图。
 * 把显示层改成"从 PBF 按需解码 + 内存 LRU 缓存"，则这几个文件可以整个
 * 不落盘，运行时只驻留视口所在省份的显示数据。
 *
 * 为什么是**省粒度**而不是瓦片粒度：实测 PBF 的 data blob 平均覆盖全省
 * 43% 的纬度跨度（北京 621 个 blob，单个 blob 跨 0.99°/1.33°，z11 视口仍命中
 * 53% 的 blob）⇒ 按 blob 跳读的收益低于建索引的成本，**不可行**。
 * 而"整省解一次 + 缓存"是可预测的：解码一省 = 全量扫一遍该省 PBF。
 *
 * ★ 为什么**不需要**解码节点坐标：polyrefs 存的是"节点内部下标"，
 * 而这个下标空间**就是** nmbin nodes.bin 的下标空间（两者都按 osm id
 * 升序排列、去掉了文件里不存在的节点）。所以坐标直接取自 nmbin 的
 * nodes.bin，本模块只需产出 areas/lines/polyrefs 三个缓冲。
 * （8 省实测：解码结果与落盘文件**逐字节相同**。）
 *
 * 产出的 NMBinFile 是**内存缓冲**（data 由 malloc 分配、mapped=0），
 * 布局与落盘的同名文件逐字节一致 ⇒ 渲染端（paint.c）零改动。
 */

typedef struct {
    NMBinFile areas;      /* 面要素记录 */
    NMBinFile lines;      /* 线要素记录 */
    NMBinFile polyrefs;   /* u32 节点内部下标数组 */
    uint64_t n_areas;
    uint64_t n_lines;
    uint64_t n_polyrefs;
} PbfMap;

/* 从 pbf_path 解码一省的显示层到 out。
 *
 * nodes_index 必须是该省 nmbin 的 nodes.bin（提供 osm_id → 内部下标的
 * 映射）。它已经在内存里（mmap），用它做二分查表省掉"再扫一遍 PBF 取坐标"。
 *
 * 返回 0 = 成功；-1 = 失败。失败时 out 的内容未定义（调用方仍应调
 * pbfmap_free 释放可能已分配的缓冲）。 */
int  pbfmap_decode(const char *pbf_path, const NMBinFile *nodes_index,
                    PbfMap *out);

/* 释放 out 持有的内存缓冲（可重复调用）。 */
void pbfmap_free(PbfMap *m);

#endif
