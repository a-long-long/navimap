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
#include "route/st.h"
#include "route/route_priv.h"
#include "route/restr.h"
#include "route/cost.h"
#include "route/sew.h"
#include "map/gpx.h"          /* GpxTrack：GPX 贴合道路用 */

/* ---------------------------------------------------------------------------
 * 跨省 A* 路由（状态主键 = 坐标，见 route.h 顶部说明）
 *
 * 双向版本：正向走出边（CSR），反向走入边（按省惰性构建的反向 CSR）。
 * 两条搜索各自一张状态表 + 一个堆，按"堆顶 f 小的那侧先走"交替推进。
 * ------------------------------------------------------------------------- */

/* 展开上限（防跑飞）。★ 800 万是在**自适应 w 之前**定的，实测会把真实路线剪断：
   郑州→大连 要 11334703 展开（15.6 s / 2.29 GB）⇒ 直接 ROUTE-FAIL；同类失败的
   还有西安/南阳/太原 → 大连，全是"从远处到半岛端点"（启发式失效那一类）。
   ⇒ 抬到 1200 万（PLAN.md §48.3）。
   ★ 再往上要小心：峰值内存随展开数近似线性（~210~290 B/展开），本机可用
     3.59 GB 且 zram swap 常满 ⇒ 别超过 1600 万。 */
#define DEFAULT_MAX_EXPAND 12000000u
/* 吸附时保留几个候选节点。>1 是为了"最近那个是死节点"时换下一个重试
   （PLAN.md §51.1）：不连通的判定是毫秒级，重试几乎不要钱；
   4 个足够（实测第 1~2 个候选就救回了全部失败样本）。 */
#define SNAP_TRY       4





static uint64_t now_ms(void)
{
    return nm_now_ms();                /* 平台层：clock_gettime / QPC */
}

/* --------------------- 反向入边索引 --------------------- */
/* 结构与落盘/构建见 route/revidx.h。这里只管"用"：优先 rev.bin，没有就建。
   只给反向搜索真正走进去的省建，副本省（被完全包含）从不建。 */




/* ------------------------ Router 生命周期 ------------------------ */


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

/* 途经点多段算路：每段独立吸附+算路，但相邻段共用"上一段的终点节点"以保证连续。
 * 见 route.h 声明处的说明。 */
int router_route_via(Router *r, const double *lats, const double *lons, int n,
                     RouteResult *out, RNode *fa, RNode *fb,
                     double *da, double *db)
{
    if (out) memset(out, 0, sizeof(*out));
    if (n < 2) return -1;

    RouteResult *legs = calloc((size_t)(n - 1), sizeof(RouteResult));
    if (!legs) return -3;

    RNode prev_end = {0};
    int rc = 0;

    /* 首段：用 router_route_ll（自带吸附 + 死节点换候选重试），取其终点做后续固定起点 */
    rc = router_route_ll(r, lats[0], lons[0], lats[1], lons[1], &legs[0], fa, fb, da, db);
    if (rc != 0) {
        route_result_free(&legs[0]);
        free(legs);
        return rc < 0 ? rc : -3;
    }
    prev_end = legs[0].pts[legs[0].n - 1];

    /* 后续段：起点固定 = 上一段终点节点；终点对应当前途经点/终点吸附后的候选 */
    for (int i = 1; i < n - 1; i++) {
        RNode B[SNAP_TRY]; double DB[SNAP_TRY];
        int nb = router_nearest_k(r, lats[i + 1], lons[i + 1], B, SNAP_TRY, DB);
        if (i == n - 2 && fb) *fb = B[0];          /* 末段终点候选先填最近的 */
        if (i == n - 2 && db) *db = DB[0];
        if (nb <= 0) { rc = -1; goto fail; }
        int found = 0;
        for (int c = 0; c < nb; c++) {
            rc = router_plan(r, prev_end, B[c], &legs[i]);
            if (rc == 0) {
                if (i == n - 2 && fb) *fb = B[c];
                if (i == n - 2 && db) *db = DB[c];
                found = 1; break;
            }
            route_result_free(&legs[i]);
            if (rc == -3) goto fail;               /* 超限：直接放弃 */
            /* -1 不连通：换候选 */
        }
        if (!found) { rc = -2; goto fail; }
        prev_end = legs[i].pts[legs[i].n - 1];
    }

    /* 拼接：首段全拷；之后每段从 index 1 开始拷（与上一端共用节点，跳过重复点） */
    {
        uint64_t total_pts = 0;
        for (int i = 0; i < n - 1; i++) total_pts += legs[i].n;
        /* 拼接时每段只跳过第 1 个点（与上一段共用节点），共跳过 (段数-1) = (n-2) 个重复点；
         * 段数 = n-1，故去掉 n-2 个重叠端点（n=2 时退化为普通两点，不减）。 */
        uint64_t N = total_pts - (uint64_t)(n - 2);   /* 实际不重复点数 */
        if (N == 0) { rc = -2; goto fail; }

        out->n = (uint32_t)N;
        out->pts    = malloc((size_t)N * sizeof(RNode));
        out->lat_e7 = malloc((size_t)N * sizeof(int32_t));
        out->lon_e7 = malloc((size_t)N * sizeof(int32_t));
        uint32_t nseg = (uint32_t)(N - 1);
        out->edge_idx  = malloc((size_t)nseg * sizeof(uint32_t));
        out->from_node = malloc((size_t)nseg * sizeof(uint32_t));
        out->edge_cls  = malloc((size_t)nseg * sizeof(uint8_t));
        out->edge_flags= malloc((size_t)nseg * sizeof(uint8_t));
        out->seg_m     = malloc((size_t)nseg * sizeof(uint32_t));
        out->seg_ds    = malloc((size_t)nseg * sizeof(uint32_t));
        if (!out->pts || !out->lat_e7 || !out->lon_e7 ||
            (nseg > 0 && (!out->edge_idx || !out->from_node || !out->edge_cls ||
                          !out->edge_flags || !out->seg_m || !out->seg_ds))) {
            rc = -3; goto fail;
        }

        uint32_t w = 0;
        for (int i = 0; i < n - 1; i++) {
            uint32_t start = (i == 0) ? 0 : 1;
            for (uint32_t j = start; j < legs[i].n; j++) {
                out->pts[w]    = legs[i].pts[j];
                out->lat_e7[w] = legs[i].lat_e7[j];
                out->lon_e7[w] = legs[i].lon_e7[j];
                w++;
            }
        }
        uint32_t s = 0;
        for (int i = 0; i < n - 1; i++) {
            for (uint32_t j = 0; j + 1 < legs[i].n; j++) {
                out->edge_idx[s]  = legs[i].edge_idx[j];
                out->from_node[s] = legs[i].from_node[j];
                out->edge_cls[s]  = legs[i].edge_cls[j];
                out->edge_flags[s]= legs[i].edge_flags[j];
                out->seg_m[s]     = legs[i].seg_m[j];
                out->seg_ds[s]    = legs[i].seg_ds[j];
                s++;
            }
        }

        double tm = 0, ts = 0; uint64_t expanded = 0, seams = 0;
        for (int i = 0; i < n - 1; i++) {
            tm += legs[i].total_m; ts += legs[i].total_s;
            expanded += legs[i].expanded; seams += legs[i].n_seams;
        }
        out->total_m   = tm;
        out->total_s   = ts;
        out->expanded  = expanded;
        out->n_seams   = seams;
        out->ok        = 1;
    }

    for (int i = 0; i < n - 1; i++) route_result_free(&legs[i]);
    free(legs);
    return 0;

fail:
    for (int i = 0; i < n - 1; i++) route_result_free(&legs[i]);
    free(legs);
    return rc;
}

/* 多方案对比（阶段 12）：同一对端点算 K 条备选。
 *
 * ★ 端点必须先按"基准选项"找到一对连通的，四个方案再各自复用这组端点。
 *   否则不同方案可能各自吸附到不同候选（router_nearest_k 取最近 4 个），
 *   比出来就不是"同起终点不同走法"了——慢的方案可能只是因为吸到了更远的候选。
 *
 * ★ r->opt 是 Router 的共享状态、router_plan 内部会读它（bidir / mode_time /
 *   avoid_* / heur_w）。这里在调用前临时把 r->opt 换成各方案自己的选项，
 *   调完 router_plan 再把 r->opt 还原成进入时的 base —— router_plan 自己也会
 *   把 heur_w 还原成进入值（route.c:918），所以来回切换不会串味。 */
int router_route_alts(Router *r, double flat, double flon, double tlat, double tlon,
                      RouteResultSet *out, RNode *fa, RNode *fb,
                      double *da, double *db)
{
    if (out) memset(out, 0, sizeof(*out));
    out->n = ROUTE_ALT_K;

    RNode A[SNAP_TRY], B[SNAP_TRY];
    double DA[SNAP_TRY], DB[SNAP_TRY];
    int na = router_nearest_k(r, flat, flon, A, SNAP_TRY, DA);
    int nb = router_nearest_k(r, tlat, tlon, B, SNAP_TRY, DB);
    if (na > 0 && fa) *fa = A[0];
    if (na > 0 && da) *da = DA[0];
    if (nb > 0 && fb) *fb = B[0];
    if (nb > 0 && db) *db = DB[0];
    if (na <= 0 || nb <= 0) return -1;

    /* 先按基准选项找一对连通端点（全部方案共用） */
    RouteOpts base = r->opt;
    RNode f = {0}, t = {0};
    int found = 0;
    for (int s = 0; s < SNAP_TRY && !found; s++) {
        for (int i = 0; i <= s && i < na; i++) {
            int j = s - i;
            if (j >= nb) continue;
            RouteResult probe; memset(&probe, 0, sizeof(probe));
            r->opt = base;
            int rc = router_plan(r, A[i], B[j], &probe);
            if (rc == 0) {
                f = A[i]; t = B[j];
                if (fa) *fa = f;
                if (fb) *fb = t;
                if (da) *da = DA[i];
                if (db) *db = DB[j];
                route_result_free(&probe);
                found = 1; break;
            }
            route_result_free(&probe);
            if (rc == -1) continue;       /* 不连通：换候选 */
            if (rc == -3) { r->opt = base; return -3; }  /* 超限：直接放弃 */
        }
    }
    if (!found) { r->opt = base; return -2; }

    /* 四套选项：最快（基准）/ 最短（按距离）/ 避收费 / 避非铺装。
     * ★ "最短"用 heur_w=1.0（标准 A*，启发式可采纳 ⇒ 严格最短距离）。
     *   否则加权 A*（w=1.25）返回的只是"近似距离极小化者"，实测会比
     *   "最快"那条还长，标签就成假的了。heur_w_hi 同步 1.0，撞顶重跑也保持精确。 */
    static const struct { int mode_time, avoid_toll, avoid_unpaved; double w; const char *tag; }
        DEF[ROUTE_ALT_K] = {
        { 1, 0, 0, 1.25, "最快" },
        { 0, 0, 0, 1.00, "最短" },
        { 1, 1, 0, 1.25, "避收费" },
        { 1, 0, 1, 1.25, "避非铺装" },
    };

    int feasible = 0;
    for (int k = 0; k < ROUTE_ALT_K; k++) {
        RouteOpts o = base;
        o.mode_time     = DEF[k].mode_time;
        o.avoid_toll    = DEF[k].avoid_toll;
        o.avoid_unpaved = DEF[k].avoid_unpaved;
        o.heur_w        = DEF[k].w;
        o.heur_w_hi     = DEF[k].w;
        r->opt = o;
        snprintf(out->tag[k], sizeof(out->tag[k]), "%s", DEF[k].tag);

        int rc = router_plan(r, f, t, &out->rr[k]);
        out->err[k] = rc;
        if (rc == 0) {
            RouteResult *rr = &out->rr[k];
            out->total_m[k]   = rr->total_m;
            out->total_s[k]   = rr->total_s;
            out->expanded[k]  = rr->expanded;
            uint32_t toll = 0, unp = 0;
            for (uint32_t i = 0; i + 1 < rr->n; i++) {
                if (rr->edge_flags[i] & EF_TOLL)    toll += rr->seg_m[i];
                if (rr->edge_flags[i] & EF_UNPAVED) unp  += rr->seg_m[i];
            }
            out->toll_m[k]    = toll;
            out->unpaved_m[k] = unp;
            feasible++;
        }
        /* 失败方案的派生字段保持为 0（memset 已做），rr[k] 也是零值结构体 */
    }
    r->opt = base;
    if (feasible == 0) return -2;   /* 端点连通但四个方案全失败：不该发生 */
    return 0;
}

/* 取落在同一坐标上的所有 (省,节点) 成员。省内部的瓦片直接返回"到达时的
   那个节点"，连缝合索引都不建。 */
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
/* ---------------------------------------------------------------------------
 * GPX 贴合道路：把一条 GPX 轨迹吸附到路网，输出一条沿道路的 RouteResult。
 *
 * 思路：把轨迹点均匀抽稀到至多 max_wp 个途经点（首尾必取），再用
 * router_route_via 依次串联——每段内部自动吸附到最近道路节点。这样既不冗余
 * （相邻近点之间不再反复算路），又能保证结果贴着道路。
 * 返回的 out 是沿道路的多段折线，与普通算路同一套 RouteResult，调用方负责
 * route_result_free。点数 < 2 返回 -1；吸附失败 -3；任一段不连通 -1。
 * ------------------------------------------------------------------------- */
int gpx_snap_to_roads(Router *r, const GpxTrack *tk, RouteResult *out, int max_wp)
{
    if (!r || !tk || !out) return -1;
    if (tk->n < 2) return -1;
    if (max_wp < 2) max_wp = 2;

    int n = (int)tk->n;
    int m = n < max_wp ? n : max_wp;          /* 实际途经点数（含首末） */
    double *la = malloc((size_t)m * sizeof(double));
    double *lo = malloc((size_t)m * sizeof(double));
    if (!la || !lo) { free(la); free(lo); return -2; }

    /* 均匀取索引：第 0 个取首点、第 m-1 个取末点，中间等距分布 */
    for (int i = 0; i < m; i++) {
        int idx = (m == 1) ? 0 : (n - 1) * i / (m - 1);
        if (idx < 0) idx = 0;
        if (idx >= n) idx = n - 1;
        la[i] = tk->lat_e7[idx] / 1e7;
        lo[i] = tk->lon_e7[idx] / 1e7;
    }

    RNode f, t; double da = 0, db = 0;
    memset(out, 0, sizeof(*out));
    int rc = router_route_via(r, la, lo, m, out, &f, &t, &da, &db);
    free(la); free(lo);
    return rc;
}

int router_plan(Router *r, RNode from, RNode to, RouteResult *out)
{
    uint64_t t0 = now_ms();
    uint64_t try_cap = r->opt.adapt_try;
    if (try_cap > r->max_expand) try_cap = r->max_expand;
    /* ★★ plan_search 会把 w 写进 r->opt.heur_w。**必须恢复**：Router 是可复用的
       （skedaddle_route_cli --repeat=N 就是同一个 Router 连算 N 次），不恢复的话第 2 次
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
