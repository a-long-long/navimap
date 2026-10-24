#include "route/restr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* restr_debug 定义在此（route.c 核心搜索也调用它，声明见 restr.h）。
 * 原 route.c 的 static 版整段迁来，行为未改。 */

/* NAVIMAP_RESTR_DEBUG 以前是在**每条边**上现调 getenv()：本机 environ 有 178 项，
   一次 getenv 实测 135 ns，京→郑一帧要调 318 万次 ⇒ 约 430 ms（占 18%）。
   只在首次调用时查一次并缓存。 */
int restr_debug(void)
{
    static int v = -1;
    if (v < 0) v = getenv("NAVIMAP_RESTR_DEBUG") != NULL;
    return v;
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

void restr_build(const MapSrc *s, RestrIdx *ri)
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
size_t kv_lower(const uint64_t *kv, size_t n, uint64_t key)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (kv[mid] < key) lo = mid + 1; else hi = mid;
    }
    return lo;
}

int restr_blocked(Router *r, uint32_t prov, uint32_t via_node,
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
int restr_blocked_only(Router *r, uint32_t prov, uint32_t via_node,
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
int restr_blocked2(Router *r, uint32_t prov, uint32_t via_node,
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
int restr_blocked2_rev(Router *r, uint32_t prov, uint32_t via_node,
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

