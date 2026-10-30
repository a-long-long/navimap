# 第 11 课：跨省路由算法（核心）

> 模块：`src/route/{route,st,revidx,lmtable,restr,cost,pqueue,maneuver}.{c,h}`
> 前置：第 5 课图集、第 6 课 CSR 图、第 7 课瓦片键与 `coord_key_e7`、A* / Dijkstra 基础、优先队列。

## 一、教学目的

读懂跨省双向加权 A* 的全过程，以及"坐标键接边、Landmark 下界、转向限制"如何配合。学完后学生应能：

- 画出"双向 A* 一次展开"的数据流（取堆顶 → `members_of` → 松弛出边/入边 → 查转向限制 → 相遇判定）；
- 解释 `coord_key_e7` 为什么消除了跨省映射表（一个物理路口只展开一次）；
- 澄清"本项目用 ALT 地标加速，不是 CH/contraction"；
- 说明加权 A* 的 `w` 与 `exact` 是两个独立旋钮（贪心度 ↔ 终止条件）。

## 二、功能构成

| 文件 | 职责 |
|---|---|
| `route.c/h` | `RouteResult`/`RouteOpts`；`router_plan`/`router_route_ll`/`plan_search`（双向加权 A*） |
| `st.c/h` | `StateTab`：开地址哈希，**主键 = 坐标 `coord_key_e7`**，前驱存 `prev_idx` |
| `revidx.c/h` | 每省反向入边 CSR（`rev.bin`） |
| `lmtable.c/h` | 地标下界表（`lm.bin`，ALT 思路，非严格下界） |
| `restr.c/h` | 三类转向限制（含 via=way 两段） |
| `cost.c/h` | `edge_cost`（边权）、`heur`（大圆÷130km/h） |
| `pqueue.h` | 4 叉最小堆，载荷存 StEnt 下标 |
| `maneuver.c/h` | `maneuver_build` 转向指令生成 |

**`RouteResult`**（`src/route/route.h:46-68`）：`ok/total_m/total_s/pts[]/lat_e7[]/lon_e7[]/edge_idx[]/from_node[]/edge_cls[]/edge_flags[]/seg_m[]/seg_ds[]/n_seams/expanded/elapsed_ms`；`REDGE_SEAM=0xFFFFFFFF` 标记接缝段。

**`RouteOpts`**（★ **必须指名初始化**，位置初始化会静默错位）：`mode_time/avoid_toll/avoid_unpaved/bidir/exact/max_expand/heur_w(默认1.4)/heur_w_hi/adapt_try`。

**`coord_key_e7`**（坐标即主键，见 `src/map/coordkey.h:26-32`）：把整数 e7 坐标本身当状态主键，跨省接边无需映射表。

**本课完整展示**：`coord_key_e7`、`plan_search` 主循环、`maneuver_build` 完整体；其余列签名 + 一行说明 + 行号。

## 三、完整实现代码

### 1. ★ 坐标即主键 `coord_key_e7`（完整）：`src/map/coordkey.h:26-32`

```h
static inline uint64_t coord_key_e7(int32_t lat_e7, int32_t lon_e7)
{
    return ((uint64_t)(uint32_t)lat_e7 << 32) | (uint32_t)lon_e7;
}

static inline int32_t coord_key_lat(uint64_t k) { return (int32_t)(uint32_t)(k >> 32); }
static inline int32_t coord_key_lon(uint64_t k) { return (int32_t)(uint32_t)(k & 0xFFFFFFFFu); }
```

搜索状态主键 = `coord_key_e7 = (lat31<<32)|lon31`，不是 `(省, 省内下标)`。好处：① 加省无需重建"局部→全局"映射；② 各省数据互相包含时（北京 ⊂ 河北 100%）重复副本不会让搜索量翻倍——它们本来就是同一个状态。**状态空间设计**范例：一个物理路口只展开一次。

### 2. 关键结构体（完整）

**`RNode` / `REDGE_SEAM` / `RouteResult`**（`src/route/route.h:32-68`）：

```c
#define REDGE_SEAM 0xFFFFFFFFu   /* 路径里表示"此处跨了省"（零长度接缝） */

typedef struct { uint32_t prov, node; } RNode;

static inline uint64_t rnode_key(RNode n)
{
    return ((uint64_t)n.prov << 32) | (uint64_t)n.node;
}
static inline RNode rnode_of(uint64_t k)
{
    RNode n; n.prov = (uint32_t)(k >> 32); n.node = (uint32_t)(k & 0xFFFFFFFFu);
    return n;
}

typedef struct {
    int      ok;            /* 1 = 找到路径 */
    double   total_m;       /* 路径总长（米，按边 dist_cm 累加） */
    double   total_s;       /* 预计耗时（秒） */
    uint32_t n;             /* 路径点数 */
    RNode   *pts;           /* n 个：起点 → 终点 */
    int32_t *lat_e7;        /* n 个 */
    int32_t *lon_e7;        /* n 个 */
    uint32_t *edge_idx;     /* n-1 个：pts[i] → pts[i+1] 用的边（pts[i+1].prov 内） */
    uint32_t *from_node;    /* n-1 个：该段边起点在同一省内的节点下标（自检用） */
    uint8_t  *edge_cls;     /* n-1 个：该段道路等级 RC_*，接缝段为 0xFF */
    uint8_t  *edge_flags;   /* n-1 个：该段边的标志位低字节（EF_ROUNDABOUT 等）。
                             * 导航指令要认环岛，几何分辨不出来，只能靠它。 */
    uint32_t *seg_m;        /* n-1 个：该段长度（米） */
    uint32_t *seg_ds;       /* n-1 个：该段耗时（0.1 秒）。与 total_s 同口径（Σ/10 == total_s）。 */
    uint32_t n_seams;       /* 相邻两段所属省份不同的次数（= 真正的跨省次数） */
    uint64_t expanded;      /* 展开节点数（含接缝合并进来的） */
    uint64_t elapsed_ms;
} RouteResult;
```

**`RouteOpts`**（★ 指名初始化；含地标 w 调参注释，`src/route/route.h:94-187`）：

```c
typedef struct {
    int      mode_time;     /* 1 = 按时间（默认），0 = 按距离 */
    int      avoid_toll;
    int      avoid_unpaved;
    int      bidir;         /* 1 = 双向 A*（默认），0 = 单向（用于 A/B 对照） */
    int      no_restr2;     /* 1 = 关掉 via=way 两段限制，仅供 A/B 对照验收 */
    int      no_restr1;     /* 1 = 关掉 only_* 限制，仅供 A/B 对照验收 */
    int      no_seam_restr; /* 1 = 关掉"双向搜索会合点"处的限制检查，仅供负向对照 */
    int      lm_no_restr;   /* ★ 只影响 lm_dijkstra（建地标表），不影响算路 */
    int      exact;         /* 1 = 双向用"保证最优"的终止条件（慢），0 = 默认快速条件 */
    uint64_t max_expand;    /* 展开上限，防跑飞（0 = 用默认 800 万） */
    double   heur_w;        /* 启发式权重 w：入堆优先级 f = g + w·h */
    double   heur_w_hi;     /* 自适应：第 1 遍撞顶换这个更重的 w 重跑 */
    uint64_t adapt_try;     /* 第 1 遍展开封顶 */
} RouteOpts;
```

（头文件中 `ROUTE_HEUR_W_DEFAULT=1.4` / `ROUTE_HEUR_W_LM=1.25` / `ROUTE_HEUR_W_HARD=1.8` / `ROUTE_ADAPT_TRY=200000` 等常量与长注释定义了 w 的档位依据，详见 `src/route/route.h:117-172`。）

**`StEnt` / `StSlot` / `StateTab`**（开地址哈希，主键=坐标，`src/route/st.h:18-54`）：

```c
typedef struct {
    uint64_t key;        /* coord_key_e7 */
    double   g;
    uint32_t prev_idx;   /* 前驱是同一张 ents 数组里的下标（UINT32_MAX = 无前驱） */
    uint32_t eprov;      /* 前驱 → 本坐标用的边所属省 */
    uint32_t eidx;       /* 边下标；REDGE_SEAM = 起点（无入边） */
    uint32_t from_node;  /* 该边的起点节点（eprov 省内下标） */
    uint32_t rep_prov;   /* 本坐标落在哪个省的节点上 */
    uint32_t rep_node;
    uint8_t  settled;
} StEnt;

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
```

`st_intern` / `st_find`（完整，`src/route/st.c:63-107`）——命中 32 位哈希后必须再用 `ents[idx].key` 回验一次：

```c
StEnt *st_intern(StateTab *t, uint64_t key)
{
    uint64_t m = mix64(key);
    uint32_t h = (uint32_t)(m >> 32) | 1u;
    size_t mask = t->cap - 1;
    size_t j = (size_t)(m & mask);
    while (t->slot[j].h) {
        if (t->slot[j].h == h && t->ents[t->slot[j].idx].key == key)
            return &t->ents[t->slot[j].idx];
        j = (j + 1) & mask;
    }
    if (t->n_ents == t->cap_ents) {
        t->cap_ents *= 2;
        t->ents = xrealloc(t->ents, t->cap_ents * sizeof(StEnt));
    }
    uint32_t ei = (uint32_t)t->n_ents++;
    t->ents[ei].key = key;
    t->ents[ei].g = INF_G;
    t->ents[ei].prev_idx = UINT32_MAX;
    t->ents[ei].eprov = 0;
    t->ents[ei].eidx = REDGE_SEAM;
    t->ents[ei].from_node = 0;
    t->ents[ei].rep_prov = 0;
    t->ents[ei].rep_node = 0;
    t->ents[ei].settled = 0;
    t->slot[j].h = h;
    t->slot[j].idx = ei;
    t->used++;
    if (t->used * 10 >= t->cap * 7) st_rehash(t);
    return &t->ents[ei];
}

StEnt *st_find(const StateTab *t, uint64_t key)
{
    uint64_t m = mix64(key);
    uint32_t h = (uint32_t)(m >> 32) | 1u;
    size_t mask = t->cap - 1;
    size_t j = (size_t)(m & mask);
    while (t->slot[j].h) {
        if (t->slot[j].h == h && t->ents[t->slot[j].idx].key == key)
            return &t->ents[t->slot[j].idx];
        j = (j + 1) & mask;
    }
    return NULL;
}
```

（`st_init` / `st_free` / `st_rehash` / `mix64` 为控制篇幅只列签名：`st_init` `st.c:27`、`st_free` `st.c:39`、`st_rehash` `st.c:45`、`mix64` `st.c:9`。）

### 3. 4 叉最小堆 `PQueue`（完整）：`src/route/pqueue.h:35-119`

```h
#define PQ_D 4

typedef struct { double f; uint64_t idx; } PQEnt;

typedef struct {
    PQEnt *a;
    size_t n, cap;
} PQueue;

static inline void pq_push(PQueue *q, double f, uint64_t idx)
{
    if (q->n == q->cap) {
        q->cap = q->cap ? q->cap * 2 : 4096;
        q->a = pq_xrealloc(q->a, q->cap * sizeof(PQEnt));
    }
    PQEnt *h = q->a;
    size_t i = q->n++;
    h[i].f = f; h[i].idx = idx;
    while (i > 0) {
        size_t p = (i - 1) / PQ_D;
        if (h[p].f <= h[i].f) break;
        PQEnt t = h[p]; h[p] = h[i]; h[i] = t;
        i = p;
    }
}

static inline uint64_t pq_pop(PQueue *q, double *f_out)
{
    PQEnt *h = q->a;
    uint64_t idx = h[0].idx;
    if (f_out) *f_out = h[0].f;
    if (--q->n == 0) return idx;

    PQEnt last = h[q->n];
    size_t i = 0, n = q->n;
    for (;;) {
        size_t c0 = PQ_D * i + 1;
        if (c0 >= n) break;
        size_t m = i;
        double fm = last.f;
        size_t cmax = c0 + PQ_D < n ? c0 + PQ_D : n;
        for (size_t c = c0; c < cmax; c++)
            if (h[c].f < fm) { fm = h[c].f; m = c; }
        if (m == i) break;
        h[i] = h[m];
        i = m;
    }
    h[i] = last;
    return idx;
}

static inline double pq_peek_f(const PQueue *q)
{
    return q->n ? q->a[0].f : (double)(1.0 / 0.0);
}

static inline void pq_clear(PQueue *q) { q->n = 0; }
static inline void pq_free(PQueue *q)  { free(q->a); q->a = NULL; q->n = q->cap = 0; }
```

★ 叉数 D=4 一枝独秀：元素 16 字节（double f + uint64 idx），4 个子节点正好 64 字节 = 一条 cache line；且 D 为 2 的幂时 `i*D+1`、`(i-1)/D` 是移位。载荷存 **StEnt 下标**而非 key：pop 后直接 `&st->ents[idx]`，省掉一次哈希查找（京→郑占 13.8%）。下沉用 **hole 法**，写流量降到约 1/3。

### 4. 反向入边索引 `RevIdx`（结构体 + 关键函数签名）：`src/route/revidx.h:31-56`

```c
typedef struct {
    int       built;      /* 1 = 可用 */
    uint32_t *start;      /* n+1 */
    uint32_t *list;       /* list_count 个 from 节点 */
    uint64_t  list_count; /* = start[n]，可能 < edge_count（脏边被跳过） */
    int       mapped;     /* 1 = start/list 指向 mmap 区，释放用 munmap 不是 free */
    uint8_t  *map_base;
    size_t    map_len;
} RevIdx;
```

| 函数 | 位置 | 说明 |
|---|---|---|
| `rev_build_mem` | `revidx.c:71-138` | 内存构建反向 CSR（`start[nd]..start[nd+1]` = 指向 nd 的入边区间，list 只存 from 节点） |
| `rev_load` | `revidx.c:209-261` | 从 `<dir>/rev.bin` mmap，结构/规模校验，正文校验和默认不跑（详见 `rev_strict`） |
| `rev_write` | `revidx.c:148-196` | 构建并写 `rev.bin`（先 `.tmp` 再 `rename`，原子替换） |
| `rev_free` | `revidx.c:263-273` | 按 `mapped` 决定 `munmap` 还是 `free` |
| `rev_equal` | `revidx.c:275-284` | 逐元素对比两份索引（含 `list_count`），供 `--verify` |

`rev_build_mem` 为完整范例（反向 CSR 构建，含脏边跳过与前缀和，为控制篇幅此处仅列签名；其完整实现见 `src/route/revidx.c:71-138`）。

### 5. 地标下界表 `LmTable`（结构体 + 关键函数签名）：`src/route/lmtable.h:43-94`

| 结构/函数 | 位置 | 说明 |
|---|---|---|
| `LmHeader` / `LmTable` | `lmtable.h:53-70` | 网格单元存"单元内节点→地标 L 的最小路程"（uint16，单位 100 m）；含图指纹 `nprov`/`total_nodes` |
| `lm_table_load` | `lmtable.c:96-121` | 载入 `lm.bin`，magic/version 不对返回 -1（调用方退回纯直线） |
| `lm_table_match` | `lmtable.c:90-94` | 图指纹校验：省数/总节点数与建表时一致才返回 1 |
| `lm_cell_of` | `lmtable.c:7-15` | 格号，网格外返回 `(size_t)-1` |
| `lm_bound_m` | `lmtable.c:17-36` | u→目标格 tc 的地标下界（米），不可用返回负数 |

`lm_bound_m`（完整，`src/route/lmtable.c:17-36`）——单侧下界 `d(u,t) ≥ d(L,t) − d(L,u)`：

```c
double lm_bound_m(const LmTable *t, size_t tc, int32_t lat_e7, int32_t lon_e7)
{
    if (!t->loaded || tc == (size_t)-1) return -1.0;
    size_t uc = lm_cell_of(t, lat_e7, lon_e7);
    if (uc == (size_t)-1) return -1.0;

    uint32_t nlm = t->h.nlm;
    const uint16_t *tb = t->v + tc * nlm;
    const uint16_t *ub = t->v + uc * nlm;
    int best = -1;
    for (uint32_t k = 0; k < nlm; k++) {
        int a = tb[k], b = ub[k];
        if (a == (int)LM_NONE || b == (int)LM_NONE) continue;
        int d = a - b;
        if (d > best) best = d;
    }
    if (best < 0) return -1.0;
    double m = (double)best * (double)t->h.unit_m - (double)t->h.penalty_m;
    return m > 0.0 ? m : 0.0;
}
```

★ 它不是**严格**下界（ALT 加速启发式）：50 条里 2 条开表更差 ⇒ 带 `lm.bin` 时 `--exact`/`w=1.0` 不再保证最优。验收真值请用 `NAVIMAP_LM=0`。

### 6. 代价与启发式（完整）：`src/route/cost.c:14-54`

```c
uint8_t fallback_speed(uint8_t cls)
{
    switch (cls) {
    case RC_MOTORWAY:    return 100;
    case RC_PRIMARY:     return 60;
    case RC_TERTIARY:    return 40;
    case RC_RESIDENTIAL: return 30;
    case RC_OTHER:       return 20;
    default:             return 20;
    }
}

double edge_cost(const Router *r, const uint8_t *er, double *meters)
{
    double m = (double)nm_get_u32(er + EF_DIST_CM) / 100.0;
    if (meters) *meters = m;
    if (!r->opt.mode_time) return m;
    uint8_t sp = er[EF_SPEED];
    if (sp == 0) sp = fallback_speed(er[EF_CLASS]);
    return m * 3.6 / (double)sp;
}

double heur(const Router *r, int dir, int32_t lat_e7, int32_t lon_e7)
{
    double d = nm_haversine_m((double)lat_e7 / 1e7, (double)lon_e7 / 1e7,
                              (double)r->tgt_lat[dir] / 1e7,
                              (double)r->tgt_lon[dir] / 1e7);
    if (r->lm.loaded && r->lm_tc[dir] != (size_t)-1) {
        double b = lm_bound_m(&r->lm, r->lm_tc[dir], lat_e7, lon_e7);
        if (b > d) d = b;
    }
    return r->opt.mode_time ? d * 3.6 / H_SPEED_KMH : d;
}
```

`heur` 取"大圆距离"与"地标下界"的 max（admissible 且 consistent：边权=时间 ≥ 距离/130km/h）。`node_coord` / `lm_enabled` / `lm_penalty_add_m` / `graph_root_dir` 为控制篇幅仅列签名（`cost.c:56-110`）。

### 7. 转向限制检查（签名 + 接缝补查完整）：`src/route/restr.c`

| 函数 | 位置 | 说明 |
|---|---|---|
| `restr_build` | `restr.c:41-84` | 把某省限制记录解析成三张排序索引表（kind 0/1/2），惰性构建 |
| `kv_lower` | `restr.c:87-95` | 在 kv 升序表里二分找 key 对应区间起点 |
| `restr_blocked` | `restr.c:97-110` | kind=0：`{via, from}→to` 命中即禁 |
| `restr_blocked_only` | `restr.c:122-137` | kind=1（only_*）：`{via, from}` 有 only_* 且 to 不在允许集里 ⇒ 禁 |
| `restr_blocked2` | `restr.c:142-161` | kind=2（via=way 两段）：`prev→from→to` 命中即禁 |
| `restr_blocked2_rev` | `restr.c:167-184` | 反向保守版：只看 `(via, from, to)` 命中即禁（prev 未知） |
| `seam_blocked` | `restr.c:190-213` | 双向会合点处的转向补查（正向最后一条边 × 反向第一条边） |

**`seam_blocked`（完整）**——双向各自展开时都看不到会合点的完整转向，必须在此补查：

```c
int seam_blocked(Router *r, uint64_t k)
{
    /* no_seam_restr 只给 restr_ab 做负向对照用：证明"判据能失败"，见 PLAN.md §33 */
    if (r->opt.no_seam_restr) return 0;
    StEnt *f = st_find(&r->st[DIR_FWD], k);
    StEnt *b = st_find(&r->st[DIR_BWD], k);
    if (!f || !b) return 0;
    uint32_t e_f = f->eidx, e_b = b->eidx;
    if (e_f == REDGE_SEAM || e_b == REDGE_SEAM) return 0;
    /* 两边必须落在同一省的同一个节点上，否则边下标无从比起 */
    if (f->rep_prov != b->rep_prov || f->rep_node != b->rep_node) return 0;
    uint32_t prov = f->rep_prov;
    if (prov >= (uint32_t)r->ms->n) return 0;
    if (restr_blocked(r, prov, f->rep_node, e_f, e_b)) return 1;
    if (restr_blocked_only(r, prov, f->rep_node, e_f, e_b)) return 1;

    uint32_t prev2 = REDGE_SEAM;
    if (f->prev_idx != UINT32_MAX) {
        StEnt *pf = &r->st[DIR_FWD].ents[f->prev_idx];
        if (pf->eprov == prov) prev2 = pf->eidx;
    }
    if (restr_blocked2(r, prov, f->rep_node, prev2, e_f, e_b)) return 1;
    return 0;
}
```

### 8. ★ 双向加权 A* 主循环 `plan_search`（完整）：`src/route/route.c:749-1089`

（含状态重置、双向终止条件、堆顶取 `ents` 下标、接缝补查、回溯拼装。为便于教学，此处给出**完整**函数体。）

```c
static int plan_search(Router *r, RNode from, RNode to, RouteResult *out,
                       double w, uint64_t max_expand)
{
    r->opt.heur_w = w;
    memset(out, 0, sizeof(*out));
    uint64_t t0 = now_ms();

    if (from.prov >= (uint32_t)r->ms->n || to.prov >= (uint32_t)r->ms->n) return -1;
    {
        const MapSrc *fs = &r->ms->srcs[from.prov], *ts = &r->ms->srcs[to.prov];
        if ((uint64_t)from.node >= fs->nodes.hdr.rec_count ||
            (uint64_t)to.node   >= ts->nodes.hdr.rec_count) return -1;
    }

    uint64_t fk = node_coord(&r->ms->srcs[from.prov], from.node);
    uint64_t tk = node_coord(&r->ms->srcs[to.prov], to.node);
    /* 启发式目标：正向朝终点，反向朝起点 */
    r->tgt_lat[DIR_FWD] = coord_key_lat(tk);
    r->tgt_lon[DIR_FWD] = coord_key_lon(tk);
    r->tgt_lat[DIR_BWD] = coord_key_lat(fk);
    r->tgt_lon[DIR_BWD] = coord_key_lon(fk);
    /* 地标下界的"目标格"：正向是终点格，反向是起点格。一次搜索只算两遍，
       不能放进 heur（那是每个候选节点都要跑的）。 */
    r->lm_tc[DIR_FWD] = r->lm.loaded
                      ? lm_cell_of(&r->lm, r->tgt_lat[DIR_FWD], r->tgt_lon[DIR_FWD])
                      : (size_t)-1;
    r->lm_tc[DIR_BWD] = r->lm.loaded
                      ? lm_cell_of(&r->lm, r->tgt_lat[DIR_BWD], r->tgt_lon[DIR_BWD])
                      : (size_t)-1;
    r->best_cut = INF_G;

    st_free(&r->st[0]); st_init(&r->st[0]);
    st_free(&r->st[1]); st_init(&r->st[1]);
    pq_clear(&r->pq[0]);
    pq_clear(&r->pq[1]);
    r->expanded = 0;

    if (fk == tk) {                        /* 同一个路口 */
        out->ok = 1;
        out->n = 1;
        out->pts = malloc(sizeof(RNode));
        out->lat_e7 = malloc(sizeof(int32_t));
        out->lon_e7 = malloc(sizeof(int32_t));
        if (!out->pts || !out->lat_e7 || !out->lon_e7) {
            route_result_free(out); return -2;
        }
        out->pts[0] = from;
        out->lat_e7[0] = coord_key_lat(fk);
        out->lon_e7[0] = coord_key_lon(fk);
        out->elapsed_ms = now_ms() - t0;
        return 0;
    }

    StEnt *se = st_intern(&r->st[DIR_FWD], fk);
    se->g = 0;
    se->prev_idx = UINT32_MAX;
    se->eidx = REDGE_SEAM;
    se->rep_prov = from.prov;
    se->rep_node = from.node;
    pq_push(&r->pq[DIR_FWD],
              heur(r, DIR_FWD, coord_key_lat(fk), coord_key_lon(fk)),
              (uint32_t)(se - r->st[DIR_FWD].ents));

    int use_bidir = r->opt.bidir;
    if (use_bidir) {
        StEnt *ge = st_intern(&r->st[DIR_BWD], tk);
        ge->g = 0;
        ge->prev_idx = UINT32_MAX;
        ge->eidx = REDGE_SEAM;
        ge->rep_prov = to.prov;
        ge->rep_node = to.node;
        pq_push(&r->pq[DIR_BWD],
                  heur(r, DIR_BWD, coord_key_lat(tk), coord_key_lon(tk)),
                  (uint32_t)(ge - r->st[DIR_BWD].ents));
    }

    double best = INF_G;
    uint32_t meet_idx[2] = {0, 0};
    int found = 0;

    while (r->pq[DIR_FWD].n || r->pq[DIR_BWD].n) {
        double ff = pq_peek_f(&r->pq[DIR_FWD]);
        double fb = pq_peek_f(&r->pq[DIR_BWD]);

        /* 双向终止条件（"最优性 ↔ 速度"的开关）：
           - 默认 lb = ff + fb：两侧堆顶 f 之和。快，但不保证最优。
           - opt.exact 时 lb = max(ff, fb)：保证最优（h 可采纳为前提）。 */
        if (use_bidir) {
            double lb = r->opt.exact ? (ff > fb ? ff : fb) : (ff + fb);
            if (lb >= best) break;
        }

        int dir = (!use_bidir || ff <= fb) ? DIR_FWD : DIR_BWD;
        if (r->pq[dir].n == 0) {
            if (r->pq[1 - dir].n == 0) break;
            dir = 1 - dir;
        }
        double fv;
        uint32_t ei = (uint32_t)pq_pop(&r->pq[dir], &fv);
        /* 堆条目直接带 ents 下标，不再用 key 哈希查找：省掉 2~3 次随机访存。 */
        StEnt *e = &r->st[dir].ents[ei];
        if (e->settled) continue;            /* 惰性删除：重复入堆的老条目 */
        uint64_t k = e->key;
        expand_dir(r, dir, k, e, ei);

        if (!use_bidir) {
            StEnt *te = st_find(&r->st[DIR_FWD], tk);
            if (te && te->settled) {
                found = 1;
                meet_idx[DIR_FWD] = (uint32_t)(te - r->st[DIR_FWD].ents);
                break;
            }
        } else {
            StEnt *o = st_find(&r->st[1 - dir], k);
            if (o && o->settled) {
                StEnt *self = &r->st[dir].ents[ei];
                uint32_t oi = (uint32_t)(o - r->st[1 - dir].ents);
                double mu = self->g + o->g;
                /* 接缝处的转向限制必须在这里补（两边各自展开时都没有完整转向可查）。 */
                if (mu < best && !seam_blocked(r, k)) {
                    best = mu;
                    meet_idx[dir]     = ei;
                    meet_idx[1 - dir] = oi;
                    found = 1;
                    r->best_cut = best;              /* 之后用它剪枝入堆 */
                }
            }
        }
        if (r->expanded > max_expand) {
            if (max_expand >= r->max_expand)
                fprintf(stderr, "路由: 展开数超过上限 %llu，放弃\n",
                        (unsigned long long)r->max_expand);
            out->elapsed_ms = now_ms() - t0;
            out->expanded = r->expanded;
            return (max_expand < r->max_expand) ? -3 : -2;
        }
    }
    out->elapsed_ms = now_ms() - t0;
    out->expanded = r->expanded;
    if (!found) {
        if (getenv("NAVIMAP_ROUTE_DEBUG")) {
            fprintf(stderr,
                    "[dbg] 未连通: 展开 %llu  堆[正]=%zu 堆[反]=%zu  "
                    "状态[正]=%llu 状态[反]=%llu  best=%g\n",
                    (unsigned long long)r->expanded,
                    r->pq[DIR_FWD].n, r->pq[DIR_BWD].n,
                    (unsigned long long)r->st[DIR_FWD].used,
                    (unsigned long long)r->st[DIR_BWD].used, best);
        }
        return -1;
    }

    /* ---------------- 回溯拼装 ---------------- */
    uint32_t cap = 1024, n = 0;
    uint64_t *keys = malloc(cap * sizeof(uint64_t));
    uint32_t *ap = malloc(cap * sizeof(uint32_t));
    uint32_t *ae = malloc(cap * sizeof(uint32_t));
    uint32_t *af = malloc(cap * sizeof(uint32_t));
    uint32_t *tmp = malloc(cap * sizeof(uint32_t));
    uint32_t tcap = cap;
    if (!keys || !ap || !ae || !af || !tmp) {
        free(keys); free(ap); free(ae); free(af); free(tmp);
        return -2;
    }
#define GROW(arr, need) do { \
        if ((need) > cap) { \
            uint32_t nc = cap; \
            while (nc < (need)) nc *= 2; \
            keys = xrealloc(keys, nc * sizeof(uint64_t)); \
            ap   = xrealloc(ap,   nc * sizeof(uint32_t)); \
            ae   = xrealloc(ae,   nc * sizeof(uint32_t)); \
            af   = xrealloc(af,   nc * sizeof(uint32_t)); \
            cap = nc; \
        } \
    } while (0)

    size_t tn = 0;
    {
        uint32_t cur = meet_idx[DIR_FWD];
        for (;;) {
            if (tn == tcap) { tcap *= 2; tmp = xrealloc(tmp, tcap * sizeof(uint32_t)); }
            tmp[tn++] = cur;
            StEnt *e = &r->st[DIR_FWD].ents[cur];
            if (e->prev_idx == UINT32_MAX) break;
            cur = e->prev_idx;
        }
    }
    for (size_t i = tn; i > 0; i--) {           /* tmp 是 相遇点..起点，翻过来 */
        uint32_t idx = tmp[i - 1];
        StEnt *e = &r->st[DIR_FWD].ents[idx];
        GROW(keys, n + 1);
        keys[n] = e->key;
        ap[n] = e->eprov;
        ae[n] = e->eidx;
        af[n] = e->from_node;
        n++;
    }
    {   /* 反向半段：相遇点之后一直走到终点。 */
        int guard = 0;
        uint32_t cur_bwd = meet_idx[DIR_BWD];
        while (use_bidir && guard++ < 20000000) {
            uint32_t cur = cur_bwd;
            StEnt *e = &r->st[DIR_BWD].ents[cur];
            if (e->key == tk) break;                 /* 到终点了 */
            if (e->prev_idx == UINT32_MAX) break;
            uint32_t nxt = e->prev_idx;
            StEnt *ne = &r->st[DIR_BWD].ents[nxt];
            GROW(keys, n + 1);
            keys[n] = ne->key;
            ap[n] = e->eprov;
            ae[n] = e->eidx;
            af[n] = e->from_node;
            n++;
            cur_bwd = nxt;
        }
    }
    free(tmp);
#undef GROW

    out->n = n;
    out->pts    = malloc((size_t)n * sizeof(RNode));
    out->lat_e7 = malloc((size_t)n * sizeof(int32_t));
    out->lon_e7 = malloc((size_t)n * sizeof(int32_t));
    if (n > 1) {
        out->edge_idx  = malloc((size_t)(n - 1) * sizeof(uint32_t));
        out->from_node = malloc((size_t)(n - 1) * sizeof(uint32_t));
        out->edge_cls  = malloc((size_t)(n - 1) * sizeof(uint8_t));
        out->edge_flags= malloc((size_t)(n - 1) * sizeof(uint8_t));
        out->seg_m     = malloc((size_t)(n - 1) * sizeof(uint32_t));
        out->seg_ds    = malloc((size_t)(n - 1) * sizeof(uint32_t));
    }
    if (!out->pts || !out->lat_e7 || !out->lon_e7 ||
        (n > 1 && (!out->edge_idx || !out->from_node ||
                   !out->edge_cls || !out->edge_flags || !out->seg_m ||
                   !out->seg_ds))) {
        free(keys); free(ap); free(ae); free(af);
        route_result_free(out);
        return -2;
    }

    double tm = 0, ts = 0;
    for (uint32_t i = 0; i < n; i++) {
        out->lat_e7[i] = coord_key_lat(keys[i]);
        out->lon_e7[i] = coord_key_lon(keys[i]);
        if (i == 0) {
            out->pts[0] = from;
        } else {
            uint32_t pr = ap[i];
            if (pr >= (uint32_t)r->ms->n || ae[i] == REDGE_SEAM) {
                free(keys); free(ap); free(ae); free(af);
                route_result_free(out);
                return -2;
            }
            const uint8_t *er = r->ms->srcs[pr].edges.data +
                                (uint64_t)ae[i] * NM_EDGE_BYTES;
            out->pts[i].prov = pr;
            out->pts[i].node = nm_get_u32(er + EF_TO);
        }
        if (i + 1 < n) {
            uint32_t pr = ap[i + 1];
            if (pr >= (uint32_t)r->ms->n || ae[i + 1] == REDGE_SEAM) {
                free(keys); free(ap); free(ae); free(af);
                route_result_free(out);
                return -2;
            }
            out->edge_idx[i]  = ae[i + 1];
            out->from_node[i] = af[i + 1];
            const uint8_t *er = r->ms->srcs[pr].edges.data +
                                (uint64_t)ae[i + 1] * NM_EDGE_BYTES;
            out->edge_cls[i] = er[EF_CLASS];
            out->edge_flags[i] = (uint8_t)(nm_get_u16(er + EF_FLAGS) & 0xFF);
            double meters = 0;
            edge_cost(r, er, &meters);
            out->seg_m[i] = (uint32_t)(meters + 0.5);
            tm += out->seg_m[i];
            uint8_t sp = er[EF_SPEED];
            if (sp == 0) sp = fallback_speed(er[EF_CLASS]);
            double sec = meters * 3.6 / (double)sp;      /* km/h → m/s */
            out->seg_ds[i] = (uint32_t)(sec * 10.0 + 0.5);
            ts += out->seg_ds[i] / 10.0;
        }
    }
    for (uint32_t i = 1; i + 1 < n; i++)
        if (ap[i] != ap[i + 1]) out->n_seams++;

    out->total_m = tm;
    out->total_s = ts;
    out->ok = 1;
    free(keys); free(ap); free(ae); free(af);
    return 0;
}
```

`expand_dir`（`route.c:573-742`，每坐标松弛出/入边、点查转向限制、入堆记 `ents` 下标）与 `members_of`（`route.c:511-564`，取同一坐标的所有跨省成员）为控制篇幅省略完整体，可在 `src/route/route.c:511` 与 `:573` 查阅。

**自适应 w 包装 `router_plan`（完整）：`src/route/route.c:1144-1178`**——两遍只在"撞顶后能换 w"时成立，且必须恢复 `w0`（见注释）：

```c
int router_plan(Router *r, RNode from, RNode to, RouteResult *out)
{
    uint64_t t0 = now_ms();
    uint64_t try_cap = r->opt.adapt_try;
    if (try_cap > r->max_expand) try_cap = r->max_expand;
    /* ★★ plan_search 会把 w 写进 r->opt.heur_w。必须恢复：Router 可复用
       （route_cli --repeat=N 同一 Router 连算 N 次）。 */
    double w0 = r->opt.heur_w;

    int two_pass = (try_cap != 0) && (r->opt.heur_w_hi != w0);

    int rc;
    if (two_pass) {
        rc = plan_search(r, from, to, out, w0, try_cap);
        if (rc != -3) goto done;         /* 有结论了：0 / -1 / -2 */
        route_result_free(out);          /* 撞顶：丢掉半成品，换更重的 w 重来 */
        rc = plan_search(r, from, to, out, r->opt.heur_w_hi, r->max_expand);
    } else {
        rc = plan_search(r, from, to, out, w0, r->max_expand);
    }
done:
    r->opt.heur_w = w0;
    if (rc == 0 || rc == -1) out->elapsed_ms = now_ms() - t0;
    return rc;
}
```

（`router_create` `route.c:63`、`router_destroy` `route.c:129`、`router_route_ll` `route.c:254`、`router_lm_loaded` `route.c:215`、`now_ms` `route.c:48` 为控制篇幅仅列签名。）

### 9. 转向指令生成 `maneuver_build`（完整）：`src/route/maneuver.c:212-304`

配合 `Maneuver` / `ManeuverList`（`maneuver.h:35-49`）：转角 = "身后一段路方位"与"前方一段路方位"之差；前后各看 `LOOK_M=45m` 再算方位避免把弯路中间节点当路口；环岛靠 `EF_ROUNDABOUT` 标志识别，出口序号 = 环内每个路口上"非环岛的出边"计数。

```c
int maneuver_build(const RouteResult *rr, const MapSet *ms, ManeuverList *out)
{
    memset(out, 0, sizeof(*out));
    if (!rr || !rr->ok || rr->n < 2) return 0;

    Maneuver m;
    memset(&m, 0, sizeof(m));
    m.idx = 0;
    m.type = MAN_START;
    road_name_of(rr, ms, 0, m.road, sizeof(m.road), &m.cls);
    ml_push(out, &m);

    double acc = 0;                 /* 自上一条指令起累积的路程 */
    uint32_t last = 0;              /* 上一条指令的点下标 */
    for (uint32_t i = 1; i + 1 < rr->n; i++) {
        acc += seg_len(rr, i - 1);
        uint16_t fprev = seg_flags(rr, i - 1);
        uint16_t fcur  = seg_flags(rr, i);

        if (fcur & EF_ROUNDABOUT) {
            if (fprev & EF_ROUNDABOUT) continue;      /* 环岛内部不报转向 */
            memset(&m, 0, sizeof(m));
            m.idx = i;
            m.type = MAN_ROUNDABOUT;
            m.dist_m = acc;
            m.exit_no = roundabout_exit_no(rr, ms, i);
            road_name_of(rr, ms, i, m.road, sizeof(m.road), &m.cls);
            ml_push(out, &m);
            acc = 0; last = i;
            continue;
        }
        if (fprev & EF_ROUNDABOUT) continue;          /* 刚出环，方向变化不算路口 */

        uint32_t ja = walk(rr, i, +1), jb = walk(rr, i, -1);
        if (ja == i || jb == i) continue;
        double ahead  = bearing_deg(rr->lat_e7[i], rr->lon_e7[i],
                                    rr->lat_e7[ja], rr->lon_e7[ja]);
        double behind = bearing_deg(rr->lat_e7[jb], rr->lon_e7[jb],
                                    rr->lat_e7[i], rr->lon_e7[i]);
        double turn = norm_turn(ahead - behind);

        if (fabs(turn) < TURN_DEADZONE) {
            /* 长距离没有路口：补一条"继续直行"，免得面板上几公里没动静 */
            if (acc >= STRAIGHT_M && i - last > 1) {
                memset(&m, 0, sizeof(m));
                m.idx = i;
                m.type = MAN_STRAIGHT;
                m.dist_m = acc;
                m.turn_deg = turn;
                road_name_of(rr, ms, i, m.road, sizeof(m.road), &m.cls);
                ml_push(out, &m);
                acc = 0; last = i;
            }
            continue;
        }
        if (acc < MIN_GAP_M) continue;                /* 太密集，当噪音丢掉 */

        memset(&m, 0, sizeof(m));
        m.idx = i;
        m.dist_m = acc;
        m.turn_deg = turn;
        road_name_of(rr, ms, i, m.road, sizeof(m.road), &m.cls);
        /* 立交匝道常被切成好几段：同一条路上连着两个同向转向，只报头一个。 */
        double at0 = fabs(turn);
        int t0 = at0 >= TURN_SHARP ? MAN_UTURN
               : at0 >= TURN_NORMAL ? (turn > 0 ? MAN_SHARP_RIGHT : MAN_SHARP_LEFT)
               : at0 >= TURN_SLIGHT ? (turn > 0 ? MAN_RIGHT : MAN_LEFT)
               : (turn > 0 ? MAN_SLIGHT_RIGHT : MAN_SLIGHT_LEFT);
        m.type = t0;
        if (out->n > 0 && acc < MERGE_SAME_M && m.road[0]) {
            const Maneuver *p = &out->v[out->n - 1];
            if (p->type == m.type && p->type != MAN_START &&
                p->type != MAN_ARRIVE && strcmp(p->road, m.road) == 0)
                continue;
        }
        ml_push(out, &m);
        acc = 0; last = i;
    }

    /* 最后一段计入"到达" */
    acc += seg_len(rr, rr->n - 2);
    memset(&m, 0, sizeof(m));
    m.idx = rr->n - 1;
    m.type = MAN_ARRIVE;
    m.dist_m = acc;
    ml_push(out, &m);

    double sum = 0;
    for (int i = 0; i < out->n; i++) sum += out->v[i].dist_m;
    out->sum_m = sum;
    return out->n;
}
```

（`bearing_deg` `maneuver.c:33`、`walk` `maneuver.c:56`、`road_name_of` `maneuver.c:69`、`roundabout_exit_no` `maneuver.c:139`、`maneuver_text` `maneuver.c:182` 等辅助函数为控制篇幅仅列签名，可在 `src/route/maneuver.c` 查阅。）

## 四、与其他功能之间的联系

- **上游（L5 图集 / L6 建图 / L7 瓦片键）**：L6 建图产出 `nodes.bin`/`edges.bin`（CSR 出边）是 L11 路由输入；L7 的 `coord_key_e7` 是状态主键，`mapset_query_tile`/`members_of` 按 Z14 瓦片跨省查同坐标。
- **下游 · L9 渲染管线**：`RouteResult` 经 L9 `navdraw` 绘制路线折线（按 `pts[]`/`seg_m[]`）。
- **下游 · L10 搜索**：**L10 搜索结果（地名/POI 坐标）即 L11 的起终点**——搜索出点 → `router_route_ll` 吸附算路。
- **下游 · L12 导航语音**：`RouteResult` 直接喂 `maneuver_build`（本课 §9）与 `navsim_start`；`RouteOpts` 指名初始化约定被 L12/L13 所有调用点遵守。
- **下游 · L14 架构门禁**：`lm_admissible`（regress.sh）直接验收地标下界；`restr_ab` / `heap_selftest` 是负向对照工具。

## 五、演示、练习与验收

- **演示**：`./build_rel/route_cli --graph=products/graph/beijing --from=39.90,116.40 --to=39.99,116.30 --geojson=route.json`；`./build_rel/route_cli --from=... --to=... --maneuvers` 看 `maneuver_build` 输出；`./build_rel/heap_selftest --expect-fail`（负向对照）；`./build_rel/revbuild_cli --verify`；`./build_rel/restr_ab`；`./build_rel/lmdist_cli`（一对多 Dijkstra 下界）。
- **练习**：把 `ROUTE_HEUR_W_DEFAULT`（route.h:137）改成 1.0（纯 Dijkstra）和 2.0，对比 `route_cli` 的 `expanded`/`elapsed_ms`；用 `NAVIMAP_LM=0` 关表后 `--exact` 应保证最优。
- **验收（读源码 / 跑命令）**：
  1. 能画出"双向 A* 一次展开"的数据流（取堆顶 → `members_of` → 松弛出/入边 → 查转向限制 → `seam_blocked` 相遇判定），并澄清"非 CH、ALT 非严格下界"。
  2. `coord_key_e7`（`coordkey.h:26`）让一个物理路口只展开一次——对比 `(省,节点)` 主键初版的 111 万状态/97 秒问题。
  3. `heap_selftest` 随机序列下每次 pop 取当前最小值；`restr_ab` 验证转向限制使里程变化只在被测因素上。
  4. `lm_table_match`（`lmtable.c:90`）保证重抽 PBF 后旧 `lm.bin` 被丢弃回退直线启发式，避免悄悄次优。
