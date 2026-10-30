# 第 1 课：坐标与大地基准

> 模块：`src/utils/geo.{c,h}`、`src/utils/datum.{c,h}`
> 前置：经纬度直觉、墨卡托投影、大地基准（datum）概念。

## 一、教学目的

学完本课，学生应能：

1. 说清"内部几何永远是 WGS84（OSM PBF 原生），坐标系只影响显示读数，不重投影底图"这一条不变量，并能在代码里指出它的落点。
2. 解释经纬度为何用 `1e-7` 度（`int32`，即 `nm_lat_to_i32`/`nm_i32_to_deg`）做落盘量化单位，以及浮点对齐/字节序问题如何被规避。
3. 区分"精确数学"与"近似/查表加速"：知道 `nm_haversine_m` 是真实大圆距离、`nm_fast_dist_m` 是等距近似、`nm_mercator_y_fast` 是故意不精确的查表版，以及各自用在哪条路径。
4. 说清"为什么测距只能用 haversine，不能用 Web Mercator 像素距离"——墨卡托在高纬度会把距离显著拉伸。
5. 理解大地基准七参数（Bursa-Wolf / Helmert）的"经纬度 ⇄ ECEF ⇄ 七参数变换"链路，并能在 `datum.c` 里找到 `datum_wgs84_to`、`datum_gk_forward`、`datum_solve_seven_param` 等函数。

## 二、功能构成

| 文件 | 关键结构体 / 关键函数 | 行号区间 |
|---|---|---|
| `geo.h` | `NM_EARTH_R_M`、`NM_DEG_NANO` 宏；`nm_lat_to_i32`/`nm_lon_to_i32`/`nm_i32_to_deg` 内联量化器 | `geo.h:12-18` |
| `geo.h` | `nm_haversine_m`、`nm_fast_dist_m`、`nm_mercator`、`nm_mercator_inv`、`nm_mercator_y_fast`、`nm_sun_is_night` 声明 | `geo.h:20-41` |
| `geo.c` | `nm_haversine_m`（大圆测距） | `geo.c:10-20` |
| `geo.c` | `nm_fast_dist_m`（等距近似，int32 e7 入参） | `geo.c:22-31` |
| `geo.c` | `nm_mercator` / `nm_mercator_inv`（精确墨卡托，只用于显示/反算） | `geo.c:33-47` |
| `geo.c` | `MY_BITS=15` 快查表：`my_table_build` + `nm_mercator_y_fast` | `geo.c:74-109` |
| `datum.h` | `DatumId` 枚举（`DATUM_WGS84`/`CGCS2000`/`BJ54`/`XA80`） | `datum.h:22-28` |
| `datum.h` | `GridTypeId` 枚举、`grid_gk_datum` | `datum.h:40-50` |
| `datum.h` | `DatumSevenParam`、`DatumTie`、`DatumGridDesc` 结构体 | `datum.h:104-151` |
| `datum.c` | `DatumDef` 内部结构、`DATUMS` 参数表 | `datum.c:93-139` |
| `datum.c` | `geod_to_ecef` / `ecef_to_geod`（大地坐标 ⇄ ECEF） | `datum.c:155-183` |
| `datum.c` | `helmert_matrix` / `inv3` / `apply_fwd` / `apply_inv`（七参数正/逆） | `datum.c:188-248` |
| `datum.c` | `datum_wgs84_to` / `datum_to_wgs84` | `datum.c:297-333` |
| `datum.c` | `datum_solve_seven_param`（重合点最小二乘解七参数） | `datum.c:501-560` |
| `datum.c` | `gauss_fwd` / `gauss_inv`（高斯-克吕格正/反算） | `datum.c:666-719` |
| `datum.c` | `datum_gk_forward` / `datum_gk_inverse`（方里格投影） | `datum.c:721-737` |

## 三、完整实现代码

### 3.1 `geo.h`（完整头文件）

```h
#ifndef NAVIMAP_GEO_H
#define NAVIMAP_GEO_H

#include <stdint.h>
#include <math.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NM_EARTH_R_M 6371008.8
#define NM_DEG_NANO  1000000000LL   /* PBF 默认 granularity=100 → 单位是 1e-7 度 */

/* 经纬度（度）↔ 量化整数（1e-7 度）。落盘统一用它，避免浮点对齐/字节序问题。 */
static inline int32_t nm_lat_to_i32(double lat) { return (int32_t)llround(lat * 1e7); }
static inline int32_t nm_lon_to_i32(double lon) { return (int32_t)llround(lon * 1e7); }
static inline double  nm_i32_to_deg(int32_t v)  { return (double)v / 1e7; }

/* 大圆距离（米）。真实距离一律用它，不要用 Web Mercator 的像素距离算。 */
double nm_haversine_m(double lat1, double lon1, double lat2, double lon2);

/* 近似距离（米）：小范围（<数公里）用等距投影，比 haversine 快很多，
   建图时算边权用它（误差在几十公里内可忽略）。 */
double nm_fast_dist_m(int32_t lat1_e7, int32_t lon1_e7, int32_t lat2_e7, int32_t lon2_e7);

/* Web Mercator：只用于显示，不用于测距。
   返回归一化到 [0,1] 的世界坐标（原点左上）。 */
void nm_mercator(double lat, double lon, double *wx, double *wy);
void nm_mercator_inv(double wx, double wy, double *lat, double *lon);

/* 同上，但 y 用查表插值算（只依赖纬度）。给**每帧几十万次以上**的热路径
   （渲染投影）用；精度损失见 geo.c 里的误差账（z18 下 <0.02 px）。
   不在 gdb/验收里期待它与 nm_mercator 逐位相等——它有意为之地差一点点，
   换来的是 5~6 倍的投影速度。 */
double nm_mercator_y_fast(double lat);

/* 日出日落：给定 lat/lon（度，北纬/东经为正）与 UTC 秒，返回 1=夜间（太阳在地平线下，
 * 用含折射的日出/日落阈值 90.833°）。本地太阳时算法，不依赖系统时区，对"自动昼夜"足够。
 * 极区恒昼/恒夜也能正确处理（天顶距恒 >90 即判夜间）。 */
int nm_sun_is_night(double lat, double lon, time_t utc);

#ifdef __cplusplus
}
#endif

#endif
```

### 3.2 `geo.c` 核心函数

```c
#include "utils/geo.h"

#include <stdlib.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

double nm_haversine_m(double lat1, double lon1, double lat2, double lon2)
{
    double p1 = lat1 * M_PI / 180.0;
    double p2 = lat2 * M_PI / 180.0;
    double dp = p2 - p1;
    double dl = (lon2 - lon1) * M_PI / 180.0;
    double a = sin(dp / 2) * sin(dp / 2) +
               cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);
    double c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
    return NM_EARTH_R_M * c;
}

double nm_fast_dist_m(int32_t lat1_e7, int32_t lon1_e7, int32_t lat2_e7, int32_t lon2_e7)
{
    double dlat = (double)(lat2_e7 - lat1_e7) / 1e7;
    double dlon = (double)(lon2_e7 - lon1_e7) / 1e7;
    double latm = (double)((lat1_e7 + lat2_e7) / 2) / 1e7;
    double kx = cos(latm * M_PI / 180.0);
    double dy = dlat * (M_PI / 180.0) * NM_EARTH_R_M;
    double dx = dlon * (M_PI / 180.0) * NM_EARTH_R_M * kx;
    return sqrt(dx * dx + dy * dy);
}

void nm_mercator(double lat, double lon, double *wx, double *wy)
{
    double x = (lon + 180.0) / 360.0;
    double s = sin(lat * M_PI / 180.0);
    double y = 0.5 - log((1.0 + s) / (1.0 - s)) / (4.0 * M_PI);
    *wx = x;
    *wy = y;
}

void nm_mercator_inv(double wx, double wy, double *lat, double *lon)
{
    *lon = wx * 360.0 - 180.0;
    double n = M_PI * (1.0 - 2.0 * wy);
    *lat = 180.0 / M_PI * atan(sinh(n));
}
```

**墨卡托快查表**（`MY_BITS=15`，即 `2^15=32768` 格、约 256 KB）。注意它故意不精确——只给渲染热路径（`mv_project`）用，绝不能指望它与 `nm_mercator` 逐位相等。

```c
#define MY_BITS   15
#define MY_N      (1u << MY_BITS)          /* 32768 格 → 256 KB */
#define MY_RANGE  85.0511287798066         /* 标准瓦片方案的纬度上限 */

static double *g_my;                       /* MY_N + 1 个采样点 */

static void my_table_build(void)
{
    if (g_my) return;
    double *t = malloc((MY_N + 1) * sizeof(double));
    if (!t) return;                        /* 分配失败就一直走精确版 */
    for (uint32_t i = 0; i <= MY_N; i++) {
        double lat = -MY_RANGE + (2.0 * MY_RANGE) * (double)i / (double)MY_N;
        double s = sin(lat * M_PI / 180.0);
        t[i] = 0.5 - log((1.0 + s) / (1.0 - s)) / (4.0 * M_PI);
    }
    g_my = t;
}

double nm_mercator_y_fast(double lat)
{
    double *t = g_my;
    if (!t) { my_table_build(); t = g_my; }
    if (!t) {                              /* 没表：退回精确公式 */
        double s = sin(lat * M_PI / 180.0);
        return 0.5 - log((1.0 + s) / (1.0 - s)) / (4.0 * M_PI);
    }
    if (lat < -MY_RANGE) lat = -MY_RANGE;
    else if (lat > MY_RANGE) lat = MY_RANGE;

    double u = (lat + MY_RANGE) * ((double)MY_N / (2.0 * MY_RANGE));
    uint32_t i = (uint32_t)u;
    if (i >= MY_N) i = MY_N - 1;
    double f = u - (double)i;
    return t[i] + (t[i + 1] - t[i]) * f;
}
```

`nm_sun_is_night`（日出日落判夜，含极区恒昼/恒夜处理）声明见 `geo.h:41`，为控制篇幅省略完整体，可在 `src/utils/geo.c:121` 查阅。

### 3.3 `datum.h` 关键类型与声明

```h
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
 * ★ datum_selftest 的「国标公式等价性」判据就靠它们：参数从**模块里读出来**，
 *   再由测试用另一条独立公式复算 ⇒ 以后改参数不会误报，只有算法错才 FAIL。 */
void datum_translation(DatumId id, double *dx, double *dy, double *dz);  /* 米 */
void datum_ellipsoid(DatumId id, double *a, double *f);                  /* 米 / 扁率 */

/* 高斯-克吕格投影（方里格叠加层用）
 *
 * 仅用于坐标网格叠加层的"方里格"（公里格网），底图几何（WGS84 Web Mercator）
 * 不受影响。对外接口统一用 WGS84 经纬度，内部透明地把经纬度先换到所选基准、
 * 再用该基准椭球做高斯投影 —— 与 datum_wgs84_to / datum_to_wgs84 同理。
 *
 *   - lon0_deg：3° 带中央经线（度）。调用方按可见区中心经度 round(lon/3)*3 给出，
 *     正算/反算必须用同一 lon0 才闭合。
 *   - X：北向坐标（米，赤道起算）；Y：东向坐标（米，含 500km 假东偏移）。
 *   - 公式到 1.5° 项（含 η²/μ 高次项），单点误差米级，叠加层足够。
 */
void datum_gk_forward(DatumId id, double lat, double lon, double lon0_deg,
                      double *X, double *Y);
void datum_gk_inverse(DatumId id, double X, double Y, double lon0_deg,
                      double *lat, double *lon);
```

**七参数结构 `DatumSevenParam`、重合点 `DatumTie`、格网描述 `DatumGridDesc`：**

```h
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
 * ★ 解出来后调用 datum_set_seven_param 装回去即生效；自测 datum_selftest
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
```

### 3.4 `datum.c` 实现（七参数与方里格）

**内部结构与出厂/运行期参数表：**

```c
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
    /* 北京 1954（克拉索夫斯基 1940 椭球）。地心三参数（米）= (15.8, −154.4, −82.3）。
     * 这是 **EPSG:4214 的默认 TOWGS84**，也就是 PROJ/GDAL/QGIS/ArcGIS 加载
     * "EPSG:4214 北京1954" 时实际使用的那组值（EPSG 条目编号 15921，+towgs84 语义
     * 与本表"基准→WGS84"一致）⇒ 与主流工具链行为一致，量级正确。 */
    { {6378245.0, 1.0 / 298.3}, 15.8, -154.4, -82.3, 0, 0, 0, 0 },
    /* 西安 1980（IAG-75 椭球）。
     * EPSG 未登记 XA80↔WGS84，但公开文献有：赵玉明等《西安1980坐标转换为
     * WGS1984坐标的3参数近似估算方法》，中国水土保持科学 2013, 11(3): 78-83。
     * 全国 10 省 / 14 片区 / 61 样点拟合，全国统一 3 参数（米）：
     *     dX = −104.57, dY = −45.94, dZ = +5.30
     * 方向 = [XYZ]84 = [XYZ]80 + d，正是本表语义（基准→WGS84，同 +towgs84）。 */
    { {6378140.0, 1.0 / 298.257}, -104.57, -45.94, 5.30, 0, 0, 0, 0 },
};

/* 运行期参数表（初始 = 出厂值，可被 datum_set_seven_param / datum_set_local_offset 改） */
static DatumDef DATUMS[DATUM_COUNT] = {
    { {6378137.0, 1.0 / 298.257223563}, 0, 0, 0, 0, 0, 0, 0 },
    { {6378137.0, 1.0 / 298.257222101}, 0, 0, 0, 0, 0, 0, 0 },
    { {6378245.0, 1.0 / 298.3}, 15.8, -154.4, -82.3, 0, 0, 0, 0 },
    { {6378140.0, 1.0 / 298.257}, -104.57, -45.94, 5.30, 0, 0, 0, 0 },
};
```

**大地坐标 ⇄ ECEF（在给定椭球上）：**

```c
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
```

**七参数矩阵、精确求逆、正/逆变换（正/反严格互逆）：**

```c
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
```

**对外变换接口（读数/标注用）：**

```c
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
```

**重合点最小二乘解七参数（Bursa-Wolf，列均衡：旋转按 µrad、尺度按 ppm 计）：**

```c
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
```

**高斯-克吕格正/反算（方里格投影核心）与对外接口：**

```c
/* 纯高斯正算：基准经纬度(lat,lon) + 椭球(a,f) + 中央经线 lon0(度) → 平面 (X北, Y东)。 */
static void gauss_fwd(double a, double f, double lon0, double lat, double lon,
                      double *X, double *Y)
{
    double e2  = f * (2.0 - f);
    double L   = (lon - lon0) * M_PI / 180.0;        /* 中央经线差（弧度） */
    double B   = lat * M_PI / 180.0;
    double sinB = sin(B), cosB = cos(B), tanB = tan(B);
    double N   = a / sqrt(1.0 - e2 * sinB * sinB);
    double T   = tanB * tanB;
    double C   = (e2 / (1.0 - e2)) * cosB * cosB;
    double Ap  = cosB * L;
    double A2 = Ap * Ap, A4 = A2 * A2, A6 = A4 * A2;
    double M = a * ((1.0 - e2/4.0 - 3.0*e2*e2/64.0 - 5.0*e2*e2*e2/256.0) * B
                    - (3.0*e2/8.0 + 3.0*e2*e2/32.0 + 45.0*e2*e2*e2/1024.0) * sin(2.0*B)
                    + (15.0*e2*e2/256.0 + 45.0*e2*e2*e2/1024.0) * sin(4.0*B)
                    - (35.0*e2*e2*e2/3072.0) * sin(6.0*B));
    double x = M + N * tanB * (A2/2.0
                    + (5.0 - T + 9.0*C + 4.0*C*C) * A4 / 24.0
                    + (61.0 - 58.0*T + T*T + 600.0*C - 330.0*e2) * A6 / 720.0);
    double y = N * (Ap + (1.0 - T + C) * Ap * A2 / 6.0
                    + (5.0 - 18.0*T + T*T + 72.0*C - 58.0*e2) * Ap * A4 / 120.0);
    *X = x;
    *Y = y + 500000.0;          /* 500km 假东偏移，与国标高斯投影一致 */
}

/* 纯高斯反算：平面 (X北, Y东) + 椭球(a,f) + 中央经线 lon0(度) → 基准经纬度(lat,lon)。 */
static void gauss_inv(double a, double f, double lon0, double X, double Y,
                      double *lat, double *lon)
{
    double e2  = f * (2.0 - f);
    double e1  = (1.0 - sqrt(1.0 - e2)) / (1.0 + sqrt(1.0 - e2));
    double yc  = Y - 500000.0;
    double M   = X;
    double mu  = M / (a * (1.0 - e2/4.0 - 3.0*e2*e2/64.0 - 5.0*e2*e2*e2/256.0));
    double B0  = mu + (3.0*e1/2.0 - 27.0*e1*e1*e1/32.0) * sin(2.0*mu)
                   + (21.0*e1*e1/16.0 - 55.0*e1*e1*e1*e1/32.0) * sin(4.0*mu)
                   + (151.0*e1*e1*e1/96.0) * sin(6.0*mu)
                   + (1097.0*e1*e1*e1*e1/512.0) * sin(8.0*mu);
    double sinB0 = sin(B0), cosB0 = cos(B0), tanB0 = tan(B0);
    double N0 = a / sqrt(1.0 - e2 * sinB0 * sinB0);
    double T0 = tanB0 * tanB0;
    double C0 = (e2 / (1.0 - e2)) * cosB0 * cosB0;
    double R0 = a * (1.0 - e2) / pow(1.0 - e2 * sinB0 * sinB0, 1.5);
    double D  = yc / N0;
    double D2 = D*D, D4 = D2*D2, D6 = D4*D2;
    double B = B0 - (N0 * tanB0 / R0) * (D2/2.0
                    - (5.0 + 3.0*T0 + 10.0*C0 - 4.0*C0*C0 - 9.0*e2) * D4 / 24.0
                    + (61.0 + 90.0*T0 + 298.0*C0 + 45.0*T0*T0 - 252.0*e2) * D6 / 720.0);
    double L = (D - (1.0 + 2.0*T0 + C0) * D2 * D / 6.0
                + (5.0 - 2.0*C0 + 28.0*T0 - 3.0*C0*C0 + 8.0*e2 + 24.0*T0*T0) * D4 * D / 120.0)
               / cosB0;
    *lat = B * 180.0 / M_PI;
    *lon = lon0 + L * 180.0 / M_PI;
}

void datum_gk_forward(DatumId id, double lat, double lon, double lon0_deg,
                      double *X, double *Y)
{
    double a, f; datum_ellipsoid(id, &a, &f);
    double lad, lod;
    datum_wgs84_to(id, lat, lon, 0, &lad, &lod, NULL);  /* WGS84 → 基准经纬度 */
    gauss_fwd(a, f, lon0_deg, lad, lod, X, Y);
}

void datum_gk_inverse(DatumId id, double X, double Y, double lon0_deg,
                      double *lat, double *lon)
{
    double a, f; datum_ellipsoid(id, &a, &f);
    double lad, lod;
    gauss_inv(a, f, lon0_deg, X, Y, &lad, &lod);        /* 平面 → 基准经纬度 */
    datum_to_wgs84(id, lad, lod, 0, lat, lon, NULL);    /* 基准经纬度 → WGS84 */
}
```

> 注：`datum.c` 顶部（约 1–73 行）是一段关于参数来源、局限与"为何不做多组参数"的长注释，以及 `datum_short`/`datum_label`/`datum_note`/`datum_count`/`datum_by_index`/`datum_translation`/`datum_ellipsoid`/`datum_get_seven_param`/`datum_set_seven_param`/`datum_reset_params`/`datum_set_local_offset`/`grid_sample`/`shift_by_meters`/`datum_grid_set`/`datum_grid_clear`/`datum_grid_active`/`datum_grid_load` 等函数。本课聚焦七参数与方里格链路，上述辅助函数与其长注释为控制篇幅未逐一全文展示，可在 `src/utils/datum.c`、`src/utils/datum.h` 按行号查阅。

## 四、与其他功能之间的联系

- **下游 · 第 6 课视口投影**：`nm_mercator_y_fast` 专供渲染热路径 `mv_project`；`nm_mercator_inv` 用于屏幕坐标↝经纬度反查（鼠标拾取、框选）。
- **下游 · 第 10 课路由**：`nm_haversine_m` 是 A* 的 admissible 启发式（`route/cost.c`，≈ `÷130km/h`）；`nm_fast_dist_m` 是建图边权与吸附距离（注意它不能做 A* 启发式，会被跨纬度高估，破坏 admissible）。
- **下游 · UI 坐标网（第 12 课 Qt 前端）**：`datum_wgs84_to` + `datum_gk_forward` 驱动 Qt 前端的经纬网/方里格叠加层与右下角读数；`grid_gk_datum` 决定方里格用哪个基准。
- **上游 · 第 3 课 PBF 解析**：PBF 落盘量化单位 `1e-7` 度（`NM_DEG_NANO`）正是 `nm_lat_to_i32`/`nm_i32_to_deg` 的语义，L3 解出的节点坐标直接以该 int32 落盘。
- **下游 · 第 11 课导航语音**：`nm_mercator`/`nm_mercator_y_fast` 决定的屏幕定位，与语音播报的"前方多少米转向"提示在视图上一致。

## 五、演示、练习与验收

- **演示**：运行 `./build_rel/datum_selftest`。它验证坐标系枚举、WGS84 恒等、BJ54/XA80 往返互逆、与国标公式的等价性，并含负向对照（错误参数应 FAIL）。
- **练习**：用 `nm_haversine_m` 与 `nm_fast_dist_m` 分别算北京→上海距离，对比误差与速度；再用 `datum_solve_seven_param` 用几组合成重合点解七参数，验证能还原 `DATUMS` 出厂参数。
- **验收**：能说清"为什么内部用 WGS84、为什么投影要分快慢两版、为什么测距不能用 Web Mercator 像素距离、七参数正/逆为何必须严格互逆"，并能在 `geo.c`/`datum.c` 中逐函数指认上述实现。
