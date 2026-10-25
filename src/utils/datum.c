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
 *     ⇒ 但**公开文献有**：赵玉明等《西安1980坐标转换为WGS1984坐标的3参数近似估算
 *       方法》（中国水土保持科学 2013, 11(3)）用全国 10 省 14 片区 61 个样点拟合出
 *       全国统一 3 参数 (−104.57, −45.94, +5.30) m，方向为 [XYZ]84 = [XYZ]80 + Δ
 *       （即本表的"基准→WGS84"语义，与 +towgs84 一致）。
 *       精度：分区参数平均经/纬向误差 < 10 m；全国统一参数平均经向 8.20 m、
 *       纬向 4.21 m，最大 14.73 m / 9.23 m。
 *     ⇒ **独立交叉验证**：国家测绘地理信息局《2000国家大地坐标系与现行坐标系关系》
 *       公布"1980西安坐标系转换到 2000 系，图幅平移量 X −9~+43 m、Y +76~+119 m"
 *       （56°N~16°N / 72°E~135°E，X=北向、Y=东向）。本参数在泾阳大地原点附近
 *       算得北向 +9.7 m、东向 +113.8 m，**两项都落在该区间内** ⇒ 数值与方向都可信。
 *
 * ★ 局限（必须让用户知道）：BJ54/XA80 的基准平移本质是**区域拟合量**，不存在
 *   全国唯一的正确常数。本模块提供的是"量级正确、可复现"的近似，用于经纬网/
 *   读数的基准呈现；工程测量请用当地测绘部门公布的参数替换 DATUMS 表。
 *
 * ★ 官方的正解（比本模块高一个量级，但**数据不公开**，这里做不到）：
 *   《国土资源数据 2000 国家大地坐标系转换技术要求》（国土资源部 / 国家测绘
 *   地理信息局，2017-02）规定：
 *     - 1:5 万及更小比例尺：用国家测绘地理信息局提供的「1:5 万 XA80→CGCS2000
 *       **图幅改正量**」，点位按**双线性内插**逐点改正（附录 B/C）；
 *     - 1:1 万：同上用 1:1 万改正量，或按 **2°×3° 分区**逐区解算转换参数；
 *     - 重合点法：用两坐标系下的重合点作控制点解参数（附录 A）；
 *     - 北京54：**两步法** —— BJ54 → XA80 → CGCS2000，改正量叠加
 *       （本模块 BJ54 是直接一步到 WGS84，没有走这条链）。
 *   图幅改正量由测绘部门按图幅/按县分发、不公开下载 ⇒ 没有它只能退到三参数。
 *
 * ★ 论文的 7 分区参数（Table 4）**评估后否决**，理由是有数据支撑的：
 *   分区值确实更准（平均经/纬向 4.59/3.05 m vs 全国值 8.20/4.21 m），但
 *   **选错分区的代价是 15.8~27.0 m**（9 个城市实测：各分区参数算出的位移相对
 *   全国值的最大偏差），**大于全国值本身的最大误差 14.7 m**；而论文只说
 *   "按经纬度相似和行政分区方便"划 7 区，**没有给出分区边界**，任何 bbox 都是
 *   我们自己发明的 ⇒ 分区判定一错就适得其反。故只用全国统一值。
 *   （分区数值备查，将来若拿到可靠的分区判据可直接替换：
 *     东北 −96.51/−61.41/−10.13 华北 −99.71/−50.78/−3.57 华中 −106.67/−58.25/−4.55
 *     华南 −117.54/−49.79/+13.87 西南 −112.03/−33.68/+3.30 西北 −107.61/−45.58/+8.40
 *     西部 −91.95/−22.06/+29.77）
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
    /* 北京 1954（克拉索夫斯基 1940 椭球）。地心三参数（米）= (15.8, −154.4, −82.3)。
     * 这是 **EPSG:4214 的默认 TOWGS84**，也就是 PROJ/GDAL/QGIS/ArcGIS 加载
     * "EPSG:4214 北京1954" 时实际使用的那组值（EPSG 条目编号 15921，+towgs84 语义
     * 与本表"基准→WGS84"一致）⇒ 与主流工具链行为一致，量级正确。
     * ⚠ 但 EPSG:15921 登记的 Area of use 是 "China - south and west Tarim basin"
     *   （塔里木盆地西南，BGP 1994 年给 ELF 的油气勘探参数），**不是全国值**：
     *   它在这里标称精度 1 m，华北/华东用会偏（自测打印：北京北向偏约 60~95 m）。
     *   EPSG 另有多条分区值（15919 黄海 / 15920 南海珠江口 / 15935 北部湾 /
     *   15918·15936 鄂尔多斯），同样都是油气勘探的局部参数，没有真正的全国值。
     * ★ 根因（文献结论）：BJ54 是**分区域布设平差**的，不是刚体 ⇒ 单一三/七参数
     *   在分区求解时残差很大，学术上建议用多项式/格网逼近而非 Helmert 七参数。 */
    { {6378245.0, 1.0 / 298.3}, 15.8, -154.4, -82.3, 0, 0, 0, 0 },
    /* 西安 1980（IAG-75 椭球）。
     * EPSG 未登记 XA80↔WGS84，但公开文献有：赵玉明等《西安1980坐标转换为
     * WGS1984坐标的3参数近似估算方法》，中国水土保持科学 2013, 11(3): 78-83。
     * 全国 10 省 / 14 片区 / 61 样点拟合，全国统一 3 参数（米）：
     *     dX = −104.57, dY = −45.94, dZ = +5.30
     * 方向 = [XYZ]84 = [XYZ]80 + d，正是本表语义（基准→WGS84，同 +towgs84）。
     * 精度：全国统一参数平均经向 8.20 m / 纬向 4.21 m，最大 14.73 m / 9.23 m。
     * ★ 曾经的坑：这里一度填 0（"EPSG 没登记就不平移"）。但"仅换椭球"是错的——
     *   IAG75 近乎 WGS84 整体放大 3 m（a +3.00、b +2.97），均匀缩放不改大地
     *   经纬度，只改高程 ⇒ 水平差 0.019 m、高程 −2.996 m，等于没转换。
     *   XA80 是参心坐标系，原点偏离地心**接近 200 m**，必须靠平移参数体现。 */
    { {6378140.0, 1.0 / 298.257}, -104.57, -45.94, 5.30, 0, 0, 0, 0 },
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
            return "克拉索夫斯基椭球 + EPSG:4214 默认 TOWGS84 (+15.8,−154.4,−82.3)"
                   "（PROJ/GDAL 同款）。⚠ 适用区是塔里木盆地西南（BGP 1994 年油气参数），"
                   "华北用北向会偏 60~95 m（北京实测水平 63.5 m）；"
                   "官方要求 BJ54→西安80→CGCS2000 两步走，本模块一步到 WGS84";
        case DATUM_XA80:
            return "IAG-75 椭球 + 公开文献拟合的全国统一 3 参数 "
                   "(−104.57,−45.94,+5.30)（赵玉明等，中国水土保持科学 2013, 11(3)）。"
                   "已双向交叉验证：国测局图幅平移量区间六点全中 + 与国标《2000国家大地"
                   "坐标系转换技术要求》附录B改正量公式等价（9 点差 0.001 m）。精度约 15 m；"
                   "官方正解是 1:5万图幅改正量+双线性内插（数据由测绘部门提供，不公开）";
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

void datum_translation(DatumId id, double *dx, double *dy, double *dz)
{
    if (id < 0 || id >= DATUM_COUNT) id = DATUM_WGS84;
    if (dx) *dx = DATUMS[id].dx;
    if (dy) *dy = DATUMS[id].dy;
    if (dz) *dz = DATUMS[id].dz;
}

void datum_ellipsoid(DatumId id, double *a, double *f)
{
    if (id < 0 || id >= DATUM_COUNT) id = DATUM_WGS84;
    if (a) *a = DATUMS[id].ell.a;
    if (f) *f = DATUMS[id].ell.f;
}
