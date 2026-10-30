# 第 10 课：跨省路由算法（核心）

> 模块：`src/route/{route,st,revidx,lmtable,restr,cost,pqueue,maneuver}.{c,h}`
> 前置：第 4 课图集、第 5 课 CSR 图、第 6 课瓦片键与 `coord_key_e7`、A* / Dijkstra 基础、优先队列。

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

以下四个辅助函数逐字摘自 `src/route/st.c`（行号见各块标题），与源码可编译一致：

**`mix64`**（64 位整数混匀，splitmix64 变体，用于哈希）：`src/route/st.c:9-15`

```c
static uint64_t mix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}
```

**`st_init`**（开地址哈希表初始化，2²⁰ 槽 / 2¹⁸ 条目）：`src/route/st.c:27-37`

```c
void st_init(StateTab *t)
{
    t->cap = 1u << 20;
    t->slot = calloc(t->cap, sizeof(StSlot));
    t->cap_ents = 1u << 18;
    t->ents = malloc(t->cap_ents * sizeof(StEnt));
    if (!t->slot || !t->ents) {
        fprintf(stderr, "路由: 状态表分配失败\n");
        exit(2);
    }
}
```

**`st_free`**（释放槽与条目数组）：`src/route/st.c:39-43`

```c
void st_free(StateTab *t)
{
    free(t->slot); free(t->ents);
    memset(t, 0, sizeof(*t));
}
```

**`st_rehash`**（翻倍扩容，h 复用、下标按新 cap 用 key 重算）：`src/route/st.c:45-61`

```c
void st_rehash(StateTab *t)
{
    size_t ncap = t->cap * 2;
    StSlot *ns = calloc(ncap, sizeof(StSlot));
    if (!ns) { fprintf(stderr, "路由: 状态表扩容失败\n"); exit(2); }
    for (size_t i = 0; i < t->cap; i++) {
        if (!t->slot[i].h) continue;
        /* h 可以复用；下标要按新 cap 重算 ⇒ 需要 key ⇒ 从 ents 取（扩容很罕见） */
        uint64_t m = mix64(t->ents[t->slot[i].idx].key);
        size_t j = (size_t)(m & (ncap - 1));
        while (ns[j].h) j = (j + 1) & (ncap - 1);
        ns[j].h = t->slot[i].h;
        ns[j].idx = t->slot[i].idx;
    }
    free(t->slot);
    t->slot = ns; t->cap = ncap;
}
```

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

`rev_build_mem` 完整范例（反向 CSR 构建，含脏边跳过与前缀和），逐字摘自 `src/route/revidx.c:71-138`：

```c
int rev_build_mem(const NMBinFile *nodes, const NMBinFile *edges,
                  const char *prov_name, RevIdx *rv)
{
    memset(rv, 0, sizeof(*rv));
    uint64_t n  = nodes->hdr.rec_count;
    uint64_t ne = edges->hdr.rec_count;
    if (n == 0 || ne == 0) return -1;
    if (ne > 0xFFFFFFFFu) return -1;     /* 反向索引用 u32 偏移，超了就不建 */

    uint64_t t0 = rev_now_ms();
    uint32_t *start = calloc((size_t)n + 1, sizeof(uint32_t));
    uint32_t *cur   = malloc(((size_t)n + 1) * sizeof(uint32_t));
    uint32_t *list  = malloc((size_t)ne * sizeof(uint32_t));
    if (!start || !cur || !list) {
        free(start); free(cur); free(list);
        fprintf(stderr, "路由: 反向索引分配失败\n");
        return -1;
    }
    const uint8_t *nds = nodes->data;
    const uint8_t *eds = edges->data;

    /* 计数：按 from 节点遍历（CSR 天然按 from 分组） */
    for (uint64_t i = 0; i < n; i++) {
        const uint8_t *nr = nds + i * NM_NODE_BYTES;
        uint32_t eb = nm_get_u32(nr + NF_EDGE_BEGIN);
        uint32_t ec = nm_get_u16(nr + NF_EDGE_COUNT);
        for (uint32_t k = 0; k < ec; k++) {
            uint64_t ei = (uint64_t)eb + k;
            if (ei >= ne) break;
            uint32_t to = nm_get_u32(eds + ei * NM_EDGE_BYTES + EF_TO);
            if ((uint64_t)to >= n) continue;      /* 脏数据，忽略 */
            start[to + 1]++;
        }
    }
    /* 前缀和：start[i] = 指向节点 i 的入边区间起点，start[i+1] = 区间终点。
       ★ 不能写成 for (i=0; i<=n; i++) { c=start[i]; start[i]=acc; acc+=c; }——
         那样 start[n] 会漏掉 cnt[n-1]（最后一个节点的入边区间被截成空）。 */
    uint64_t acc = 0;
    for (uint64_t i = 0; i < n; i++) {
        uint32_t c = start[i + 1];
        start[i] = (uint32_t)acc;
        acc += c;
    }
    start[n] = (uint32_t)acc;
    memcpy(cur, start, ((size_t)n + 1) * sizeof(uint32_t));
    for (uint64_t i = 0; i < n; i++) {
        const uint8_t *nr = nds + i * NM_NODE_BYTES;
        uint32_t eb = nm_get_u32(nr + NF_EDGE_BEGIN);
        uint32_t ec = nm_get_u16(nr + NF_EDGE_COUNT);
        for (uint32_t k = 0; k < ec; k++) {
            uint64_t ei = (uint64_t)eb + k;
            if (ei >= ne) break;
            uint32_t to = nm_get_u32(eds + ei * NM_EDGE_BYTES + EF_TO);
            if ((uint64_t)to >= n) continue;
            list[cur[to]++] = (uint32_t)i;
        }
    }
    free(cur);
    rv->built = 1;
    rv->start = start;
    rv->list  = list;
    rv->list_count = start[n];
    fprintf(stderr, "路由: 为 %s 建反向索引（%llu 边 / %llu 节点，%llu ms）\n",
            prov_name ? prov_name : nodes->hdr.source,
            (unsigned long long)ne, (unsigned long long)n,
            (unsigned long long)(rev_now_ms() - t0));
    return 0;
}
```

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

`heur` 取"大圆距离"与"地标下界"的 max（admissible 且 consistent：边权=时间 ≥ 距离/130km/h）。以下四个辅助函数逐字摘自 `src/route/cost.c`（行号见各块标题），与源码可编译一致：

**`node_coord`**（节点下标 → 坐标键 `coord_key_e7`）：`src/route/cost.c:56-60`

```c
uint64_t node_coord(const MapSrc *s, uint32_t node)
{
    const uint8_t *nr = s->nodes.data + (uint64_t)node * NM_NODE_BYTES;
    return coord_key_e7(nm_get_i32(nr + NF_LAT), nm_get_i32(nr + NF_LON));
}
```

**`lm_enabled`**（地标表开关，默认开；`NAVIMAP_LM=0` 关，只读一次）：`src/route/cost.c:64-72`

```c
int lm_enabled(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("NAVIMAP_LM");
        v = !(e && e[0] == '0' && e[1] == '\0');
    }
    return v;
}
```

**`lm_penalty_add_m`**（诊断旋钮：在表自带 penalty 上再加 N 米，只读一次）：`src/route/cost.c:79-89`

```c
uint32_t lm_penalty_add_m(void)
{
    static long v = -1;
    if (v < 0) {
        const char *e = getenv("NAVIMAP_LM_PENALTY_ADD_M");
        v = e ? atol(e) : 0;
        if (v < 0) v = 0;
        if (v > 1000000) v = 1000000;
    }
    return (uint32_t)v;
}
```

**`graph_root_dir`**（图目录 = 省目录上上一级，全图只有一份 `lm.bin`/`rev.bin`）：`src/route/cost.c:93-110`

```c
int graph_root_dir(const MapSet *ms, char *out, size_t cap)
{
    if (!ms || ms->n == 0 || !ms->srcs[0].dir[0]) return -1;
    const char *d = ms->srcs[0].dir;
    const char *slash = strrchr(d, '/');
    size_t n;
    if (!slash) {                        /* 相对的单层目录 ⇒ 父目录是 "." */
        if (cap < 2) return -1;
        out[0] = '.'; out[1] = '\0';
        return 0;
    }
    n = (size_t)(slash - d);
    if (n == 0) n = 1;                   /* "/beijing" ⇒ 父目录是 "/" */
    if (n >= cap) return -1;
    memcpy(out, d, n);
    out[n] = '\0';
    return 0;
}
```

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

**`members_of`**（取同一坐标上的所有跨省成员，逐字摘自 `src/route/route.c:511-564`）——省内部瓦片直接返回到达时的节点，跨多省的缝合坐标用二分在 `SewSlot` 里把所有 `(prov,node)` 成员捞出来：

```c
size_t members_of(Router *r, uint64_t ck, uint32_t in_prov, uint32_t in_node,
                         SewEnt *mem)
{
    size_t nmem = 0;
    int32_t la = coord_key_lat(ck), lo = coord_key_lon(ck);
    int tx, ty;
    nm_tile_of((double)la / 1e7, (double)lo / 1e7, &tx, &ty);
    SewSlot *sl = sew_get(r, tx, ty);
    if (sl->nprov < 0) sl->nprov = tile_prov_count(r->ms, tx, ty);

    if (sl->nprov >= 2) {
        sew_build(r, sl);
        size_t lo_i = 0, hi = sl->n;
        while (lo_i < hi) {
            size_t mid = (lo_i + hi) / 2;
            if (sl->v[mid].coord < ck) lo_i = mid + 1; else hi = mid;
        }
        while (lo_i < sl->n && sl->v[lo_i].coord == ck && nmem < MAX_MEMBER)
            mem[nmem++] = sl->v[lo_i++];
    }
    if (nmem == 0) {
        /* 单省瓦片（或索引里没有）：用到达时的那个节点。
           若它属于副本省（起终点吸附有时还是会碰到），换成宿主省里的
           同坐标节点——副本下标在宿主省里没有意义。 */
        uint32_t p = in_prov;
        while (p < (uint32_t)r->ms->n && r->ms->srcs[p].dup_of >= 0)
            p = (uint32_t)r->ms->srcs[p].dup_of;
        int resolved = 0;
        if (p != in_prov && sl) {
            sew_build(r, sl);
            size_t lo_i = 0, hi = sl->n;
            while (lo_i < hi) {
                size_t mid = (lo_i + hi) / 2;
                if (sl->v[mid].coord < ck) lo_i = mid + 1; else hi = mid;
            }
            for (size_t z = lo_i; z < sl->n && sl->v[z].coord == ck; z++)
                if (sl->v[z].prov == p) {
                    mem[nmem].coord = ck;
                    mem[nmem].prov = p;
                    mem[nmem].node = sl->v[z].node;
                    nmem++;
                    resolved = 1;
                    break;
                }
        }
        if (!resolved) {
            mem[nmem].coord = ck;
            mem[nmem].prov = in_prov;
            mem[nmem].node = in_node;
            nmem++;
        }
    }
    return nmem;
}
```

**`expand_dir`**（展开一个坐标：松弛出/入边、点查转向限制、入堆记 `ents` 下标，逐字摘自 `src/route/route.c:573-742`）：

```c
static void expand_dir(Router *r, int dir, uint64_t ck, StEnt *e0,
                       uint32_t self_idx)
{
    double gd = e0->g;
    uint32_t in_prov = e0->rep_prov, in_node = e0->rep_node, in_edge = e0->eidx;

    /* 三段限制要的"再前一条边"。正向时它已知（前驱状态已确定）；
       反向时前驱在搜索方向上还没展开，只能用保守版，见 restr_blocked2_rev。
       ★ prev_idx 是本表的下标，直接索引即可（原先是一次冷 st_find）：
         ents 只 append，下标不会因为 realloc 而失效。 */
    uint32_t prev2_edge = REDGE_SEAM;
    if (dir == DIR_FWD && e0->prev_idx != UINT32_MAX) {
        StEnt *pe = &r->st[dir].ents[e0->prev_idx];
        if (pe->eprov == in_prov) prev2_edge = pe->eidx;
    }

    /* 原来写 st_intern(ck)->settled = 1：为了置一个已知存在的位又跑一遍
       哈希查找（含可能的 rehash）。ck 一定已被 intern（它是从堆里 pop
       出来的），直接置即可。京→郑 83 万次展开 ⇒ 省一次 st_find + 一次
       st_intern（PLAN.md §40）。 */
    e0->settled = 1;
    r->expanded++;

    SewEnt mem[MAX_MEMBER];
    size_t nmem = members_of(r, ck, in_prov, in_node, mem);

    for (size_t mi = 0; mi < nmem; mi++) {
        uint32_t p = mem[mi].prov, nd = mem[mi].node;
        if (p >= (uint32_t)r->ms->n) continue;
        /* 完全被别省包含的副本：它的边在包含它的那份里都有，再走一遍
           只是把同一个路口展开两次（实测北京→天津因此多花一倍时间）。 */
        if (r->ms->srcs[p].dup_of >= 0) continue;
        const MapSrc *s = &r->ms->srcs[p];
        if ((uint64_t)nd >= s->nodes.hdr.rec_count) continue;
        uint64_t ne = s->edges.hdr.rec_count;
        /* 转向限制只在"入边确实落在这个省的这一个节点上"时才有意义；
           其他成员（别的省的副本）没有可用的入边信息，不检查。 */
        int check_restr = (p == in_prov && nd == in_node && in_edge != REDGE_SEAM);

        if (dir == DIR_FWD) {
            const uint8_t *nr = s->nodes.data + (uint64_t)nd * NM_NODE_BYTES;
            uint32_t eb = nm_get_u32(nr + NF_EDGE_BEGIN);
            uint32_t ec = nm_get_u16(nr + NF_EDGE_COUNT);
            for (uint32_t k = 0; k < ec; k++) {
                uint64_t ei = (uint64_t)eb + k;
                if (ei >= ne) break;
                const uint8_t *er = s->edges.data + ei * NM_EDGE_BYTES;
                uint16_t fl = nm_get_u16(er + EF_FLAGS);
                if (fl & EF_NO_ACCESS) continue;
                if (r->opt.avoid_toll && (fl & EF_TOLL)) continue;
                if (r->opt.avoid_unpaved && (fl & EF_UNPAVED)) continue;
                if (check_restr) {
                    if (restr_debug())
                        fprintf(stderr, "DBGf nd=%u in=%u prev2=%u ei=%llu\n",
                                nd, in_edge, prev2_edge, (unsigned long long)ei);
                    if (restr_blocked(r, p, nd, in_edge, (uint32_t)ei))
                        continue;
                    if (restr_blocked_only(r, p, nd, in_edge, (uint32_t)ei))
                        continue;
                    if (restr_blocked2(r, p, nd, prev2_edge, in_edge,
                                       (uint32_t)ei))
                        continue;
                }

                double meters = 0;
                double w = edge_cost(r, er, &meters);
                if (w <= 0) continue;

                uint32_t to = nm_get_u32(er + EF_TO);
                if ((uint64_t)to >= s->nodes.hdr.rec_count) continue;
                uint64_t ck2 = node_coord(s, to);
                if (ck2 == ck) continue;                  /* 自环 */

                StEnt *te = st_intern(&r->st[dir], ck2);
                if (te->settled) continue;
                double ng = gd + w;
                if (ng < te->g) {
                    te->g = ng;
                    te->prev_idx = self_idx;
                    te->eprov = p;
                    te->eidx = (uint32_t)ei;
                    te->from_node = nd;
                    te->rep_prov = p;
                    te->rep_node = to;
                    /* 加权 A*：f = g + w·h（OsmAnd RoutePlanner.cpp:746
                       roadPriorityComparator）。w 默认 1.2，见 route.h。 */
                    double f = ng + r->opt.heur_w * heur(r, dir,
                                         coord_key_lat(ck2), coord_key_lon(ck2));
                    /* 入堆时记下 ents 下标（ents 只 append，realloc 不改下标），
                       pop 之后就不必再哈希查找。见 pqueue.h 顶部。 */
                    if (f < r->best_cut)
                        pq_push(&r->pq[dir], f,
                                (uint32_t)(te - r->st[dir].ents));
                }
            }
        } else {
            RevIdx *rv = &r->rv[p];
            if (!rv->built) {
                /* 优先用构建期落盘的 rev.bin；没有或规模对不上就现场建。
                   两条路共用 rev_build_mem()，见 revidx.h 顶部的说明。 */
                if (rev_load(s->dir, &s->nodes, &s->edges, rv) != 0)
                    rev_build_mem(&s->nodes, &s->edges, s->name, rv);
            }
            if (!rv->start || !rv->list) continue;
            uint32_t b = rv->start[nd], e = rv->start[nd + 1];
            for (uint32_t k = b; k < e; k++) {
                uint32_t u = rv->list[k];
                if ((uint64_t)u >= s->nodes.hdr.rec_count) continue;
                if (u == nd) continue;
                /* 反向索引只存 from 节点，边下标要扫 u 的出边找 EF_TO==nd。
                   出度是个位数，比再存 4 字节/边划算。平行边取代价最小的。 */
                const uint8_t *ur = s->nodes.data + (uint64_t)u * NM_NODE_BYTES;
                uint32_t eb = nm_get_u32(ur + NF_EDGE_BEGIN);
                uint32_t ec = nm_get_u16(ur + NF_EDGE_COUNT);
                uint64_t bei = REDGE_SEAM;
                double bw = INF_G;
                for (uint32_t q = 0; q < ec; q++) {
                    uint64_t ei = (uint64_t)eb + q;
                    if (ei >= ne) break;
                    const uint8_t *er = s->edges.data + ei * NM_EDGE_BYTES;
                    if (nm_get_u32(er + EF_TO) != nd) continue;
                    uint16_t fl = nm_get_u16(er + EF_FLAGS);
                    if (fl & EF_NO_ACCESS) continue;
                    if (r->opt.avoid_toll && (fl & EF_TOLL)) continue;
                    if (r->opt.avoid_unpaved && (fl & EF_UNPAVED)) continue;
                    double meters = 0;
                    double w = edge_cost(r, er, &meters);
                    if (w > 0 && w < bw) { bw = w; bei = ei; }
                }
                if (bei == REDGE_SEAM) continue;
                /* 反向的限制检查：正向路径是 u --bei--> nd --in_edge--> ...
                   ⇒ via = nd, from = bei, to = in_edge，与正向完全对称。 */
                if (check_restr) {
                    if (restr_debug())
                        fprintf(stderr, "DBGb nd=%u in=%u bei=%llu\n",
                                nd, in_edge, (unsigned long long)bei);
                    if (restr_blocked(r, p, nd, (uint32_t)bei, in_edge))
                        continue;
                    if (restr_blocked_only(r, p, nd, (uint32_t)bei, in_edge))
                        continue;
                    if (restr_blocked2_rev(r, p, nd, (uint32_t)bei, in_edge))
                        continue;
                }

                uint64_t ck2 = node_coord(s, u);
                if (ck2 == ck) continue;

                StEnt *te = st_intern(&r->st[dir], ck2);
                if (te->settled) continue;
                double ng = gd + bw;
                if (ng < te->g) {
                    te->g = ng;
                    te->prev_idx = self_idx;   /* ck 在"更靠近终点"的一侧 */
                    te->eprov = p;
                    te->eidx = (uint32_t)bei;
                    te->from_node = u;
                    te->rep_prov = p;
                    te->rep_node = u;
                    /* 加权 A*：f = g + w·h（OsmAnd RoutePlanner.cpp:746
                       roadPriorityComparator）。w 默认 1.2，见 route.h。 */
                    double f = ng + r->opt.heur_w * heur(r, dir,
                                         coord_key_lat(ck2), coord_key_lon(ck2));
                    if (f < r->best_cut)
                        pq_push(&r->pq[dir], f,
                                (uint32_t)(te - r->st[dir].ents));
                }
            }
        }
    }
}
```

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

以下五个辅助函数逐字摘自 `src/route/route.c`（行号见各块标题），与源码可编译一致：

**`now_ms`**（毫秒计时，平台层 `nm_now_ms` 封装）：`src/route/route.c:48-51`

```c
static uint64_t now_ms(void)
{
    return nm_now_ms();                /* 平台层：clock_gettime / QPC */
}
```

**`router_lm_loaded`**（查询地标下界表是否真加载上，给验收脚本用）：`src/route/route.c:215-218`

```c
int router_lm_loaded(const Router *r)
{
    return r && r->lm.loaded ? 1 : 0;
}
```

**`router_create`**（Router 生命周期：加载地标表、`w` 兜底、初始化状态表/限制索引/反向索引）：`src/route/route.c:63-127`

```c
Router *router_create(const MapSet *ms, const RouteOpts *opt)
{
    if (!ms || ms->n == 0) return NULL;
    Router *r = calloc(1, sizeof(Router));
    if (!r) return NULL;
    r->ms = ms;
    /* lm.bin 是**可选的加速表**：没有/版本不对就退回纯直线启发式，不算错误。
       ★ 必须在 w 兜底之前加载 —— w 的默认值取决于有没有这张表（见下）。 */
    r->lm_tc[0] = (size_t)-1;
    r->lm_tc[1] = (size_t)-1;
    if (lm_enabled()) {
        char root[4096];
        if (graph_root_dir(ms, root, sizeof(root)) == 0)
            lm_table_load(&r->lm, root);
        if (r->lm.loaded) {
            uint32_t add = lm_penalty_add_m();
            if (add) {
                printf("地标表       penalty %u + %u = %u 米（诊断旋钮）\n",
                       r->lm.h.penalty_m, add, r->lm.h.penalty_m + add);
                r->lm.h.penalty_m += add;
            }
        }
        /* ★ 图指纹校验：省数据重抽之后旧表必须失效。它不会崩、不会报错，
           只是给出偏大的下界（admissible 被破坏），路线悄悄变差 —— 所以
           这里必须主动把它丢掉，退回纯直线启发式。 */
        if (r->lm.loaded) {
            uint64_t tot = 0;
            for (int i = 0; i < ms->n; i++) tot += ms->srcs[i].nodes.hdr.rec_count;
            if (!lm_table_match(&r->lm, (uint32_t)ms->n, tot)) {
                fprintf(stderr,
                        "lm.bin 与当前路网不匹配（表：%u 省 / %llu 节点；"
                        "现在：%d 省 / %llu 节点）——已忽略，退回直线启发式。"
                        "重跑 scripts/sync_maps.sh 可重建。\n",
                        r->lm.h.nprov, (unsigned long long)r->lm.h.total_nodes,
                        ms->n, (unsigned long long)tot);
                lm_table_free(&r->lm);
            }
        }
    }
    r->opt = opt ? *opt : (RouteOpts){ .mode_time = 1, .bidir = 1, .heur_w = ROUTE_HEUR_W_DEFAULT };
    if (r->opt.max_expand == 0) r->opt.max_expand = DEFAULT_MAX_EXPAND;
    /* 调用点如果漏给 heur_w（指名初始化没写到的字段是 0），兜底成标准 A*。
       有了 ROUTE_HEUR_W_DEFAULT 兜底，加新字段就不必担心旧调用点静默拿到 0。
       ★★ 有 lm.bin 时兜底变成 ROUTE_HEUR_W_LM，而且**关闭自适应升级**
         （hi 也用同一个值）—— 理由见 PLAN.md §50.8 / §51.4：
         w 的"有效贪心度"是 w × h/真值，h 从 44~74% 提到 92~97% 之后，
         1.4 相当于原来的 2 倍贪心；1.8 更是到 1.71，实测质量 +4.1%。
         显式传了 --heur-w / --heur-w-hi 的调用点不受影响。 */
    if (!(r->opt.heur_w > 0.0))
        r->opt.heur_w = r->lm.loaded ? ROUTE_HEUR_W_LM : ROUTE_HEUR_W_DEFAULT;
    if (!(r->opt.heur_w_hi > 0.0))
        r->opt.heur_w_hi = r->lm.loaded ? ROUTE_HEUR_W_LM : ROUTE_HEUR_W_HARD;
    if (r->opt.adapt_try == 0) r->opt.adapt_try = ROUTE_ADAPT_TRY;
    r->max_expand = r->opt.max_expand;
    st_init(&r->st[0]);
    st_init(&r->st[1]);
    r->ri = calloc((size_t)ms->n, sizeof(RestrIdx));
    r->rv = calloc((size_t)ms->n, sizeof(RevIdx));
    if (!r->ri || !r->rv) {
        st_free(&r->st[0]); st_free(&r->st[1]);
        free(r->ri); free(r->rv); free(r);
        return NULL;
    }
    return r;
}
```

**`router_destroy`**（释放状态表/堆/限制索引/反向索引/地标表）：`src/route/route.c:129-151`

```c
void router_destroy(Router *r)
{
    if (!r) return;
    st_free(&r->st[0]);
    st_free(&r->st[1]);
    pq_free(&r->pq[0]);
    pq_free(&r->pq[1]);
    for (int i = 0; i < SEW_SLOTS; i++) free(r->sew[i].v);
    for (int i = 0; i < r->ms->n; i++) {
        free(r->ri[i].kv);
        free(r->ri[i].to);
        free(r->ri[i].kv2);
        free(r->ri[i].to2);
        free(r->ri[i].prev2);
        free(r->ri[i].kv1);
        free(r->ri[i].to1);
        rev_free(&r->rv[i]);
    }
    free(r->ri);
    free(r->rv);
    lm_table_free(&r->lm);
    free(r);
}
```

**`router_route_ll`**（经纬度→经纬度算路，吸附到死节点时换下一个候选重试）：`src/route/route.c:254-289`

```c
int router_route_ll(Router *r, double flat, double flon, double tlat, double tlon,
                    RouteResult *out, RNode *fa, RNode *fb,
                    double *da, double *db)
{
    RNode A[SNAP_TRY], B[SNAP_TRY];
    double DA[SNAP_TRY], DB[SNAP_TRY];
    if (out) memset(out, 0, sizeof(*out));
    int na = router_nearest_k(r, flat, flon, A, SNAP_TRY, DA);
    int nb = router_nearest_k(r, tlat, tlon, B, SNAP_TRY, DB);
    /* 就算失败也把 fa/fb 填上（至少填第一个候选）：调用方拿它做"吸附标记"，
       未初始化的 RNode 会让编译器 -Wmaybe-uninitialized 告警（零警告门禁）。 */
    if (na > 0 && fa) *fa = A[0];
    if (na > 0 && da) *da = DA[0];
    if (nb > 0 && fb) *fb = B[0];
    if (nb > 0 && db) *db = DB[0];
    if (na <= 0 || nb <= 0) return -1;

    for (int s = 0; s < SNAP_TRY; s++) {
        for (int i = 0; i <= s && i < na; i++) {
            int j = s - i;
            if (j >= nb) continue;
            int rc = router_plan(r, A[i], B[j], out);
            if (rc == 0) {
                if (fa) *fa = A[i];
                if (fb) *fb = B[j];
                if (da) *da = DA[i];
                if (db) *db = DB[j];
                return 0;
            }
            route_result_free(out);
            if (rc == -1) continue;     /* 不连通：换候选 */
            return -3;                  /* 超限 */
        }
    }
    return -2;
}
```

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

以下五个辅助函数逐字摘自 `src/route/maneuver.c`（行号见各块标题），与源码可编译一致：

**`bearing_deg`**（方位角，正北为 0，顺时针增大）：`src/route/maneuver.c:33-44`

```c
static double bearing_deg(int32_t la0_e7, int32_t lo0_e7,
                          int32_t la1_e7, int32_t lo1_e7)
{
    double p1 = la0_e7 * 1e-7 * M_PI / 180.0;
    double p2 = la1_e7 * 1e-7 * M_PI / 180.0;
    double dlam = (lo1_e7 - lo0_e7) * 1e-7 * M_PI / 180.0;
    double y = sin(dlam) * cos(p2);
    double x = cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dlam);
    double b = atan2(y, x) * 180.0 / M_PI;
    if (b < 0) b += 360.0;
    return b;
}
```

**`walk`**（从点 i 沿 dir 方向累积到 `LOOK_M` 米，返回落点下标）：`src/route/maneuver.c:56-66`

```c
static uint32_t walk(const RouteResult *rr, uint32_t i, int dir)
{
    double acc = 0;
    uint32_t j = i;
    if (dir > 0) {
        while (acc < LOOK_M && j + 1 < rr->n) { acc += seg_len(rr, j); j++; }
    } else {
        while (acc < LOOK_M && j > 0) { acc += seg_len(rr, j - 1); j--; }
    }
    return j;
}
```

**`road_name_of`**（段 `s`（`pts[s]→pts[s+1]`）的路名，edge 属于 `pts[s+1].prov`）：`src/route/maneuver.c:69-88`

```c
static void road_name_of(const RouteResult *rr, const MapSet *ms,
                         uint32_t s, char *buf, size_t cap, uint8_t *cls_out)
{
    buf[0] = '\0';
    if (cls_out) *cls_out = 0xFF;
    if (s + 1 >= rr->n) return;
    uint32_t prov = rr->pts[s + 1].prov;
    uint32_t eidx = rr->edge_idx[s];
    if (!ms || prov >= (uint32_t)ms->n || eidx == REDGE_SEAM) return;
    mapset_ensure_loaded((MapSet *)ms, (int)prov);   /* 该省可能已被 set_view 卸载 */
    const MapSrc *src = &ms->srcs[prov];
    if ((uint64_t)eidx >= src->edges.hdr.rec_count) return;
    const uint8_t *er = src->edges.data + (uint64_t)eidx * NM_EDGE_BYTES;
    if (cls_out) *cls_out = er[EF_CLASS];
    uint32_t nid = nm_get_u32(er + EF_NAME_ID);
    const NamePool *np = &src->np;
    if (!nid || !np->ready || nid > np->n) return;
    const char *s2 = np->blob + np->off[nid];
    snprintf(buf, cap, "%s", s2);
}
```

**`roundabout_exit_no`**（环岛出口序号 = 环上各路口非环岛出边计数之和）：`src/route/maneuver.c:139-149`

```c
static int roundabout_exit_no(const RouteResult *rr, const MapSet *ms,
                              uint32_t entry)
{
    int exits = 0;
    uint32_t k = entry;
    while (k + 1 < rr->n && (seg_flags(rr, k) & EF_ROUNDABOUT)) {
        k++;                                   /* 走到环上的下一个路口 */
        exits += count_side_exits(ms, rr, k);   /* 含我们最后驶出的那条 */
    }
    return exits;                              /* 0 = 数据不全，文案退化为"进入环岛" */
}
```

**`maneuver_text`**（把一条转向指令渲染成中文文案）：`src/route/maneuver.c:182-210`

```c
void maneuver_text(const Maneuver *m, char *buf, size_t n)
{
    char d[32];
    fmt_distance(m->dist_m, d, sizeof(d));
    if (m->type == MAN_START) {
        if (m->road[0]) snprintf(buf, n, "沿%s出发", m->road);
        else            snprintf(buf, n, "从起点出发");
        return;
    }
    if (m->type == MAN_ARRIVE) {
        snprintf(buf, n, "行驶%s后到达目的地", d);
        return;
    }
    if (m->type == MAN_STRAIGHT) {
        if (m->road[0]) snprintf(buf, n, "沿%s直行 %s", m->road, d);
        else            snprintf(buf, n, "直行%s", d);
        return;
    }
    if (m->type == MAN_ROUNDABOUT) {
        if (m->exit_no > 0)
            snprintf(buf, n, "%s后进入环岛，第%d个出口驶出", d, m->exit_no);
        else
            snprintf(buf, n, "%s后进入环岛", d);
        return;
    }
    const char *k = maneuver_kind_text(m);
    if (m->road[0]) snprintf(buf, n, "%s后%s进入%s", d, k, m->road);
    else            snprintf(buf, n, "%s后%s", d, k);
}
```

## 四、与其他功能之间的联系

- **上游（L4 图集 / L5 建图 / L6 瓦片键）**：L5 建图产出 `nodes.bin`/`edges.bin`（CSR 出边）是 L10 路由输入；L6 的 `coord_key_e7` 是状态主键，`mapset_query_tile`/`members_of` 按 Z14 瓦片跨省查同坐标。
- **下游 · L8 渲染管线**：`RouteResult` 经 L8 `navdraw` 绘制路线折线（按 `pts[]`/`seg_m[]`）。
- **下游 · L9 搜索**：**L9 搜索结果（地名/POI 坐标）即 L10 的起终点**——搜索出点 → `router_route_ll` 吸附算路。
- **下游 · L11 导航语音**：`RouteResult` 直接喂 `maneuver_build`（本课 §9）与 `navsim_start`；`RouteOpts` 指名初始化约定被 L11/L12 所有调用点遵守。
- **下游 · L13 架构门禁**：`lm_admissible`（regress.sh）直接验收地标下界；`restr_ab` / `heap_selftest` 是负向对照工具。

## 五、演示、练习与验收

- **演示**：`./build_rel/route_cli --graph=products/graph/beijing --from=39.90,116.40 --to=39.99,116.30 --geojson=route.json`；`./build_rel/route_cli --from=... --to=... --maneuvers` 看 `maneuver_build` 输出；`./build_rel/heap_selftest --expect-fail`（负向对照）；`./build_rel/revbuild_cli --verify`；`./build_rel/restr_ab`；`./build_rel/lmdist_cli`（一对多 Dijkstra 下界）。
- **练习**：把 `ROUTE_HEUR_W_DEFAULT`（route.h:137）改成 1.0（纯 Dijkstra）和 2.0，对比 `route_cli` 的 `expanded`/`elapsed_ms`；用 `NAVIMAP_LM=0` 关表后 `--exact` 应保证最优。
- **验收（读源码 / 跑命令）**：
  1. 能画出"双向 A* 一次展开"的数据流（取堆顶 → `members_of` → 松弛出/入边 → 查转向限制 → `seam_blocked` 相遇判定），并澄清"非 CH、ALT 非严格下界"。
  2. `coord_key_e7`（`coordkey.h:26`）让一个物理路口只展开一次——对比 `(省,节点)` 主键初版的 111 万状态/97 秒问题。
  3. `heap_selftest` 随机序列下每次 pop 取当前最小值；`restr_ab` 验证转向限制使里程变化只在被测因素上。
  4. `lm_table_match`（`lmtable.c:90`）保证重抽 PBF 后旧 `lm.bin` 被丢弃回退直线启发式，避免悄悄次优。
