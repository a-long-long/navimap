#include "route/route.h"      /* RouteResult / REDGE_SEAM */
#include "route/route_priv.h" /* 完整 Router / SewEnt */

#include "route/cost.h"       /* node_coord / fallback_speed */
#include "route/restr.h"      /* restr_blocked* */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 结果的释放与自检（原 route.c 的 route_result_free / route_check，整段迁来，
 * 行为未改）。两者都是 route.h 里的公共接口（ui/window.c、tools/restr_ab.c
 * 调用），故不加 static，也不需要新头文件。
 * 只依赖已导出符号：node_coord / fallback_speed / restr_blocked*，
 * 不碰搜索核心的任何静态函数。 */

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

/* 释放整个 RouteResultSet。逐条 route_result_free——每条 rr 独立持有自己的堆数组，
 * 互不重叠，所以调用方把某条 rr 的指针"搬"到别处时（例如 App 把选中方案
 * 挪进 a->rr），只要把源 rr memset 成零值，这里就不会重复释放。 */
void route_result_set_free(RouteResultSet *s)
{
    if (!s) return;
    for (int k = 0; k < s->n; k++)
        route_result_free(&s->rr[k]);
    memset(s, 0, sizeof(*s));
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
