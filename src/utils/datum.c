/* 坐标系（大地基准）变换：WGS84 / CGCS2000 / 北京1954 / 西安1980。
 *
 * 数学：经纬度 ⇄ ECEF（地心地固直角坐标）在各自椭球上互算，再用 7 参数
 * 赫尔默特（Helmert）变换在 ECEF 空间里把"本地基准"对齐到 WGS84。
 * 本项目只用平移 3 参数（旋转/尺度=0），旋转/尺度字段保留以便将来接精确 7 参数。
 *
 * 参数来源（★ 只登记"有出处"的值，宁可退化为椭球变换也不放编造的常数）：
 *   - WGS84：恒等。
 *   - CGCS2000：a 与 WGS84 完全相同，f 仅末位不同；框架差亚米级。
 *     ⇒ 平移取 0（与 EPSG/PROJ 对 CGCS2000↔WGS84 的处理一致）。
 *   - 北京 1954：EPSG:15921「Beijing 1954 to WGS 84 (4)」，地心三参数
 *     (dX,dY,dZ) = (+15.8, −154.4, −82.3) m，标称精度 1 m，
 *     适用区 China – Tarim。**全国通用近似值**，其它地区残差可达数十米。
 *   - 西安 1980：★ EPSG 数据集**没有登记** XA80 ↔ WGS84 的任何变换
 *     （PROJ/GDAL 因此退化为"仅换椭球、基准平移按 0 处理"，精度标 unknown）。
 *     ⇒ 这里同样取平移 0，只做椭球变换（IAG75 ↔ WGS84），实测偏移约 2~3 m。
 *       这与主流工具链行为一致；若手上有本测区的 CORS 三/七参数，改下面这一行即可。
 *
 * ★ 局限（必须让用户知道）：BJ54/XA80 的基准平移本质是**区域拟合量**，不存在
 *   全国唯一的正确常数。本模块提供的是"量级正确、可复现"的近似，用于经纬网/
 *   读数的基准呈现；工程测量请用当地测绘部门公布的参数替换 DATUMS 表。
 */

#include "utils/datum.h"

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
    double a;   /* 长半轴（米） */
    double f;   /* 扁率 = (a-b)/a */
} Ellipsoid;

/* 7 参数（基准 → WGS84，ECEF 平移 dx,dy,dz 米；旋转 rx,ry,rz 角秒；尺度 ds ppm）。
 * 本项目 R=I、ds=0，只剩平移。 */
typedef struct {
    Ellipsoid ell;
    double dx, dy, dz;     /* 米 */
    double rx, ry, rz;     /* 角秒（本项目恒 0） */
    double ds;             /* ppm（本项目恒 0） */
} DatumDef;

static const DatumDef DATUMS[DATUM_COUNT] = {
    /* WGS84：恒等 */
    { {6378137.0, 1.0 / 298.257223563}, 0, 0, 0, 0, 0, 0, 0 },
    /* CGCS2000：椭球与 WGS84 近等，平移 0 ⇒ 与 WGS84 差 < 2 m */
    { {6378137.0, 1.0 / 298.257222101}, 0, 0, 0, 0, 0, 0, 0 },
    /* 北京 1954（克拉索夫斯基 1940 椭球）。EPSG:15921 地心三参数（米）。 */
    { {6378245.0, 1.0 / 298.3}, 15.8, -154.4, -82.3, 0, 0, 0, 0 },
    /* 西安 1980（IAG-75 椭球）。EPSG 未登记 XA80↔WGS84 ⇒ 平移 0，只换椭球。
     * 实测后果（自测 datum_selftest 打印）：水平差 0.019 m、高程差 −2.996 m。
     * 即 IAG75 几乎是 WGS84 整体放大 3 m（a +3.00、b +2.97）——均匀缩放不改大地
     * 经纬度，只改高程。所以**本实现下西安80 的经纬网/读数与 WGS84 几乎重合**。
     * 真实业务里 XA80 与 WGS84 差几十米，那是"区域基准平移"，EPSG 没发布全国值。 */
    { {6378140.0, 1.0 / 298.257}, 0.0, 0.0, 0.0, 0, 0, 0, 0 },
};

/* 大地坐标 → ECEF（在给定椭球上） */
static void geod_to_ecef(double lat_deg, double lon_deg, double h,
                         const Ellipsoid *e, double *X, double *Y, double *Z)
{
    double la = lat_deg * M_PI / 180.0, lo = lon_deg * M_PI / 180.0;
    double e2 = e->f * (2.0 - e->f);
    double sl = sin(la), cl = cos(la);
    double N = e->a / sqrt(1.0 - e2 * sl * sl);
    *X = (N + h) * cl * cos(lo);
    *Y = (N + h) * cl * sin(lo);
    *Z = (N * (1.0 - e2) + h) * sl;
}

/* ECEF → 大地坐标（Bowring 闭式反算，精度 ~mm） */
static void ecef_to_geod(double X, double Y, double Z,
                         const Ellipsoid *e, double *lat_deg, double *lon_deg, double *h)
{
    *lon_deg = atan2(Y, X);
    double b = e->a * (1.0 - e->f);
    double e2 = e->f * (2.0 - e->f);
    double ep2 = (e->a * e->a - b * b) / (b * b);
    double p = sqrt(X * X + Y * Y);
    double theta = atan2(Z * e->a, p * b);
    double st = sin(theta), ct = cos(theta);
    *lat_deg = atan2(Z + ep2 * b * st * st * st, p - e2 * e->a * ct * ct * ct);
    double N = e->a / sqrt(1.0 - e2 * sin(*lat_deg) * sin(*lat_deg));
    if (h) *h = p / cos(*lat_deg) - N;
    *lat_deg *= 180.0 / M_PI;
    *lon_deg *= 180.0 / M_PI;
}

/* 基准 → WGS84：Xw = (1+ds)·R·Xs + T；本项目 R=I, ds=0 ⇒ Xw = Xs + T */
static void apply_fwd(const DatumDef *d,
                      double Xs, double Ys, double Zs,
                      double *Xw, double *Yw, double *Zw)
{
    (void)d; /* 旋转/尺度字段预留，当前恒 0 */
    *Xw = Xs + d->dx;
    *Yw = Ys + d->dy;
    *Zw = Zs + d->dz;
}

/* WGS84 → 基准：逆变换 Xs = Xw - T */
static void apply_inv(const DatumDef *d,
                      double Xw, double Yw, double Zw,
                      double *Xs, double *Ys, double *Zs)
{
    (void)d;
    *Xs = Xw - d->dx;
    *Ys = Yw - d->dy;
    *Zs = Zw - d->dz;
}

void datum_to_wgs84(DatumId id, double lat, double lon, double h,
                    double *olat, double *olon, double *oh)
{
    if (id < 0 || id >= DATUM_COUNT) id = DATUM_WGS84;
    const DatumDef *d = &DATUMS[id];
    const DatumDef *w = &DATUMS[DATUM_WGS84];
    double Xs, Ys, Zs, Xw, Yw, Zw;
    geod_to_ecef(lat, lon, h, &d->ell, &Xs, &Ys, &Zs);
    apply_fwd(d, Xs, Ys, Zs, &Xw, &Yw, &Zw);
    ecef_to_geod(Xw, Yw, Zw, &w->ell, olat, olon, oh);
}

void datum_wgs84_to(DatumId id, double lat, double lon, double h,
                    double *olat, double *olon, double *oh)
{
    if (id < 0 || id >= DATUM_COUNT) id = DATUM_WGS84;
    const DatumDef *d = &DATUMS[id];
    const DatumDef *w = &DATUMS[DATUM_WGS84];
    double Xw, Yw, Zw, Xs, Ys, Zs;
    geod_to_ecef(lat, lon, h, &w->ell, &Xw, &Yw, &Zw);
    apply_inv(d, Xw, Yw, Zw, &Xs, &Ys, &Zs);
    ecef_to_geod(Xs, Ys, Zs, &d->ell, olat, olon, oh);
}

const char *datum_short(DatumId id)
{
    switch (id) {
        case DATUM_WGS84:    return "WGS84";
        case DATUM_CGCS2000: return "CGCS2000";
        case DATUM_BJ54:     return "北京54";
        case DATUM_XA80:     return "西安80";
        default:             return "?";
    }
}

const char *datum_label(DatumId id)
{
    switch (id) {
        case DATUM_WGS84:    return "WGS84（默认）";
        case DATUM_CGCS2000: return "CGCS2000（2000国家大地坐标系）";
        case DATUM_BJ54:     return "北京 1954";
        case DATUM_XA80:     return "西安 1980";
        default:             return "?";
    }
}

const char *datum_note(DatumId id)
{
    switch (id) {
        case DATUM_WGS84:
            return "OSM 原生基准，恒等（不换算）";
        case DATUM_CGCS2000:
            return "与 WGS84 近等：椭球 a 相同、f 仅末位不同，差异亚米级";
        case DATUM_BJ54:
            return "克拉索夫斯基椭球 + EPSG:15921 地心三参数"
                   "（全国近似，精度约 1 m@塔里木，其它地区残差可达数十米）";
        case DATUM_XA80:
            return "IAG-75 椭球。EPSG 未登记 XA80↔WGS84 基准平移，故只换椭球："
                   "经纬度差 <1 m、高程约 −3 m；要本测区参数请改 datum.c 的 DATUMS 表";
        default:
            return "";
    }
}

int datum_count(void) { return DATUM_COUNT; }
DatumId datum_by_index(int i)
{
    if (i < 0 || i >= DATUM_COUNT) return DATUM_WGS84;
    return (DatumId)i;
}
