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
 *   8) 完整 7 参数接口：读/写/reset 自洽；**填非零旋转+尺度后往返仍严格可逆**
 *      （逆变换用矩阵逆，不是"减平移"——后者在有旋转时就不成立）。
 *   9) 入口①：重合点解 7 参数（官方附录A「重合点法」）—— 用模块自己正算生成的
 *      合成点反解，应还原出厂参数（< 1 mm）；污染一点后必须明显偏离。
 *  10) 入口②：图幅改正量格网 —— 常量格网位移=改正量、线性场双线性内插精确、
 *      格网外 clamp 不外推、文件加载生效、clear 后完全恢复、挂格网时正/反自洽。
 *  11) 入口③：局部已知偏移 —— 设"北 +20 m / 东 −30 m"后该点实测就是这么多。
 *
 * ★ 8~11 是三个扩展入口的**接口验收**：现在没有真实数据，但接口必须随时可用，
 *   将来拿到重合点 / 图幅改正量 / 局部经验值，喂进来就能生效，不用再改算法。
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
#include "utils/plat.h"  /* nm_config_dir（临时格网文件） */

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

/* 精确版 ENU 位移：用 WGS84 椭球的子午圈/卯酉圈曲率半径把经纬度差换成米。
 * ★ 为什么要有两套口径：enu_shift 用 111320 的近似，是为了与论文/国测局文件
 *   （"图幅平移量 X/Y"）的口径一致——那两条判据要跟公布的区间对齐，必须同口径。
 *   但格网判据要验证的是"改正量米数是否被准确施加"，若仍用 111320，
 *   中纬度 0.4% 的系统差（2.5 m 上就是 0.010 m）会盖住真正的内插误差，
 *   所以这里改跟 datum.c 内部shift_by_meters 同一口径（真实 M / N）。 */
static void enu_shift_exact(DatumId id, double lat, double lon, double *east, double *north)
{
    double la2, lo2, a, f;
    datum_to_wgs84(id, lat, lon, 0, &la2, &lo2, NULL);
    datum_ellipsoid(DATUM_WGS84, &a, &f);
    const double e2 = f * (2.0 - f);
    const double la = (lat + la2) * 0.5 * M_PI / 180.0;
    const double sl = sin(la), cl = cos(la);
    const double W = sqrt(1.0 - e2 * sl * sl);
    const double N = a / W;                            /* 卯酉圈 */
    const double M = a * (1.0 - e2) / (W * W * W);     /* 子午圈 */
    *north = (la2 - lat) * (M_PI / 180.0) * M;
    *east  = (lo2 - lon) * (M_PI / 180.0) * N * cl;
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

    /* 8) 完整 7 参数接口：读/写/reset 自洽；非零旋转+尺度下仍严格可逆。
     *    ★ 逆变换用的是**矩阵逆**而不是"减平移" ⇒ 一旦填了旋转/尺度，
     *      "减平移"就不再成立，这条能抓住那个错误。 */
    {
        DatumSevenParam p;
        datum_get_seven_param(DATUM_XA80, &p);
        check(fabs(p.dx + 104.57) < 1e-9 && fabs(p.dy + 45.94) < 1e-9 &&
              fabs(p.dz - 5.30) < 1e-9 && p.rx == 0.0 && p.ry == 0.0 &&
              p.rz == 0.0 && p.ds == 0.0, "7参数读出 = 出厂三参数（旋转/尺度为 0）");

        double e0, n0;
        enu_shift(DATUM_XA80, 39.9042, 116.4074, &e0, &n0);
        check(datum_set_seven_param(DATUM_XA80, &p) == 0, "set_seven_param 返回 0");
        double e1, n1;
        enu_shift(DATUM_XA80, 39.9042, 116.4074, &e1, &n1);
        check(fabs(e1 - e0) < 1e-9 && fabs(n1 - n0) < 1e-9, "填回同一份参数：位移逐位不变");
        check(datum_set_seven_param(DATUM_WGS84, &p) == -1, "负向对照: WGS84 不许改参数");

        DatumSevenParam q;
        q.dx = 30.0; q.dy = -20.0; q.dz = 15.0;
        q.rx = 0.8; q.ry = -1.2; q.rz = 0.5;   /* 角秒 */
        q.ds = 2.0;                             /* ppm */
        datum_set_seven_param(DATUM_XA80, &q);
        double r1 = roundtrip_err(DATUM_XA80, 39.9042, 116.4074);
        double r2 = roundtrip_err(DATUM_XA80, 23.1291, 113.2644);
        printf("    全 7 参数往返误差: %.3e / %.3e 度\n", r1, r2);
        check(r1 < 1e-9 && r2 < 1e-9, "非零旋转+尺度下往返仍严格可逆（逆=矩阵逆）");
        datum_reset_params(DATUM_XA80);
        double e2, n2;
        enu_shift(DATUM_XA80, 39.9042, 116.4074, &e2, &n2);
        check(fabs(e2 - e0) < 1e-9 && fabs(n2 - n0) < 1e-9, "reset 恢复出厂位移");
    }

    /* 9) 入口①：重合点解 7 参数（官方附录A「重合点法」）。
     *    用**模块自己正算**生成的合成点反解 ⇒ 应还原出厂参数（这是解算器
     *    正确性的强判据：设计矩阵、列均衡、符号约定全对才能还原）。
     *    ★ 负向对照：污染一个点后解算必须明显偏离 —— 证明解算器真的在读点，
     *      而不是恒返回某个常数。 */
    {
        static const double PTS[][2] = {
            { 34.5333, 108.9167 }, { 39.9042, 116.4074 }, { 23.1291, 113.2644 },
            { 31.2304, 121.4737 }, { 43.8256,  87.6168 }, { 30.5728, 104.0668 },
            { 45.8038, 126.5349 }, { 29.6520,  91.1721 },
        };
        const int NPT = (int)(sizeof(PTS) / sizeof(PTS[0]));
        DatumTie ties[16];
        for (int i = 0; i < NPT; i++) {
            double la, lo, h;
            datum_wgs84_to(DATUM_XA80, PTS[i][0], PTS[i][1], 0, &la, &lo, &h);
            ties[i].lat_src = la;  ties[i].lon_src = lo;  ties[i].h_src = h;
            ties[i].lat_wgs = PTS[i][0]; ties[i].lon_wgs = PTS[i][1]; ties[i].h_wgs = 0.0;
        }
        DatumSevenParam s;
        double rms = 0;
        int rc = datum_solve_seven_param(DATUM_XA80, ties, NPT, &s, &rms);
        printf("    解算: d=(%.4f, %.4f, %.4f)  r=(%.2e, %.2e, %.2e)″  ds=%.2e ppm  RMS=%.2e m\n",
               s.dx, s.dy, s.dz, s.rx, s.ry, s.rz, s.ds, rms);
        check(rc == 0, "solve 返回 0");
        double dd = sqrt((s.dx + 104.57) * (s.dx + 104.57) +
                         (s.dy + 45.94) * (s.dy + 45.94) +
                         (s.dz - 5.30) * (s.dz - 5.30));
        check(dd < 1e-3, "解算还原出厂平移（误差 < 1 mm）");
        check(fabs(s.rx) < 1e-6 && fabs(s.ry) < 1e-6 && fabs(s.rz) < 1e-6 &&
              fabs(s.ds) < 1e-6, "解算出的旋转/尺度 ≈ 0（数据本无旋转）");
        check(rms < 1e-6, "残差 RMS ≈ 0（合成数据自洽）");

        check(datum_solve_seven_param(DATUM_XA80, ties, 2, &s, NULL) == -1, "n < 3 返回 -1");

        DatumTie same[4];
        for (int i = 0; i < 4; i++) same[i] = ties[0];
        check(datum_solve_seven_param(DATUM_XA80, same, 4, &s, NULL) == -2,
              "点全部重合（法方程奇异）返回 -2");

        ties[NPT - 1].lat_wgs += 300.0 / 111320.0;   /* 把一个点挪 300 m */
        DatumSevenParam s2;
        double rms2 = 0;
        datum_solve_seven_param(DATUM_XA80, ties, NPT, &s2, &rms2);
        double dev = sqrt((s2.dx - s.dx) * (s2.dx - s.dx) +
                          (s2.dy - s.dy) * (s2.dy - s.dy) +
                          (s2.dz - s.dz) * (s2.dz - s.dz));
        printf("    污染一点后: 解算偏离 %.2f m, RMS %.2f m\n", dev, rms2);
        check(dev > 10.0, "负向对照: 污染一点后解算明显偏离（解算器真的在读点）");
        check(rms2 > 10.0, "负向对照: 污染后残差 RMS 显著变大");
    }

    /* 10) 入口②：图幅改正量格网（官方 1:5万 流程：改正量 + 双线性内插） */
    {
        double e_base, n_base;
        enu_shift_exact(DATUM_XA80, 31.0, 112.0, &e_base, &n_base);

        /* (a) 常量格网 2x2：任何内部点的位移都应恰好 +50 北 / +100 东 */
        float dn[4], de[4];
        for (int i = 0; i < 4; i++) { dn[i] = 50.0f; de[i] = 100.0f; }
        DatumGridDesc g;
        g.lat0 = 30.0; g.lon0 = 110.0; g.dlat = 5.0; g.dlon = 5.0;
        g.nrows = 2; g.ncols = 2; g.dn = dn; g.de = de;
        check(datum_grid_set(DATUM_XA80, &g) == 0, "grid_set 返回 0");
        check(datum_grid_active(DATUM_XA80) == 1, "grid_active == 1");
        double e1, n1;
        enu_shift_exact(DATUM_XA80, 31.0, 112.0, &e1, &n1);
        printf("    常量格网 @(31,112): Δ北 %.3f m  Δ东 %.3f m（期望 +50 / +100）\n",
               n1 - n_base, e1 - e_base);
        check(fabs((n1 - n_base) - 50.0) < 0.5 && fabs((e1 - e_base) - 100.0) < 0.5,
              "常量格网：位移 = 改正量（±0.5 m）");

        /* (b) 格网外 clamp 到边缘值（不崩、不外推） */
        double e2, n2;
        enu_shift_exact(DATUM_XA80, 40.0, 112.0, &e2, &n2);
        double e2b, n2b;
        datum_grid_clear(DATUM_XA80);
        enu_shift_exact(DATUM_XA80, 40.0, 112.0, &e2b, &n2b);
        check(fabs((n2 - n2b) - 50.0) < 0.5 && fabs((e2 - e2b) - 100.0) < 0.5,
              "格网外 clamp 到边缘值（不外推）");

        /* (c) 线性格网 3x3：双线性内插必须**精确**重现线性场 */
        float dn3[9], de3[9];
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) { dn3[i * 3 + j] = (float)(i * 10); de3[i * 3 + j] = (float)(j * 5); }
        g.lat0 = 30.0; g.lon0 = 110.0; g.dlat = 1.0; g.dlon = 1.0;
        g.nrows = 3; g.ncols = 3; g.dn = dn3; g.de = de3;
        datum_grid_set(DATUM_XA80, &g);
        double e3, n3;
        enu_shift_exact(DATUM_XA80, 30.25, 110.40, &e3, &n3);   /* v=0.25 → dn=2.5；u=0.40 → de=2.0 */
        datum_grid_clear(DATUM_XA80);
        double e3b, n3b;
        enu_shift_exact(DATUM_XA80, 30.25, 110.40, &e3b, &n3b);
        printf("    线性格网 @(30.25,110.40): Δ北 %.6f m（期望 2.5）  Δ东 %.6f m（期望 2.0）\n",
               n3 - n3b, e3 - e3b);
        check(fabs((n3 - n3b) - 2.5) < 0.01 && fabs((e3 - e3b) - 2.0) < 0.01,
              "线性场双线性内插精确（±0.01 m）");

        /* (d) 从文件加载（拿到改正量数据后转成这个格式即可用） */
        char path[512];
        snprintf(path, sizeof path, "%s/datum_grid_test.txt", nm_config_dir());
        FILE *f = fopen(path, "w");
        if (f) {
            fprintf(f, "# 改正量测试格网：北 +12.5 m / 东 -37.5 m\n");
            fprintf(f, "31.0 111.0 1.0 1.0 2 2\n");
            for (int i = 0; i < 4; i++) fprintf(f, "12.5 -37.5\n");
            fclose(f);
            check(datum_grid_load(DATUM_XA80, path) == 0, "grid_load 返回 0");
            double e4, n4;
            enu_shift_exact(DATUM_XA80, 31.5, 111.5, &e4, &n4);
            datum_grid_clear(DATUM_XA80);
            double e4b, n4b;
            enu_shift_exact(DATUM_XA80, 31.5, 111.5, &e4b, &n4b);
            printf("    文件加载 @(31.5,111.5): Δ北 %.3f m  Δ东 %.3f m（期望 +12.5 / -37.5）\n",
                   n4 - n4b, e4 - e4b);
            check(fabs((n4 - n4b) - 12.5) < 0.5 && fabs((e4 - e4b) + 37.5) < 0.5,
                  "文件加载的格网生效（±0.5 m）");
            check(datum_grid_load(DATUM_XA80, "/nonexistent/x.txt") == -1,
                  "负向对照: 打不开的文件返回 -1");
            remove(path);
        } else {
            check(0, "临时格网文件可写");
        }

        /* (e) clear 后必须完全恢复；挂格网时正/反仍自洽 */
        double e5, n5;
        enu_shift_exact(DATUM_XA80, 31.0, 112.0, &e5, &n5);
        check(fabs(e5 - e_base) < 1e-9 && fabs(n5 - n_base) < 1e-9,
              "grid_clear 后位移完全恢复");
        datum_grid_set(DATUM_XA80, &g);
        double rt = roundtrip_err(DATUM_XA80, 30.25, 110.40);
        printf("    挂格网时往返误差: %.3e 度\n", rt);
        check(rt < 1e-6, "挂格网时正/反仍自洽（< 1e-6 度 ≈ 11 cm）");
        datum_grid_clear(DATUM_XA80);
        check(datum_grid_active(DATUM_XA80) == 0, "clear 后 active == 0");
    }

    /* 11) 入口③：局部已知偏移（"我们这儿东偏 110 m"这类经验值） */
    {
        double e0, n0;
        enu_shift_exact(DATUM_XA80, 39.9042, 116.4074, &e0, &n0);
        check(datum_set_local_offset(DATUM_XA80, 39.9042, 116.4074, 20.0, -30.0) == 0,
              "set_local_offset 返回 0");
        double e1, n1;
        enu_shift_exact(DATUM_XA80, 39.9042, 116.4074, &e1, &n1);
        /* ★ 语义：set_local_offset 是**替换**出厂平移，使该点的**绝对**位移
         *   恰好等于设定值 —— 不是"在出厂位移上再叠加"。所以比的是 e1/n1
         *   本身，不是 e1−e0。（n0 只有 0.6 m，用差值会碰巧看着对，是陷阱。） */
        printf("    局部偏移 @北京: 位移 北 %+.3f m（设 +20）  东 %+.3f m（设 -30）\n", n1, e1);
        check(fabs(n1 - 20.0) < 0.5 && fabs(e1 + 30.0) < 0.5,
              "局部偏移：该点位移 = 设定值（±0.5 m）");
        check(datum_set_local_offset(DATUM_WGS84, 39.9, 116.4, 1, 1) == -1,
              "负向对照: WGS84 不许设偏移");
        datum_reset_params(DATUM_XA80);
        double e2, n2;
        enu_shift_exact(DATUM_XA80, 39.9042, 116.4074, &e2, &n2);
        check(fabs(e2 - e0) < 1e-9 && fabs(n2 - n0) < 1e-9, "reset 后恢复出厂位移");
    }

    if (g_fail == 0) {
        printf("DATUM-OK  枚举/恒等/往返/偏移/国标等价/三个扩展入口 全部通过\n");
        return 0;
    }
    printf("DATUM-FAIL  %d 项未通过\n", g_fail);
    return 1;
}
