/* route_cli —— 路线计算的命令行入口（阶段 8）
 *
 * GUI 没法无头断言，所以路由也做成 CLI：给定经纬度起终点，输出路径统计、
 * 逐段自检结果，可选导出 GeoJSON。退出码 0 = 成功且自检通过。
 *
 * 用法：
 *   ./route_cli --graph=products/graph --from=39.90,116.40 --to=34.75,113.63
 *               [--mode=time|dist] [--avoid-toll] [--avoid-unpaved]
 *               [--geojson=out.json] [--max-expand=N] [--bidir=0|1]
 *               [--no-restr2] [--exact] [--maneuvers] [--avoid-toll] [--avoid-unpaved]
 *               [--maneuvers]
 *
 * --bidir=0 退化成单向 A*，用于和双向结果做 A/B 对照（两条路线的最优代价
 * 必须一致，否则说明终止条件或启发式有问题）。
 * --maneuvers 打印转向指令（导航层用同一套 maneuver_build，GUI 面板上
 *   看到的就是这份）。不变量：所有指令的 dist_m 之和 == 总里程。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

#include "map/mapset.h"
#include "map/autobuild.h"
#include "route/route.h"
#include "route/maneuver.h"
#include "utils/geo.h"

/* ★ 别再手写 strncmp(a, "--x=", N)：N 该写字面量长度还是长度-1，这个项目
   已经踩过三次（少 1 永不相等、多 1 把 '\0' 比进去），而且取值的偏移
   a+N 还得跟着对。统一走这里：不匹配返回 NULL，否则返回 '=' 之后的串。 */
static const char *arg_val(const char *a, const char *prefix)
{
    size_t n = strlen(prefix);
    return strncmp(a, prefix, n) == 0 ? a + n : NULL;
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
    /* 同一进程内重复算 N 次。★ 测性能时用 --repeat=5 并**丢掉第 1 次**：
       跨进程对比的噪声有 ±100 ms（进程启动 + 图 mmap 的页表建立），
       而同一进程第 2 次起稳定在 ±15 ms，能分辨 30 ms 级的差异
       （PLAN.md §38.1）。第 1 次偏慢 50~300 ms，不计入。 */
    int repeat = 1;
    const char *graph = "products/graph";
    const char *from_s = NULL, *to_s = NULL, *geojson = NULL;
    int want_man = 0;
    /* ★ heur_w / heur_w_hi 故意留 0：让 router_create 按"有没有 lm.bin"兜底
       （有表 1.3 + 关掉撞顶升级，无表 1.4 + 升 1.8，见 route.h 的
       ROUTE_HEUR_W_LM）。写死 1.4 会把兜底顶掉 —— 2026-09-28 就踩了这个：
       命令行没给 --heur-w，却一直是 1.4。要定点档位请用 --heur-w。 */
    RouteOpts opt = { .mode_time = 1, .bidir = 1 };  /* 默认双向；--bidir=0 退回单向做对照 */
    AutoBuildOpt ab;
    memset(&ab, 0, sizeof(ab));
    ab.enable = 1;   /* 产物缺失/损坏时自动重建 */

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v;
        if ((v = arg_val(a, "--graph=")))      graph = v;
        else if ((v = arg_val(a, "--from=")))  from_s = v;
        else if ((v = arg_val(a, "--to=")))    to_s = v;
        else if ((v = arg_val(a, "--geojson="))) geojson = v;
        else if ((v = arg_val(a, "--mode=")))
            opt.mode_time = (strcmp(v, "dist") != 0);
        else if ((v = arg_val(a, "--max-expand=")))
            opt.max_expand = strtoull(v, NULL, 10);
        else if ((v = arg_val(a, "--bidir=")))
            opt.bidir = (atoi(v) != 0);
        else if (strcmp(a, "--maneuvers") == 0)     want_man = 1;
        else if (strcmp(a, "--avoid-toll") == 0)    opt.avoid_toll = 1;
        else if (strcmp(a, "--avoid-unpaved") == 0) opt.avoid_unpaved = 1;
        /* 关掉 via=way 两段限制（单点限制仍生效）。只用于 A/B 对照：
           同一对起终点跑两遍，里程变长说明两段限制真的拦住了东西。 */
        else if (strcmp(a, "--no-restr2") == 0)     opt.no_restr2 = 1;
        /* 同理，关掉 only_* 限制。两个一起给 = 完全不看转向限制。 */
        else if (strcmp(a, "--no-restr1") == 0)     opt.no_restr1 = 1;
        else if (strcmp(a, "--no-seam-restr") == 0) opt.no_seam_restr = 1;
        /* 双向 A* 的"保证最优"模式：默认终止条件很快但可能差 ~0.4%，
           开这个开关改用严格条件（两侧都搜到 best），代价是慢好几倍。
           用于验收/对拍：--exact 与 --bidir=0 的结果必须一致。 */
        else if (strcmp(a, "--exact") == 0)         opt.exact = 1;
        /* 加权 A*（OsmAnd heuristicCoefficient）：f = g + w·h。
           w=1 标准 A*（默认，保证最优）；w>1 快但路径可能略长。
           用于出"速度 ↔ 路径质量"的档位表。 */
        else if ((v = arg_val(a, "--heur-w=")))    opt.heur_w = atof(v);
        else if ((v = arg_val(a, "--heur-w-hi="))) opt.heur_w_hi = atof(v);
        else if ((v = arg_val(a, "--adapt-try="))) opt.adapt_try = strtoull(v, NULL, 10);
        else if (strcmp(a, "--no-autobuild") == 0)  ab.enable = 0;
        else if (strncmp(a, "--repeat=", 9) == 0)   repeat = atoi(a + 9);
        else {
            fprintf(stderr, "未知参数 %s\n", a);
            return 2;
        }
    }
    if (!from_s || !to_s) {
        fprintf(stderr, "用法: route_cli --graph=<dir> --from=lat,lon --to=lat,lon\n");
        return 2;
    }
    double flat, flon, tlat, tlon;
    if (parse_ll(from_s, &flat, &flon) != 0 ||
        parse_ll(to_s, &tlat, &tlon) != 0) {
        fprintf(stderr, "起终点格式应为 lat,lon\n");
        return 2;
    }

    MapSet ms;
    if (mapset_load_repair(&ms, graph, &ab, NULL) != 0) return 1;

    Router *r = router_create(&ms, &opt);
    if (!r) { fprintf(stderr, "路由初始化失败\n"); return 1; }

    /* ★ 地标**个数**要打出来：它直接决定路线结果与耗时，而"表少建了几个地标"
       既不报错也不崩，只是悄悄变慢变次优 ⇒ 回归必须能把它钉住（§52）。 */
    if (router_lm_loaded(r))
        printf("地标表       已加载（%d 个地标，生效 w=%.2f）\n",
               router_lm_nlm(r), router_heur_w(r));
    else
        printf("地标表       未加载，退回纯直线启发式（生效 w=%.2f）\n", router_heur_w(r));

    RNode a, b;
    double da = 0, db = 0;
    RouteResult rr;
    /* ★ 用 router_route_ll：吸附撞上"有小连通块里的死节点"会自动换候选重试
       （PLAN.md §51.1）。它顺带完成第 1 次算路，所以 repeat 不会多跑一趟。 */
    int rc = router_route_ll(r, flat, flon, tlat, tlon, &rr, &a, &b, &da, &db);
    if (rc == -1) {
        printf("起点吸附      失败：附近无路网（%.5f,%.5f / %.5f,%.5f）\n",
               flat, flon, tlat, tlon);
        printf("ROUTE-FAIL 起点无路\n");
        router_destroy(r); mapset_free(&ms);
        return 1;
    }
    printf("起点吸附      %-10s #%-9u 偏差 %.1f m\n",
           ms.srcs[a.prov].name, a.node, da);
    printf("终点吸附      %-10s #%-9u 偏差 %.1f m\n",
           ms.srcs[b.prov].name, b.node, db);
    if (rc != 0) {
        printf("路线          %s\n", rc == -2 ? "不连通（已试过多组吸附候选）"
                                              : "超出规模上限");
        printf("ROUTE-FAIL 无路径\n");
        router_destroy(r); mapset_free(&ms);
        return 1;
    }

    double straight = nm_haversine_m(flat, flon, tlat, tlon);

    if (repeat > 1) {
        printf("[repeat 1] 展开 %llu 节点，耗时 %llu ms\n",
               (unsigned long long)rr.expanded,
               (unsigned long long)rr.elapsed_ms);
        route_result_free(&rr);
        for (int it = 1; it < repeat; it++) {
            rc = router_plan(r, a, b, &rr);
            if (rc != 0) break;
            printf("[repeat %d] 展开 %llu 节点，耗时 %llu ms\n", it + 1,
                   (unsigned long long)rr.expanded,
                   (unsigned long long)rr.elapsed_ms);
            if (it + 1 < repeat) { route_result_free(&rr); continue; }
        }
        if (rc != 0) {
            printf("路线          %s\n", rc == -1 ? "不连通" : "超出规模上限");
            printf("ROUTE-FAIL 无路径\n");
            router_destroy(r); mapset_free(&ms);
            return 1;
        }
    }

    printf("路线          %u 段，跨省接缝 %u 次\n", rr.n - 1, rr.n_seams);
    printf("总长          %.2f km（直线 %.2f km，绕行系数 %.2f）\n",
           rr.total_m / 1000.0, straight / 1000.0,
           straight > 1 ? rr.total_m / straight : 0.0);
    printf("预计          %.0f 分（按限速）\n", rr.total_s / 60.0);
    printf("搜索          展开 %llu 节点，耗时 %llu ms\n",
           (unsigned long long)rr.expanded, (unsigned long long)rr.elapsed_ms);
    {
        struct rusage ru;
        getrusage(RUSAGE_SELF, &ru);
        printf("峰值内存      %.0f MB\n", ru.ru_maxrss / 1024.0);
    }

    /* 经过的省（压缩连续重复） */
    /* 经过的省（压缩连续重复）。用"最具体的省"标注：北京 ⊂ 河北，路由走的是
       hebei 那份数据，但显示在北京市内就该说 beijing。 */
    printf("经过          ");
    int last = -1;
    for (uint32_t i = 0; i < rr.n; i++) {
        int p = mapset_detail_prov(&ms, rr.lat_e7[i], rr.lon_e7[i],
                                   (int)rr.pts[i].prov);
        if (p != last) {
            printf("%s%s", last >= 0 ? " → " : "", ms.srcs[p].name);
            last = p;
        }
    }
    printf("\n");

    int fail = route_check(r, &rr);

    if (want_man) {
        ManeuverList ml;
        int nm = maneuver_build(&rr, &ms, &ml);
        printf("转向指令      %d 条\n", nm);
        char t[160];
        for (int i = 0; i < nm; i++) {
            maneuver_text(&ml.v[i], t, sizeof(t));
            printf("  %2d  %8.0f m  %-12s %s\n", i + 1, ml.v[i].dist_m,
                   maneuver_kind_text(&ml.v[i]), t);
        }
        /* 不变量：指令里程之和必须等于总里程。不等就说明有段被漏掉或重复计。 */
        double tol = rr.total_m * 0.001 + 1.0;
        if (nm < 2 || fabs(ml.sum_m - rr.total_m) > tol) {
            printf("ASSERT-FAIL 指令里程和 %.1f m != 总里程 %.1f m\n",
                   ml.sum_m, rr.total_m);
            fail = 1;
        }
        printf("指令里程和    %.1f m（总里程 %.1f m，差 %.2f m）\n",
               ml.sum_m, rr.total_m, ml.sum_m - rr.total_m);
        maneuver_free(&ml);
    }

    /* 合理性断言：路径不该比直线近，也不该绕得离谱 */
    if (rr.total_m + 1.0 < straight) {
        printf("ASSERT-FAIL 路径比直线还短（%.1f m < %.1f m）\n",
               rr.total_m, straight);
        fail = 1;
    }
    if (straight > 2000.0 && rr.total_m > straight * 6.0) {
        printf("ASSERT-FAIL 绕行系数过大 %.2f（直线 %.1f km）\n",
               rr.total_m / straight, straight / 1000.0);
        fail = 1;
    }

    if (geojson) {
        FILE *f = fopen(geojson, "w");
        if (!f) {
            fprintf(stderr, "写不了 %s\n", geojson);
            fail = 1;
        } else {
            fprintf(f, "{\"type\":\"FeatureCollection\",\"features\":[{\"type\":\"Feature\","
                       "\"properties\":{\"meters\":%.1f,\"seconds\":%.1f,\"seams\":%u},"
                       "\"geometry\":{\"type\":\"LineString\",\"coordinates\":[",
                    rr.total_m, rr.total_s, rr.n_seams);
            for (uint32_t i = 0; i < rr.n; i++)
                fprintf(f, "%s[%.7f,%.7f]", i ? "," : "",
                        rr.lon_e7[i] / 1e7, rr.lat_e7[i] / 1e7);
            fprintf(f, "]}}]}\n");
            fclose(f);
            printf("GeoJSON       %s\n", geojson);
        }
    }

    route_result_free(&rr);
    router_destroy(r);
    mapset_free(&ms);
    printf("%s\n", fail ? "ROUTE-FAIL" : "ROUTE-OK");
    return fail ? 1 : 0;
}
