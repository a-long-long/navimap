#ifndef NAVIMAP_ROUTE_PRIV_H
#define NAVIMAP_ROUTE_PRIV_H

/* route 模块内部私有定义：完整 Router / RestrIdx / Sew* 结构体，
 * 以及 route.c 与 restr.c 共用的小宏。
 * 不进 route/route.h —— 后者是公共接口，且被 render/ui 模块包含，
 * 不能把完整 Router 泄漏出去（也避免把 mapset/st/pqueue/revidx/lmtable
 * 一整套头文件强加给渲染层）。 */

#include "route/route.h"     /* REDGE_SEAM, RouteOpts, Router(fwd), MapSet, MapSrc */
#include "route/st.h"        /* StateTab, StEnt, st_find */
#include "route/pqueue.h"    /* PQueue */
#include "route/revidx.h"    /* RevIdx */
#include "route/lmtable.h"   /* LmTable */
#include "map/binfmt.h"      /* NM_NODE_BYTES / RF_* / NM_RESTR_BYTES / EF_* */
#include "map/road_tags.h"   /* RC_* 道路等级 */
#include "map/coordkey.h"    /* coord_key_e7 */
#include "utils/geo.h"       /* nm_haversine_m */

/* 双向搜索方向下标（正向=朝终点，反向=朝起点）；route.c 与 restr.c 共用。 */
#define DIR_FWD 0
#define DIR_BWD 1

/* 瓦片缝合缓存槽数（2 的幂，哈希 + 线性探测）。详见 route.c 顶部注释历史。 */
#define SEW_SLOTS 16384

/* 同一坐标最多取多少个跨省成员（route.c 的 members_of/expand_dir 与
 * lmdist.c 的 lm_dijkstra 三方共用 ⇒ 放在这里共享）。 */
#define MAX_MEMBER     64

/* A* 启发式分母；需 >= 全图最高限速才 admissible（原在 route.c，cost.c 的 heur 也用）。 */
#define H_SPEED_KMH    130.0

/* 同一坐标的成员（跨国 / 重复副本省）临时结构 */
typedef struct { uint64_t coord; uint32_t prov, node; } SewEnt;

typedef struct {
    int      used;      /* 0 = 空槽 */
    int      tx, ty;
    int      nprov;     /* 覆盖这片瓦片且有边的省数；-1 = 还没算 */
    int      full;      /* v 是否已构建 */
    SewEnt  *v;
    size_t   n;
} SewSlot;

/* 取落在同一坐标上的所有 (省,节点) 成员：route.c 定义，
 * 搜索核心 expand_dir 与 lmdist.c 的 lm_dijkstra 都要用 ⇒ 导出。 */
size_t members_of(Router *r, uint64_t ck, uint32_t in_prov, uint32_t in_node,
                  SewEnt *mem);

typedef struct {
    int       built;
    uint64_t *kv;      /* (via_node << 32) | from_edge，升序 */
    uint32_t *to;
    size_t    n;
    /* kind=2（via=way 两段）单独一张：除了 (via, from, to) 还要比 prev_edge，
       塞进上面那张要改键结构，不如分开——数量也是几百条级。 */
    uint64_t *kv2;     /* (via_node << 32) | from_edge，升序 */
    uint32_t *to2;
    uint32_t *prev2;   /* 三段序列里的第一条边 */
    size_t    n2;
    /* kind=1（only_*）再单独一张。它跟上面两张**语义相反**：
       上面是"命中即禁"；这张是"命中 {via,from} 之后，只有集合里的 to 允许走，
       其余全禁"。所以不能并进 kv/to——那张表的语义在调用点已经写死了。 */
    uint64_t *kv1;     /* (via_node << 32) | from_edge，升序 */
    uint32_t *to1;     /* 允许的出边；同一 key 的多条记录共同组成一个允许集 */
    size_t    n1;
} RestrIdx;

/* 路由器的完整内部状态（仅 route 模块内的 .c 可见，不进 route.h 公共接口） */
struct Router {
    const MapSet *ms;
    RouteOpts     opt;
    StateTab      st[2];
    PQueue        pq[2];
    SewSlot       sew[SEW_SLOTS];
    RestrIdx     *ri;
    RevIdx       *rv;
    int32_t       tgt_lat[2], tgt_lon[2];  /* 启发式目标：[0]=终点 [1]=起点 */
    LmTable       lm;                      /* 地标下界表（可选；没有就退回纯直线） */
    size_t        lm_tc[2];                /* 两端所在格号，(size_t)-1 = 不在网格内 */
    double        best_cut;                /* 剪枝阈值：超过它不是最优解 */
    uint64_t      expanded;
    uint64_t      max_expand;
};

#endif
