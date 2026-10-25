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
 *
 * ★★ 为什么不做成"多组参数 + 下拉选择"（全球惯例与演进路径，2026-10-25 查证）：
 *   "一个基准配多组区域变换"是全球常态——最典型的是北美 NAD27：EPSG 注册了
 *   **85 条** NAD27→WGS84 变换（78 条在用），没有唯一全国值：若干全国性条目
 *   （15851 NADCON 网格 5 m、加拿大 NTv2 1693 2 m、7 个 HARN 全国 1.5 m）
 *   + 大量州/省级区域参数（43 条精度 1.5 m）。BJ54 在 EPSG 有 6 条、全是油气
 *   局部参数；XA80 零条。XA80 之所以没有区域变换：它是**过渡性基准**
 *   （EPSG:4610 remarks: "Replaces Beijing 1954. Replaced by CGCS2000"，
 *    1980 起用、2008 后全国转 CGCS2000）⇒ 没人投入做全国区域变换测量。
 *   而美国能有 85 条，根因是它有海量控制点（每州 HARN 站）——区域参数的
 *   精度是用测量堆出来的，不是拟合出来的。
 *   ★ 但看美国的最终答案：它**没有停在"多组参数让人选"，而是演进到格网**
 *   （NADCON/NTv2，全国 5 m / 州级 1.5 m）——与中国官方方案（1:5万图幅改正量
 *   + 双线性内插）是同一条路径：参数法 → 格网法。原因是"多组让人选"有个死结：
 *   **谁来选？** 用户往往不知道自己所在的拟合分区，而选错的代价
 *   （15.8~27.0 m）比用全国值本身的最大误差（14.7 m）还大。格网没有这个死结：
 *   按坐标自动取值、边界就是格网间隔、无选择歧义，格网够密时就是连续改正量场，
 *   精度天然高于分区常数。
 *   ⇒ 本项目对应设计：**单组全国参数（~15 m）+ 入口②格网接口**（等数据）。
 *     若将来拿到的是"某省 7 参数"而非格网，入口①/③ 也能装，但优先级建议
 *     格网 > 分区参数：前者无歧义、后者要靠人选。
 */

#include "utils/datum.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    double rx, ry, rz;     /* 角秒（出厂值恒 0；可通过 datum_set_seven_param 填） */
    double ds;             /* ppm（出厂值恒 0，同上） */
} DatumDef;

/* 出厂参数表（供 datum_reset_params 用）。DATUMS 本身是可变的：三个扩展入口
 * 都是往里写值，写完立即生效。 */
static const DatumDef DATUMS_FACTORY[DATUM_COUNT] = {
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

/* 运行期参数表（初始 = 出厂值，可被 datum_set_seven_param / datum_set_local_offset 改） */
static DatumDef DATUMS[DATUM_COUNT] = {
    { {6378137.0, 1.0 / 298.257223563}, 0, 0, 0, 0, 0, 0, 0 },
    { {6378137.0, 1.0 / 298.257222101}, 0, 0, 0, 0, 0, 0, 0 },
    { {6378245.0, 1.0 / 298.3}, 15.8, -154.4, -82.3, 0, 0, 0, 0 },
    { {6378140.0, 1.0 / 298.257}, -104.57, -45.94, 5.30, 0, 0, 0, 0 },
};

/* ---- 图幅改正量格网（入口②）。每个基准各挂一个，进程内可变 ---- */
typedef struct {
    int    active;
    double lat0, lon0, dlat, dlon;
    int    nrows, ncols;
    float *dn, *de;
} DatumGrid;

static DatumGrid GRIDS[DATUM_COUNT];

static const double ARCSEC_TO_RAD = (M_PI / 180.0) / 3600.0;
static const double RAD_TO_ARCSEC = (180.0 / M_PI) * 3600.0;

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

/* 构造完整 7 参数矩阵 M = (1+ds)·(I + Ω)，Ω 为 (rx,ry,rz) 的反对称阵。
 * ★ rx=ry=rz=0、ds=0 时 M 精确等于单位阵（k=1.0、非对角 ±0.0）
 *   ⇒ 结果与"只加平移"的旧实现**逐位一致**，不引入回归。 */
static void helmert_matrix(const DatumDef *d, double m[9])
{
    double rx = d->rx * ARCSEC_TO_RAD;
    double ry = d->ry * ARCSEC_TO_RAD;
    double rz = d->rz * ARCSEC_TO_RAD;
    double k  = 1.0 + d->ds * 1e-6;
    m[0] = k;        m[1] = -k * rz;  m[2] = k * ry;
    m[3] = k * rz;   m[4] = k;        m[5] = -k * rx;
    m[6] = -k * ry;  m[7] = k * rx;   m[8] = k;
}

/* 3x3 精确求逆（Cramer）。用于逆变换，保证正/反严格互逆（不是"减平移"近似）。 */
static int inv3(const double m[9], double out[9])
{
    const double c00 = m[4] * m[8] - m[5] * m[7];
    const double c01 = m[5] * m[6] - m[3] * m[8];
    const double c02 = m[3] * m[7] - m[4] * m[6];
    const double det = m[0] * c00 + m[1] * c01 + m[2] * c02;
    if (!(fabs(det) > 1e-30)) return -1;
    const double id = 1.0 / det;
    out[0] = c00 * id;
    out[1] = (m[2] * m[7] - m[1] * m[8]) * id;
    out[2] = (m[1] * m[5] - m[2] * m[4]) * id;
    out[3] = c01 * id;
    out[4] = (m[0] * m[8] - m[2] * m[6]) * id;
    out[5] = (m[2] * m[3] - m[0] * m[5]) * id;
    out[6] = c02 * id;
    out[7] = (m[1] * m[6] - m[0] * m[7]) * id;
    out[8] = (m[0] * m[4] - m[1] * m[3]) * id;
    return 0;
}

/* 基准 → WGS84：Xw = T + M·Xs */
static void apply_fwd(const DatumDef *d,
                      double Xs, double Ys, double Zs,
                      double *Xw, double *Yw, double *Zw)
{
    double m[9];
    helmert_matrix(d, m);
    *Xw = d->dx + m[0] * Xs + m[1] * Ys + m[2] * Zs;
    *Yw = d->dy + m[3] * Xs + m[4] * Ys + m[5] * Zs;
    *Zw = d->dz + m[6] * Xs + m[7] * Ys + m[8] * Zs;
}

/* WGS84 → 基准：Xs = M⁻¹·(Xw − T)。★ 用真正的矩阵逆，而不是"减平移"——
 * 一旦将来填了旋转/尺度，"减平移"就不成立；这里无论参数如何都严格互逆。 */
static void apply_inv(const DatumDef *d,
                      double Xw, double Yw, double Zw,
                      double *Xs, double *Ys, double *Zs)
{
    double m[9], mi[9];
    helmert_matrix(d, m);
    const double px = Xw - d->dx, py = Yw - d->dy, pz = Zw - d->dz;
    if (inv3(m, mi) != 0) {   /* 实际不会发生（det≈1）；兜底退化成纯平移 */
        *Xs = px; *Ys = py; *Zs = pz;
        return;
    }
    *Xs = mi[0] * px + mi[1] * py + mi[2] * pz;
    *Ys = mi[3] * px + mi[4] * py + mi[5] * pz;
    *Zs = mi[6] * px + mi[7] * py + mi[8] * pz;
}

/* 双线性内插取改正量（附录 C 的做法）。格网外的点取边缘格值（clamp），
 * 不返回失败 —— 改正量本来就是"图幅内的经验值"，外推没有意义。
 * ★ 行列号用 floor + clamp，nrows/ncols 为 1 的退化格网也能正确工作。 */
static int grid_sample(const DatumGrid *g, double lat, double lon,
                       double *dn, double *de)
{
    double v = (lat - g->lat0) / g->dlat;   /* 行方向（纬度） */
    double u = (lon - g->lon0) / g->dlon;   /* 列方向（经度） */
    if (u < 0.0) u = 0.0; else if (u > (double)(g->ncols - 1)) u = (double)(g->ncols - 1);
    if (v < 0.0) v = 0.0; else if (v > (double)(g->nrows - 1)) v = (double)(g->nrows - 1);

    int i0 = (int)floor(v), j0 = (int)floor(u);
    double tv = v - i0, tu = u - j0;
    if (i0 > g->nrows - 2) { i0 = g->nrows - 1; tv = 0.0; }
    if (j0 > g->ncols - 2) { j0 = g->ncols - 1; tu = 0.0; }
    if (i0 < 0) { i0 = 0; tv = 0.0; }
    if (j0 < 0) { j0 = 0; tu = 0.0; }
    const int i1 = (i0 + 1 <= g->nrows - 1) ? i0 + 1 : i0;
    const int j1 = (j0 + 1 <= g->ncols - 1) ? j0 + 1 : j0;

    const size_t r0 = (size_t)i0 * (size_t)g->ncols, r1 = (size_t)i1 * (size_t)g->ncols;
    const double n0 = g->dn[r0 + (size_t)j0] * (1.0 - tu) + g->dn[r0 + (size_t)j1] * tu;
    const double n1 = g->dn[r1 + (size_t)j0] * (1.0 - tu) + g->dn[r1 + (size_t)j1] * tu;
    const double e0 = g->de[r0 + (size_t)j0] * (1.0 - tu) + g->de[r0 + (size_t)j1] * tu;
    const double e1 = g->de[r1 + (size_t)j0] * (1.0 - tu) + g->de[r1 + (size_t)j1] * tu;
    *dn = n0 * (1.0 - tv) + n1 * tv;
    *de = e0 * (1.0 - tv) + e1 * tv;
    return 0;
}

/* 把经纬度点沿北/东方向平移 dn/de 米（在 WGS84 椭球上用曲率半径换算）。 */
static void shift_by_meters(double lat, double lon, double dn, double de,
                            double *olat, double *olon)
{
    const Ellipsoid *e = &DATUMS[DATUM_WGS84].ell;
    const double e2 = e->f * (2.0 - e->f);
    const double la = lat * M_PI / 180.0;
    const double sl = sin(la), cl = cos(la);
    const double W = sqrt(1.0 - e2 * sl * sl);
    const double N = e->a / W;                       /* 卯酉圈曲率半径 */
    const double M = e->a * (1.0 - e2) / (W * W * W);/* 子午圈曲率半径 */
    double den = N * cl;
    if (fabs(den) < 1e-6) den = (den >= 0) ? 1e-6 : -1e-6;   /* 极点保护 */
    *olat = lat + (dn / M) * (180.0 / M_PI);
    *olon = lon + (de / den) * (180.0 / M_PI);
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
    /* 叠加图幅改正量（入口②）。格网以**源基准坐标**索引（与国标同口径）。 */
    if (GRIDS[id].active && olat && olon) {
        double dn, de;
        if (grid_sample(&GRIDS[id], lat, lon, &dn, &de) == 0)
            shift_by_meters(*olat, *olon, dn, de, olat, olon);
    }
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
    /* 反向：减去同一份改正量。★ 索引用的是 **WGS84 坐标**而不是基准坐标 ——
     * 二者相差 ~100 m，相对格网步长（1:5万图幅 ≈ 十几 km）造成的改正量差异
     * 远小于改正量本身的空间变化率 ⇒ 正/反仍然自洽（自测按 1e-7 度验收）。 */
    if (GRIDS[id].active && olat && olon) {
        double dn, de;
        if (grid_sample(&GRIDS[id], lat, lon, &dn, &de) == 0)
            shift_by_meters(*olat, *olon, -dn, -de, olat, olon);
    }
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

/* ==========================================================================
 * 三个扩展入口的实现
 * ========================================================================== */

void datum_get_seven_param(DatumId id, DatumSevenParam *out)
{
    if (!out) return;
    if (id < 0 || id >= DATUM_COUNT) id = DATUM_WGS84;
    const DatumDef *d = &DATUMS[id];
    out->dx = d->dx; out->dy = d->dy; out->dz = d->dz;
    out->rx = d->rx; out->ry = d->ry; out->rz = d->rz;
    out->ds = d->ds;
}

int datum_set_seven_param(DatumId id, const DatumSevenParam *p)
{
    if (!p || id < 0 || id >= DATUM_COUNT) return -1;
    if (id == DATUM_WGS84) return -1;   /* WGS84 是恒等基准，不许改 */
    DatumDef *d = &DATUMS[id];
    d->dx = p->dx; d->dy = p->dy; d->dz = p->dz;
    d->rx = p->rx; d->ry = p->ry; d->rz = p->rz;
    d->ds = p->ds;
    return 0;
}

void datum_reset_params(DatumId id)
{
    if (id < 0 || id >= DATUM_COUNT) return;
    DATUMS[id] = DATUMS_FACTORY[id];
}

/* 入口③：已知某点处的北/东偏移（米）⇒ 反算地心三参数。
 * 东向单位向量 ê = (−sinλ, cosλ, 0)
 * 北向单位向量 n̂ = (−sinφ·cosλ, −sinφ·sinλ, cosφ)
 * 两者正交，故 d = de·ê + dn·n̂ 就是能让该点恰好位移 (dn 北, de 东) 的 ECEF 平移。 */
int datum_set_local_offset(DatumId id, double lat, double lon,
                           double dnorth, double deast)
{
    if (id < 0 || id >= DATUM_COUNT) return -1;
    if (id == DATUM_WGS84) return -1;
    const double la = lat * M_PI / 180.0, lo = lon * M_PI / 180.0;
    const double sl = sin(la), cl = cos(la), slo = sin(lo), clo = cos(lo);
    const double ex = -slo,          ey = clo,           ez = 0.0;
    const double nx = -sl * clo,     ny = -sl * slo,     nz = cl;
    DatumDef *d = &DATUMS[id];
    d->dx = deast * ex + dnorth * nx;
    d->dy = deast * ey + dnorth * ny;
    d->dz = deast * ez + dnorth * nz;
    d->rx = d->ry = d->rz = d->ds = 0.0;
    return 0;
}

/* 7x7 线性方程组求解（高斯消元 + 部分选主元）。返回 0 成功，奇异返回 -1。 */
static int solve_lin7(double A[49], double b[7], double x[7])
{
    for (int c = 0; c < 7; c++) {
        int piv = c;
        double best = fabs(A[c * 7 + c]);
        for (int r = c + 1; r < 7; r++) {
            double v = fabs(A[r * 7 + c]);
            if (v > best) { best = v; piv = r; }
        }
        if (!(best > 1e-12)) return -1;
        if (piv != c) {
            for (int k = c; k < 7; k++) {
                double t = A[c * 7 + k]; A[c * 7 + k] = A[piv * 7 + k]; A[piv * 7 + k] = t;
            }
            double t = b[c]; b[c] = b[piv]; b[piv] = t;
        }
        const double dg = A[c * 7 + c];
        for (int r = c + 1; r < 7; r++) {
            const double f = A[r * 7 + c] / dg;
            if (f == 0.0) continue;
            for (int k = c; k < 7; k++) A[r * 7 + k] -= f * A[c * 7 + k];
            b[r] -= f * b[c];
        }
    }
    for (int r = 6; r >= 0; r--) {
        double s = b[r];
        for (int k = r + 1; k < 7; k++) s -= A[r * 7 + k] * x[k];
        x[r] = s / A[r * 7 + r];
    }
    return 0;
}

/* 入口①：重合点最小二乘解 7 参数（官方附录 A「重合点法」）。
 *
 * 模型（Bursa-Wolf，与 apply_fwd 完全一致）：
 *     X_wgs = T + (1+s)·(I+Ω)·X_src
 * 线性化（忽略 s·Ω 二阶项）：
 *     ΔX = dx            − rz·Ys + ry·Zs + s·Xs
 *     ΔY = dy + rz·Xs            − rx·Zs + s·Ys
 *     ΔZ = dz − ry·Xs + rx·Ys            + s·Zs
 * 每点 3 个方程、7 个未知数 ⇒ n≥3 可解，n 越大越稳。
 *
 * ★ 列均衡：旋转列按 µrad、尺度列按 ppm 计（未知数乘了 1e-6），
 *   否则设计矩阵里"平移列 ≈ 1"与"旋转列 ≈ 6.4e6"量级差 7 个数量级，
 *   法方程条件数会差到 1e13，double 下平移解会被污染到毫米以上。
 *   缩放后各列量级都在 O(1~10)，条件数降到几十。 */
int datum_solve_seven_param(DatumId id, const DatumTie *ties, int n,
                            DatumSevenParam *out, double *out_rms)
{
    if (!ties || !out || n < 3) return -1;
    if (id < 0 || id >= DATUM_COUNT) id = DATUM_WGS84;
    const Ellipsoid *es = &DATUMS[id].ell;
    const Ellipsoid *ew = &DATUMS[DATUM_WGS84].ell;

    double N[49], b[7];
    memset(N, 0, sizeof N);
    memset(b, 0, sizeof b);

    for (int i = 0; i < n; i++) {
        double X1, Y1, Z1, X2, Y2, Z2;
        geod_to_ecef(ties[i].lat_src, ties[i].lon_src, ties[i].h_src, es, &X1, &Y1, &Z1);
        geod_to_ecef(ties[i].lat_wgs, ties[i].lon_wgs, ties[i].h_wgs, ew, &X2, &Y2, &Z2);
        const double r[3] = { X2 - X1, Y2 - Y1, Z2 - Z1 };
        const double row[3][7] = {
            { 1, 0, 0,       0.0,  Z1 * 1e-6, -Y1 * 1e-6,  X1 * 1e-6 },
            { 0, 1, 0, -Z1 * 1e-6,      0.0,   X1 * 1e-6,  Y1 * 1e-6 },
            { 0, 0, 1,  Y1 * 1e-6, -X1 * 1e-6,      0.0,   Z1 * 1e-6 },
        };
        for (int p = 0; p < 3; p++) {
            for (int q = 0; q < 7; q++) {
                for (int k = 0; k < 7; k++) N[q * 7 + k] += row[p][q] * row[p][k];
                b[q] += row[p][q] * r[p];
            }
        }
    }

    double t[7];
    if (solve_lin7(N, b, t) != 0) return -2;

    out->dx = t[0]; out->dy = t[1]; out->dz = t[2];
    /* 解出的旋转单位是 µrad、尺度单位是 ppm */
    out->rx = t[3] * 1e-6 * RAD_TO_ARCSEC;
    out->ry = t[4] * 1e-6 * RAD_TO_ARCSEC;
    out->rz = t[5] * 1e-6 * RAD_TO_ARCSEC;
    out->ds = t[6];

    if (out_rms) {
        double ss = 0.0;
        int m = 0;
        for (int i = 0; i < n; i++) {
            double X1, Y1, Z1, X2, Y2, Z2;
            geod_to_ecef(ties[i].lat_src, ties[i].lon_src, ties[i].h_src, es, &X1, &Y1, &Z1);
            geod_to_ecef(ties[i].lat_wgs, ties[i].lon_wgs, ties[i].h_wgs, ew, &X2, &Y2, &Z2);
            /* 用解出的参数预测：Δ = A·t，其中旋转/尺度项要还原 1e-6 */
            const double rx = t[3] * 1e-6, ry = t[4] * 1e-6, rz = t[5] * 1e-6, s = t[6] * 1e-6;
            const double pX = out->dx        - rz * Y1 + ry * Z1 + s * X1;
            const double pY = out->dy + rz * X1        - rx * Z1 + s * Y1;
            const double pZ = out->dz - ry * X1 + rx * Y1        + s * Z1;
            const double eX = (X2 - X1) - pX, eY = (Y2 - Y1) - pY, eZ = (Z2 - Z1) - pZ;
            ss += eX * eX + eY * eY + eZ * eZ;
            m += 3;
        }
        *out_rms = (m > 0) ? sqrt(ss / m) : 0.0;
    }
    return 0;
}

/* ---- 入口②：格网 ---- */

int datum_grid_set(DatumId id, const DatumGridDesc *g)
{
    if (!g || id < 0 || id >= DATUM_COUNT) return -1;
    if (g->nrows < 1 || g->ncols < 1 || g->dlat <= 0.0 || g->dlon <= 0.0) return -1;
    if (!g->dn || !g->de) return -1;
    const size_t cnt = (size_t)g->nrows * (size_t)g->ncols;
    float *ndn = (float *)malloc(cnt * sizeof(float));
    float *nde = (float *)malloc(cnt * sizeof(float));
    if (!ndn || !nde) { free(ndn); free(nde); return -2; }
    memcpy(ndn, g->dn, cnt * sizeof(float));
    memcpy(nde, g->de, cnt * sizeof(float));

    DatumGrid *G = &GRIDS[id];
    free(G->dn);
    free(G->de);
    G->dn = ndn;
    G->de = nde;
    G->lat0 = g->lat0; G->lon0 = g->lon0;
    G->dlat = g->dlat; G->dlon = g->dlon;
    G->nrows = g->nrows; G->ncols = g->ncols;
    G->active = 1;
    return 0;
}

void datum_grid_clear(DatumId id)
{
    if (id < 0 || id >= DATUM_COUNT) return;
    DatumGrid *G = &GRIDS[id];
    free(G->dn);
    free(G->de);
    G->dn = NULL;
    G->de = NULL;
    G->active = 0;
}

int datum_grid_active(DatumId id)
{
    if (id < 0 || id >= DATUM_COUNT) return 0;
    return GRIDS[id].active;
}

int datum_grid_load(DatumId id, const char *path)
{
    if (id < 0 || id >= DATUM_COUNT) return -5;
    if (!path) return -1;
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    double lat0 = 0, lon0 = 0, dlat = 0, dlon = 0;
    int nrows = 0, ncols = 0, hdr = 0;
    long need = 0, got = 0;
    float *dn = NULL, *de = NULL;
    char line[512];
    int rc = 0;

    while (fgets(line, (int)sizeof line, f)) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '#' || *s == '\n' || *s == '\r' || *s == '\0') continue;
        if (!hdr) {
            if (sscanf(s, "%lf %lf %lf %lf %d %d", &lat0, &lon0, &dlat, &dlon,
                       &nrows, &ncols) != 6) { rc = -2; break; }
            if (nrows <= 0 || ncols <= 0 || dlat <= 0.0 || dlon <= 0.0) { rc = -2; break; }
            need = (long)nrows * (long)ncols;
            dn = (float *)malloc((size_t)need * sizeof(float));
            de = (float *)malloc((size_t)need * sizeof(float));
            if (!dn || !de) { rc = -4; break; }
            hdr = 1;
            continue;
        }
        if (got >= need) break;
        float a = 0, b = 0;
        if (sscanf(s, "%f %f", &a, &b) != 2) { rc = -3; break; }
        dn[got] = a;
        de[got] = b;
        got++;
    }
    fclose(f);

    if (rc == 0 && !hdr) rc = -2;
    if (rc == 0 && got < need) rc = -3;
    if (rc == 0) {
        DatumGridDesc gd;
        gd.lat0 = lat0; gd.lon0 = lon0; gd.dlat = dlat; gd.dlon = dlon;
        gd.nrows = nrows; gd.ncols = ncols; gd.dn = dn; gd.de = de;
        rc = datum_grid_set(id, &gd);
    }
    free(dn);
    free(de);
    return rc;
}
