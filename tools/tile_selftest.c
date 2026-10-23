/* tile_selftest —— 瓦片下标"查表版 vs 精确版"的逐点等价性验收
 *
 * 用法:
 *   tile_selftest --graph=<dir>                默认：nm_tile_of 必须与精确版逐点相同
 *   tile_selftest --graph=<dir> --expect-diff   对照组：nm_tile_of_raw（不带边界
 *                                               回退）必须**有**差异
 *
 * ★ 为什么不抽样：nm_tile_of 用的是 nm_mercator_y_fast() 插值表，误差 3.8e-6
 *   瓦片，平时被取整吃掉，只有点落在瓦片边界 3.8e-6 以内才暴露 —— 52M 个节点里
 *   这种点只有万分之几，抽样几乎一定测不到（§31 的教训：抽样没差异不算判据）。
 *   所以这里做两件事：
 *     ① 全图 5228 万个真实节点**逐点**比；
 *     ② 定向构造正好落在瓦片边界上的点（每条 x/y 边界 ± 几个扰动量）——
 *        这些点必然落在误差带里，是"随机样本测不到"的那类输入。
 * ★ 判据要双向：--expect-diff 证明"边界回退不是摆设"。如果它也没差异，说明
 *   查表误差根本没到跨格的程度，那条回退分支就是死代码（或者表精度被改了）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#include "map/mapset.h"
#include "map/binfmt.h"
#include "map/tileidx.h"
#include "utils/geo.h"
#include "utils/plat.h"

static uint64_t now_ms(void)
{
    return nm_now_ms();                /* 平台层：clock_gettime / QPC */
}

static uint64_t g_tested, g_diff;
static int     g_expect_diff;

static void cmp_one(double lat, double lon, const char *tag, int *first)
{
    int fx, fy, ex, ey;
    if (g_expect_diff) nm_tile_of_raw(lat, lon, &fx, &fy);
    else               nm_tile_of(lat, lon, &fx, &fy);
    nm_tile_of_exact(lat, lon, &ex, &ey);
    g_tested++;
    if (fx != ex || fy != ey) {
        g_diff++;
        if (g_diff <= 5)
            fprintf(stderr, "  差异[%s] lat=%.9f lon=%.9f  被测(%d,%d) 精确(%d,%d)\n",
                    tag, lat, lon, fx, fy, ex, ey);
        if (first) *first = 1;
    }
}

int main(int argc, char **argv)
{
    const char *graph = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strncmp(a, "--graph=", 8)) graph = a + 8;
        else if (!strcmp(a, "--expect-diff")) g_expect_diff = 1;
        else { fprintf(stderr, "未知参数 %s\n", a); return 1; }
    }
    if (!graph) {
        fprintf(stderr, "用法: tile_selftest --graph=<dir> [--expect-diff]\n");
        return 1;
    }

    MapSet ms;
    memset(&ms, 0, sizeof(ms));
    if (mapset_load(&ms, graph) != 0) {
        fprintf(stderr, "图集加载失败: %s\n", graph);
        return 1;
    }

    uint64_t t0 = now_ms();

    /* ① 全图真实节点逐点比 */
    uint64_t nodes = 0;
    for (int i = 0; i < ms.n; i++) {
        const MapSrc *s = &ms.srcs[i];
        uint64_t n = s->nodes.hdr.rec_count;
        const uint8_t *nd = s->nodes.data;
        for (uint64_t k = 0; k < n; k++) {
            const uint8_t *nr = nd + (uint64_t)k * NM_NODE_BYTES;
            double lat = (double)nm_get_i32(nr + NF_LAT) / 1e7;
            double lon = (double)nm_get_i32(nr + NF_LON) / 1e7;
            cmp_one(lat, lon, s->name, NULL);
        }
        nodes += n;
    }
    uint64_t diff_real = g_diff;

    /* ② 定向构造：瓦片边界上的点，扰动按**瓦片单位**给（不是按度 ——
       墨卡托在极区 1 度能跨 500 多个瓦片，按度扰动测不到误差带）。
       覆盖从"远在带内"(0) 到"远在带外"(0.1 瓦片)，既验证带内会回退到精确
       公式，也验证带外查表本身足够准。另一轴固定在瓦片正中，避免双轴互相掩盖。 */
    double Z = (double)(1u << NM_TILE_Z);
    double lat_mid, lon_mid;
    nm_mercator_inv((8192 + 0.5) / Z, (8192 + 0.5) / Z, &lat_mid, &lon_mid);
    static const double dt[] = { 0.0, 1e-6, -1e-6, 1e-5, -1e-5, 1e-4, -1e-4,
                                 5e-4, -5e-4, 2e-3, -2e-3, 1e-2, -1e-2, 1e-1, -1e-1 };

    for (int k = 0; k <= (int)Z; k++) {
        double lat_b, lon_b, dummy;
        /* y 边界：wy = k/Z 反解纬度 */
        nm_mercator_inv((8192 + 0.5) / Z, (double)k / Z, &lat_b, &dummy);
        double deg_per_tile_y = 360.0 * cos(lat_b * M_PI / 180.0) / Z;
        for (size_t d = 0; d < sizeof(dt)/sizeof(dt[0]); d++)
            cmp_one(lat_b + dt[d] * deg_per_tile_y, lon_mid, "y边界", NULL);
        /* x 边界：wx = k/Z 反解经度（经度方向每瓦片度数恒定） */
        nm_mercator_inv((double)k / Z, (8192 + 0.5) / Z, &dummy, &lon_b);
        for (size_t d = 0; d < sizeof(dt)/sizeof(dt[0]); d++)
            cmp_one(lat_mid, lon_b + dt[d] * 360.0 / Z, "x边界", NULL);
    }
    uint64_t diff_crafted = g_diff - diff_real;

    uint64_t ms_el = now_ms() - t0;
    printf("节点逐点   %llu 个，差异 %llu\n", (unsigned long long)nodes,
           (unsigned long long)diff_real);
    printf("边界构造   %llu 个，差异 %llu\n",
           (unsigned long long)(g_tested - nodes), (unsigned long long)diff_crafted);
    printf("合计       %llu 点，差异 %llu，耗时 %llu ms\n",
           (unsigned long long)g_tested, (unsigned long long)g_diff,
           (unsigned long long)ms_el);

    if (!g_expect_diff) {
        if (g_diff) { fprintf(stderr, "TILE-SELFTEST-FAIL\n"); return 1; }
        printf("TILE-SELFTEST-OK\n");
        return 0;
    }
    /* 对照组：必须有差异，否则"边界回退"是死代码 / 精度被改了 */
    if (g_diff == 0) { fprintf(stderr, "TILE-SELFTEST-NODIFF（回退分支没被证明有必要）\n"); return 1; }
    printf("TILE-SELFTEST-DIFF-OK\n");
    return 0;
}
