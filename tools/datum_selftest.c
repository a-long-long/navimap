/* 坐标系（大地基准）自测：验证 datum 模块的几个不变量。
 *
 * 判据（全绿即 DATUM-OK）：
 *   1) 枚举/名字映射自洽：datum_by_index / datum_short / datum_label 与 DatumId 一致。
 *   2) WGS84 是恒等：wgs84_to(WGS84,x) == x，to_wgs84(WGS84,x) == x（精确）。
 *   3) CGCS2000 ≈ WGS84：往返（wgs84_to → to_wgs84）与原点差 < 1e-9 度（亚毫米）。
 *   4) 北京54 / 西安80 往返一致：wgs84_to → to_wgs84 回到原点 < 1e-9 度（变换可逆）。
 *      往返必须把 h 原样带回，否则测的是"丢了高程"而不是"不可逆"。
 *   5) ★ 西安80 的位移必须落进**国家测绘地理信息局公布的图幅平移量区间**：
 *      "1980西安坐标系下的 X 平移量为 −9~+43 m，Y 方向的平移量为 +76~+119 m"
 *      （X=北向、Y=东向）。取 6 个分布点全中 ⇒ 同时钉住数值与方向。
 *   6) 北京54 产生合理非零偏移（量级判据；它与官方口径的偏差如实打印）。
 *   7) ★ 西安80 的**实现**必须与《国土资源数据 2000 国家大地坐标系转换技术要求》
 *      （国土资源部/国家测绘地理信息局 2017-02）附录 B 的改正量公式等价（< 0.05 m，
 *      实测 0.001 m）。这是第三方交叉验证：两条独立算法路径一致 ⇒ 平移符号、
 *      椭球、正/反算都没搞反。参数从模块读出，故调参数不会误报。
 *
 * 负向对照（都必须能失败）：
 *   - 若把 XA80 的平移参数清零（退化成"仅换椭球"——踩过这个坑）⇒ 东向掉到
 *     ~0.02 m，第 5 条立刻越界 FAIL。
 *   - 若把 XA80 / BJ54 的参数抄成同一份 ⇒「两者位移显著不同」FAIL。
 *   - 若给 WGS84 也错误地加了平移 ⇒「WGS84 偏移 ≈ 0」FAIL。
 *   - 若在 datum.c 里把「加平移」那一步去掉（apply_fwd 退化成恒等）⇒ 第 7 条
 *     两侧差出 ~114 m，立刻 FAIL（第 5 条也会跟着 FAIL）。
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

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* 把"基准 → WGS84"的经纬度差换算成当地东向/北向米数。
 * 口径与论文/国测局文件一致：北向 = Δφ·111320，东向 = Δλ·111320·cos φ。 */
static void enu_shift(DatumId id, double lat, double lon, double *east, double *north)
{
    double la2, lo2;
    datum_to_wgs84(id, lat, lon, 0, &la2, &lo2, NULL);
    double mid = (lat + la2) * 0.5 * M_PI / 180.0;
    *north = (la2 - lat) * 111320.0;
    *east  = (lo2 - lon) * 111320.0 * cos(mid);
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

    /* 5) 西安80：★ 硬判据 —— 拿国家测绘地理信息局公布的图幅平移量区间来核对。
     *    《2000国家大地坐标系与现行坐标系关系》：
     *      "1980西安坐标系下的 X 平移量为 −9~+43 m，Y 方向的平移量为 +76~+119 m"
     *      （范围 56°N~16°N / 72°E~135°E，不考虑椭球差异；X=北向、Y=东向）
     *    本模块用的是赵玉明等《西安1980坐标转换为WGS1984坐标的3参数近似估算方法》
     *    （中国水土保持科学 2013, 11(3): 78-83）拟合的全国统一值
     *    dX,dY,dZ = (−104.57, −45.94, +5.30)，方向 [XYZ]84 = [XYZ]80 + d。
     *    ⇒ 六点全中，就同时验证了**数值与方向**。
     *    ★ 负向对照：若把平移清零（退化成"仅换椭球"，这是踩过的坑），东向会掉到
     *      ~0.02 m，立刻越界 FAIL。 */
    {
        static const struct { const char *name; double la, lo; } PTS[] = {
            { "泾阳/大地原点", 34.5333, 108.9167 },
            { "北京",          39.9042, 116.4074 },
            { "广州",          23.1291, 113.2644 },
            { "上海",          31.2304, 121.4737 },
            { "乌鲁木齐",      43.8256,  87.6168 },
            { "成都",          30.5728, 104.0668 },
        };
        const int NPT = (int)(sizeof(PTS) / sizeof(PTS[0]));
        int ok = 1;
        double dxa = 0;
        for (int i = 0; i < NPT; i++) {
            double e, n;
            enu_shift(DATUM_XA80, PTS[i].la, PTS[i].lo, &e, &n);
            int in = (e >= 76.0 && e <= 119.0) && (n >= -9.0 && n <= 43.0);
            printf("    西安80 %-14s 东向 %7.1f m  北向 %7.1f m%s\n",
                   PTS[i].name, e, n, in ? "" : "   <-- 越界");
            if (!in) ok = 0;
            if (i == 1) dxa = sqrt(e * e + n * n);   /* 北京，用于与 BJ54 比较 */
        }
        check(ok, "西安80: 六点位移全落国测局区间(东76~119/北−9~43)");

        /* 6) 北京54：量级判据 + 与官方口径的已知偏差（如实打印，不断言相符）。
         *    用的是 EPSG:4214 默认 TOWGS84 (15.8,−154.4,−82.3)（PROJ/GDAL 同款），
         *    但它登记适用区是塔里木盆地西南 ⇒ 华北会偏，这里只卡量级。 */
        double la, lo;
        datum_wgs84_to(DATUM_BJ54, 39.9042, 116.4074, 0, &la, &lo, NULL);
        double dbj = nm_haversine_m(39.9042, 116.4074, la, lo);
        printf("    北京54 @北京: 水平偏移 %.1f m\n", dbj);
        check(dbj > 10.0 && dbj < 500.0, "北京54 偏移合理且非零（10 m < d < 500 m）");

        /* 负向对照：两个参心基准的参数若被抄成同一份，位移会雷同 ⇒ FAIL。 */
        check(fabs(dxa - dbj) > 20.0, "负向对照: 西安80/北京54 位移显著不同（参数未抄混）");

        /* 负向对照：WGS84 必须（浮点噪声内）零偏移 —— 若某天给 WGS84 也加了
         * 平移，这条会 FAIL。用 1 µm 容差而不是 == 0.0（ECEF 往返有 ~1e-9 m 噪声）。 */
        datum_wgs84_to(DATUM_WGS84, 39.9042, 116.4074, 0, &la, &lo, NULL);
        double dw = nm_haversine_m(39.9042, 116.4074, la, lo);
        printf("    WGS84 自往返: 偏移 %.3e m\n", dw);
        check(dw < 1e-6, "负向对照: WGS84 偏移 ≈ 0（< 1 µm）");
    }

    /* 7) ★ 第三方等价性：用**国标算法**独立复算西安80 → 地心系，与本模块比对。
     *    依据《国土资源数据 2000 国家大地坐标系转换技术要求》（国土资源部 /
     *    国家测绘地理信息局，2017-02）附录 B「坐标转换改正量计算」：
     *        dB = −(Δx/M)·sinB·cosL − (Δy/M)·sinB·sinL + (Δz/M)·cosB
     *             + (1/M)·[ (e²/W)·Δa + N/(2W²)·(2 − e²sin²B)·Δe² ]·sinB·cosB
     *        dL = −(1/(N·cosB))·(Δx·sinL − Δy·cosL)
     *        W = sqrt(1 − e²sin²B),  N = a/W,  M = a(1−e²)/W³
     *        Δa = a_2000 − a_80,  Δe² = e²_2000 − e²_80
     *    这是"我的严格 ECEF 实现" vs "国标线性化公式"两条独立路径的等价判据
     *    （与渲染侧的 blend_equiv / clip_equiv 同类）——比自测自己强得多：
     *    两条路径一致 ⇒ 平移符号、椭球、正/反算都没搞反。
     *    ★ 参数从模块里读（datum_translation / datum_ellipsoid），不是抄一份常数
     *      ⇒ 以后调参数不会误报，只有**算法**错才 FAIL。
     *    实测 9 点最大差 0.001 m（= 线性化残差 + CGCS2000/WGS84 椭球末位差）。
     *    容差取 0.05 m：宽松到不测浮点噪声，严到足以抓住符号/椭球/漏平移的错误。 */
    {
        double dx, dy, dz, a80, f80;
        datum_translation(DATUM_XA80, &dx, &dy, &dz);
        datum_ellipsoid(DATUM_XA80, &a80, &f80);
        const double A00 = 6378137.0, F00 = 1.0 / 298.257222101;  /* CGCS2000 */

        static const struct { const char *name; double la, lo; } PTS[] = {
            { "泾阳/大地原点", 34.5333, 108.9167 },
            { "北京",          39.9042, 116.4074 },
            { "广州",          23.1291, 113.2644 },
            { "上海",          31.2304, 121.4737 },
            { "乌鲁木齐",      43.8256,  87.6168 },
            { "成都",          30.5728, 104.0668 },
            { "哈尔滨",        45.8038, 126.5349 },
            { "拉萨",          29.6520,  91.1721 },
            { "三亚",          18.2528, 109.5119 },
        };
        const int NPT = (int)(sizeof(PTS) / sizeof(PTS[0]));
        double worst = 0.0;

        for (int i = 0; i < NPT; i++) {
            const double B = PTS[i].la, L = PTS[i].lo;
            /* (a) 本模块：严格 ECEF —— IAG75 正算 → 加平移 → WGS84 椭球反算 */
            double b1, l1;
            datum_to_wgs84(DATUM_XA80, B, L, 0, &b1, &l1, NULL);

            /* (b) 国标：线性化改正量公式（XA80 → CGCS2000） */
            const double la = B * M_PI / 180.0, lo = L * M_PI / 180.0;
            const double E00 = F00 * (2.0 - F00);
            const double E80 = f80 * (2.0 - f80);
            const double sl = sin(la), cl = cos(la);
            const double W = sqrt(1.0 - E00 * sl * sl);
            const double N = A00 / W, M = A00 * (1.0 - E00) / (W * W * W);
            const double da = A00 - a80, de2 = E00 - E80;
            const double dB = -dx / M * sl * cos(lo) - dy / M * sl * sin(lo)
                              + dz / M * cl
                              + (1.0 / M) * ((E00 / W) * da
                                             + N / (2.0 * W * W)
                                               * (2.0 - E00 * sl * sl) * de2)
                                * sl * cl;
            const double dL = -(1.0 / (N * cl)) * (dx * sin(lo) - dy * cos(lo));
            const double b2 = B + dB * 180.0 / M_PI, l2 = L + dL * 180.0 / M_PI;

            /* 两套结果都换算成当地东/北米数再比（口径同 #5） */
            double e1 = (l1 - L) * 111320.0 * cos((B + b1) * 0.5 * M_PI / 180.0);
            double n1 = (b1 - B) * 111320.0;
            double e2 = (l2 - L) * 111320.0 * cos((B + b2) * 0.5 * M_PI / 180.0);
            double n2 = (b2 - B) * 111320.0;
            double d = sqrt((e1 - e2) * (e1 - e2) + (n1 - n2) * (n1 - n2));
            if (d > worst) worst = d;
        }
        printf("    西安80 严格ECEF vs 国标附录B公式: 9 点最大差 %.4f m\n", worst);
        check(worst < 0.05, "西安80: 实现与国标附录B改正量公式等价（< 0.05 m）");
    }

    if (g_fail == 0) {
        printf("DATUM-OK  枚举/恒等/往返/偏移/国标等价 全部通过\n");
        return 0;
    }
    printf("DATUM-FAIL  %d 项未通过\n", g_fail);
    return 1;
}
