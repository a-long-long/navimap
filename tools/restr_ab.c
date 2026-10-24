/* restr_ab —— 转向限制的定向 A/B 对照验收。
 *
 * 存在理由：限制只在极少数路口生效。随机路线几乎不可能恰好撞上——via=way
 * 那批实测「12 条随机路线、2000 多段，一次都没触发」，only_* 这批（8 省 381 处）
 * 也一样。所以这里一律做**定向**验收：走到每条限制所在的路口上去测。
 *
 * 判据是双向的，缺一边都可能自欺：
 *   1. 拿同样起终点算一条"关掉限制"的路线 rB。限制若真在起作用，rB 里应当
 *      出现被禁的那次转向（说明最短路本来就想这么走）；
 *   2. 再算"开着限制"的 rA，它必须不再包含那次转向；
 *   3. rA 还必须过 route_check（一条限制都不踩）。
 *   只做 2、3 是自欺：限制压根没生效时它们同样是绿的。
 *
 * 目前覆盖两类：
 *   via=way 两段（kind=2）：P0 --e_from--> A --e_via--> B --e_to--> C 被禁
 *   only_*（kind=1）      ：从 from 进路口后，只允许走允许集里的那几支
 *
 * 用法：./restr_ab [--graph=dir] [--probe=N] [--only=N] [--random=N]
 *   --probe=N  定向取前 N 条 via=way 限制（默认 40）
 *   --only=N   定向取前 N 组 only_* 限制（默认 40）
 *   --random=N   补充：随机 N 对起终点做里程对照
 *   --no-seam-restr  负向对照：只关掉会合点处的限制检查（**必须报 FAIL**）
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "map/mapset.h"
#include "map/autobuild.h"
#include "map/binfmt.h"
#include "route/route.h"
#include "tool_common.h"

/* 固定种子的 xorshift：验收必须可复现，不能用 rand()（不同 libc 序列不同） */
static uint64_t rs = 88172645463325252ULL;
static double rnd01(void)
{
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return (double)(rs >> 11) / 9007199254740992.0;
}


static int32_t node_lat(const MapSrc *s, uint32_t nd)
{
    return nm_get_i32(s->nodes.data + (uint64_t)nd * NM_NODE_BYTES + NF_LAT);
}
static int32_t node_lon(const MapSrc *s, uint32_t nd)
{
    return nm_get_i32(s->nodes.data + (uint64_t)nd * NM_NODE_BYTES + NF_LON);
}

/* 边下标 → 起点节点。CSR 只存 EF_TO，起点只能这样反查一遍。 */
static uint32_t *build_efrom(const MapSrc *s)
{
    uint64_t ne = s->edges.hdr.rec_count, nn = s->nodes.hdr.rec_count;
    uint32_t *efrom = malloc((size_t)ne * sizeof(uint32_t));
    if (!efrom) return NULL;
    for (uint64_t i = 0; i < ne; i++) efrom[i] = UINT32_MAX;
    for (uint64_t nd = 0; nd < nn; nd++) {
        const uint8_t *nr = s->nodes.data + nd * NM_NODE_BYTES;
        uint32_t b = nm_get_u32(nr + NF_EDGE_BEGIN);
        uint16_t c = nm_get_u16(nr + NF_EDGE_COUNT);
        for (uint16_t k = 0; k < c; k++)
            if ((uint64_t)b + k < ne) efrom[b + k] = (uint32_t)nd;
    }
    return efrom;
}

/* 路径里是否出现了 (e0 → e1) 这一对相邻边（要求两条边同省：边下标是省内的） */
static int path_has_pair(const RouteResult *rr, uint32_t prov,
                         uint32_t e0, uint32_t e1)
{
    for (uint32_t i = 0; i + 1 < rr->n; i++) {
        if (rr->edge_idx[i] != e0 || rr->edge_idx[i + 1] != e1) continue;
        if (rr->pts[i].prov != prov || rr->pts[i + 1].prov != prov) continue;
        return 1;
    }
    return 0;
}
static int path_has_seq(const RouteResult *rr, uint32_t prov,
                        uint32_t e0, uint32_t e1, uint32_t e2)
{
    for (uint32_t i = 0; i + 2 < rr->n; i++) {
        if (rr->edge_idx[i] != e0 || rr->edge_idx[i + 1] != e1 ||
            rr->edge_idx[i + 2] != e2) continue;
        if (rr->pts[i].prov != prov || rr->pts[i + 1].prov != prov ||
            rr->pts[i + 2].prov != prov) continue;
        return 1;
    }
    return 0;
}

/* 定向验收：逐条 via=way 限制构造 P0→C 的路线，看开/关限制是否不同 */
static int probe_restr(MapSet *ms, Router *ra, Router *rb, int maxprobe,
                       int *n_tried, int *n_seq_off, int *n_seq_on,
                       int *n_dupskip)
{
    int fail = 0;
    int n_bad_shown = 0;

    for (int p = 0; p < ms->n; p++) {
        MapSrc *s = &ms->srcs[p];
        if (!s->has_restr || s->restrictions.hdr.rec_count == 0) continue;
        uint64_t nn = s->nodes.hdr.rec_count, ne = s->edges.hdr.rec_count;
        if (nn == 0 || ne == 0) continue;
        /* ★ 被别省整份包含的副本必须跳过：路由器自己也不展开它
           （expand_dir 里 `srcs[p].dup_of >= 0 → continue`），所以这里构造出来的
           路线用的是**包含方那份**的节点/边下标，拿副本省的下标去比对永远
           匹配不上——多省跑会假报"0 命中"，看着像限制没写对。 */
        if (s->dup_of >= 0) { *n_dupskip += (int)s->restrictions.hdr.rec_count; continue; }

        /* 边下标 → 起点节点。CSR 只存 EF_TO，起点只能这样反查一遍。 */
        uint32_t *efrom = build_efrom(s);
        if (!efrom) { fprintf(stderr, "内存不足\n"); return 1; }

        const uint8_t *d = s->restrictions.data;
        uint64_t nr = s->restrictions.hdr.rec_count;
        for (uint64_t i = 0; i < nr && *n_tried < maxprobe; i++) {
            const uint8_t *rec = d + i * NM_RESTR_BYTES;
            if (rec[RF_KIND] != 2) continue;

            uint32_t e_from = nm_get_u32(rec + RF_PREV_EDGE);
            uint32_t e_via  = nm_get_u32(rec + RF_FROM_EDGE);
            uint32_t e_to   = nm_get_u32(rec + RF_TO_EDGE);
            uint32_t B      = nm_get_u32(rec + RF_VIA_NODE);
            if (e_from >= ne || e_via >= ne || e_to >= ne || B >= nn) continue;
            uint32_t P0 = efrom[e_from];
            uint32_t A  = nm_get_u32(s->edges.data + (uint64_t)e_from * NM_EDGE_BYTES + EF_TO);
            uint32_t C  = nm_get_u32(s->edges.data + (uint64_t)e_to   * NM_EDGE_BYTES + EF_TO);
            if (P0 == UINT32_MAX || A >= nn || C >= nn) continue;

            (*n_tried)++;
            RNode st, gl;
            double dd;
            if (router_nearest(ra, node_lat(s, P0) / 1e7, node_lon(s, P0) / 1e7, &st, &dd) != 0)
                continue;
            /* 终点必须取 C（e_to 的终点）：被禁的是"三段"序列，终点停在 B
               的话路径里根本没有 e_to，检查必然 0 命中（第一版就踩了这个坑）。 */
            if (router_nearest(ra, node_lat(s, C) / 1e7, node_lon(s, C) / 1e7, &gl, &dd) != 0)
                continue;

            RouteResult rA, rB;
            memset(&rA, 0, sizeof(rA));
            memset(&rB, 0, sizeof(rB));
            if (router_plan(rb, st, gl, &rB) != 0 || !rB.ok) {
                route_result_free(&rB); continue;
            }
            if (router_plan(ra, st, gl, &rA) != 0 || !rA.ok) {
                route_result_free(&rA); route_result_free(&rB); continue;
            }

            int off_has = path_has_seq(&rB, (uint32_t)p, e_from, e_via, e_to);
            int on_has  = path_has_seq(&rA, (uint32_t)p, e_from, e_via, e_to);
            if (off_has) (*n_seq_off)++;
            if (on_has)  { (*n_seq_on)++; fail = 1; }

            /* 开着限制算出来的路线必须干净 */
            if (route_check(ra, &rA) != 0) {
                fail = 1;
                if (n_bad_shown < 3) {
                    printf("  ★ 踩限制案例 %d：起点 %.6f,%.6f  终点 %.6f,%.6f"
                           "  (P0=%u A=%u B=%u C=%u)\n",
                           n_bad_shown, node_lat(s, P0) / 1e7, node_lon(s, P0) / 1e7,
                           node_lat(s, C) / 1e7, node_lon(s, C) / 1e7,
                           P0, A, B, C);
                    n_bad_shown++;
                }
            }

            route_result_free(&rA);
            route_result_free(&rB);
        }
        free(efrom);
    }
    return fail;
}

/* 定向验收 only_*（kind=1）。
 *
 * 构造方式：取一组 (via V, from fe)，在 V 的出边里找一条**不在允许集**里的，
 * 记 forb。然后以 fe 的起点 P0 为起点、forb 的终点 C 为终点算路线——最短路
 * 本来就该是"P0 --fe--> V --forb--> C"两步。于是：
 *   关限制时 rB 应当走出这一步（n_pair_off 计数）；
 *   开限制时 rA 必须不再走出它（n_pair_on 必须为 0）。
 *
 * ⚠ 反过来取"允许集里的那条"去测是不行的：允许集里的转向开不开限制都能走，
 *   开关两遍结果一样，测不出任何东西。 */
static int probe_only(MapSet *ms, Router *ra, Router *rb, int maxprobe,
                      int *n_tried, int *n_pair_off, int *n_pair_on,
                      int *n_allallowed, int *n_degenerate, int *n_dupskip)
{
    typedef struct { uint32_t via, from, to; } R1;
    int fail = 0, shown = 0;

    for (int p = 0; p < ms->n && *n_tried < maxprobe; p++) {
        MapSrc *s = &ms->srcs[p];
        if (!s->has_restr || s->restrictions.hdr.rec_count == 0) continue;
        uint64_t nn = s->nodes.hdr.rec_count, ne = s->edges.hdr.rec_count;
        if (nn == 0 || ne == 0) continue;
        if (s->dup_of >= 0) { *n_dupskip += (int)s->restrictions.hdr.rec_count; continue; }

        uint32_t *efrom = build_efrom(s);
        if (!efrom) { fprintf(stderr, "内存不足\n"); return 1; }

        uint64_t nrec = s->restrictions.hdr.rec_count;
        R1 *v1 = malloc((size_t)nrec * sizeof(R1));
        if (!v1) { free(efrom); fprintf(stderr, "内存不足\n"); return 1; }
        size_t nv1 = 0;
        const uint8_t *d = s->restrictions.data;
        for (uint64_t i = 0; i < nrec; i++) {
            const uint8_t *rec = d + i * NM_RESTR_BYTES;
            if (rec[RF_KIND] != 1) continue;
            v1[nv1].via  = nm_get_u32(rec + RF_VIA_NODE);
            v1[nv1].from = nm_get_u32(rec + RF_FROM_EDGE);
            v1[nv1].to   = nm_get_u32(rec + RF_TO_EDGE);
            nv1++;
        }

        for (size_t a = 0; a < nv1 && *n_tried < maxprobe; a++) {
            /* 一组 (via, from) 只在第一次出现时整组处理 */
            int first = 1;
            for (size_t b = 0; b < a; b++)
                if (v1[b].via == v1[a].via && v1[b].from == v1[a].from) { first = 0; break; }
            if (!first) continue;

            uint32_t V = v1[a].via, fe = v1[a].from;
            if (V >= nn || fe >= ne) continue;

            uint32_t forb = UINT32_MAX;
            const uint8_t *vr = s->nodes.data + (uint64_t)V * NM_NODE_BYTES;
            uint32_t eb = nm_get_u32(vr + NF_EDGE_BEGIN);
            uint16_t ec = nm_get_u16(vr + NF_EDGE_COUNT);
            for (uint16_t k = 0; k < ec; k++) {
                uint64_t e = (uint64_t)eb + k;
                if (e >= ne) break;
                int allowed = 0;
                for (size_t b = 0; b < nv1; b++)
                    if (v1[b].via == V && v1[b].from == fe && v1[b].to == (uint32_t)e) {
                        allowed = 1; break;
                    }
                if (!allowed) { forb = (uint32_t)e; break; }
            }
            if (forb == UINT32_MAX) { (*n_allallowed)++; continue; }

            uint32_t P0 = efrom[fe];
            uint32_t C  = nm_get_u32(s->edges.data + (uint64_t)forb * NM_EDGE_BYTES + EF_TO);
            if (P0 == UINT32_MAX || C >= nn) continue;
            /* forb 是"掉头回 P0"的那条边时起终点重合，算出来是空路径——
               这不是违反限制，只是这条用例没有判别力，跳过。
               ★ continue 必须落在 (*n_tried)++ **之前**。写成"先 -- 再 ++"
               的话净变化是 0，预算约束失效，会多跑很多组——实测报出过
               "定向测试 80 组、命中 84 次"这种不可能的数。 */
            if (C == P0) { (*n_degenerate)++; continue; }
            (*n_tried)++;
            RNode st, gl; double dd;
            if (router_nearest(ra, node_lat(s, P0) / 1e7, node_lon(s, P0) / 1e7, &st, &dd) != 0) continue;
            if (router_nearest(ra, node_lat(s, C)  / 1e7, node_lon(s, C)  / 1e7, &gl, &dd) != 0) continue;

            RouteResult rA, rB;
            memset(&rA, 0, sizeof(rA));
            memset(&rB, 0, sizeof(rB));
            if (router_plan(rb, st, gl, &rB) != 0 || !rB.ok) {
                route_result_free(&rB); continue;
            }
            if (router_plan(ra, st, gl, &rA) != 0 || !rA.ok) {
                route_result_free(&rA); route_result_free(&rB); continue;
            }

            int off_has = path_has_pair(&rB, (uint32_t)p, fe, forb);
            int on_has  = path_has_pair(&rA, (uint32_t)p, fe, forb);
            if (off_has) (*n_pair_off)++;
            if (on_has) {
                (*n_pair_on)++; fail = 1;
                if (shown < 3) {
                    printf("  ★ 开限制仍走了被禁转向 %d：via=%u from=%u forb=%u"
                           "  起点 %.6f,%.6f 终点 %.6f,%.6f\n", shown,
                           V, fe, forb,
                           node_lat(s, P0) / 1e7, node_lon(s, P0) / 1e7,
                           node_lat(s, C)  / 1e7, node_lon(s, C)  / 1e7);
                    shown++;
                }
            }
            if (route_check(ra, &rA) != 0) {
                fail = 1;
                if (shown < 6)
                    printf("  ★ 开限制路线未能通过自检：via=%u from=%u forb=%u\n",
                           V, fe, forb);
            }

            route_result_free(&rA);
            route_result_free(&rB);
        }
        free(v1);
        free(efrom);
    }
    return fail;
}

int main(int argc, char **argv)
{
    const char *graph = "products/graph/beijing";
    int nprobe = 40, nrandom = 0, nonly = 40, noseam = 0;
    double lat0 = 39.78, lat1 = 40.02, lon0 = 116.22, lon1 = 116.58;
    AutoBuildOpt ab;
    tc_ab_init(&ab);

    for (int i = 1; i < argc; i++) {
        const char *v;
        if ((v = tc_arg_val(argv[i], "--graph=")))  graph = v;
        else if ((v = tc_arg_val(argv[i], "--probe=")))  nprobe = atoi(v);
        else if ((v = tc_arg_val(argv[i], "--only=")))   nonly = atoi(v);
        else if ((v = tc_arg_val(argv[i], "--random="))) nrandom = atoi(v);
        else if ((v = tc_arg_val(argv[i], "--lat0=")))   lat0 = atof(v);
        else if ((v = tc_arg_val(argv[i], "--lat1=")))   lat1 = atof(v);
        else if ((v = tc_arg_val(argv[i], "--lon0=")))   lon0 = atof(v);
        else if ((v = tc_arg_val(argv[i], "--lon1=")))   lon1 = atof(v);
        else if (strcmp(argv[i], "--no-seam-restr") == 0) noseam = 1;
        else if (strcmp(argv[i], "--no-autobuild") == 0) ab.enable = 0;
        else { fprintf(stderr, "未知参数 %s\n", argv[i]); return 2; }
    }

    MapSet ms;
    if (tc_load_graph(&ms, graph, &ab) != 0) {
        fprintf(stderr, "加载失败: %s\n", graph);
        return 1;
    }
    printf("图集 %d 个省\n", ms.n);

    /* ★ 对拍必须用 w=1；heur_w_hi 也要设成 1.0 —— 自适应会在第 1 遍撞顶后
       换更重的 w 重跑，而"限制开/关"两条腿的难度不同，可能一条撞顶一条没撞，
       那样里程差就混进了 w 的影响，A/B 失效（见 route.h 的 heur_w_hi 注释）。 */
    RouteOpts on  = { .mode_time = 1, .bidir = 1, .heur_w = 1.0, .heur_w_hi = 1.0 };  /* 限制全开 */
    RouteOpts off = { .mode_time = 1, .bidir = 1, .no_restr2 = 1, .no_restr1 = 1,
                       .heur_w = 1.0, .heur_w_hi = 1.0 };                             /* 限制全关 */
    /* 负向对照：把"会合点处的限制检查"关掉，其余限制全开。
       正常跑 restr_ab 必须报 OK；加了 --no-seam-restr 必须报 FAIL——
       这条"能失败"是整套判据有效性的前提，缺了它就只剩单向自证。 */
    if (noseam) on.no_seam_restr = 1;
    Router *ra = router_create(&ms, &on);
    Router *rb = router_create(&ms, &off);
    if (!ra || !rb) { fprintf(stderr, "建路由器失败\n"); return 1; }

    int fail = 0;

    /* ---------- 定向验收 ---------- */
    int n_tried = 0, n_seq_off = 0, n_seq_on = 0, n_dupskip = 0;
    printf("\n===== 定向：逐条 via=way 限制构造路线 =====\n");
    if (probe_restr(&ms, ra, rb, nprobe, &n_tried, &n_seq_off, &n_seq_on,
                    &n_dupskip))
        fail = 1;
    printf("定向测试          %d 条\n", n_tried);
    printf("  关限制时走进被禁序列 %d 条（限制确实在拦东西）\n", n_seq_off);
    printf("  开限制时仍走进序列   %d 条（必须为 0）\n", n_seq_on);
    if (n_seq_on) printf("ASSERT-FAIL 开着限制仍然走进了被禁序列\n");

    /* ---------- only_* 定向验收 ---------- */
    int n_otried = 0, n_pair_off = 0, n_pair_on = 0, n_allallowed = 0,
        n_degenerate = 0;
    printf("\n===== 定向：逐组 only_* 限制构造路线 =====\n");
    if (probe_only(&ms, ra, rb, nonly, &n_otried, &n_pair_off, &n_pair_on,
                   &n_allallowed, &n_degenerate, &n_dupskip))
        fail = 1;
    printf("定向测试          %d 组\n", n_otried);
    printf("  关限制时走了被禁转向 %d 组（限制确实在拦东西）\n", n_pair_off);
    printf("  开限制时仍走         %d 组（必须为 0）\n", n_pair_on);
    if (n_pair_on) printf("ASSERT-FAIL 开着限制仍然走了 only_* 禁止的转向\n");
    if (n_allallowed)
        printf("  注：%d 组的允许集覆盖了全部出边，这组限制其实什么都不禁\n",
               n_allallowed);
    if (n_degenerate)
        printf("  注：%d 组是「掉头回起点」，起终点重合，无判别力已跳过\n",
               n_degenerate);
    if (n_dupskip)
        printf("  注：%d 条限制属于被别省整份包含的副本省（如北京 ⊂ 河北），"
               "路由器不展开它\n      ⇒ 这些限制在运行时是死数据，未纳入判别\n",
               n_dupskip);

    /* ---------- 随机补充 ---------- */
    if (nrandom > 0) {
        printf("\n===== 随机对照（补充）=====\n");
        int n_diff = 0, n_shorter = 0, n_viol_a = 0;
        double sum_a = 0, sum_b = 0;
        for (int k = 0; k < nrandom; k++) {
            double fl = lat0 + rnd01() * (lat1 - lat0);
            double fo = lon0 + rnd01() * (lon1 - lon0);
            double tl = lat0 + rnd01() * (lat1 - lat0);
            double to = lon0 + rnd01() * (lon1 - lon0);
            RNode a, b; double da, db;
            if (router_nearest(ra, fl, fo, &a, &da) != 0 ||
                router_nearest(ra, tl, to, &b, &db) != 0) continue;
            RouteResult rA, rB;
            memset(&rA, 0, sizeof(rA));
            memset(&rB, 0, sizeof(rB));
            if (router_plan(ra, a, b, &rA) != 0 || !rA.ok ||
                router_plan(rb, a, b, &rB) != 0 || !rB.ok) {
                route_result_free(&rA); route_result_free(&rB); continue;
            }
            if (route_check(ra, &rA) != 0) { n_viol_a++; fail = 1; }
            double dm = rA.total_m - rB.total_m;
            if (fabs(dm) > 0.5) n_diff++;
            if (dm < -0.5) { n_shorter++; fail = 1; }
            sum_a += rA.total_m; sum_b += rB.total_m;
            route_result_free(&rA); route_result_free(&rB);
        }
        printf("里程不同的路线    %d / %d\n", n_diff, nrandom);
        printf("开限制仍踩限制    %d 条（必须为 0）\n", n_viol_a);
        printf("开限制反而更短    %d 条（必须为 0）\n", n_shorter);
        printf("总里程  开 %.0f  关 %.0f  差 %+.0f m\n",
               sum_a, sum_b, sum_a - sum_b);
    }

    printf("\n%s\n", fail ? "RESTR-AB-FAIL" : "RESTR-AB-OK");
    router_destroy(ra);
    router_destroy(rb);
    mapset_free(&ms);
    return fail ? 1 : 0;
}
