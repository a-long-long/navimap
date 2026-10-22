/* coverage_cli —— "这里有没有地图"的无头验收
 *
 * 用法: coverage_cli [--graph=products/graph] [--lat=<度> --lon=<度>]
 *                    [--radius=3000] [--no-autobuild]
 *
 * 不带 --lat/--lon 时跑内置自检：
 *   北京中心（39.90,116.40）必须有路网；
 *   （30.00,110.00）在 8 省之外，必须判为无数据并且给出下载提示。
 * GUI 上的提示条用的就是同一套函数，这里能过，界面上那条提示就是对的。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "map/mapset.h"
#include "map/autobuild.h"
#include "map/coverage.h"

static const char *cov_text(int c)
{
    switch (c) {
    case COV_HAS_ROAD:  return "有路网";
    case COV_BBOX_ONLY: return "在省范围内但附近无路";
    case COV_NONE:      return "无地图数据";
    default:            return "无法判断";
    }
}

static void report(const MapSet *ms, const char *tag, double lat, double lon,
                   double radius)
{
    char prov[64], hint[1024];
    int cov = mapset_coverage(ms, lat, lon, radius, prov, sizeof(prov));
    coverage_hint(ms, lat, lon, cov, prov, hint, sizeof(hint));
    printf("%-12s %.5f,%.5f  覆盖=%-16s 省=%-10s\n",
           tag, lat, lon, cov_text(cov), prov[0] ? prov : "—");
    if (cov != COV_HAS_ROAD) printf("%s\n", hint);
}

int main(int argc, char **argv)
{
    const char *graph = "products/graph";
    double lat = 0, lon = 0, radius = 3000;
    int have_ll = 0;

    AutoBuildOpt ab;
    memset(&ab, 0, sizeof(ab));
    ab.enable = 1;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strncmp(a, "--graph=", 8))       graph = a + 8;
        else if (!strncmp(a, "--lat=", 6))  { lat = atof(a + 6); have_ll |= 1; }
        else if (!strncmp(a, "--lon=", 6))  { lon = atof(a + 6); have_ll |= 2; }
        else if (!strncmp(a, "--radius=", 9))  radius = atof(a + 9);
        else if (!strcmp(a, "--no-autobuild")) ab.enable = 0;
        else { fprintf(stderr, "未知参数 %s\n", a); return 2; }
    }

    MapSet ms;
    if (mapset_load_repair(&ms, graph, &ab, NULL) != 0) return 1;
    printf("图集 %d 个省\n", ms.n);

    if (have_ll == 3) {
        report(&ms, "查询点", lat, lon, radius);
        mapset_free(&ms);
        return 0;
    }

    int fails = 0;
#define FAIL(...) do { printf("ASSERT-FAIL    " __VA_ARGS__); fails++; } while (0)

    report(&ms, "北京中心", 39.90, 116.40, radius);
    report(&ms, "省外点",   30.00, 110.00, radius);

    char prov[64], hint[1024];

    /* 1) 北京中心必须有路 */
    int c1 = mapset_coverage(&ms, 39.90, 116.40, radius, prov, sizeof(prov));
    if (c1 != COV_HAS_ROAD)
        FAIL("北京中心(39.90,116.40) 应判为有路网，实际 %s\n", cov_text(c1));
    if (!prov[0])
        FAIL("北京中心应落在某个省内，省名为空\n");

    /* 2) 8 省之外的点必须判为无数据 */
    int c2 = mapset_coverage(&ms, 30.00, 110.00, radius, prov, sizeof(prov));
    if (c2 != COV_NONE)
        FAIL("(30.00,110.00) 在 8 省之外，应判为无地图数据，实际 %s\n",
             cov_text(c2));

    /* 3) 无数据时必须给出可执行的下载指引（不是只说一句"没有"） */
    coverage_hint(&ms, 30.00, 110.00, c2, prov, hint, sizeof(hint));
    if (!strstr(hint, "sources/") || !strstr(hint, "sync_maps.sh"))
        FAIL("提示里必须写清\"放进 sources/ 并运行 sync_maps.sh\"\n");
    if (!strstr(hint, "http"))
        FAIL("提示里必须给出下载来源链接\n");

    /* 4) 有数据时不该再提示下载 */
    coverage_hint(&ms, 39.90, 116.40, c1, prov, hint, sizeof(hint));
    if (strstr(hint, "下载"))
        FAIL("有地图时不该出现下载提示：%s\n", hint);

    mapset_free(&ms);
    printf("%s\n", fails ? "COVERAGE-FAIL" : "COVERAGE-OK");
    return fails ? 1 : 0;
}
