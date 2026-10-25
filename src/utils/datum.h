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

/* WGS84 经纬度 → 指定基准（读数/标注用）。lat/lon 单位度；h 高程米（通常传 0，
 * 可传 NULL 表示不关心）。out_lat/out_lon 为基准下的经纬度（度）。 */
void datum_wgs84_to(DatumId id, double lat, double lon, double h,
                    double *out_lat, double *out_lon, double *out_h);

/* 指定基准经纬度 → WGS84（把该基准下的经纬网线投到 WGS84 地图上再投影）。 */
void datum_to_wgs84(DatumId id, double lat, double lon, double h,
                    double *out_lat, double *out_lon, double *out_h);

#ifdef __cplusplus
}
#endif

#endif
