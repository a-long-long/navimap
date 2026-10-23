#include "route/route.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "map/binfmt.h"
#include "map/coordkey.h"
#include "map/road_tags.h"
#include "map/tileidx.h"
#include "route/pqueue.h"
#include "route/revidx.h"
#include "route/lmtable.h"
#include "utils/geo.h"
#include "utils/plat.h"

/* ---------------------------------------------------------------------------
 * 跨省 A* 路由（状态主键 = 坐标，见 route.h 顶部说明）
 *
 * 双向版本：正向走出边（CSR），反向走入边（按省惰性构建的反向 CSR）。
 * 两条搜索各自一张状态表 + 一个堆，按"堆顶 f 小的那侧先走"交替推进。
 * ------------------------------------------------------------------------- */

#define INF_G          1e30
/* 展开上限（防跑飞）。★ 800 万是在**自适应 w 之前**定的，实测会把真实路线剪断：
   郑州→大连 要 11334703 展开（15.6 s / 2.29 GB）⇒ 直接 ROUTE-FAIL；同类失败的
   还有西安/南阳/太原 → 大连，全是"从远处到半岛端点"（启发式失效那一类）。
   ⇒ 抬到 1200 万（PLAN.md §48.3）。
   ★ 再往上要小心：峰值内存随展开数近似线性（~210~290 B/展开），本机可用
     3.59 GB 且 zram swap 常满 ⇒ 别超过 1600 万。 */
#define DEFAULT_MAX_EXPAND 12000000u
/* 瓦片缝合缓存槽数（2 的幂，哈希 + 线性探测）。
   两次定案，**第二次推翻了第一次**（完整过程见 PLAN.md §39）：
   - 256 → 4096（§36）：京→郑命中率 49% → 97.8%，省 ~2.2 s。
   - 4096 → 16384（§39）：★ §36 那句"再往上没用（16384 净 0）"是
     **路线选择偏差** —— 它只在京→郑上测过，而京→郑碰到的瓦片数刚好
     4096 就够；换到青岛→西安（跨 4 省，瓦片数多得多），members_of 占
     24.4%（672 ms），扩到 16384 实测省 **600~900 ms（22~32%）**。
   - 65536 没有额外收益（青西 2245 vs 16384 的 2278，在噪声内；
     京郑反而慢 ~30 ms）。262144 同理。
   ⇒ 16384（槽数组 655 KB，可忽略）。
   ★ 教训：缓存容量的最优值依赖"工作集大小"，必须拿**工作集最大**的那条
     路线测，拿最短/最简单的路线测出来的结论会偏向小容量。 */
#define SEW_SLOTS      16384
#define H_SPEED_KMH    130.0     /* A* 启发式分母；需 >= 全图最高限速才 admissible */
#define MAX_MEMBER     64        /* 同一坐标最多取多少个跨省成员 */
/* 吸附时保留几个候选节点。>1 是为了"最近那个是死节点"时换下一个重试
   （PLAN.md §51.1）：不连通的判定是毫秒级，重试几乎不要钱；
   4 个足够（实测第 1~2 个候选就救回了全部失败样本）。 */
#define SNAP_TRY       4

#define DIR_FWD 0
#define DIR_BWD 1

/* NAVIMAP_RESTR_DEBUG 以前是在**每条边**上现调 getenv()：本机 environ 有 178 项，
   一次 getenv 实测 135 ns，京→郑一帧要调 318 万次 ⇒ 约 430 ms（占 18%）。
   只在首次调用时查一次并缓存。 */
static int restr_debug(void)
{
    static int v = -1;
    if (v < 0) v = getenv("NAVIMAP_RESTR_DEBUG") != NULL;
    return v;
}

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n);
    if (!q) {
        fprintf(stderr, "路由: 内存不足（需要 %zu 字节）\n", n);
        exit(2);
    }
    return q;
}

static uint64_t mix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

static uint64_t now_ms(void)
{
    return nm_now_ms();                /* 平台层：clock_gettime / QPC */
}

/* ---------------------------- 状态表 ---------------------------- */

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

static void st_init(StateTab *t)
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

static void st_free(StateTab *t)
{
    free(t->slot); free(t->ents);
    memset(t, 0, sizeof(*t));
}

static void st_rehash(StateTab *t)
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

static StEnt *st_intern(StateTab *t, uint64_t key)
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

static StEnt *st_find(const StateTab *t, uint64_t key)
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

/* --------------------- 反向入边索引 --------------------- */
/* 结构与落盘/构建见 route/revidx.h。这里只管"用"：优先 rev.bin，没有就建。
   只给反向搜索真正走进去的省建，副本省（被完全包含）从不建。 */
/* --------------------- 瓦片缝合缓存 / 限制表 类型 --------------------- */

typedef struct { uint64_t coord; uint32_t prov, node; } SewEnt;

typedef struct {
    int      used;      /* 0 = 空槽 */
    int      tx, ty;
    int      nprov;     /* 覆盖这片瓦片且有边的省数；-1 = 还没算 */
    int      full;      /* v 是否已构建 */
    SewEnt  *v;
    size_t   n;
} SewSlot;

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

/* ------------------------ 瓦片缝合缓存 ------------------------ */

typedef struct {
    const MapSet *ms;
    SewEnt *v;
    size_t  n, cap;
} SewCtx;

static void sew_cb(const MapSrc *s, uint32_t from, uint32_t edge, void *ud)
{
    SewCtx *c = ud;
    (void)edge;
    int prov = (int)(s - c->ms->srcs);
    const uint8_t *nr = s->nodes.data + (uint64_t)from * NM_NODE_BYTES;
    if (c->n == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 1024;
        c->v = xrealloc(c->v, c->cap * sizeof(SewEnt));
    }
    SewEnt e;
    e.coord = coord_key_e7(nm_get_i32(nr + NF_LAT), nm_get_i32(nr + NF_LON));
    e.prov = (uint32_t)prov;
    e.node = from;
    c->v[c->n++] = e;
}

static int cmp_pn(const void *a, const void *b)
{
    const SewEnt *x = a, *y = b;
    if (x->prov != y->prov) return x->prov < y->prov ? -1 : 1;
    if (x->node != y->node) return x->node < y->node ? -1 : 1;
    return 0;
}

static int cmp_coord_prov(const void *a, const void *b)
{
    const SewEnt *x = a, *y = b;
    if (x->coord != y->coord) return x->coord < y->coord ? -1 : 1;
    if (x->prov != y->prov) return x->prov < y->prov ? -1 : 1;
    return 0;
}

/* 只看瓦片目录，不遍历边：判断这片瓦片被几个省覆盖。
 * 只有 ≥2 个省时才有缝合的可能，省内部的瓦片连索引都不用建——
 * 这是搜索热路径上最大的一笔开销，能省就省。 */
static int tile_prov_count(const MapSet *ms, int tx, int ty)
{
    /* 统计"去重后"的省数：被完全包含的副本省映射到它的宿主，同一宿主只算一次。
       北京城区那片瓦片 beijing 和 hebei 都有数据，但 beijing ⊂ hebei，
       有效省数 = 1 ⇒ 根本没有缝合的必要，连索引都不用建。 */
    int seen[32], ns = 0, c = 0;
    for (int i = 0; i < ms->n; i++) {
        const MapSrc *s = &ms->srcs[i];
        if (!s->has_tiles) continue;
        const uint8_t *td = s->tiles.data;
        uint32_t minx = nm_get_u32(td + TF_EDGE_OFF);
        uint32_t miny = nm_get_u32(td + TF_EDGE_CNT);
        uint32_t nx   = nm_get_u32(td + NM_TILE_REC_BYTES + TF_EDGE_OFF);
        uint32_t ny   = nm_get_u32(td + NM_TILE_REC_BYTES + TF_EDGE_CNT);
        if (tx < (int)minx || ty < (int)miny ||
            tx >= (int)(minx + nx) || ty >= (int)(miny + ny))
            continue;
        if ((uint64_t)nx * ny > s->tiles.hdr.rec_count - 2) continue;
        size_t t = ((size_t)(ty - miny) * nx) + (tx - minx);
        const uint8_t *dir = td + (2 + t) * NM_TILE_REC_BYTES;
        if (nm_get_u32(dir + TF_EDGE_CNT) == 0) continue;
        int root = i;
        while (root >= 0 && ms->srcs[root].dup_of >= 0) root = ms->srcs[root].dup_of;
        int dup = 0;
        for (int k = 0; k < ns; k++) if (seen[k] == root) { dup = 1; break; }
        if (!dup && ns < 32) seen[ns++] = root;
        if (++c >= 2 && ns >= 2) return ns;
    }
    return ns;
}

static SewSlot *sew_get(Router *r, int tx, int ty)
{
    size_t mask = SEW_SLOTS - 1;
    size_t h = ((size_t)tx * 73856093u + (size_t)ty * 19349663u) & mask;
    for (size_t p = 0; p < 8; p++) {
        size_t j = (h + p) & mask;
        SewSlot *s = &r->sew[j];
        if (s->used && s->tx == tx && s->ty == ty) return s;
        if (!s->used) {
            s->used = 1; s->tx = tx; s->ty = ty;
            s->nprov = -1; s->full = 0; s->v = NULL; s->n = 0;
            return s;
        }
    }
    /* 探测窗口全被占：直接淘汰起点那个槽。局部性好，代价是一次重建。 */
    SewSlot *s = &r->sew[h];
    free(s->v);
    s->used = 1; s->tx = tx; s->ty = ty;
    s->nprov = -1; s->full = 0; s->v = NULL; s->n = 0;
    return s;
}

/* 需要完整成员列表时才调（起终点吸附、多省瓦片的缝合） */
static void sew_build(Router *r, SewSlot *s)
{
    if (s->full) return;
    SewCtx c;
    memset(&c, 0, sizeof(c));
    c.ms = r->ms;
    mapset_query_tile(r->ms, s->tx, s->ty, sew_cb, &c);
    if (c.n == 0) {
        free(c.v);
        s->full = 1; s->v = NULL; s->n = 0;
        return;
    }
    /* 同一节点会因有多条出边而重复出现，先按 (prov,node) 去重 */
    qsort(c.v, c.n, sizeof(SewEnt), cmp_pn);
    size_t w = 0;
    for (size_t i = 0; i < c.n; i++)
        if (i == 0 || cmp_pn(&c.v[i], &c.v[w - 1]) != 0) c.v[w++] = c.v[i];
    qsort(c.v, w, sizeof(SewEnt), cmp_coord_prov);
    s->v = c.v; s->n = w; s->full = 1;
}

/* ------------------------ 转向限制 ------------------------ */

/* 按 kv 升序排三/两个平行数组（w 只有几百条，插入排序足够） */
static void sort_kv(uint64_t *kv, uint32_t *a, uint32_t *b, size_t w)
{
    for (size_t i = 1; i < w; i++) {
        uint64_t k = kv[i];
        uint32_t x = a ? a[i] : 0, y = b ? b[i] : 0;
        size_t j = i;
        while (j > 0 && kv[j - 1] > k) {
            kv[j] = kv[j - 1];
            if (a) a[j] = a[j - 1];
            if (b) b[j] = b[j - 1];
            j--;
        }
        kv[j] = k;
        if (a) a[j] = x;
        if (b) b[j] = y;
    }
}

static void restr_build(const MapSrc *s, RestrIdx *ri)
{
    ri->built = 1;
    if (!s->has_restr || s->restrictions.hdr.rec_count == 0) return;
    size_t n = (size_t)s->restrictions.hdr.rec_count;
    ri->kv  = malloc(n * sizeof(uint64_t));
    ri->to  = malloc(n * sizeof(uint32_t));
    ri->kv2 = malloc(n * sizeof(uint64_t));
    ri->to2 = malloc(n * sizeof(uint32_t));
    ri->prev2 = malloc(n * sizeof(uint32_t));
    ri->kv1 = malloc(n * sizeof(uint64_t));
    ri->to1 = malloc(n * sizeof(uint32_t));
    if (!ri->kv || !ri->to || !ri->kv2 || !ri->to2 || !ri->prev2 ||
        !ri->kv1 || !ri->to1) {
        fprintf(stderr, "路由: 限制表分配失败\n"); exit(2);
    }
    const uint8_t *d = s->restrictions.data;
    size_t w = 0, w2 = 0, w1 = 0;
    for (size_t i = 0; i < n; i++) {
        const uint8_t *rec = d + i * NM_RESTR_BYTES;
        uint64_t key = ((uint64_t)nm_get_u32(rec + RF_VIA_NODE) << 32) |
                        nm_get_u32(rec + RF_FROM_EDGE);
        if (rec[RF_KIND] == 2) {
            ri->kv2[w2]   = key;
            ri->to2[w2]   = nm_get_u32(rec + RF_TO_EDGE);
            ri->prev2[w2] = nm_get_u32(rec + RF_PREV_EDGE);
            w2++;
        } else if (rec[RF_KIND] == 0) {
            ri->kv[w] = key;
            ri->to[w] = nm_get_u32(rec + RF_TO_EDGE);
            w++;
        } else if (rec[RF_KIND] == 1) {
            /* kind=1 的 to_edge 是"允许"的那一支；一个 only_* 关系有多个 to
               成员就有多条记录，它们共享同一个 key，合起来才是允许集。 */
            ri->kv1[w1] = key;
            ri->to1[w1] = nm_get_u32(rec + RF_TO_EDGE);
            w1++;
        }
    }
    ri->n = w; ri->n2 = w2; ri->n1 = w1;
    sort_kv(ri->kv,  ri->to,  NULL,   w);
    sort_kv(ri->kv2, ri->to2, ri->prev2, w2);
    sort_kv(ri->kv1, ri->to1, NULL,   w1);
}

/* 在 kv 升序表里找 key 对应的区间起点 */
static size_t kv_lower(const uint64_t *kv, size_t n, uint64_t key)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (kv[mid] < key) lo = mid + 1; else hi = mid;
    }
    return lo;
}

static int restr_blocked(Router *r, uint32_t prov, uint32_t via_node,
                         uint32_t from_edge, uint32_t to_edge)
{
    if (from_edge == REDGE_SEAM || to_edge == REDGE_SEAM) return 0;
    if (prov >= (uint32_t)r->ms->n) return 0;
    RestrIdx *ri = &r->ri[prov];
    if (!ri->built) restr_build(&r->ms->srcs[prov], ri);
    if (ri->n == 0) return 0;
    uint64_t want = ((uint64_t)via_node << 32) | from_edge;
    size_t i = kv_lower(ri->kv, ri->n, want);
    for (; i < ri->n && ri->kv[i] == want; i++)
        if (ri->to[i] == to_edge) return 1;
    return 0;
}

/* kind=1（only_*）：语义跟上面两张表**相反**。
   OSM 的 only_* 是"从 from 进来只许往这几支拐"，所以文件里存的是**允许集**
   （一个关系有多个 to 成员就有多条记录，共享同一个 key）。于是查表反过来：
     - 这个 {via, from} 没有 only_* 记录 ⇒ 不限制（返回 0）
     - 有 ⇒ to 不在允许集里就禁（返回 1）
   ★ 与 kind=0 正交：同一个路口可以同时有 no_* 和 only_*，两个都要查。
   ★ 掉头回 from_edge 本身也算"不在允许集里"——除非 OSM 显式列了它，
     与语义一致（only_straight_on 就是不许掉头）。
   ★ 反向搜索不需要"保守版"：only_* 是点限制，只依赖 (via, from, to)，
     反向松弛时这三个都已知，跟 kind=0 一样对称。 */
static int restr_blocked_only(Router *r, uint32_t prov, uint32_t via_node,
                              uint32_t from_edge, uint32_t to_edge)
{
    if (r->opt.no_restr1) return 0;
    if (from_edge == REDGE_SEAM || to_edge == REDGE_SEAM) return 0;
    if (prov >= (uint32_t)r->ms->n) return 0;
    RestrIdx *ri = &r->ri[prov];
    if (!ri->built) restr_build(&r->ms->srcs[prov], ri);
    if (ri->n1 == 0) return 0;
    uint64_t want = ((uint64_t)via_node << 32) | from_edge;
    size_t i = kv_lower(ri->kv1, ri->n1, want);
    if (i >= ri->n1 || ri->kv1[i] != want) return 0;   /* 这个来路没有 only_* */
    for (; i < ri->n1 && ri->kv1[i] == want; i++)
        if (ri->to1[i] == to_edge) return 0;           /* 在允许集里 */
    return 1;
}

/* via=way 两段限制：三段序列 prev → from → to 被禁。
   prev_edge 是"再往前一条边"，REDGE_SEAM 表示没有（起点附近）或跨了省
   （边下标是省内的，跨省无从比起），这两种情况都放行——宁可漏禁也不误禁。 */
static int restr_blocked2(Router *r, uint32_t prov, uint32_t via_node,
                          uint32_t prev_edge, uint32_t from_edge,
                          uint32_t to_edge)
{
    if (r->opt.no_restr2) return 0;
    if (from_edge == REDGE_SEAM || to_edge == REDGE_SEAM) return 0;
    if (prev_edge == REDGE_SEAM) return 0;
    if (prov >= (uint32_t)r->ms->n) return 0;
    RestrIdx *ri = &r->ri[prov];
    if (!ri->built) restr_build(&r->ms->srcs[prov], ri);
    if (ri->n2 == 0) return 0;
    uint64_t want = ((uint64_t)via_node << 32) | from_edge;
    size_t i = kv_lower(ri->kv2, ri->n2, want);
    for (; i < ri->n2 && ri->kv2[i] == want; i++)
        if (ri->to2[i] == to_edge && ri->prev2[i] == prev_edge) return 1;
    if (restr_debug())
        fprintf(stderr, "DBG2 miss via=%u prev=%u from=%u to=%u (n2=%zu)\n",
                via_node, prev_edge, from_edge, to_edge, ri->n2);
    return 0;
}

/* 反向搜索用的保守版：反向是从终点往起点推，站在 B 时"到达 A 的那条边"
   （三段序列里的 prev）在搜索方向上还没确定，无从精确判定。
   所以只要 (via, from, to) 命中就禁——会误伤"从别的路合法进入"的情形，
   但方向是保守的：不会给出被禁转向。代价是反向可能绕一点远。 */
static int restr_blocked2_rev(Router *r, uint32_t prov, uint32_t via_node,
                              uint32_t from_edge, uint32_t to_edge)
{
    if (r->opt.no_restr2) return 0;
    if (from_edge == REDGE_SEAM || to_edge == REDGE_SEAM) return 0;
    if (prov >= (uint32_t)r->ms->n) return 0;
    RestrIdx *ri = &r->ri[prov];
    if (!ri->built) restr_build(&r->ms->srcs[prov], ri);
    if (ri->n2 == 0) return 0;
    uint64_t want = ((uint64_t)via_node << 32) | from_edge;
    size_t i = kv_lower(ri->kv2, ri->n2, want);
    for (; i < ri->n2 && ri->kv2[i] == want; i++)
        if (ri->to2[i] == to_edge) return 1;
    if (restr_debug())
        fprintf(stderr, "DBGr miss via=%u from=%u to=%u\n",
                via_node, from_edge, to_edge);
    return 0;
}

/* 双向搜索的接缝检查：正向段的最后一条边 e_f 与反向段的第一条边 e_b
   在相遇坐标 k 处构成一个完整转向（e_f 进入 k、e_b 离开 k）。
   两个方向各自展开时都看不到这个转向——正向在 k 停住没走 e_b，反向在 k
   停住没走 e_f——所以只能在这里补查。不合法的相遇点不接受，继续搜。 */
static int seam_blocked(Router *r, uint64_t k)
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

/* ------------------------ 代价与启发式 ------------------------ */

static uint8_t fallback_speed(uint8_t cls)
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

static double edge_cost(const Router *r, const uint8_t *er, double *meters)
{
    double m = (double)nm_get_u32(er + EF_DIST_CM) / 100.0;
    if (meters) *meters = m;
    if (!r->opt.mode_time) return m;
    uint8_t sp = er[EF_SPEED];
    if (sp == 0) sp = fallback_speed(er[EF_CLASS]);
    return m * 3.6 / (double)sp;
}

/* 大圆距离：nm_fast_dist_m 是等距投影近似，跨纬度大时会失真，
   一旦高估就破坏 admissible（A* 会给出次优解）。
   正向搜索朝终点估，反向搜索朝起点估（两边必须各自 admissible）。

   再与**地标下界**取 max（PLAN.md §48/§49）：
       d(u→t) ≥ d(L→t) − d(L→u) ⇒ 表给出的是米，按 H_SPEED_KMH 换成秒后
   与直线同口径比较。表是可选的：没加载/该侧不在网格内就当它不存在。
   ★ 单位必须一致：mode_time=0 时下面两条都是米，mode_time=1 时都是秒。 */
static double heur(const Router *r, int dir, int32_t lat_e7, int32_t lon_e7)
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

static uint64_t node_coord(const MapSrc *s, uint32_t node)
{
    const uint8_t *nr = s->nodes.data + (uint64_t)node * NM_NODE_BYTES;
    return coord_key_e7(nm_get_i32(nr + NF_LAT), nm_get_i32(nr + NF_LON));
}

/* ------------------------ Router 生命周期 ------------------------ */

/* 地标表开关：默认开；`NAVIMAP_LM=0` 关掉（A/B 用，也用于现场退回纯直线）。
   ★ 写成"只查一次"的函数：environ 在本机有 178 项，热路径上一次 getenv 135 ns。 */
static int lm_enabled(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("NAVIMAP_LM");
        v = !(e && e[0] == '0' && e[1] == '\0');
    }
    return v;
}

/* 【诊断用】在表自带的 penalty 上再加 N 米。默认 0 = 不改行为。
 * ★ 为什么留这个旋钮：§52.8 发现下界会轻微越界，判据是"开表 w=1.0 的预计分钟
 *   必须等于无表 w=1.0"。要定位"到底需要多大余量"就得能现场扫 penalty，
 *   否则每试一个值都要重建一次 30 地标的表（249 秒）。
 * ★ 只读一次（getenv 在热路径上一次 135 ns，见 §29）。 */
static uint32_t lm_penalty_add_m(void)
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

/* 图目录 = 省目录的上一级（products/graph/beijing → products/graph）。
   lm.bin 与 rev.bin 不同：**全图只有一份**，写在父目录里。 */
static int graph_root_dir(const MapSet *ms, char *out, size_t cap)
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

int router_nearest_k(Router *r, double lat, double lon, RNode *out, int k,
                     double *dist_m)
{
    if (k > SNAP_TRY) k = SNAP_TRY;
    if (k <= 0) return 0;
    int32_t la = nm_lat_to_i32(lat), lo = nm_lon_to_i32(lon);
    int tx, ty;
    nm_tile_of(lat, lon, &tx, &ty);

    int    nf = 0;
    double bd[SNAP_TRY];

    /* ★ nodes.bin 里六成节点没有任何出边（OSM 的地址点 / POI / 建筑中心，
       根本不进路网，连入边也没有）⇒ 吸附到它们必然"不连通"（PLAN.md §51.1）。
       所以只在"有出边"的候选里挑；整圈都没有才退回"最近节点"，
       免得把原本能找到节点的地方变成找不到。 */
    for (int want_edge = 1; want_edge >= 0; want_edge--) {
        for (int rad = 0; rad <= 3; rad++) {
            for (int dy = -rad; dy <= rad; dy++) {
                for (int dx = -rad; dx <= rad; dx++) {
                    if (rad > 0 && abs(dx) != rad && abs(dy) != rad) continue;
                    SewSlot *s = sew_get(r, tx + dx, ty + dy);
                    if (s->nprov < 0) s->nprov = tile_prov_count(r->ms, s->tx, s->ty);
                    if (s->nprov == 0) continue;
                    sew_build(r, s);
                    for (size_t i = 0; i < s->n; i++) {
                        const MapSrc *src = &r->ms->srcs[s->v[i].prov];
                        /* 吸附不要落在"被完全包含的副本"上：那份在路由里会被跳过，
                           拿它当起点就没有出边可走（实测北京→天津直接"不连通"）。 */
                        if (src->dup_of >= 0) continue;
                        const uint8_t *nr = src->nodes.data +
                                            (uint64_t)s->v[i].node * NM_NODE_BYTES;
                        if (want_edge && nm_get_u16(nr + NF_EDGE_COUNT) == 0)
                            continue;
                        double d = nm_fast_dist_m(la, lo,
                                                  nm_get_i32(nr + NF_LAT),
                                                  nm_get_i32(nr + NF_LON));
                        /* 插进有序的前 k 个（k 很小，直接冒泡） */
                        if (nf == k && d >= bd[nf - 1]) continue;
                        int at = (nf < k) ? nf : k - 1;
                        while (at > 0 && bd[at - 1] > d) {
                            bd[at] = bd[at - 1]; out[at] = out[at - 1];
                            if (dist_m) dist_m[at] = dist_m[at - 1];
                            at--;
                        }
                        bd[at] = d;
                        out[at].prov = s->v[i].prov;
                        out[at].node = s->v[i].node;
                        if (dist_m) dist_m[at] = d;
                        if (nf < k) nf++;
                    }
                }
            }
            if (nf > 0) return nf;      /* 这一圈够用了 */
        }
        if (nf > 0) return nf;
    }
    return nf;
}

/* 地标下界表有没有真的加载上。表是可选的（没有就退回直线启发式，不算错误），
   所以"表丢了"不会报错、只会让算路悄悄变慢 —— 验收脚本得有办法问出来。 */
int router_lm_loaded(const Router *r)
{
    return r && r->lm.loaded ? 1 : 0;
}

/* 表里有几个地标。★ 必须能验收：地标个数直接决定路线结果（§52），
   而"表少建了几个地标"不会报错、不会崩，只是**悄悄变慢变次优**。 */
int router_lm_nlm(const Router *r)
{
    return (r && r->lm.loaded) ? (int)r->lm.h.nlm : 0;
}

/* 实际生效的 w（兜底之后的），给诊断/验收打印用。 */
double router_heur_w(const Router *r)
{
    return r ? r->opt.heur_w : 0.0;
}

int router_nearest(Router *r, double lat, double lon, RNode *out, double *dist_m)
{
    RNode n[SNAP_TRY];
    double d[SNAP_TRY];
    int nret = router_nearest_k(r, lat, lon, n, SNAP_TRY, d);
    if (nret <= 0) return -1;
    *out = n[0];
    if (dist_m) *dist_m = d[0];
    return 0;
}

/* 经纬度 → 经纬度算路：**吸附到死节点时换下一个候选重试**。
 *
 * ★ 为什么需要：光"最近 + 有出边"还不够——石家庄吸附到的 #6204583 有出边，
 *   却整个悬在一个与主干断开的小连通块里（双向都不可达）。
 *   这种失败的判定是毫秒级的（小连通块立刻穷尽），所以重试几乎不要钱。
 * ★ 只在 rc == -1（不连通）时换候选；-2（超限）立刻放弃，不要越试越糟。
 * ★ 按 (i+j) 升序试：优先只换一端，且先换起点。
 *
 * 返回 0（out 有效，fa/fb/da/db 是实际采用的那对）；
 *      -1 = 附近无路网；-2 = 所有候选都不连通；-3 = 超出规模上限。 */
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

/* 取落在同一坐标上的所有 (省,节点) 成员。省内部的瓦片直接返回"到达时的
   那个节点"，连缝合索引都不建。 */
static size_t members_of(Router *r, uint64_t ck, uint32_t in_prov, uint32_t in_node,
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

/* 展开一个坐标。
   dir=0 正向：松弛所有成员的出边（到 ck2 的那条边记在 ck2 上）。
   dir=1 反向：松弛所有成员的入边（边 u→nd 记在 u 的状态上，prev = ck，
                语义是"从 u 出发沿这条边能走到 ck"）。 */
/* ★ e0 由调用方给出（主循环 pop 时已经 st_find 过一次，没必要再查一遍）；
   非 NULL 且未 settled 是调用方的不变式。传进来只在"使用前"有效——
   下面第一次 st_intern 之后 ents 可能 realloc，所以仍然是一次取完。 */
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

/* 用给定的 w 与展开上限跑**一遍**完整搜索。
 * 返回 0 / -1 = 有结论；-3 = 撞到 max_expand 但那只是自适应上限（调用方可以
 * 换个 w 重跑）；-2 = 真的超限或分配失败。
 * ★ 状态（两张表、两个堆、expanded、best_cut）在函数开头全部重置，
 *   所以可以被连续调用两次 —— 自适应 w 就靠这个（见下面的 router_plan）。 */
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
    /* 相遇点在两张表里的 ents 下标（回溯时用下标走 prev_idx 链，
       不再需要记坐标 key —— 见 StEnt.prev_idx 的注释）。 */
    uint32_t meet_idx[2] = {0, 0};
    int found = 0;

    while (r->pq[DIR_FWD].n || r->pq[DIR_BWD].n) {
        double ff = pq_peek_f(&r->pq[DIR_FWD]);
        double fb = pq_peek_f(&r->pq[DIR_BWD]);

        /* ★ 终止条件必须是 max(ff,fb) >= best，不能写成 ff + fb >= best。
           若存在更优路径（代价 C* < best），它必然经过某个接缝节点 n，且两侧
           f_F(n) <= C*、f_B(n) <= C*（h 可采纳）⇒ 两侧堆顶都 < best ⇒
           max < best，循环就不会停 —— 反过来说，max >= best 时确实没有更优解了。
           而 ff + fb >= best 允许一侧堆顶远小于 best（另一侧大），此时
           最优接缝可能还埋在小堆顶那一侧，实测北京城内就漏了：双向给出
           9.57 km，真正最优是 9.53 km。 */
        /* ★★ 下面"max(ff,fb) 保证最优"的证明，前提是 **h 可采纳**。
           加载了 lm.bin 之后这个前提**不成立**（§52.8：地标下界不是严格下界，
           实测 50 条里 2 条开表给出次优解）⇒ **开表时 --exact 只是更慢的搜索，
           不再保证最优**。要真值请 `NAVIMAP_LM=0`（那时 h 是直线，可采纳）。
           `regress.sh` 的 `lm_admissible` 项盯住它不要进一步恶化。 */
        /* 双向终止条件（★ 这里是"最优性 ↔ 速度"的开关）：
           - 默认 lb = ff + fb：两侧堆顶 f 之和。快，但**不保证最优**——
             允许一侧堆顶远小于 best，最优接缝可能还埋在那一侧没展开。
             实测北京城内：双向给 9.57 km，真正最优 9.53 km（差 0.4%）。
           - opt.exact 时 lb = max(ff, fb)：保证最优。理由：若存在更优解
             （代价 C* < best），它必经某个接缝 n，两侧 f_F(n) <= C*、
             f_B(n) <= C*（h 可采纳）⇒ 两侧堆顶都 < best ⇒ max < best，
             循环不会停。代价是两侧都要搜到 best，京→郑实测
             233 万节点/7.2 s → 787 万节点/54.8 s（比单向还慢）。
           所以默认要快、验收要准：--exact 只在对拍/验收时开。 */
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
        /* ★ 堆条目直接带 ents 下标，不再用 key 哈希查找：省掉 2~3 次随机访存
           （实测这一行原先占京→郑 13.8% / 164 ms，PLAN.md §41）。
           下标永远有效：ents 只 append，rehash/realloc 都不改下标。 */
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
                /* ★ 原来这里又 st_find 一次自己：ents 可能已 realloc，指针会悬空，
                   但**下标不会**，直接 ents[ei] 即可（与 §41 同一招）。 */
                StEnt *self = &r->st[dir].ents[ei];
                uint32_t oi = (uint32_t)(o - r->st[1 - dir].ents);
                double mu = self->g + o->g;
                /* ★ 接缝处的转向限制必须在这里补：
                   正向在 k 停住（入边 e_f），反向也在 k 停住（出边 e_b），
                   两边各自展开时都没有"e_f → e_b"这个完整转向可查，
                   直接拼接会把被禁的转向放过去（实测北京城内踩限制的路
                   线全是这么漏的，含单点限制）。这里把两段拼成一个转向
                   再查一次，不合法就不接受这个相遇点，继续搜。 */
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
            /* 只是自适应上限 ⇒ -3，让调用方换更重的 w 重跑；真的超限才报错 */
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

    /* ---------------- 回溯拼装 ----------------
     * keys[0..n-1] = 起点 → 终点；ap/ae/af[i] = 第 i 个点的"到达边"
     *   （所属省 / 边下标 / 该边起点节点）。起点没有到达边。
     * 正向半段（起点..相遇点）从 st[0] 取；反向半段从 st[1] 的 prev 链走，
     *   每个点 X 的到达边 = 它在反向表里"上一个点"那条边的记录
     *   （反向状态的 eidx 语义就是"从本点出发走向 prev 的那条边"）。
     * 路径点身份一律取 (到达边所属省, 到达边的 EF_TO)，天然自洽，
     *   接缝处也不会出现"省份和节点下标对不上"。 */
    uint32_t cap = 1024, n = 0;
    uint64_t *keys = malloc(cap * sizeof(uint64_t));
    uint32_t *ap = malloc(cap * sizeof(uint32_t));
    uint32_t *ae = malloc(cap * sizeof(uint32_t));
    uint32_t *af = malloc(cap * sizeof(uint32_t));
    uint32_t *tmp = malloc(cap * sizeof(uint32_t));   /* 存 ents 下标，不是 key */
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
    {   /* 反向半段：相遇点之后一直走到终点。用 use_bidir 守住——单向时
           st[DIR_BWD] 是空表，meet_idx[DIR_BWD] 没有意义。 */
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
            /* 段 i（pts[i]→pts[i+1]）的边归 pts[i+1] 所有——到达边的语义
               就是"前驱 → 我"，取反了会整体错位一格（踩过一次）。 */
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
            /* ★ 同 seg_ds 的道理：total_m 必须用**舍入后**的段长累计。
               段长存的是整数米，全程却在累加精确的 meters，两者就不相等了——
               导航里"剩余里程"是按 seg_m 累加算的投影里程，恒等式
               已走+剩余==全程 会凭空差出 Σ(舍入误差)（实测 152 段差 1.07 m，
               长路线恰好小于阈值才没暴露）。自洽比绝对精度重要。 */
            tm += out->seg_m[i];
            uint8_t sp = er[EF_SPEED];
            if (sp == 0) sp = fallback_speed(er[EF_CLASS]);
            double sec = meters * 3.6 / (double)sp;      /* km/h → m/s */
            out->seg_ds[i] = (uint32_t)(sec * 10.0 + 0.5);
            /* ★ total_s 用**舍入后**的段耗时累计：seg_ds 和 total_s 必须严格
               相等（route_check 会查，导航剩余 ETA 也按 seg_ds 累计）。
               各自四舍五入再求和的话，几千段能差出 ~2 s，自检必炸。 */
            ts += out->seg_ds[i] / 10.0;
        }
    }
    /* 跨省次数 = 相邻两段所属省份不同的次数 */
    for (uint32_t i = 1; i + 1 < n; i++)
        if (ap[i] != ap[i + 1]) out->n_seams++;

    out->total_m = tm;
    out->total_s = ts;
    out->ok = 1;
    free(keys); free(ap); free(ae); free(af);
    return 0;
}

void route_result_free(RouteResult *rr)
{
    if (!rr) return;
    free(rr->pts);
    free(rr->lat_e7);
    free(rr->lon_e7);
    free(rr->edge_idx);
    free(rr->from_node);
    free(rr->edge_cls);
    free(rr->edge_flags);
    free(rr->seg_m);
    free(rr->seg_ds);
    memset(rr, 0, sizeof(*rr));
}

int route_check(Router *r, const RouteResult *rr)
{
    int fail = 0;
    double sum_ds = 0;
    if (!rr->ok || rr->n < 2) {
        printf("ASSERT-FAIL 路径为空\n");
        return 1;
    }
    for (uint32_t i = 0; i + 1 < rr->n; i++) {
        RNode a = rr->pts[i], b = rr->pts[i + 1];
        if (a.prov >= (uint32_t)r->ms->n || b.prov >= (uint32_t)r->ms->n) {
            printf("ASSERT-FAIL 段 %u 省下标越界\n", i);
            fail = 1;
            continue;
        }
        if (rr->edge_idx[i] == REDGE_SEAM) {
            printf("ASSERT-FAIL 段 %u 是起点段（不应出现）\n", i);
            fail = 1;
            continue;
        }
        /* 边的省份 = 终点的省份（prev_edge 就是从 pts[i] 走到 pts[i+1] 那条） */
        const MapSrc *s = &r->ms->srcs[b.prov];
        uint64_t ei = rr->edge_idx[i];
        if (ei >= s->edges.hdr.rec_count) {
            printf("ASSERT-FAIL 段 %u 边下标越界\n", i);
            fail = 1;
            continue;
        }
        const uint8_t *er = s->edges.data + ei * NM_EDGE_BYTES;
        if (nm_get_u32(er + EF_TO) != b.node) {
            printf("ASSERT-FAIL 段 %u 边终点 %u != 路径点 %u\n",
                   i, nm_get_u32(er + EF_TO), b.node);
            fail = 1;
            continue;
        }
        if (rr->from_node[i] >= s->nodes.hdr.rec_count) {
            printf("ASSERT-FAIL 段 %u 边起点越界\n", i);
            fail = 1;
            continue;
        }
        /* 边起点与本段起点必须落在同一个坐标上（可能不是同一个省） */
        uint64_t fc = node_coord(s, rr->from_node[i]);
        if (coord_key_lat(fc) != rr->lat_e7[i] || coord_key_lon(fc) != rr->lon_e7[i]) {
            printf("ASSERT-FAIL 段 %u 边起点坐标 (%d,%d) != 路径点 (%d,%d)\n",
                   i, coord_key_lat(fc), coord_key_lon(fc),
                   rr->lat_e7[i], rr->lon_e7[i]);
            fail = 1;
        }
        /* 边终点坐标 == 下一个路径点坐标 */
        uint64_t tc = node_coord(s, b.node);
        if (coord_key_lat(tc) != rr->lat_e7[i + 1] ||
            coord_key_lon(tc) != rr->lon_e7[i + 1]) {
            printf("ASSERT-FAIL 段 %u 边终点坐标与路径点不符\n", i);
            fail = 1;
        }
        uint32_t dcm = nm_get_u32(er + EF_DIST_CM);
        if (llabs((long long)(dcm / 100) - (long long)rr->seg_m[i]) > 1) {
            printf("ASSERT-FAIL 段 %u 长度 %u m != 记录 %u cm\n",
                   i, rr->seg_m[i], dcm);
            fail = 1;
        }
        /* 每段耗时要与该段限速自洽：否则导航的剩余 ETA 会凭空多出/少掉时间 */
        uint8_t sp = er[EF_SPEED];
        if (sp == 0) sp = fallback_speed(er[EF_CLASS]);
        double want = (dcm / 100.0) * 3.6 / (double)sp * 10.0;
        if (fabs((double)rr->seg_ds[i] - want) > 1.5) {
            printf("ASSERT-FAIL 段 %u 耗时 %.1f s != 限速 %u 推算 %.1f s\n",
                   i, rr->seg_ds[i] / 10.0, sp, want / 10.0);
            fail = 1;
        }
        sum_ds += rr->seg_ds[i];
    }
    /* Σ seg_ds 必须等于 total_s（导航 ETA 与路线总时长是同一个口径） */
    if (rr->n > 1 && fabs(sum_ds / 10.0 - rr->total_s) > 0.5) {
        printf("ASSERT-FAIL 分段耗时和 %.1f s != 总时长 %.1f s\n",
               sum_ds / 10.0, rr->total_s);
        fail = 1;
    }

    /* 路径不得踩到任何转向限制。
       单点限制（kind=0）：在 pts[i] 处 入边 edge_idx[i-1] → 出边 edge_idx[i]。
       via=way 两段（kind=2）：连续三段 e0 → e1 → e2 的序列被禁。
       两者都要求三段/两段同省——边的下标是省内的，跨了省无从比起。 */
    uint32_t n_hit1 = 0, n_hit2 = 0;
    for (uint32_t i = 1; i + 1 < rr->n; i++) {
        uint32_t e_in = rr->edge_idx[i - 1], e_out = rr->edge_idx[i];
        if (e_in == REDGE_SEAM || e_out == REDGE_SEAM) continue;
        if (rr->pts[i - 1].prov != rr->pts[i].prov) continue;
        uint32_t pv = rr->pts[i].prov;
        if (pv >= (uint32_t)r->ms->n) continue;
        if (restr_blocked(r, pv, rr->pts[i].node, e_in, e_out)) {
            printf("ASSERT-FAIL 段 %u 踩了单点禁止转向（via 节点 %u）\n",
                   i, rr->pts[i].node);
            fail = 1; n_hit1++;
        }
        if (restr_blocked_only(r, pv, rr->pts[i].node, e_in, e_out)) {
            printf("ASSERT-FAIL 段 %u 踩了 only_* 限制（via 节点 %u，"
                   "出边 %u 不在允许集里）\n", i, rr->pts[i].node, e_out);
            fail = 1; n_hit1++;
        }
        if (i + 2 < rr->n) {
            uint32_t e0 = e_in, e1 = e_out, e2 = rr->edge_idx[i + 1];
            /* ★ 转向点是 e1 的终点 = pts[i+1]，不是 pts[i]。
               序列 pts[i-1] --e0--> pts[i] --e1--> pts[i+1] --e2--> pts[i+2]，
               限制在"从 pts[i+1] 拐出去"这一刻生效，与搜索时展开 ck=pts[i+1]
               一致。第一版写成 pts[i]，自检永远查不到，于是"限制没生效"
               看起来像自检通过。 */
            if (e2 != REDGE_SEAM && rr->pts[i + 1].prov == pv) {
                if (restr_blocked2(r, pv, rr->pts[i + 1].node, e0, e1, e2)) {
                    printf("ASSERT-FAIL 段 %u 踩了 via=way 两段禁止序列"
                           "（%u → %u → %u）\n", i, e0, e1, e2);
                    fail = 1; n_hit2++;
                }
            }
        }
    }
    if (n_hit1 || n_hit2)
        printf("              踩限制：单点 %u 处、两段 %u 处\n", n_hit1, n_hit2);

    printf("路径自检      %s（%u 段，跨省次数 %u）\n",
           fail ? "失败" : "通过", rr->n - 1, rr->n_seams);
    return fail;
}

/* ---------------------------------------------------------------------------
 * 自适应 w：先省着搜，搜不动才加重（PLAN.md §45.8）
 *
 * 加权 A* 的 w 是"速度 ↔ 路径质量"的旋钮。全局调大 w 会让**所有**路线都变差，
 * 但真正慢的只有长途/高绕行系数那几条：随机样本上 500~600 万展开 / 4~10 s，
 * 而城内路线只要 5 千展开 / 8 ms —— 后者根本不需要加速。
 *
 * ⇒ 两遍：
 *   第 1 遍用基准 w，展开数封顶 adapt_try。**在顶以内结束的路线结果与基准 w
 *   完全一致**（今天的行为，一分钱质量都没多花）。
 *   只有撞顶（说明这条真的难走）才换 heur_w_hi 重跑 —— 那就值得付质量代价了。
 *
 * 代价：撞顶的路线白付第 1 遍（≤ adapt_try 那点展开，实测 20 万 ≈ 0.3 s）。
 * ★ 判据见 §45.8：别只看手挑的路线定 w，它们恰好是好走的那类 ——
 *   我第一版就是这么把收益说成 −90%，无偏随机样本上其实只有 −18%。
 * ------------------------------------------------------------------------- */
int router_plan(Router *r, RNode from, RNode to, RouteResult *out)
{
    uint64_t t0 = now_ms();
    uint64_t try_cap = r->opt.adapt_try;
    if (try_cap > r->max_expand) try_cap = r->max_expand;
    /* ★★ plan_search 会把 w 写进 r->opt.heur_w。**必须恢复**：Router 是可复用的
       （route_cli --repeat=N 就是同一个 Router 连算 N 次），不恢复的话第 2 次
       会把上一遍的重 w 当成基准 w，--repeat 的第 2 次起全被污染 —— 表现为
       "自适应看起来和纯 1.8 一样快"，其实是白赚的假数字（踩到过）。 */
    double w0 = r->opt.heur_w;

    /* ★★ 两遍只在"撞顶后能换个 w"时才成立。heur_w_hi == heur_w 时，撞顶之后
       要换的是**同一个 w**，那 ≤ adapt_try 次展开（默认 20 万 ≈ 200~300 ms）
       是纯浪费 —— 而且它不计进报告的展开数（第 2 遍会重置计数器），
       所以从数字上根本看不出来。
       ★ 有 lm.bin 之后 w 和 hi 都兜底成 1.3（关掉升级，PLAN.md §51.4），
       于是"两遍"从边角情况变成了**默认路径**：50 条里展开中位 21.5 万 > 20 万，
       一半以上的路线都在白扔这 20 万。⇒ 这种配置直接单遍搜到底。 */
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
    /* ★ 报总耗时（两遍加起来），别只报最后一遍 —— 否则自适应看起来"不要钱" */
    if (rc == 0 || rc == -1) out->elapsed_ms = now_ms() - t0;
    return rc;
}

size_t route_restr2_count(const Router *r, uint32_t prov)
{
    if (!r || prov >= (uint32_t)r->ms->n) return 0;
    return r->ri[prov].n2;
}

/* ---------------- 地标距离：一对多 Dijkstra ----------------
 *
 * 设计要点（为什么要单独写一套，见 route.h 的注释）：
 *   - 状态是 **(省, 节点)**，不是坐标键。省掉了坐标哈希表（全图要 1 GB 以上），
 *     代价是同一坐标的多省成员各有一份 entry。
 *   - 同一坐标的成员靠 **pop 时的零代价转移** 拉齐：pop 出 (p,n) 时先用
 *     members_of 把 D 摊给所有成员。语义上等价于"坐标即状态"，而且比 A* 更细
 *     （每个成员各自记到达边），所以只会更短、不会更长 —— 这正是下界要的方向。
 *   - 转向限制照抄 expand_dir 的三段式（kind0 / only_* / via=way），
 *     prev2 也存了 ⇒ 与 A* 用的是同一套"允许/禁止"判定。
 *   - 副本省（beijing ⊂ hebei）**完全不展开**：它的每个坐标在宿主省里都有，
 *     展开它只是把同一片路网走两遍。
 */
#define LM_SCALE 1000          /* 代价存成 uint32：cost × 1000 */
#define LM_INF   0xFFFFFFFFu

static uint32_t lm_prov_of(const LmDist *d, uint64_t flat)
{
    /* 只有 8 个省，线性扫比二分快（且 off 常在 cache 里） */
    for (uint32_t p = 0; p < d->nprov; p++)
        if (flat < d->off[p + 1]) return p;
    return d->nprov - 1;
}

int lm_dijkstra(Router *r, RNode from, LmDist *out,
                uint64_t *n_pop, uint64_t *n_push)
{
    if (!r || !out) return 1;
    const MapSet *ms = r->ms;
    if (from.prov >= (uint32_t)ms->n) return 1;
    const MapSrc *s0 = &ms->srcs[from.prov];
    if (from.node >= s0->nodes.hdr.rec_count) return 1;

    memset(out, 0, sizeof(*out));
    out->nprov = (uint32_t)ms->n;
    out->off = malloc((ms->n + 1) * sizeof(uint64_t));
    if (!out->off) return 2;
    uint64_t tot = 0;
    for (int i = 0; i < ms->n; i++) {
        out->off[i] = tot;
        tot += ms->srcs[i].nodes.hdr.rec_count;
    }
    out->off[ms->n] = tot;
    out->total = tot;
    if (tot == 0 || tot > 0xFFFFFFFFu) { free(out->off); return 2; }

    out->cost  = malloc(tot * sizeof(uint32_t));
    out->arr   = malloc(tot * sizeof(uint32_t));
    out->prev2 = malloc(tot * sizeof(uint32_t));
    out->done  = calloc((tot >> 3) + 1, 1);
    if (!out->cost || !out->arr || !out->prev2 || !out->done) {
        lm_dist_free(out);
        return 2;
    }
    memset(out->cost, 0xff, tot * sizeof(uint32_t));
    memset(out->arr,  0xff, tot * sizeof(uint32_t));
    memset(out->prev2, 0xff, tot * sizeof(uint32_t));

    PQueue pq;
    pq.a = NULL; pq.n = 0; pq.cap = 0;
    uint64_t npop = 0, npush = 0;

    uint64_t st = out->off[from.prov] + from.node;
    out->cost[st] = 0;
    pq_push(&pq, 0.0, st);
    npush++;

    double fv;
    while (pq.n) {
        uint64_t fi = (uint64_t)pq_pop(&pq, &fv);
        npop++;
        uint32_t pi = lm_prov_of(out, fi);
        if (out->done[fi >> 3] & (uint8_t)(1u << (fi & 7))) continue;
        out->done[fi >> 3] |= (uint8_t)(1u << (fi & 7));

        uint32_t D  = out->cost[fi];
        uint32_t E  = out->arr[fi];
        uint32_t P2 = out->prev2[fi];
        const MapSrc *s = &ms->srcs[pi];
        /* 副本省不展开（beijing/tianjin ⊂ hebei）：它的坐标在宿主省里都有 */
        if (s->dup_of >= 0) continue;
        uint32_t nd = (uint32_t)(fi - out->off[pi]);
        uint64_t ck = node_coord(s, nd);

        /* ① 零代价转移到同一坐标的其它省的成员 */
        SewEnt mem[MAX_MEMBER];
        size_t nmem = members_of(r, ck, pi, nd, mem);

        /* ② 展开出边（只对"到达时那个"省节点做；跨缝的成员由它们自己 pop 时展开） */
        uint32_t eb = nm_get_u32(s->nodes.data + (uint64_t)nd * NM_NODE_BYTES +
                                 NF_EDGE_BEGIN);
        uint32_t ec = nm_get_u16(s->nodes.data + (uint64_t)nd * NM_NODE_BYTES +
                                 NF_EDGE_COUNT);
        uint64_t ne = s->edges.hdr.rec_count;
        for (uint32_t k = 0; k < ec; k++) {
            uint64_t ei = (uint64_t)eb + k;
            if (ei >= ne) break;
            const uint8_t *er = s->edges.data + ei * NM_EDGE_BYTES;
            uint16_t fl = nm_get_u16(er + EF_FLAGS);
            if (fl & EF_NO_ACCESS) continue;
            if (r->opt.avoid_toll && (fl & EF_TOLL)) continue;
            if (r->opt.avoid_unpaved && (fl & EF_UNPAVED)) continue;
            /* ★ 只有 opt.lm_no_restr 时整体跳过（建表诊断用，§52.8）。
               正常建表**必须带限制**：不带的话距离偏小，下界偏大 ⇒ 越界。 */
            if (E != REDGE_SEAM && !r->opt.lm_no_restr) {
                if (restr_blocked(r, pi, nd, E, (uint32_t)ei)) continue;
                if (restr_blocked_only(r, pi, nd, E, (uint32_t)ei)) continue;
                if (P2 != REDGE_SEAM &&
                    restr_blocked2(r, pi, nd, P2, E, (uint32_t)ei)) continue;
            }
            double c = edge_cost(r, er, NULL);
            if (c <= 0) continue;
            uint32_t to = nm_get_u32(er + EF_TO);
            if ((uint64_t)to >= s->nodes.hdr.rec_count) continue;
            uint64_t nf = out->off[pi] + to;
            uint64_t nv = D + (uint64_t)(c * LM_SCALE + 0.5);
            if (nv >= LM_INF) nv = LM_INF - 1;
            if ((uint32_t)nv < out->cost[nf]) {
                out->cost[nf]  = (uint32_t)nv;
                out->arr[nf]   = (uint32_t)ei;
                out->prev2[nf] = (E != REDGE_SEAM) ? E : REDGE_SEAM;
                pq_push(&pq, (double)nv, nf);
                npush++;
            }
        }

        /* ① 的转移放在展开之后：先把本省能更新的更新掉，再把 D 摊给别的省 */
        for (size_t mi = 0; mi < nmem; mi++) {
            uint32_t p = mem[mi].prov, n = mem[mi].node;
            if (p >= (uint32_t)ms->n) continue;
            if (ms->srcs[p].dup_of >= 0) continue;
            if (p == pi && n == nd) continue;
            if (n >= ms->srcs[p].nodes.hdr.rec_count) continue;
            uint64_t mf = out->off[p] + n;
            if (D < out->cost[mf]) {
                /* ★ 跨缝之后"到达边"在目标省里没有意义 ⇒ 记 REDGE_SEAM，
                   与 expand_dir 的 `in_edge != REDGE_SEAM` 判定一致：
                   跨缝后的第一条出边不做限制检查。 */
                out->cost[mf]  = D;
                out->arr[mf]   = REDGE_SEAM;
                out->prev2[mf] = REDGE_SEAM;
                pq_push(&pq, (double)D, mf);
                npush++;
            }
        }
    }
    free(pq.a);
    if (n_pop)  *n_pop  = npop;
    if (n_push) *n_push = npush;
    return 0;
}

void lm_dist_free(LmDist *d)
{
    if (!d) return;
    free(d->off); free(d->cost); free(d->arr); free(d->prev2); free(d->done);
    memset(d, 0, sizeof(*d));
}

uint32_t lm_cost_at(const LmDist *d, uint32_t prov, uint32_t node)
{
    if (!d || !d->cost || prov >= d->nprov) return LM_UNREACHABLE;
    uint64_t f = d->off[prov] + node;
    if (node >= d->off[prov + 1] - d->off[prov]) return LM_UNREACHABLE;
    return d->cost[f];
}
