#ifndef NAVIMAP_MAPSET_H
#define NAVIMAP_MAPSET_H

#include "map/binfmt.h"
#include "map/pbfmap.h"
#include "render/labels.h"
#include "utils/plat.h"     /* NmMutex：后台解码线程的锁 */

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
    /* ---- 等高线（SRTM 生成，可选）----
     * 三件套与 lines/areas 平级：contours.bin（记录，类 lines）+ contour_polyrefs.bin
     * （u32 顶点下标序列）+ contour_pts.bin（独立顶点池 lat/lon e7）。
     * 不进 OSM 导入管线，由 gen_contours 直接产出；与 disp tile 无关（整省一份）。 */
    NMBinFile contours, contour_polyrefs, contour_pts;
    int       has_contours;             /* 三者齐备才为 1 */
    int32_t  *contour_bbox;            /* 等高线缓存（paint_contours 懒填，nc*4 个 i32） */
    /* ★ 显示层的"来源"：0 = 从 nmbin 打开（默认）；1 = 从 PBF 运行时解码。
     * 两种模式下 lines/areas/polyrefs 的内容**逐字节相同**（8 省实测），
     * 所以渲染端不区分，只看 has_features。 */
    int       dmap_from_pbf;
    int       pbf_display_mode;  /* 1 = 本省走 PBF 显示层（不打开 nmbin 的
                                   * lines/areas/polyrefs） */
    PbfMap    dmap;                     /* 从 PBF 解码出的显示层（内存缓冲） */
    uint64_t  dmap_used;                /* LRU 时间戳（越大越新） */
    int       dmap_inflight;  /* 后台解码进行中：src_unload 不得卸 nodes 映射 */
    int       dmap_failed;    /* 该省 PBF 解码失败：重载前不再重试 */
    int       dmap_progress;  /* 0..100，后台解码实时进度（dmap_inflight/排队期间有效） */
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
    NMBinFile lanes;        /* 每条边的车道转向串 id（lanes.bin，可选） */
    NamePool  lnp;          /* 车道转向串池（lane_names.bin，可选） */
    int       has_lanes;    /* lanes.bin + lane_names.bin 齐备 */
    int32_t  *area_bbox;    /* 面缓存（paint_areas 懒填，na*4 个 i32） */
    int32_t  *line_bbox;    /* 线缓存（paint_lines 懒填，nl*4 个 i32）。
                             * 线要素没有 tile 索引，缺了它每帧要白投影 10 万条线
                             * 的全部顶点——实测占 z14 一帧的 75%。 */

    /* ---- 显示层分 tile（运行时按需加载，只 mmap 视口内 tile）----
     * has_disp_tiles：本省有 disp.idx（抽取期已切好）。有则**不走**整省
     * nmbin/pbf 解码，改为按视口把相交 tile 拼接进 areas/lines/polyrefs。
     * 拼接缓冲 dareas/dlines/dpoly 是手 malloc 的（NMBinFile.mapped=0，
     * nmbin_close 只置空不释放），卸载时由 src_unload/close 负责 free。 */
    int       has_disp_tiles;
    int64_t   disp_tile_e7, disp_lat0, disp_lon0;  /* 网格参数（e7） */
    int       disp_ncols, disp_nrows;
    uint8_t  *dareas, *dlines, *dpoly;   /* 拼接缓冲（手动 malloc/free） */
    size_t    dea, del, depo;             /* 三个缓冲当前字节数 */
    int64_t   disp_sig;      /* 上次拼接的 tile 集合签名，避免每帧重建 */
} MapSrc;

typedef struct {
    MapSrc *srcs;
    int     n, cap;
    int32_t bbox[4];       /* 全体省份的并集（e7） */
    /* ---- PBF 显示层按需解码 + LRU ----
     * pbf_dir：各省原始 PBF 所在目录（形如 sources/，内含 <省名>.osm.pbf）。
     * 为空则禁用 PBF 模式（退回从 nmbin 打开显示层）。 */
    char    pbf_dir[4096];
    int     dmap_lru_cap;  /* 同时驻留几个省的解码结果（默认 3） */
    uint64_t dmap_clock;   /* LRU 时钟（单调递增） */

    /* ---- 后台解码线程（避免同步解码阻塞渲染 0.6~4.4s）----
     * 单一 worker 串行解码；主线程每帧 publish 已完成的结果、并 dispatch 新任务。
     * 所有字段都受 dmap_mtx 保护（job_prov / job_done / job_*）。 */
    NmMutex  *dmap_mtx;         /* 保护下面的任务槽；NULL 则退回同步解码 */
    int       dmap_quit;        /* 置 1 让 worker 退出 */
    int       dmap_stopped;     /* worker 已退出（free 时等待） */
    int       dmap_worker_started;
    int       job_prov;         /* 正在解码的省下标；-1 = 空闲 */
    int       job_done;         /* 有结果待发布 */
    int       job_done_prov;    /* 待发布结果属于哪个省 */
    int       job_ok;           /* 1 成功 / 0 失败 */
    PbfMap    job_result;       /* 解码产物（malloc 缓冲），发布时移交 src */
} MapSet;

/* graph_dir 可以是单个省目录（含 nodes.bin），也可以是父目录
 * （扫描所有含 manifest.txt 的子目录，跳过 *.tmp.* 与隐藏目录）。
 * 返回 0 = 至少加载了一个省；-1 = 一个都没有。 */
int  mapset_load(MapSet *ms, const char *graph_dir);
void mapset_free(MapSet *ms);

/* 视口驱动的按需加载：只保留与 vb（e7 的 [lat0,lon0,lat1,lon1]）相交（含边距）的
 * 省份打开，其余卸载。省份下标恒定（槽位 = 省 id），迭代函数跳过 !loaded，因此
 * 路由/转向里写进结果的 prov 下标始终有效，不会因重排而悬垂。
 *
 * 若 ms->pbf_dir 非空，则**显示层**（lines/areas/polyrefs）改为从 PBF
 * 运行时解码：视口进省才解码，滚出则按 LRU 淘汰（驻留上限 ms->dmap_lru_cap）。
 * 解码结果与 nmbin 落盘文件**逐字节相同**（8 省实测）⇒ 渲染结果一致。
 * ⚠️ 解码在**后台线程**异步进行（dmap_worker），主线程不阻塞；解码完成前
 * 该省 has_features=0、渲染自然跳过（空白），就绪后下一帧经 publish 挂上。
 * 实时进度见 mapset_dmap_progress / 渲染端的"解码中 NN%"占位。 */
void mapset_set_view(MapSet *ms, const int32_t vb[4]);

/* 按省下标确保该省已加载（maneuver/snap 访问特定省前调用，避免卸载后悬垂访问）。 */
void mapset_ensure_loaded(MapSet *ms, int prov);

/* 设置 PBF 显示层模式：pbf_dir 形如 "sources"（内含 <省名>.osm.pbf）。
 * 返回 0 = 成功；-1 = 目录不可用（调用方应继续用 nmbin 模式）。 */
int  mapset_enable_pbf_display(MapSet *ms, const char *pbf_dir, int lru_cap);

/* 确保某省的显示层已从 PBF 解码（供后台线程调用）。
 * 返回 0 = 已就绪；-1 = 失败（该省没有 PBF 或解码出错）。 */
int  mapset_ensure_display(MapSet *ms, int prov);

/* 取某省后台解码进度（0..100）。无在途/未启用时返回 0。
 * 读受 dmap_mtx 保护（与 worker 的进度回调串行）。 */
int  mapset_dmap_progress(const MapSet *ms, int prov);

/* 供状态栏/日志用的汇总：总节点数、总边数 */
void mapset_totals(const MapSet *ms, uint64_t *nodes, uint64_t *edges);

/* 车道转向指示（turn:lanes）：按 (省, 省内边下标) 取该边的车道串，
 * 形如 "left|through|right"（按行进方向排列）。返回 NULL = 无车道信息
 * （该省没有 lanes.bin，或这条边本来就没打 turn:lanes）。字符串属于该省
 * 的池，生命周期同 MapSrc（省份被卸载后失效）。 */
const char *mapset_edge_lanes(const MapSet *ms, int prov, uint32_t edge);

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
