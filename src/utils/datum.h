#ifndef NAVMAP_DATUM_H
#define NAVMAP_DATUM_H

#include <stddef.h>

/* ★ C++ 端（src/ui_qt 目录下的 .cpp）也要 include 本头 ⇒ 必须 extern "C"，
 * 否则符号被名字改编、链接期报 undefined reference。
 * 与 settings.h / plat.h 的惯例一致。 */
#ifdef __cplusplus
extern "C" {
#endif

/* 坐标系（大地基准）枚举。
 *
 * 内部地图数据始终是 WGS84（OSM PBF 原生）——街道/路网的几何不动。
 * 这里的"基准"只影响**显示/标注**：坐标网（经纬网）与右下角读数按所选基准呈现。
 * 切基准**不会**重投影底图，只会移动经纬网与读数（同一点在不同基准下经纬度不同）。
 *
 * 为什么这样设计：显示层分 tile 后整省几何已量化进 nodes.bin（WGS84），
 * 重投所有节点既没必要也不该做；用户要的是"用哪种坐标读这张图"，不是改图本身。
 */
typedef enum {
    DATUM_WGS84    = 0,   /* WGS84（OSM 原生，默认） */
    DATUM_CGCS2000 = 1,   /* 中国 2000 国家大地坐标系：与 WGS84 近等（差 < 2 m） */
    DATUM_BJ54     = 2,   /* 北京 1954（克拉索夫斯基 1940 椭球） */
    DATUM_XA80     = 3,   /* 西安 1980（1975 IUGG 椭球） */
    DATUM_COUNT
} DatumId;

const char *datum_short(DatumId id);   /* "WGS84" 等缩写，用于读数前缀 */
const char *datum_label(DatumId id);   /* "WGS84（默认）" 等中文标签，用于 UI */
/* 该基准的**来源与精度**说明（一行，用于 UI 提示）。为什么放在这里而不是 UI 层：
 * 说明必须和 datum.c 里的参数表同步改，分散到 UI 就会过期说谎。 */
const char *datum_note(DatumId id);
int         datum_count(void);
DatumId    datum_by_index(int i);

/* 网格类型（与坐标系解耦）：经纬网（度）/ 方里格（高斯-克吕格公里网）。
 * ★ 坐标系（datum）只管坐标**读数**，网格类型独立决定画哪种网格，默认方里格。 */
typedef enum {
    GRID_LATLON = 0,   /* 经纬网（度） */
    GRID_GK     = 1    /* 方里格（高斯-克吕格公里网，默认） */
} GridTypeId;

/* 方里格投影所用的基准：读数基准是 WGS84 时用 CGCS2000（国标公里网），
 * 读数基准是某中文基准时直接用该基准，保证方里格与读数口径一致。 */
static inline DatumId grid_gk_datum(DatumId readout)
{
    return (readout == DATUM_WGS84) ? DATUM_CGCS2000 : readout;
}

/* WGS84 经纬度 → 指定基准（读数/标注用）。lat/lon 单位度；h 高程米（通常传 0，
 * 可传 NULL 表示不关心）。out_lat/out_lon 为基准下的经纬度（度）。 */
void datum_wgs84_to(DatumId id, double lat, double lon, double h,
                    double *out_lat, double *out_lon, double *out_h);

/* 指定基准经纬度 → WGS84（把该基准下的经纬网线投到 WGS84 地图上再投影）。 */
void datum_to_wgs84(DatumId id, double lat, double lon, double h,
                    double *out_lat, double *out_lon, double *out_h);

/* 只读访问参数表（便于自测/UI 显示，不必把常数抄一份到别处）。
 * ★ skedaddle_datum_selftest 的「国标公式等价性」判据就靠它们：参数从**模块里读出来**，
 *   再由测试用另一条独立公式复算 ⇒ 以后改参数不会误报，只有算法错才 FAIL。 */
void datum_translation(DatumId id, double *dx, double *dy, double *dz);  /* 米 */
void datum_ellipsoid(DatumId id, double *a, double *f);                  /* 米 / 扁率 */

/* ==========================================================================
 * 高斯-克吕格投影（方里格叠加层用）
 *
 * 仅用于坐标网格叠加层的"方里格"（公里格网），底图几何（WGS84 Web Mercator）
 * 不受影响。对外接口统一用 WGS84 经纬度，内部透明地把经纬度先换到所选基准、
 * 再用该基准椭球做高斯投影 —— 与 datum_wgs84_to / datum_to_wgs84 同理。
 *
 *   - lon0_deg：3° 带中央经线（度）。调用方按可见区中心经度 round(lon/3)*3 给出，
 *     正算/反算必须用同一 lon0 才闭合。
 *   - X：北向坐标（米，赤道起算）；Y：东向坐标（米，含 500km 假东偏移）。
 *   - 公式到 1.5° 项（含 η²/μ 高次项），单点误差米级，叠加层足够。
 * ========================================================================== */
void datum_gk_forward(DatumId id, double lat, double lon, double lon0_deg,
                      double *X, double *Y);
void datum_gk_inverse(DatumId id, double X, double Y, double lon0_deg,
                      double *lat, double *lon);

/* ==========================================================================
 * 三个扩展入口：现在没有数据，但**接口做实并可验证**，将来拿到数据直接喂进来
 * 即可生效，不需要再改 datum.c 的算法部分。
 *
 *   入口①  有重合点（同一物理点在两套坐标下的成果）  → datum_solve_seven_param
 *           （即官方《2000国家大地坐标系转换技术要求》附录 A 的"重合点法"）
 *   入口②  有 1:5万 图幅改正量（测绘部门按图幅分发）  → datum_grid_load / datum_grid_set
 *           （官方 1:5万 及以下的正解：改正量 + 双线性内插，附录 B/C）
 *   入口③  只有局部经验值（"我们这儿东偏 110 m"）      → datum_set_local_offset
 *           （或直接 datum_set_seven_param 填已知三/七参数）
 *
 * 三者都通过 datum_set_seven_param / datum_grid_set 落到模块里，
 * datum_wgs84_to / datum_to_wgs84 立即生效 ⇒ 经纬网与读数随之变化，无需改 UI。
 * 所有设置都是**进程内的**，不落盘；重启回到出厂值（要持久化再接 settings）。
 * ========================================================================== */

/* 完整 7 参数（Bursa-Wolf / EPSG:9606 Position Vector 约定，与 +towgs84 同向）：
 *     X_wgs = T + (1 + ds)·(I + Ω)·X_src
 * 其中 T=(dx,dy,dz) 米、Ω 由 (rx,ry,rz) 构成（角秒）、ds 为尺度（ppm）。
 * 当前出厂值只有平移（旋转/尺度恒 0）⇒ 退化为三参数；字段与算法都支持全 7 参数。 */
typedef struct {
    double dx, dy, dz;   /* 平移，米 */
    double rx, ry, rz;   /* 旋转，角秒 */
    double ds;           /* 尺度，ppm */
} DatumSevenParam;

void datum_get_seven_param(DatumId id, DatumSevenParam *out);
/* 运行时替换参数（入口①解出的结果 / 入口③的已知参数，填进来即生效）。
 * 返回 0 成功，id 越界或不许改（WGS84）返回 -1。 */
int  datum_set_seven_param(DatumId id, const DatumSevenParam *p);
void datum_reset_params(DatumId id);   /* 恢复内置出厂参数（不影响已挂的格网） */

/* 入口①：重合点（同一物理点在"源基准"与 WGS84 下的大地坐标，单位度/米）。 */
typedef struct {
    double lat_src, lon_src, h_src;   /* 源基准（id 指定的那个）下 */
    double lat_wgs, lon_wgs, h_wgs;   /* WGS84 下 */
} DatumTie;

/* 用重合点最小二乘解 7 参数（官方附录 A「重合点法」）。
 *   n < 3 返回 -1；法方程奇异（点分布退化，如全部共线/重合）返回 -2。
 *   成功返回 0：out 为解出的参数，out_rms（可 NULL）为残差 RMS（米）。
 * ★ 实践提醒：3 点刚好解、但无冗余 ⇒ 残差恒为 0，看不出数据质量；
 *   建议 ≥5 个分布均匀的点，用 out_rms 判断（米级说明参数可信，十米级说明
 *   该区域不适合单一 Helmert —— BJ54 尤其如此，它是分区平差的、不是刚体）。
 * ★ 解出来后调用 datum_set_seven_param 装回去即生效；自测 skedaddle_datum_selftest
 *   用"模块自己正算生成的合成点"验证解算能还原出厂参数。 */
int datum_solve_seven_param(DatumId id, const DatumTie *ties, int n,
                            DatumSevenParam *out, double *out_rms);

/* 入口③（最省事）：已知某点处"基准相对 WGS84"的北/东偏移（米），反算地心三参数。
 * 适用于"我们测区西安80 比 WGS84 东偏 110 m、北偏 8 m"这类局部经验值。
 * 内部按该点的东/北单位向量把米数转成 ECEF 平移；旋转/尺度清零。
 * 返回 0 成功；id 越界或 id==DATUM_WGS84（恒等，不许改）返回 -1。 */
int datum_set_local_offset(DatumId id, double lat, double lon,
                           double dnorth, double deast);

/* 入口②：图幅改正量格网（规则经纬网格 + 双线性内插，即官方附录 C 的做法）。
 * 语义：dn/de 是"**基准 → WGS84**"方向上的北向/东向改正量（米），
 *       与国标「XA80→CGCS2000 图幅改正量」同向；要反方向就传负值。
 * 格网原点在西南角 (lat0,lon0)，沿纬度方向 nrows 行、沿经度方向 ncols 列，
 * 步长 dlat/dlon（度）。dn/de 行优先（lat 递增在外、lon 递增在内）。 */
typedef struct {
    double lat0, lon0;    /* 西南角经纬度，度 */
    double dlat, dlon;    /* 步长，度（必须 > 0） */
    int    nrows, ncols;  /* 行数 / 列数（必须 >= 1） */
    const float *dn;      /* 北向改正量（米），nrows*ncols 个 */
    const float *de;      /* 东向改正量（米），nrows*ncols 个 */
} DatumGridDesc;

/* 挂格网（内部复制一份数据，调用方可立即释放）。返回 0 成功；
 * 参数不合法返回 -1；内存不足返回 -2。挂上后该基准的换算自动叠加改正量。 */
int  datum_grid_set(DatumId id, const DatumGridDesc *g);
void datum_grid_clear(DatumId id);   /* 卸下格网并释放内存 */
int  datum_grid_active(DatumId id);  /* 是否已挂格网 */

/* 从文本文件加载格网并挂到指定基准 —— 拿到改正量数据后转成这个格式即可用。
 * 格式（'#' 开头为注释，空行忽略；第一行有效数据是表头）：
 *     lat0 lon0 dlat dlon nrows ncols
 *     dn de          ← 共 nrows*ncols 行，行优先
 * 返回 0 成功；-1 打不开 / -2 表头错 / -3 数据行数不足 / -4 内存不足 / -5 id 越界。 */
int datum_grid_load(DatumId id, const char *path);

#ifdef __cplusplus
}
#endif

#endif
