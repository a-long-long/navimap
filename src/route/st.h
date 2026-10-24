#ifndef NAVIMAP_ROUTE_ST_H
#define NAVIMAP_ROUTE_ST_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* 状态表（开地址哈希）的私有定义 + 操作。
 *
 * 从 route.c 抽离：route.c 的搜索核心与转向限制索引都用它，但它是
 * 一个**与项目类型无关的通用哈希表**，单独成文件便于复用与单测。
 * ★ 行为等价：本文件是 route.c 原代码整块搬迁，API 与语义一字未改。 */

#define INF_G 1e30   /* 无穷大 g（与 route.c 原本一致） */

/* 一个坐标 = 一个状态。rep_* 是"到达/离开时落在哪个省的节点上"，只用于输出
 * 路径与转向限制；搜索本身只认坐标。 */
typedef struct {
    uint64_t key;        /* coord_key_e7 */
    double   g;
    /* ★ 前驱存的是**同一张 ents 数组里的下标**（UINT32_MAX = 没有前驱），
       不是坐标 key。理由：ents 只 append、realloc 也不改下标，所以拿到前驱时
       直接 &ents[prev_idx] 即可，不必再用 key 去哈希查找一次
       （那次冷查找原本值 186 ns/次，PLAN.md §41 / §42）。
       副作用是 StEnt 从 48 字节缩到 40 字节（原 uint64 prev + 尾部 padding）。
       ★ 本表之外（例如 st.h 的使用者）不要把它当成 key 用。 */
    uint32_t prev_idx;
    uint32_t eprov;      /* 前驱 → 本坐标用的边所属省 */
    uint32_t eidx;       /* 边下标；REDGE_SEAM = 起点（无入边） */
    uint32_t from_node;  /* 该边的起点节点（eprov 省内下标） */
    uint32_t rep_prov;   /* 本坐标落在哪个省的节点上 */
    uint32_t rep_node;
    uint8_t  settled;
} StEnt;

/* 开地址哈希表的槽。★ 存的是 **32 位哈希**而不是 64 位 key：
   - 槽从 8B(key)+4B(idx) 两个数组（共 12 B/槽、命中时要摸两个数组）合并成
     **一个 8 B 数组**，命中时 idx 跟 h 在同一条 cache line 里；
   - 探测数组体积直接减半（8 省最大搜索 8M 槽 ⇒ 64 MB/表 → 32 MB/表）。
   代价是 h 可能撞（32 位），所以命中 h 之后必须再用 ents[idx].key **回验一次**。
   回验不贵：ents[idx] 本来就要读（te->settled / te->g）。
   ★ h 用 mix64 的**高 32 位**、下标用**低 32 位**，一次 mix64 出两个值；
     h |= 1 保证非零（0 表示空槽）。 */
typedef struct {
    uint32_t h;         /* 32 位哈希（|1，保证非 0）；0 = 空槽 */
    uint32_t idx;       /* → ents 下标 */
} StSlot;

typedef struct {
    StSlot   *slot;
    size_t    cap, used;
    StEnt    *ents;
    size_t    n_ents, cap_ents;
} StateTab;

/* ★ ents 会 realloc，所以 StEnt* 不能跨 st_intern() 持有。
   全文件遵循：intern/find → 立刻用完 → 下次重新查。 */

/* 通用分配器（route.c 的 sew_cb / graph_root_dir 也用，故在此共享声明）。 */
void *xrealloc(void *p, size_t n);

void   st_init(StateTab *t);
void   st_free(StateTab *t);
void   st_rehash(StateTab *t);
StEnt *st_intern(StateTab *t, uint64_t key);
StEnt *st_find(const StateTab *t, uint64_t key);

#endif
