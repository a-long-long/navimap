#ifndef NAVIMAP_GEO_H
#define NAVIMAP_GEO_H

#include <stdint.h>
#include <math.h>

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

#endif
