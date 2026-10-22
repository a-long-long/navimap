#ifndef NAVIMAP_MAP_VIEW_H
#define NAVIMAP_MAP_VIEW_H

#include <stdint.h>
#include "utils/geo.h"      /* nm_mercator_y_fast：mv_proj 内联要用 */

/* 视口：中心经纬度 + 缩放级别 + 屏幕尺寸。
 * 坐标链只有这一处定义，别在别的地方手写投影：
 *   经纬度 → Web Mercator 归一化 [0,1] → 屏幕像素
 * 反过来（unproject）用于鼠标拖拽与点击选点。
 * 注意：测距不要用 Web Mercator（高纬变形大），用 utils/geo.h 的 haversine。
 */
typedef struct {
    double lat, lon;     /* 视口中心 */
    double zoom;         /* 类似瓦片 z：scale = 256 * 2^zoom 像素/世界 */
    int    w, h;         /* 屏幕尺寸（像素） */

    /* 投影缓存（惰性填充，见 map_view.c 的 mv_prepare）。
     * 视口中心的墨卡托坐标与缩放在一帧内是常量，但原先每次 mv_project
     * 都要重算它们：一次 sin + 一次 log + 一次 pow，正好是每次投影开销的
     * 一半。渲染一帧要投影几十万到几百万次，这些全是重复计算。
     * 这里连"缓存键"一起存，逐字段比对做惰性失效，所以调用方直接改
     * lat/lon/zoom/w/h 也不会画错（不用记着去通知缓存）。 */
    int    c_valid;
    double c_lat, c_lon, c_zoom;
    int    c_w, c_h;
    double cx, cy, scale;
} MapView;

void mv_init(MapView *v, double lat, double lon, double zoom, int w, int h);

/* 经纬度 → 屏幕像素（返回 0 表示在视口内、1 表示在外，外也给出坐标以便裁剪） */
int  mv_project(const MapView *v, double lat, double lon, int *x, int *y);

/* ---------------------------------------------------------------------------
 * 批量投影：把"每次调用都要重读的视口缓存"提到循环外（PLAN.md §30）
 *
 * 一帧要投影 250 万次（面 115 万顶点 + 路 118 万端点 + 线），而视口缓存是常量。
 * 但 mv_project 为了"调用方随便改 view 也不会画错"，每次都要比对 5 个缓存键
 * 字段再重新加载 cx/cy/scale/w/h —— 实测这部分占单次投影的 **45%**
 * （1.15M 次：31.4 ms → 17.2 ms；**开了 LTO 也仍在**，因为缓存键比对带写回，
 *  编译器不敢把它提出循环）。
 *
 * 用法：一个 pass 开头 mv_proj_begin() 取一次快照，循环里用 mv_proj()。
 * ★ mv_proj 与 mv_project 是**逐位等价**的（115 万样本比对 0 差异），
 *   只是省掉每次的缓存键比对与字段加载，不是"近似投影"。
 * ★ 快照只在"这一个 pass 内视口不变"时有效——渲染一帧正满足这个条件。
 * ------------------------------------------------------------------------- */
typedef struct {
    double cx, cy, scale;   /* 视口中心的墨卡托坐标 + 每世界单位的像素数 */
    double hw, hh;          /* 画布中心偏移：w/2.0、h/2.0 */
} MvProj;

void mv_proj_begin(const MapView *v, MvProj *p);

static inline void mv_proj(const MvProj *p, double lat, double lon, int *x, int *y)
{
    double wx = (lon + 180.0) / 360.0;
    double wy = nm_mercator_y_fast(lat);
    *x = (int)lround((wx - p->cx) * p->scale + p->hw);
    *y = (int)lround((wy - p->cy) * p->scale + p->hh);
}

/* e7 整数版：热路径手里的经纬度本来就是 int32 e7（落盘格式），省得每个调用点
 * 都写一遍 /1e7。★ 它就是 mv_proj 的包装，逐位等价，没有额外的性能含义
 * （试过让 e7 版"少算一次除法"或"直算查表下标"，都要动浮点结合顺序，
 *   等价性会破，见 PLAN.md §31）。 */
static inline void mv_proj_e7(const MvProj *p, int32_t lat_e7, int32_t lon_e7,
                              int *x, int *y)
{
    mv_proj(p, (double)lat_e7 / 1e7, (double)lon_e7 / 1e7, x, y);
}

/* 屏幕像素 → 经纬度 */
void mv_unproject(const MapView *v, int x, int y, double *lat, double *lon);

/* 当前可见的经纬度范围 lat_min lon_min lat_max lon_max */
void mv_visible_bbox(const MapView *v, double bbox[4]);

/* 拖动：屏幕位移 → 中心点移动 */
void mv_pan_pixels(MapView *v, int dx, int dy);

/* 以屏幕上某点为锚点缩放（滚轮缩放时鼠标位置不动） */
void mv_zoom_at(MapView *v, double dzoom, int px, int py);

void mv_clamp_zoom(MapView *v, double zmin, double zmax);

/* 让给定经纬度矩形刚好装进视口（导航里"缩放到整条路线"用它）。
   margin_px 是四周留白。zoom 由调用方 clamp。 */
void mv_fit_bbox(MapView *v, double lat0, double lon0,
                 double lat1, double lon1, double margin_px);

double mv_scale(const MapView *v);   /* 像素/世界单位 */

#endif
