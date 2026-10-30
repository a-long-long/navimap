/* skedaddle_lmdist_cli —— 地标距离的一对多 Dijkstra（PLAN.md §48）
 *
 * 给 A* 做地标下界用的第一块砖：从**一个**地标节点跑到全图，输出任意经纬度
 * 处的最小代价。这一版不做网格聚合、不落盘，只用来**验证 Dijkstra 本身对不对**
 * （拿 skedaddle_route_cli 的路线距离逐条对拍，见 scripts 里的 lmdist_check）。
 *
 * 用法：
 *   ./skedaddle_lmdist_cli --graph=products/graph --from=39.90,116.40 \
 *                --q=36.65,117.02 --q=38.91,121.61 ...
 *   [--mode=time|dist]       代价口径（默认 time，与 router 一致 ⇒ 单位是秒）
 *   [--stats]                打印出堆/入堆数与峰值内存（验收用）
 *
 * 输出：每个 --q 一行 `Q <lat,lon> <省> <节点> <代价秒> <km 换算>`，
 *       够不到打印 UNREACHABLE。最后一行 LM-DIST-OK / LM-DIST-FAIL。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "map/mapset.h"
#include "map/autobuild.h"
#include "route/route.h"
#include "utils/geo.h"
#include "utils/plat.h"
#include "tool_common.h"

static uint64_t mono_ms(void)
{
    return nm_now_ms();                /* 平台层：clock_gettime / QPC */
}


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

int main(int argc, char **argv)
{
    const char *graph = "products/graph";
    const char *from_s = NULL;
    double q[256][2];
    int nq = 0;
    int want_stats = 0;
    RouteOpts opt = { .mode_time = 1, .bidir = 1, .heur_w = ROUTE_HEUR_W_DEFAULT };
    AutoBuildOpt ab;
    tc_ab_init(&ab);

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v;
        if ((v = tc_arg_val(a, "--graph=")))      graph = v;
        else if ((v = tc_arg_val(a, "--from=")))  from_s = v;
        else if ((v = tc_arg_val(a, "--mode=")))  opt.mode_time = (strcmp(v, "dist") != 0);
        else if ((v = tc_arg_val(a, "--q="))) {
            if (nq >= 256) { fprintf(stderr, "--q 太多（上限 256）\n"); return 2; }
            if (parse_ll(v, &q[nq][0], &q[nq][1]) != 0) {
                fprintf(stderr, "--q 格式应为 lat,lon\n"); return 2;
            }
            nq++;
        }
        else if (strcmp(a, "--stats") == 0)    want_stats = 1;
        else { fprintf(stderr, "未知参数 %s\n", a); return 2; }
    }
    if (!from_s || nq == 0) {
        fprintf(stderr, "用法: skedaddle_lmdist_cli --graph=<dir> --from=lat,lon --q=lat,lon [--q=...]\n");
        return 2;
    }
    double flat, flon;
    if (parse_ll(from_s, &flat, &flon) != 0) {
        fprintf(stderr, "地标格式应为 lat,lon\n"); return 2;
    }

    MapSet ms;
    if (tc_load_graph(&ms, graph, &ab) != 0) return 1;

    Router *r = router_create(&ms, &opt);
    if (!r) { fprintf(stderr, "路由初始化失败\n"); return 1; }

    RNode a;
    double da = 0;
    if (router_nearest(r, flat, flon, &a, &da) != 0) {
        printf("ROUTE-FAIL 地标无路\n");
        router_destroy(r); mapset_free(&ms); return 1;
    }
    printf("地标吸附      %-10s #%-9u 偏差 %.1f m\n",
           ms.srcs[a.prov].name, a.node, da);

    /* 先把所有查询点吸附好，再跑 Dijkstra —— 吸附会碰瓦片/缝合缓存，
       放在计时之外，免得污染统计。 */
    RNode qn[256];
    double qd[256];
    int bad = 0;
    for (int i = 0; i < nq; i++) {
        if (router_nearest(r, q[i][0], q[i][1], &qn[i], &qd[i]) != 0) {
            printf("Q %.5f,%.5f UNREACHABLE（附近无路网）\n", q[i][0], q[i][1]);
            bad++;
        }
    }

    LmDist d;
    uint64_t npop = 0, npush = 0;
    double ru0 = nm_peak_rss_mb();     /* 平台层：getrusage / GetProcessMemoryInfo */
    uint64_t t0 = mono_ms();
    int rc = lm_dijkstra(r, a, &d, &npop, &npush);
    uint64_t t1 = mono_ms();
    double ru1 = nm_peak_rss_mb();
    if (rc != 0) {
        printf("LM-DIST-FAIL Dijkstra 返回 %d\n", rc);
        router_destroy(r); mapset_free(&ms); return 1;
    }

    const char *unit = opt.mode_time ? "秒" : "米";
    for (int i = 0; i < nq; i++) {
        uint32_t c = lm_cost_at(&d, qn[i].prov, qn[i].node);
        if (c == LM_UNREACHABLE) {
            printf("Q %.5f,%.5f UNREACHABLE\n", q[i][0], q[i][1]);
            bad++;
            continue;
        }
        double val = (double)c / 1000.0;
        printf("Q %.5f,%.5f %-10s #%-9u %.3f %s\n",
               q[i][0], q[i][1], ms.srcs[qn[i].prov].name, qn[i].node, val, unit);
    }
    if (want_stats) {
        printf("统计          出堆 %llu 入堆 %llu 耗时 %llu ms 节点总数 %llu\n",
               (unsigned long long)npop, (unsigned long long)npush,
               (unsigned long long)(t1 - t0), (unsigned long long)d.total);
        printf("峰值内存      %.0f MB → %.0f MB\n", ru0, ru1);
    }

    lm_dist_free(&d);
    router_destroy(r);
    mapset_free(&ms);
    printf(bad ? "LM-DIST-FAIL 有 %d 个查询点没结果\n" : "LM-DIST-OK\n", bad);
    return bad ? 1 : 0;
}
