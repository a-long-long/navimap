/* coverage_cli —— "这里有没有地图、这是哪儿"的无头验收
 *
 * 用法: coverage_cli [--graph=products/graph] [--lat=<度> --lon=<度>]
 *                    [--radius=3000] [--places=<地名库>|--no-places]
 *                    [--no-autobuild]
 *
 * 不带 --lat/--lon 时跑内置自检：
 *   北京中心（39.90,116.40）必须有路网，且反查出"中国 · 北京"；
 *   （30.00,110.00）在 8 省之外，必须判为无数据、报出最近的省会与建议下载项；
 *   渤海湾（38.90,118.20）在省内但无路 ⇒ 必须说"数据已加载、此处无路网"，
 *   而**不能**建议下载一个已经加载的省（这条曾是真实误导）。
 *
 * GUI / Qt 弹窗上的提示用的就是同一套函数，这里能过，界面上那条提示就是对的。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "map/mapset.h"
#include "map/autobuild.h"
#include "map/coverage.h"
#include "render/labels.h"
#include "tool_common.h"

static const char *cov_text(int c)
{
    switch (c) {
    case COV_HAS_ROAD:  return "有路网";
    case COV_BBOX_ONLY: return "在省范围内但附近无路";
    case COV_NONE:      return "无地图数据";
    default:            return "无法判断";
    }
}

/* 反查一次完整结果：省/国 来自 coverage_locate，市/区 来自地名库。
 * 两块分开算、在这里合并 —— 与 ui/window.c 的 app_update_coverage 走同一条路径。 */
static int locate_full(const MapSet *ms, LabelCache *lc, double lat, double lon,
                       CovPlace *pl)
{
    int hit = coverage_locate(ms, lat, lon, pl);
    if (lc) label_nearest_admin(lc, lat, lon,
                                pl->city, sizeof(pl->city),
                                pl->district, sizeof(pl->district));
    return hit;
}

static void report(const MapSet *ms, LabelCache *lc, const char *tag,
                   double lat, double lon, double radius)
{
    char prov[64], hint[1024];
    int cov = mapset_coverage(ms, lat, lon, radius, prov, sizeof(prov));
    CovPlace pl;
    locate_full(ms, lc, lat, lon, &pl);
    coverage_hint(ms, lat, lon, cov, &pl, hint, sizeof(hint));
    printf("%-12s %.5f,%.5f  覆盖=%-16s 省=%-10s\n",
           tag, lat, lon, cov_text(cov), prov[0] ? prov : "—");
    printf("%-12s 反查: 国家=%s 省=%s%s 市=%s 区=%s 距离=%.0fkm 建议下载=%s\n", "",
           pl.country[0] ? pl.country : "—",
           pl.prov[0] ? pl.prov : "—",
           pl.prov_guessed ? "(推断)" : "",
           pl.city[0] ? pl.city : "—",
           pl.district[0] ? pl.district : "—",
           pl.nearest_km,
           pl.dl_label[0] ? pl.dl_label : "—");
    if (cov != COV_HAS_ROAD) printf("%s\n", hint);
}

int main(int argc, char **argv)
{
    const char *graph  = "products/graph";
    const char *places = "products/places_auto.db";
    double lat = 0, lon = 0, radius = 3000;
    int have_ll = 0, want_places = 1;

    AutoBuildOpt ab;
    tc_ab_init(&ab);

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strncmp(a, "--graph=", 8))       graph = a + 8;
        else if (!strncmp(a, "--lat=", 6))  { lat = atof(a + 6); have_ll |= 1; }
        else if (!strncmp(a, "--lon=", 6))  { lon = atof(a + 6); have_ll |= 2; }
        else if (!strncmp(a, "--radius=", 9))  radius = atof(a + 9);
        else if (!strncmp(a, "--places=", 9))  places = a + 9;
        else if (!strcmp(a, "--no-places"))    want_places = 0;
        else if (!strcmp(a, "--no-autobuild")) ab.enable = 0;
        else { fprintf(stderr, "未知参数 %s\n", a); return 2; }
    }

    MapSet ms;
    if (tc_load_graph(&ms, graph, &ab) != 0) return 1;
    printf("图集 %d 个省\n", ms.n);

    /* 地名库是可选的：缺了照样能跑，只是市/区那两栏为空。
     * ★ 不把"没有地名库"当失败——它是可选数据，判据应按"有则必须对"来写。 */
    LabelCache lc;
    int has_places = 0;
    if (want_places) {
        if (label_cache_init(&lc, places) == 0) {
            has_places = 1;
            printf("地名库 %s\n", places);
        } else {
            printf("地名库 %s 不可用（市/区反查将为空）\n", places);
        }
    }

    if (have_ll == 3) {
        report(&ms, has_places ? &lc : NULL, "查询点", lat, lon, radius);
        if (has_places) label_cache_free(&lc);
        mapset_free(&ms);
        return 0;
    }

    int fails = 0;
#define FAIL(...) do { printf("ASSERT-FAIL    " __VA_ARGS__); fails++; } while (0)

    LabelCache *pl_lc = has_places ? &lc : NULL;
    report(&ms, pl_lc, "北京中心", 39.90, 116.40, radius);
    report(&ms, pl_lc, "省外点",   30.00, 110.00, radius);
    report(&ms, pl_lc, "渤海湾",   38.90, 118.20, radius);

    char prov[64], hint[1024];
    CovPlace pl;

    /* 1) 北京中心必须有路 */
    int c1 = mapset_coverage(&ms, 39.90, 116.40, radius, prov, sizeof(prov));
    if (c1 != COV_HAS_ROAD)
        FAIL("北京中心(39.90,116.40) 应判为有路网，实际 %s\n", cov_text(c1));
    if (!prov[0])
        FAIL("北京中心应落在某个省内，省名为空\n");

    /* 2) 省中文名反查：北京中心必须报出"北京"，而不是拼音 beijing */
    locate_full(&ms, pl_lc, 39.90, 116.40, &pl);
    if (!strstr(pl.prov, "北京"))
        FAIL("北京中心反查省名应为「北京」，实际「%s」\n", pl.prov);
    if (strcmp(pl.prov, "beijing") == 0)
        FAIL("省名退化成了拼音，说明中文表没命中\n");
    if (!strstr(pl.country, "中国"))
        FAIL("北京中心反查国别应为「中国」，实际「%s」\n", pl.country);
    if (pl.prov_guessed)
        FAIL("北京中心命中了 bbox，不该标成「推断」\n");
    /* 市/区（需要地名库）：北京中心应能反查到"北京市"
     * ★ 有地名库才断言——可选数据缺失不该判失败，否则门禁在别的机器上必红。 */
    if (has_places) {
        if (!strstr(pl.city, "北京"))
            FAIL("北京中心应反查到市级「北京市」，实际「%s」\n", pl.city);
        if (!pl.district[0])
            FAIL("北京中心应反查到区县级，实际为空\n");
    }

    /* 3) 8 省之外的点必须判为无数据 */
    int c2 = mapset_coverage(&ms, 30.00, 110.00, radius, prov, sizeof(prov));
    if (c2 != COV_NONE)
        FAIL("(30.00,110.00) 在 8 省之外，应判为无地图数据，实际 %s\n",
             cov_text(c2));

    /* 4) 省外点反查：必须给出"大致哪个省 + 距离 + 建议下载项"。
     *    这是本次改造的核心 —— 只报"没有数据"等于什么都没说。 */
    locate_full(&ms, pl_lc, 30.00, 110.00, &pl);
    if (!pl.dl_label[0])
        FAIL("省外点必须给出建议下载的地区名（dl_label 为空）\n");
    if (pl.nearest_km <= 1.0)
        FAIL("省外点必须报出距离，实际 %.1f km\n", pl.nearest_km);
    if (!pl.prov_guessed)
        FAIL("省外点的省名应标成「按最近省会推断」\n");
    if (!pl.prov[0])
        FAIL("省外点应能按省会表推断出大致省份，实际为空\n");

    /* 5) 提示文案必须可执行：说清该下哪块 + 从哪个入口下 */
    coverage_hint(&ms, 30.00, 110.00, c2, &pl, hint, sizeof(hint));
    if (!strstr(hint, "下载全球地图"))
        FAIL("提示里必须指出下载入口（『地图 → 下载全球地图』）\n");
    if (pl.dl_label[0] && !strstr(hint, pl.dl_label))
        FAIL("提示里必须点名建议下载的地区「%s」\n", pl.dl_label);

    /* 6) ★ bbox-only（数据有了、这块没路）不能建议下载已加载的省 —— 真实误导 */
    int c3 = mapset_coverage(&ms, 38.90, 118.20, radius, prov, sizeof(prov));
    if (c3 == COV_BBOX_ONLY) {
        CovPlace p3;
        locate_full(&ms, pl_lc, 38.90, 118.20, &p3);
        coverage_hint(&ms, 38.90, 118.20, c3, &p3, hint, sizeof(hint));
        if (strstr(hint, "建议下载"))
            FAIL("省内无路网时不该建议下载（该省已加载）：%s\n", hint);
        if (!strstr(hint, "已加载"))
            FAIL("省内无路网时应说明「数据已加载、此处没有路网」\n");
    }

    /* 7) 有数据时不该再提示下载 */
    locate_full(&ms, pl_lc, 39.90, 116.40, &pl);
    coverage_hint(&ms, 39.90, 116.40, c1, &pl, hint, sizeof(hint));
    if (strstr(hint, "建议下载"))
        FAIL("有地图时不该出现下载提示：%s\n", hint);

    if (has_places) label_cache_free(&lc);
    mapset_free(&ms);
    printf("%s\n", fails ? "COVERAGE-FAIL" : "COVERAGE-OK");
    return fails ? 1 : 0;
}
