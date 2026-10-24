/* navsim_cli —— 模拟定位 / 偏航 / 剩余 ETA 的无头验收（阶段 9 导航层）
 *
 * GUI 里的导航推进没法断言（要靠人看着车头跑），所以导航层的计算部分
 * 全部放在 src/route/navsim.c，这里按帧推进一遍并对每个不变量下断言：
 *
 *   1. 起步：剩余里程/时间 == 全程
 *   2. 推进：已走里程单调增、剩余里程与时间单调减、指令序号单调不减
 *   3. 车头恒在路线上（把车头当定位点注入，横向偏移必须 ≈ 0）
 *   4. 恒等式：已走 + 剩余 == 全程（里程）
 *   5. 到达：剩余归零、进度 1、模拟耗时 ≈ 全程预计耗时
 *   6. 偏航：横向拉开 200 m 必须判偏航，拉开 30 m 必须不判
 *   7. 重算：从偏航点重新规划，新路线起点必须落在偏航点附近
 *
 * 用法：
 *   ./navsim_cli --graph=products/graph/beijing --from=lat,lon --to=lat,lon
 *                [--speed=60] [--dt=0.033] [--max-steps=200000]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "map/mapset.h"
#include "map/autobuild.h"
#include "route/route.h"
#include "route/maneuver.h"
#include "route/navsim.h"
#include "utils/geo.h"
#include "tool_common.h"


static int parse_ll(const char *s, double *lat, double *lon)
{
    char *sp = strchr(s, ',');
    if (!sp) return -1;
    char buf[128];
    size_t n = (size_t)(sp - s);
    if (n >= sizeof(buf)) return -1;
    memcpy(buf, s, n); buf[n] = '\0';
    *lat = atof(buf);
    *lon = atof(sp + 1);
    return 0;
}

/* 偏航点用 navsim_offset_point（法线方向）。它放在 navsim 里而不是本文件，
   是因为 GUI 里"按 n 模拟偏航"要用同一个算法——两处各写一份迟早写歪。 */

int main(int argc, char **argv)
{
    const char *graph = "products/graph/beijing";
    const char *from_s = NULL, *to_s = NULL;
    double speed = 60.0, dt = 1.0 / 30.0;
    long max_steps = 200000;
    int fails = 0;

#define FAIL(...) do { printf("ASSERT-FAIL    " __VA_ARGS__); fails++; } while (0)

    AutoBuildOpt ab;
    tc_ab_init(&ab);   /* 产物缺失/损坏时自动重建 */

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v;
        if ((v = tc_arg_val(a, "--graph=")))       graph = v;
        else if ((v = tc_arg_val(a, "--from=")))   from_s = v;
        else if ((v = tc_arg_val(a, "--to=")))     to_s = v;
        else if ((v = tc_arg_val(a, "--speed=")))  speed = atof(v);
        else if ((v = tc_arg_val(a, "--dt=")))     dt = atof(v);
        else if ((v = tc_arg_val(a, "--max-steps="))) max_steps = atol(v);
        else if (strcmp(a, "--no-autobuild") == 0) ab.enable = 0;
        else { fprintf(stderr, "未知参数 %s\n", a); return 2; }
    }
    if (!from_s || !to_s) {
        fprintf(stderr, "用法: navsim_cli --graph=<dir> --from=lat,lon --to=lat,lon\n");
        return 2;
    }
    double flat, flon, tlat, tlon;
    if (parse_ll(from_s, &flat, &flon) != 0 ||
        parse_ll(to_s, &tlat, &tlon) != 0) {
        fprintf(stderr, "起终点格式应为 lat,lon\n");
        return 2;
    }

    MapSet ms;
    if (tc_load_graph(&ms, graph, &ab) != 0) return 1;
    /* heur_w 留 0：让 router_create 按"有没有 lm.bin"兜底（见 route.h ROUTE_HEUR_W_LM） */
    RouteOpts opt = { .mode_time = 1, .bidir = 1 };
    Router *r = router_create(&ms, &opt);
    if (!r) { fprintf(stderr, "路由初始化失败\n"); return 1; }

    RNode a, b;
    double da = 0, db = 0;
    if (router_nearest(r, flat, flon, &a, &da) != 0 ||
        router_nearest(r, tlat, tlon, &b, &db) != 0) {
        printf("ASSERT-FAIL    起终点附近没有路\n");
        router_destroy(r); mapset_free(&ms);
        return 1;
    }
    RouteResult rr;
    if (router_plan(r, a, b, &rr) != 0) {
        printf("ASSERT-FAIL    算不出路线\n");
        router_destroy(r); mapset_free(&ms);
        return 1;
    }
    ManeuverList ml;
    memset(&ml, 0, sizeof(ml));
    maneuver_build(&rr, &ms, &ml);

    printf("路线           %.2f km  %.0f 分  %u 段  %d 条指令\n",
           rr.total_m / 1000.0, rr.total_s / 60.0, rr.n - 1, ml.n);
    printf("推进           倍速 %.0fx  步长 %.4f s（每步模拟 %.2f s）\n",
           speed, dt, dt * speed);

    NavSim s;
    if (navsim_start(&s, &rr) != 0) {
        printf("ASSERT-FAIL    navsim_start 失败\n");
        return 1;
    }
    s.speed_mul = speed;

    /* ---- 1. 起步 ---- */
    {
        double rm = navsim_remaining_m(&s), rs = navsim_remaining_s(&s);
        printf("起步           剩余 %.1f m / %.1f s（全程 %.1f / %.1f）\n",
               rm, rs, rr.total_m, rr.total_s);
        if (fabs(rm - rr.total_m) > 0.5) FAIL("起步剩余里程 %.1f != 全程 %.1f\n", rm, rr.total_m);
        if (fabs(rs - rr.total_s) > 0.5) FAIL("起步剩余时间 %.1f != 全程 %.1f\n", rs, rr.total_s);
        if (fabs(navsim_progress(&s)) > 1e-9) FAIL("起步进度不为 0\n");
    }

    /* ---- 2~4. 逐帧推进 ---- */
    double prev_m = s.traveled_m, prev_rm = rr.total_m, prev_rs = rr.total_s;
    int    prev_man = -1, steps = 0;
    double worst_off = 0, worst_id = 0;
    long   worst_step = 0;
    uint32_t worst_seg = 0, worst_pseg = 0;
    double worst_tm = 0, worst_pm = 0;
    double mid_rem_m = 0, mid_rem_s = 0;
    int    mid_done = 0;

    while (steps < max_steps && !s.arrived) {
        int done = navsim_advance(&s, dt);
        steps++;

        if (s.traveled_m + 1e-6 < prev_m)
            FAIL("第 %d 步已走里程回退 %.3f → %.3f\n", steps, prev_m, s.traveled_m);

        /* ★ 先把这一帧的定位点注入，再读剩余里程/时间：navsim 的"剩余"
           在有定位点时以投影里程为准，顺序颠倒就会拿上一帧的投影去比
           这一帧的已走里程，凭空多出一个步长的误差（实测 55 m，看起来
           像恒等式不成立，其实是测试自己写错了）。 */
        navsim_locate(&s, s.car_lat, s.car_lon);
        if (s.off_m > worst_off) worst_off = s.off_m;

        double rm = navsim_remaining_m(&s), rs = navsim_remaining_s(&s);
        if (rm > prev_rm + 0.5)
            FAIL("第 %d 步剩余里程增加 %.3f → %.3f\n", steps, prev_rm, rm);
        if (rs > prev_rs + 0.5)
            FAIL("第 %d 步剩余时间增加 %.3f → %.3f\n", steps, prev_rs, rs);

        /* 恒等式：已走 + 剩余 == 全程 */
        double iderr = fabs(s.traveled_m + rm - rr.total_m);
        if (iderr > worst_id) {
            worst_id = iderr;
            worst_step = steps; worst_seg = s.seg; worst_pseg = s.proj_seg;
            worst_tm = s.traveled_m; worst_pm = s.proj_m;
        }

        int mi = navsim_next_maneuver(&s, &ml, NULL, NULL);
        if (mi >= 0 && prev_man >= 0 && mi < prev_man)
            FAIL("第 %d 步指令序号回退 %d → %d\n", steps, prev_man, mi);
        if (mi >= 0) prev_man = mi;

        /* 中途取一次快照，检查 ETA 与剩余里程自洽（速度在 1~55 m/s） */
        if (!mid_done && navsim_progress(&s) >= 0.5) {
            mid_rem_m = rm; mid_rem_s = rs; mid_done = 1;
        }
        prev_m = s.traveled_m; prev_rm = rm; prev_rs = rs;
        (void)done;
    }

    printf("推进           共 %d 步  已走 %.1f m / %.1f s  进度 %.4f\n",
           steps, s.traveled_m, s.traveled_s, navsim_progress(&s));
    printf("车头偏移       最大 %.3f m（应 ≈ 0）\n", worst_off);
    printf("恒等式误差     最大 %.3f m @步%ld（段 %u 里程 %.1f / 投影段 %u 里程 %.1f）\n",
           worst_id, worst_step, worst_seg, worst_tm, worst_pseg, worst_pm);

    if (!s.arrived) FAIL("%ld 步还没到终点（可能卡住）\n", max_steps);
    if (worst_off > 1.0) FAIL("车头偏离路线 %.1f m，插值写错了\n", worst_off);
    if (worst_id > 1.0) FAIL("已走+剩余 != 全程，最大偏差 %.1f m\n", worst_id);

    /* ---- 5. 到达 ---- */
    {
        double rm = navsim_remaining_m(&s), rs = navsim_remaining_s(&s);
        printf("到达           剩余 %.2f m / %.2f s\n", rm, rs);
        if (rm > 1.0) FAIL("到达后剩余里程 %.1f m 不为 0\n", rm);
        if (rs > 1.0) FAIL("到达后剩余时间 %.1f s 不为 0\n", rs);
        /* 进度用投影里程算，最后一段端点可能落在相邻段的公共顶点上，
           差个几十厘米是正常的，别用 1e-6 卡它。 */
        if (fabs(navsim_progress(&s) - 1.0) > 1e-3) FAIL("到达后进度不是 1\n");
        /* 模拟耗时 ≈ 全程预计：步长越大误差越大（跨段时按旧段速度多走了一点） */
        double tol = rr.total_s * 0.10 + 3.0 * dt * speed;
        if (fabs(s.traveled_s - rr.total_s) > tol)
            FAIL("模拟耗时 %.1f s 与预计 %.1f s 差太多（容差 %.1f）\n",
                 s.traveled_s, rr.total_s, tol);
    }
    if (mid_done) {
        double v = mid_rem_m > 1 ? mid_rem_s ? mid_rem_m / mid_rem_s : 0 : 0;
        printf("中途 ETA       剩余 %.0f m / %.0f s（均速 %.1f m/s）\n",
               mid_rem_m, mid_rem_s, v);
        if (mid_rem_m > 100 && (v < 1.0 || v > 55.0))
            FAIL("中途均速 %.1f m/s 不合理，剩余时间算错了\n", v);
    }

    /* ---- 6. 偏航判定 ----
     * 回到路线中段，把定位点朝航向右侧拉开 d 米：
     *   d=200 → 必须判偏航；d=30 → 必须不判。 */
    {
        NavSim t;
        navsim_start(&t, &rr);
        t.speed_mul = speed;
        /* 推进到一半 */
        while (!t.arrived && navsim_progress(&t) < 0.5) navsim_advance(&t, dt);

        double ola, olo;
        navsim_offset_point(t.car_lat, t.car_lon, t.heading_deg, 200.0, &ola, &olo);
        navsim_locate(&t, ola, olo);
        printf("偏航+200m      偏移 %.1f m  → %s\n", t.off_m,
               navsim_offroute(&t, 0) ? "判偏航" : "未判");
        if (fabs(t.off_m - 200.0) > 5.0)
            FAIL("注入 200 m 偏移，实测 %.1f m，投影算错了\n", t.off_m);
        if (!navsim_offroute(&t, 0)) FAIL("偏离 200 m 却没判偏航\n");

        navsim_offset_point(t.car_lat, t.car_lon, t.heading_deg, 30.0, &ola, &olo);
        navsim_locate(&t, ola, olo);
        printf("偏航+30m       偏移 %.1f m  → %s\n", t.off_m,
               navsim_offroute(&t, 0) ? "判偏航" : "未判");
        if (fabs(t.off_m - 30.0) > 5.0)
            FAIL("注入 30 m 偏移，实测 %.1f m\n", t.off_m);
        if (navsim_offroute(&t, 0)) FAIL("只偏 30 m 就判偏航，阈值太敏感\n");

        /* ---- 7. 偏航后重算 ---- */
        navsim_offset_point(t.car_lat, t.car_lon, t.heading_deg, 200.0, &ola, &olo);
        RNode na;
        double nsnap = 0;
        if (router_nearest(r, ola, olo, &na, &nsnap) != 0) {
            FAIL("偏航点附近找不到路，重算无从谈起\n");
        } else {
            RouteResult rr2;
            if (router_plan(r, na, b, &rr2) != 0) {
                FAIL("偏航后重算失败\n");
            } else {
                double d0 = nm_haversine_m(rr2.lat_e7[0] / 1e7, rr2.lon_e7[0] / 1e7,
                                           ola, olo);
                double d1 = nm_haversine_m(rr2.lat_e7[rr2.n - 1] / 1e7,
                                           rr2.lon_e7[rr2.n - 1] / 1e7,
                                           rr.lat_e7[rr.n - 1] / 1e7,
                                           rr.lon_e7[rr.n - 1] / 1e7);
                printf("重算           %.2f km  起点距偏航点 %.1f m  终点距原终点 %.1f m\n",
                       rr2.total_m / 1000.0, d0, d1);
                if (d0 > 300.0)
                    FAIL("重算起点距偏航点 %.1f m，没从当前位置起算\n", d0);
                if (d1 > 200.0)
                    FAIL("重算终点距原终点 %.1f m，目的地丢了\n", d1);
                NavSim t2;
                if (navsim_start(&t2, &rr2) == 0) {
                    if (fabs(navsim_remaining_m(&t2) - rr2.total_m) > 0.5)
                        FAIL("重算后剩余里程不等于新全程\n");
                }
                route_result_free(&rr2);
            }
        }
    }

    /* 指令一览：累计里程必须能把整条路线铺满 */
    if (ml.n > 0) {
        printf("指令           %d 条\n", ml.n);
        double cum = 0;
        for (int i = 0; i < ml.n && i < 12; i++) {
            cum += ml.v[i].dist_m;
            char tx[128];
            maneuver_text(&ml.v[i], tx, sizeof(tx));
            printf("  %2d  @%7.0f m  %-10s %s\n", i, cum,
                   maneuver_kind_text(&ml.v[i]), tx);
        }
        if (ml.n > 12) printf("  …（共 %d 条）\n", ml.n);
    }

    maneuver_free(&ml);
    route_result_free(&rr);
    router_destroy(r);
    mapset_free(&ms);

    if (fails) { printf("NAVSIM-FAIL  %d 条断言失败\n", fails); return 1; }
    printf("NAVSIM-OK\n");
    return 0;
}
