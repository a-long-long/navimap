# 第 6 课：建图：PBF → 路网

> 模块：`src/map/graph_build.{c,h}`、`src/map/coordkey.h`
> 前置：第 4 课 PBF 解析、第 5 课 NMBIN 自描述二进制、第 2 课 `nm_fast_dist_m` 边权近似。

## 一、教学目的

理解从原始 OSM 要素到可导航图（CSR 出边 + 转向限制）的抽取过程。学完后学生应能：描述"给一个 way，建图后它变成了图里的什么结构"；解释两遍扫描为何必要（way 只给 node 引用，坐标在另一处 DenseNodes 里）；说明 `pass1`/`pass2`/`build_index`/`compact_nodes`/`build_edges`/`resolve_restrictions` 各自职责；读懂 DenseNodes 的 delta 累积解码循环；说明节点与边如何落地成 NMBIN 二进制（nodes.bin / edges.bin / …）；并理解 `coord_key_e7` 如何用"坐标即 id"解决跨省缝合。

## 二、功能构成

| 文件 | 职责 |
|---|---|
| `graph_build.h` | `GraphBuildStats` 计数闸门；`graph_build()` 入口声明 |
| `graph_build.c` | 两遍扫描：挑可通行 way → 节点下标映射 → 建 CSR 边 + 限制 → 落盘 |
| `coordkey.h` | `coord_key_e7` 坐标主键（路由跨省缝合用） |

**`GraphBuildStats`**（`graph_build.h:15-47`）：建图全程计数器，`rt_unknown_drivable` 是必须为 0 的闸门（漏掉一条能走的路，比多画一条走不通的严重）。

**建图上下文 `Graph`**（`graph_build.c:71-102`）：临时 Buf 池（`ways`/`refs`/`restr`/`lines`/`areas`、边数组 `e_*`）、第 1 遍后去重排序的 `ids[]` 与经纬度 `lat/lon/osm_id`、落盘后的 CSR（`node_edge_begin`/`node_edge_count`/`csr_of` 等）、名字池与限制落盘缓冲。

**helper 函数签名一览**（均在 `graph_build.c`，附一行作用）：

| 签名 | 行号 | 一行作用 |
|---|---|---|
| `static int buf_reserve(Buf *b, size_t add)` | `20-30` | 变长数组倍增扩容 |
| `static void buf_free(Buf *b)` | `32` | 释放 Buf |
| `static void bb_update(Graph *g, int32_t lat, int32_t lon)` | `104-114` | 维护经纬度包围盒 |
| `static const char *str_fn(uint32_t sid, void *ud)` | `121-124` | 从 PbfBlock 取字符串给 road_tags |
| `static int cmp_i64(const void *a, const void *b)` | `126-130` | int64 比较（qsort） |
| `static size_t lower_bound_i64(const int64_t *a, size_t n, int64_t key)` | `132-140` | 二分下界，定位 node 内部下标 |
| `static uint8_t restr_dir_from_value(const char *v)` | `142-150` | 解析 restriction 的方向掩码 |
| `static int pass1(Graph *g, const char *path)` | `152-337` | 第 1 遍：挑 way + restriction，收集 node 引用 |
| `static int build_index(Graph *g)` | `342-363` | 排序去重 → 内部连续下标 |
| `static int pass2(Graph *g, const char *path)` | `368-408` | 第 2 遍：回填坐标（含 delta 解码循环） |
| `static void compact_nodes(Graph *g)` | `413-428` | 剔除无坐标节点 |
| `static int build_edges(Graph *g)` | `433-554` | 建有向边 + CSR 计数排序 |
| `static int resolve_restrictions(Graph *g)` | `639-773` | 把 (from,via,to) 定位到具体有向边 |
| `static int write_nodes(Graph *g, const char *path, const char *src)` | `778-813` | 落盘 nodes.bin |
| `static int write_edges(Graph *g, const char *path, const char *src)` | `815-867` | 落盘 edges.bin（按 CSR 序） |
| `static int write_osmid(Graph *g, const char *path, const char *src)` | `869-887` | 落盘 osmid.bin |
| `static int write_names(Graph *g, const char *path, const char *src)` | `889-902` | 落盘 names.bin |
| `static int write_lanes(Graph *g, const char *path, const char *src)` | `907-934` | 落盘 lanes.bin（按 CSR 序） |
| `static int write_lane_names(Graph *g, const char *path, const char *src)` | `938-951` | 落盘 lane_names.bin |
| `static int write_restrictions(Graph *g, const char *path, const char *src)` | `953-965` | 落盘 restrictions.bin |
| `static int disp_append(...)` | `975-990` | 显示层分 tile 的 polyrefs 重基写入 |
| `static int write_disp_tiles(Graph *g, const char *dir, const char *src, const Buf *pr)` | `997-1179` | 显示层切规则网格 tile（为控制篇幅省略完整体，可在 `graph_build.c:997-1179` 查阅） |
| `static int write_lines_areas(Graph *g, const char *dir, const char *src)` | `1181-1300` | 落盘 lines/areas/polyrefs（为控制篇幅省略完整体，可在 `graph_build.c:1181-1300` 查阅） |
| `static int write_spatial_index(Graph *g, const char *dir, const char *src)` | `1305-1475` | 生成 tile→(from,edge) 空间索引（为控制篇幅省略完整体，可在 `graph_build.c:1305-1475` 查阅） |
| `static void graph_free(Graph *g)` | `1480-1492` | 释放全部缓冲 |

**`coord_key_e7`**（`coordkey.h:26-32`）：把 e7 整数经纬度直接打包成 uint64 主键，跨省缝合时坐标相同的节点即同一节点，无需"本省下标→全局下标"映射表。

## 三、完整实现代码

### 1. 公共头 `graph_build.h`（完整体）

```h
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
    uint64_t disp_tiles;               /* 显示层分 tile 数（0 = 未生成 / 失败） */

    uint64_t names;                 /* 路名池条目数 */
    uint64_t ways_with_lanes;       /* 带 turn:lanes 的可路由 way 数 */
    uint64_t lane_strings;          /* 车道串池条目数 */
    int32_t  bbox[4];               /* lat_min lon_min lat_max lon_max，1e-7 度 */
    double   elapsed_sec;
} GraphBuildStats;

/* sources[0..n-1] 合图（当前实现按单文件逐次调用即可，跨文件去重在下一阶段）。 */
int graph_build(const char *pbf_path, const char *out_dir, GraphBuildStats *st);

#endif
```

### 2. 坐标主键 `coordkey.h`（完整体）

```h
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
```

### 3. 建图主循环 `graph_build()`（完整体，两遍扫描编排）

这是两遍扫描的总编排：`pass1 → build_index → pass2 → compact_nodes → build_edges → resolve_restrictions → write_*`。注意 way 只携带 node 引用，真实坐标在 `DenseNodes` 里、需第二遍回填，这就是为什么必须扫两遍。

```c
int graph_build(const char *pbf_path, const char *out_dir, GraphBuildStats *st)
{
    clock_t t0 = clock();
    Graph g;
    memset(&g, 0, sizeof(g));
    g.names = sp_create(1 << 20);
    g.lane_names = sp_create(1 << 12);
    g.ways.esz = sizeof(WayRec);
    g.refs.esz = sizeof(int64_t);
    g.restr.esz = sizeof(PendRestr);
    g.out_restr.esz = NM_RESTR_BYTES;
    g.lines.esz = sizeof(LineRec);
    g.areas.esz = sizeof(AreaRec);
    if (!g.names || !g.lane_names) return -1;

    int rc = 0;
    if (pass1(&g, pbf_path) != 0) { rc = -1; goto done; }
    if (build_index(&g) != 0) { rc = -1; goto done; }
    if (pass2(&g, pbf_path) != 0) { rc = -1; goto done; }
    compact_nodes(&g);
    if (build_edges(&g) != 0) { rc = -1; goto done; }
    if (resolve_restrictions(&g) != 0) { rc = -1; goto done; }

    char path[4096];
    const char *src = strrchr(pbf_path, '/') ? strrchr(pbf_path, '/') + 1 : pbf_path;
#define OUTF(p, name) (snprintf(p, sizeof(p), "%s/%s", out_dir, name), (const char *)(p))
    if (write_nodes(&g, OUTF(path, "nodes.bin"), src) != 0) { rc = -1; goto done; }
    if (write_edges(&g, OUTF(path, "edges.bin"), src) != 0) { rc = -1; goto done; }
    if (write_osmid(&g, OUTF(path, "osmid.bin"), src) != 0) { rc = -1; goto done; }
    if (write_names(&g, OUTF(path, "names.bin"), src) != 0) { rc = -1; goto done; }
    /* 车道指示是可选的：写不出来（比如磁盘满）不该让整省建图失败 */
    if (write_lanes(&g, OUTF(path, "lanes.bin"), src) != 0 ||
        write_lane_names(&g, OUTF(path, "lane_names.bin"), src) != 0)
        fprintf(stderr, "警告: 车道指示（lanes.bin）生成失败，该省无车道箭头\n");
    if (write_restrictions(&g, OUTF(path, "restrictions.bin"), src) != 0) { rc = -1; goto done; }
    if (write_lines_areas(&g, out_dir, src) != 0) { rc = -1; goto done; }
    /* 空间索引失败不致命：渲染回退全量扫描 */
    if (write_spatial_index(&g, out_dir, src) != 0)
        fprintf(stderr, "警告: 空间索引生成失败，渲染将全量扫描\n");
#undef OUTF

done:
    g.st.names = sp_count(g.names);
    g.st.lane_strings = sp_count(g.lane_names);
    g.st.elapsed_sec = (double)(clock() - t0) / CLOCKS_PER_SEC;
    for (int i = 0; i < 4; i++) g.st.bbox[i] = (int32_t)g.bb[i];
    *st = g.st;
    graph_free(&g);
    return rc;
}
```

### 4. 第 2 遍坐标回填 `pass2()`（完整体，含 DenseNodes delta 累积解码循环）

`pass2` 重新流式读取 PBF，只关心 `DenseNodes`。DenseNodes 里的 `id`/`lat`/`lon` 都是**相对前一个元素的 delta**，所以解码时必须用 `acc_id/acc_lat/acc_lon` 累积求和（`graph_build.c:389-402`）。这是建图里真正的"解码在 graph_build.c 内、不在 pbf 层"的循环。

```c
static int pass2(Graph *g, const char *path)
{
    PbfStream *s = pbf_stream_open(path);
    if (!s) return -1;

    const char *type; const uint8_t *buf; size_t len;
    PbfStatus rc;
    while ((rc = pbf_stream_next(s, &type, &buf, &len)) == PBF_OK) {
        if (!type || strcmp(type, "OSMData") != 0) continue;
        PbfBlock *blk = NULL;
        if (pbf_unpack_block(buf, len, &blk) != 0) { pbf_stream_close(s); return -1; }

        for (size_t gi = 0; gi < blk->pb->n_primitivegroup; gi++) {
            PrimitiveGroup *pg = blk->pb->primitivegroup[gi];
            if (!pg || !pg->dense) continue;
            DenseNodes *dn = pg->dense;
            if (dn->n_lat != dn->n_id || dn->n_lon != dn->n_id) {
                pbf_block_free(blk); pbf_stream_close(s);
                fprintf(stderr, "DenseNodes 三个数组长度不一致\n");
                return -1;
            }
            int64_t acc_id = 0, acc_lat = 0, acc_lon = 0;
            for (size_t i = 0; i < dn->n_id; i++) {
                acc_id  += dn->id[i];
                acc_lat += dn->lat[i];
                acc_lon += dn->lon[i];
                size_t pos = lower_bound_i64(g->ids, g->n_ids, acc_id);
                if (pos >= g->n_ids || g->ids[pos] != acc_id) continue;
                int32_t la = (int32_t)pbf_to_e7(blk, acc_lat, blk->pb->lat_offset);
                int32_t lo = (int32_t)pbf_to_e7(blk, acc_lon, blk->pb->lon_offset);
                g->lat[pos]    = la;
                g->lon[pos]    = lo;
                g->osm_id[pos] = acc_id;
                bb_update(g, la, lo);
            }
        }
        pbf_block_free(blk);
    }
    pbf_stream_close(s);
    return rc == PBF_ERR ? -1 : 0;
}
```

### 5. 建边与 CSR `build_edges()`（完整体）

把 way 内相邻 node 对变成有向边：`oneway==0` 双向（第二方向打 `EF_REVERSE`），`oneway==2` 只允许按节点顺序逆向走（**不能**打 `EF_REVERSE`，否则会被渲染当成回程边跳掉）。边权用 `nm_fast_dist_m` 近似成厘米。最后按 `from` 做**计数排序**成 CSR（`node_edge_begin/count`、`csr_of`）。

```c
static int build_edges(Graph *g)
{
    const int64_t *rp = (const int64_t *)g->refs.p;

    g->e_from.esz = 4; g->e_to.esz = 4; g->e_dist.esz = 4; g->e_name.esz = 4;
    g->e_flags.esz = 2; g->e_cls.esz = 1; g->e_speed.esz = 1; g->e_lanes.esz = 2;
    g->e_lane.esz  = 4;   /* 车道转向串 id（u32，CSR 边序） */
    g->e_way.esz = 8;

    size_t nways = g->ways.n / sizeof(WayRec);
    const WayRec *wp = (const WayRec *)g->ways.p;

    for (size_t wi = 0; wi < nways; wi++) {
        const WayRec *w = &wp[wi];
        for (uint32_t k = 0; k + 1 < w->ref_cnt; k++) {
            int64_t a = rp[w->ref_off + k];
            int64_t b = rp[w->ref_off + k + 1];
            size_t pa = lower_bound_i64(g->ids, g->n_ids, a);
            size_t pb = lower_bound_i64(g->ids, g->n_ids, b);
            if (pa >= g->n_ids || g->ids[pa] != a ||
                pb >= g->n_ids || g->ids[pb] != b) {
                g->st.dropped_missing_node++;
                continue;
            }
            /* 坐标没回填上（文件里根本没有这个节点）也要跳过 */
            if (g->osm_id[pa] == 0 || g->osm_id[pb] == 0) {
                g->st.dropped_missing_node++;
                continue;
            }
            double d = nm_fast_dist_m(g->lat[pa], g->lon[pa], g->lat[pb], g->lon[pb]);
            uint32_t dcm = (uint32_t)(d * 100.0 + 0.5);
            if (dcm == 0) dcm = 1;

            struct { size_t from, to; uint16_t flags; int rev; } dirs[2];
            int ndir = 1;
            dirs[0].from = pa; dirs[0].to = pb; dirs[0].flags = w->flags;
            dirs[0].rev = 0;
            if (w->oneway == 0) {
                dirs[1].from = pb; dirs[1].to = pa;
                dirs[1].flags = w->flags | EF_REVERSE;
                dirs[1].rev = 1;
                ndir = 2;
            } else if (w->oneway == 2) {
                /* oneway=-1：只允许按节点顺序逆向走。
                   这里**不能**打 EF_REVERSE —— 它是这条路唯一的一条边，
                   打了会被渲染当成"双向路的回程边"跳掉，整条路就画不出来了。
                   rev=1 只影响车道串的取法（用 :backward）。 */
                dirs[0].from = pb; dirs[0].to = pa; dirs[0].flags = w->flags;
                dirs[0].rev = 1;
            }

            for (int di = 0; di < ndir; di++) {
                uint32_t f = dirs[di].from, t = dirs[di].to;
                if (buf_reserve(&g->e_from, 4) || buf_reserve(&g->e_to, 4) ||
                    buf_reserve(&g->e_dist, 4) || buf_reserve(&g->e_name, 4) ||
                    buf_reserve(&g->e_flags, 2) || buf_reserve(&g->e_cls, 1) ||
                    buf_reserve(&g->e_speed, 1) || buf_reserve(&g->e_lanes, 2) ||
                    buf_reserve(&g->e_way, 8) || buf_reserve(&g->e_lane, 4)) return -1;

                uint32_t fi = (uint32_t)g->e_from.n / 4;
                memcpy(g->e_from.p + g->e_from.n, &f, 4); g->e_from.n += 4;
                memcpy(g->e_to.p   + g->e_to.n,   &t, 4); g->e_to.n   += 4;
                memcpy(g->e_dist.p + g->e_dist.n, &dcm, 4); g->e_dist.n += 4;
                memcpy(g->e_name.p + g->e_name.n, &w->name_id, 4); g->e_name.n += 4;
                memcpy(g->e_flags.p + g->e_flags.n, &dirs[di].flags, 2); g->e_flags.n += 2;
                uint8_t cl = w->cls, sp = (uint8_t)(w->speed > 255 ? 255 : w->speed);
                memcpy(g->e_cls.p   + g->e_cls.n,   &cl, 1); g->e_cls.n   += 1;
                memcpy(g->e_speed.p + g->e_speed.n, &sp, 1); g->e_speed.n += 1;
                memcpy(g->e_lanes.p + g->e_lanes.n, &w->lanes, 2); g->e_lanes.n += 2;
                memcpy(g->e_way.p   + g->e_way.n,   &w->osm_id, 8); g->e_way.n += 8;
                /* 车道转向串：逆向边取 :backward，正向取 :forward/generic */
                uint32_t lsid = dirs[di].rev ? w->lane_bwd : w->lane_fwd;
                memcpy(g->e_lane.p + g->e_lane.n, &lsid, 4); g->e_lane.n += 4;
                (void)fi;
            }
        }
    }

    size_t ne = g->e_to.n / 4;
    g->st.edges = ne;
    g->st.nodes = g->n_ids;

    /* ---- CSR：按 from 做计数排序 ---- */
    uint32_t *cnt = calloc(g->n_ids + 1, sizeof(uint32_t));
    uint32_t *begin = malloc((g->n_ids + 2) * sizeof(uint32_t));
    uint32_t *pos = malloc((g->n_ids + 1) * sizeof(uint32_t));
    g->node_class_mask = calloc(g->n_ids ? g->n_ids : 1, sizeof(uint32_t));
    g->node_edge_begin = calloc(g->n_ids + 1, sizeof(uint32_t));
    g->node_edge_count = calloc(g->n_ids ? g->n_ids : 1, sizeof(uint16_t));
    g->node_flags      = calloc(g->n_ids ? g->n_ids : 1, sizeof(uint8_t));
    g->csr_of          = malloc((ne ? ne : 1) * sizeof(uint32_t));
    if (!cnt || !begin || !pos || !g->node_class_mask || !g->node_edge_begin ||
        !g->node_edge_count || !g->node_flags || !g->csr_of) {
        free(cnt); free(begin); free(pos); return -1;
    }

    const uint32_t *ef = (const uint32_t *)g->e_from.p;
    const uint8_t  *ec = (const uint8_t *)g->e_cls.p;
    for (size_t i = 0; i < ne; i++) {
        cnt[ef[i]]++;
        g->node_class_mask[ef[i]] |= (1u << (ec[i] & 7));
    }
    uint64_t acc = 0;
    for (size_t i = 0; i < g->n_ids; i++) {
        begin[i] = (uint32_t)acc;
        pos[i]   = (uint32_t)acc;
        acc += cnt[i];
    }
    begin[g->n_ids] = (uint32_t)acc;
    for (size_t i = 0; i < ne; i++) g->csr_of[i] = pos[ef[i]]++;

    /* 度数 ≥3 记为路口，渲染/路由都用它 */
    for (size_t i = 0; i < g->n_ids; i++) {
        if (cnt[i] >= 3) g->node_flags[i] |= 1u;
        g->node_edge_begin[i] = begin[i];
        g->node_edge_count[i] = cnt[i] > 65535u ? 65535u : (uint16_t)cnt[i];
    }
    g->node_edge_begin[g->n_ids] = begin[g->n_ids];

    free(cnt); free(begin); free(pos);
    return 0;
}
```

### 6. 节点 / 边如何落盘：以 `write_nodes` 与 `write_edges` 头部为例（完整体节选）

两张表分别用 `nmbin_create` + `nmbin_add_field` 声明自描述字段（`NF_*` / `EF_*` 字段编号见 `binfmt.h`），再按记录批量写出。注意 `write_edges` 按 **CSR 顺序** 写：`csr_of[i]` 给出边缘在 CSR 里的目标下标。

```c
static int write_nodes(Graph *g, const char *path, const char *src)
{
    NMBinWriter *w = nmbin_create(path, NMBIN_KIND_NODES, NM_NODE_BYTES, src);
    if (!w) return -1;
    nmbin_add_field(w, "lat_e7",      NF_LAT,        NMT_I32, 4);
    nmbin_add_field(w, "lon_e7",      NF_LON,        NMT_I32, 4);
    nmbin_add_field(w, "edge_begin",  NF_EDGE_BEGIN, NMT_U32, 4);
    nmbin_add_field(w, "edge_count",  NF_EDGE_COUNT, NMT_U16, 2);
    nmbin_add_field(w, "node_flags",  NF_NODE_FLAGS, NMT_U16, 2);
    nmbin_add_field(w, "osm_id",      NF_OSM_ID,     NMT_U64, 8);
    nmbin_add_field(w, "class_mask",  NF_CLASS_MASK, NMT_U32, 4);

    uint8_t rec[NM_NODE_BYTES];
    const size_t CHUNK = 8192;
    uint8_t *bulk = malloc(CHUNK * NM_NODE_BYTES);
    if (!bulk) { nmbin_abort(w); return -1; }
    size_t i = 0;
    while (i < g->n_ids) {
        size_t n = g->n_ids - i < CHUNK ? g->n_ids - i : CHUNK;
        for (size_t k = 0; k < n; k++) {
            memset(rec, 0, sizeof(rec));
            nm_put_i32(rec + NF_LAT,        g->lat[i + k]);
            nm_put_i32(rec + NF_LON,        g->lon[i + k]);
            nm_put_u32(rec + NF_EDGE_BEGIN, g->node_edge_begin[i + k]);
            nm_put_u16(rec + NF_EDGE_COUNT, g->node_edge_count[i + k]);
            nm_put_u16(rec + NF_NODE_FLAGS, (uint16_t)g->node_flags[i + k]);
            nm_put_u64(rec + NF_OSM_ID,     (uint64_t)g->osm_id[i + k]);
            nm_put_u32(rec + NF_CLASS_MASK, g->node_class_mask[i + k]);
            memcpy(bulk + k * NM_NODE_BYTES, rec, NM_NODE_BYTES);
        }
        if (nmbin_write_records(w, bulk, n) != 0) { free(bulk); nmbin_abort(w); return -1; }
        i += n;
    }
    free(bulk);
    return nmbin_finalize(w);
}
```

## 四、与其他功能之间的联系

- **上游 · 第 4 课 PBF 解析**：`pass1`/`pass2` 通过 `pbf_stream_next`/`pbf_unpack_block` 与 `DenseNodes` 的 delta 解码取出原始要素；`road_tags_parse`（第 4 课所属）把 way 标签变成 `RoadTags`。
- **上游 · 第 5 课 NMBIN 自描述二进制**：本课所有 `write_*` 都走 `nmbin_create`/`nmbin_add_field`/`nmbin_write_records`/`nmbin_finalize`，落盘格式即 L5 定义的图集。
- **下游 · 第 11 课路由**：本课的 `nodes/edges/restrictions/...` 即 `mapset_load` 加载内容，`route.c`/`sew.c` 基于此寻路。本课是 L5 图集与 L11 路由之间的"数据生产"环节。
- **下游 · 第 12 课导航语音 / 第 13 课 Qt 前端**：`names.bin` 的路名池、`lanes.bin` 的转向车道串是播报和 UI 的数据源。
- **跨省缝合 · 第 11 课 `sew.c`**：`coord_key_e7`（`coordkey.h`）把 L6 建出的节点 e7 坐标打包成主键，配合 L7 的 `nm_tile_of` 把不同省的同一坐标合成一个图节点，实现跨省接边。
- **空间索引 · 第 9 课渲染管线**：`write_spatial_index` 产出的 `edgelist.bin`/`tiles.bin` 让 L9 渲染按需加载视口内 tile，而非整省全量扫描。

## 五、演示、练习与验收

- **演示**：`./build_rel/graph_extract --help`、`graph_check`、`tag_stats sources/beijing.osm.pbf`，观察 `GraphBuildStats` 计数（尤其 `rt_unknown_drivable` 必须为 0）。
- **练习**：选一个 `highway=tertiary`，在 `graph_build.c:152` 的 `pass1` 里追它如何经 `road_tags_parse` 变成 `WayRec`，再进入 `build_edges` 生成 CSR 边；再找一个 `oneway=-1` 的 way，解释 `build_edges`（`graph_build.c:475-482`）为何 `oneway==2` 分支不打 `EF_REVERSE`。
- **验证 delta 解码**：在 `graph_build.c:390-402` 的 `pass2` 循环设断点，确认 `acc_id/acc_lat/acc_lon` 随 `dn->id[i]` 等 delta 累积而非直接使用原始值。
- **验收**：能描述"一个 way → 图里的 CSR 边 + 标志位"的映射，说明两遍扫描的必要性，并解释 `coord_key_e7` 如何支撑跨省缝合。
