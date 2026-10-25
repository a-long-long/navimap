/* 坐标系（大地基准）自测：验证 datum 模块的几个不变量。
 *
 * 判据（全绿即 DATUM-OK）：
 *   1) 枚举/名字映射自洽：datum_by_index / datum_short / datum_label 与 DatumId 一致。
 *   2) WGS84 是恒等：wgs84_to(WGS84,x) == x，to_wgs84(WGS84,x) == x（精确）。
 *   3) CGCS2000 ≈ WGS84：往返（wgs84_to → to_wgs84）与原点差 < 1e-9 度（亚毫米）。
 *   4) 北京54 / 西安80 往返一致：wgs84_to → to_wgs84 回到原点 < 1e-9 度（变换可逆）。
 *      往返必须把 h 原样带回，否则测的是"丢了高程"而不是"不可逆"。
 *   5) 北京54 / 西安80 在已知点上产生**合理且非零**的基准偏移，量级按物理事实定，
 *      并互相约束（西安80 必然远小于北京54）。
 *
 * 负向对照（都必须能失败）：
 *   - 若把 BJ54/XA80 的平移参数清零（退化成恒等）⇒ 第 5 条 FAIL。
 *   - 若把 XA80 的参数抄成 BJ54 的 ⇒「西安80 偏移 << 北京54」FAIL。
 *   - 若给 WGS84 也错误地加了平移 ⇒「WGS84 偏移 ≈ 0」FAIL。
 *
 * 用法：datum_selftest
 */
#include "utils/datum.h"
#include "utils/geo.h"   /* nm_haversine_m */

#include <stdio.h>
#include <string.h>
#include <math.h>

static int g_fail = 0;
static void check(int ok, const char *what)
{
    printf("  %-50s %s\n", what, ok ? "OK" : "FAIL");
    if (!ok) g_fail++;
}

/* WGS84 → 基准 → WGS84 往返误差（度）。
 * ★ 必须把第一趟算出的 h 原样带回第二趟：换基准后 h 往往不是 0（BJ54 椭球比
 *   WGS84 大 108 m，同一点的 h 会差上百米），强制 h=0 等于把点沿法线挪了上百米，
 *   测出来的是"我把高程丢了"而不是"变换不可逆"。 */
static double roundtrip_err(DatumId id, double lat, double lon)
{
    double la1, lo1, h1, la2, lo2;
    datum_wgs84_to(id, lat, lon, 0, &la1, &lo1, &h1);
    datum_to_wgs84(id, la1, lo1, h1, &la2, &lo2, NULL);
    double dlat = la2 - lat, dlon = lo2 - lon;
    return sqrt(dlat * dlat + dlon * dlon);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("datum 自测: 坐标系（大地基准）\n");

    /* 1) 枚举/名字自洽 */
    check(datum_count() == DATUM_COUNT, "datum_count == DATUM_COUNT");
    check(datum_by_index(0) == DATUM_WGS84,   "by_index(0)=WGS84");
    check(datum_by_index(1) == DATUM_CGCS2000,"by_index(1)=CGCS2000");
    check(datum_by_index(2) == DATUM_BJ54,    "by_index(2)=BJ54");
    check(datum_by_index(3) == DATUM_XA80,    "by_index(3)=XA80");
    check(datum_by_index(99) == DATUM_WGS84,  "越界 index 回退 WGS84");
    check(strcmp(datum_short(DATUM_BJ54), "北京54") == 0, "BJ54 短名");
    check(strcmp(datum_label(DATUM_XA80), "西安 1980") == 0 ||
          strstr(datum_label(DATUM_XA80), "西安") != NULL, "XA80 标签含「西安」");

    /* 2) WGS84 恒等（精确） */
    {
        double la, lo;
        datum_wgs84_to(DATUM_WGS84, 39.9042, 116.4074, 0, &la, &lo, NULL);
        check(fabs(la - 39.9042) < 1e-12 && fabs(lo - 116.4074) < 1e-12,
              "WGS84→WGS84 恒等");
        datum_to_wgs84(DATUM_WGS84, 39.9042, 116.4074, 0, &la, &lo, NULL);
        check(fabs(la - 39.9042) < 1e-12 && fabs(lo - 116.4074) < 1e-12,
              "WGS84←WGS84 恒等");
    }

    /* 3) CGCS2000 ≈ WGS84（亚毫米） */
    {
        double e = roundtrip_err(DATUM_CGCS2000, 39.9042, 116.4074);
        check(e < 1e-9, "CGCS2000 往返误差 < 1e-9 度（亚毫米）");
        double la, lo;
        datum_wgs84_to(DATUM_CGCS2000, 39.9042, 116.4074, 0, &la, &lo, NULL);
        double shift = nm_haversine_m(39.9042, 116.4074, la, lo);
        check(shift < 5.0, "CGCS2000 相对 WGS84 偏移 < 5 m（近等）");
    }

    /* 4) 北京54 / 西安80 往返可逆（变换自身一致） */
    {
        double e = roundtrip_err(DATUM_BJ54, 39.9042, 116.4074);
        check(e < 1e-9, "北京54 往返可逆（误差 < 1e-9 度）");
        e = roundtrip_err(DATUM_XA80, 23.1291, 113.2644);
        check(e < 1e-9, "西安80 往返可逆（误差 < 1e-9 度）");
    }

    /* 5) 北京54 / 西安80 产生合理非零偏移
     *    量级按物理事实定，不拍脑袋：
     *      - BJ54：原点在 Pulkovo 系、椭球比 WGS84 大 108 m ⇒ 全国范围偏移上百米。
     *      - XA80：准地心、椭球与 WGS84 只差 3 m；EPSG 未登记其基准平移，
     *              PROJ 按"仅换椭球"处理 ⇒ 偏移只有几米，**必然远小于 BJ54**。 */
    {
        double la, lo;
        datum_wgs84_to(DATUM_BJ54, 39.9042, 116.4074, 0, &la, &lo, NULL);
        double dbj = nm_haversine_m(39.9042, 116.4074, la, lo);
        printf("    北京54 @北京: 偏移 %.1f m\n", dbj);
        check(dbj > 10.0 && dbj < 3000.0, "北京54 偏移合理且非零（10 m < d < 3000 m）");

        double hxa = 0;
        datum_wgs84_to(DATUM_XA80, 23.1291, 113.2644, 0, &la, &lo, &hxa);
        double dxa = nm_haversine_m(23.1291, 113.2644, la, lo);
        printf("    西安80 @广州: 水平偏移 %.3f m, 高程差 %.3f m\n", dxa, hxa);
        check(dxa < 5.0, "西安80 水平偏移 < 5 m（EPSG 未登记基准平移）");

        /* 负向对照：XA80 必须显著小于 BJ54（若把两个基准的参数抄成同一份会 FAIL） */
        check(dxa < dbj * 0.5, "负向对照: 西安80 偏移 << 北京54（参数未被抄混）");

        /* 负向对照：WGS84 必须（在浮点噪声内）零偏移 —— 若某天给 WGS84 也加了
         * 平移，这条会 FAIL。用 1 µm 容差而不是 == 0.0（ECEF 往返有 ~1e-9 m 噪声）。 */
        datum_wgs84_to(DATUM_WGS84, 39.9042, 116.4074, 0, &la, &lo, NULL);
        double dw = nm_haversine_m(39.9042, 116.4074, la, lo);
        printf("    WGS84 自往返: 偏移 %.3e m\n", dw);
        check(dw < 1e-6, "负向对照: WGS84 偏移 ≈ 0（< 1 µm）");
    }

    if (g_fail == 0) {
        printf("DATUM-OK  枚举/恒等/往返/偏移全部通过\n");
        return 0;
    }
    printf("DATUM-FAIL  %d 项未通过\n", g_fail);
    return 1;
}
