#include "route/route.h"      /* LmDist / LM_UNREACHABLE 等公共声明 */
#include "route/route_priv.h" /* 完整 Router / SewEnt / SewSlot / members_of */

#include "route/cost.h"       /* edge_cost / node_coord */
#include "route/restr.h"      /* restr_blocked* */
#include "route/sew.h"        /* sew_get / sew_build / tile_prov_count */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 地标距离：一对多 Dijkstra（原 route.c 尾段，整段迁来，行为未改）。
 * lm_dijkstra / lm_dist_free / lm_cost_at 是 route.h 里的公共接口
 * （tools/lmbuild_cli.c、tools/lmdist_cli.c 调用），故不加 static；
 * lm_prov_of 与 LM_SCALE / LM_INF 只在本文件用，保持私有。
 * 依赖的 members_of 在 route.c 定义、经 route_priv.h 导出。 */

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
