#ifndef NAVIMAP_GPX_H
#define NAVIMAP_GPX_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

/* ★ C++ 端（ui_qt 下的 .cpp）也要 include 本头 ⇒ 必须 extern "C"，
 * 否则符号被名字改编、链接期报 undefined reference（datum.h 踩过）。 */

#ifdef __cplusplus
extern "C" {
#endif

/* GPX 1.1 子集：轨迹（trk/rte）与航点（wpt）。
 *
 * 定位：导航项目缺的"落地"一环——把户外记录仪/手机 App 导出的 GPX 导进来看，
 * 把本程序的收藏/轨迹导出成标准 GPX 交给别人用。只实现 1.1 的最常用子集：
 *   <trk><name>..</name><trkseg><trkpt lat lon>…</trkseg></trk>   轨迹
 *   <rte><name>..</name><rtept lat lon>…</rte>                     路线（归一成轨迹）
 *   <wpt lat lon><name>..</name></wpt>                              航点 → 收藏
 * ele/time/cmt 等其余元素跳过（不影响坐标）。
 *
 * 坐标用 int32 e7（1e-7 度，≈1.1 cm）——与 nodes/places 全项目一致，避免混用单位。
 * 名字按 UTF-8 原样保存（GPX 规范就是 UTF-8），不做任何转码。
 */

#define GPX_NAME_MAX 128

typedef struct {
    char     name[GPX_NAME_MAX];
    int32_t *lat_e7;
    int32_t *lon_e7;
    uint32_t n;
    uint32_t cap;
    uint8_t  r, g, b;        /* 渲染颜色（导入时按序号从调色轮轮换） */
    int64_t  db_id;          /* 入库后的 id（0=还没写库）；删除时按它删库记录 */
} GpxTrack;

typedef struct {
    char    name[GPX_NAME_MAX];
    double  lat, lon;
    int64_t db_id;           /* 同上 */
} GpxWpt;

typedef struct {
    GpxTrack *trk;
    int       n_trk, cap_trk;
    GpxWpt   *wpt;
    int       n_wpt, cap_wpt;
} GpxStore;

/* ---- 内存store：init/free/追加 ---- */
void gpx_store_init(GpxStore *s);
void gpx_store_free(GpxStore *s);
/* 追加一条空轨迹（颜色从调色板按当前条数轮换），返回其下标；失败 -1。 */
int  gpx_store_add_track(GpxStore *s, const char *name);
/* 给轨迹追加点（n==0 时只确保容量）。返回 0。 */
int  gpx_track_push(GpxTrack *t, double lat, double lon);
/* 追加收藏点。 */
int  gpx_store_add_wpt(GpxStore *s, const char *name, double lat, double lon);
/* 删除第 i 条轨迹 / 第 i 个收藏（越界不动）。 */
void gpx_store_del_track(GpxStore *s, int i);
void gpx_store_del_wpt(GpxStore *s, int i);

/* ---- 解析 / 写出 ---- */
/* 解析一段 GPX XML，结果**追加**到 out（导入多个文件不清空旧的）。
 * 容错：跳过注释 / CDATA / BOM；缺 lat 或 lon 的 trkpt 跳过；坏标签不致命。
 * 返回：>=0 解析出的轨迹条数；-1 参数错；-2 一个点都没解析到。 */
int  gpx_parse(const char *xml, size_t len, GpxStore *out);
/* 从文件读并解析（小文件一次性读；轨迹文件通常 < 几十 MB）。 */
int  gpx_parse_file(const char *path, GpxStore *out);
/* 写出标准 GPX 1.1（UTF-8，带 <gpx> 头与 xmlns）。返回 0。 */
int  gpx_write(const GpxStore *s, const char *path);

/* ---- sqlite 持久化（tracks.db）----
 * 与 places.db 的关系：places.db 是抽取期产物、只读；tracks.db 是**用户数据**，
 * 可写。分两个文件，避免用户数据混进可再生的产物里。
 * 三张表：track(id,name,r,g,b) / tpoint(tid,seq,lat,lon) / fav(id,name,lat,lon)。 */
void *gpx_db_open(const char *path);          /* 失败 NULL（句柄不透明） */
void  gpx_db_close(void *db);
/* 把整库轨迹/收藏读进 store（先清空 store）。返回 0；库不存在返回 0 且 store 为空。 */
int   gpx_db_load_all(void *db, GpxStore *s);
/* 写入（幂等按 name 替换：同名轨迹先删后插，重复导入不翻倍）。返回新 id（>=0）。 */
int   gpx_db_save_track(void *db, const GpxTrack *t);
int   gpx_db_add_fav(void *db, const char *name, double lat, double lon);
/* 查同名同坐标的收藏：命中返回其 id（>0），没有返回 0。
 * 用来把"同一个文件导入两次"挡成一次（航点没有 name 唯一约束，
 * 不去重就会每次导入都翻一倍）。 */
int   gpx_db_find_fav(void *db, const char *name, double lat, double lon);
void  gpx_db_del_fav(void *db, int id);
void  gpx_db_del_track(void *db, int id);
/* 默认库路径：<config dir>/tracks.db。返回 0。 */
int   gpx_db_default_path(char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* NAVIMAP_GPX_H */
