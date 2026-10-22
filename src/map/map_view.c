#include "map/map_view.h"
#include "utils/geo.h"
#include <math.h>

void mv_init(MapView *v, double lat, double lon, double zoom, int w, int h)
{
    v->lat = lat; v->lon = lon; v->zoom = zoom; v->w = w; v->h = h;
}

double mv_scale(const MapView *v)
{
    return 256.0 * pow(2.0, v->zoom);
}

/* 填好"视口中心墨卡托 + 缩放"这份缓存。
 * 为什么值得：一帧里 mv_project 被调用几十万到几百万次（z9 光路面就有
 * 上百万次投影），而视口中心和缩放是常量——原先每次都替它们重算一遍
 * sin + log + pow，等于把投影成本翻了一倍。
 * 惰性失效（比对缓存键）而不是要求调用方通知：window.c 里到处是直接改
 * view.lat / view.zoom 的，要求"改完必须调 invalidate"迟早会漏一处，
 * 漏了就是画面错位——这类 bug 靠肉眼很难发现。 */
static void mv_prepare(const MapView *v)
{
    MapView *m = (MapView *)v;        /* 只写缓存字段，逻辑上仍是 const */
    if (m->c_valid &&
        m->c_lat == v->lat && m->c_lon == v->lon && m->c_zoom == v->zoom &&
        m->c_w == v->w && m->c_h == v->h)
        return;
    nm_mercator(v->lat, v->lon, &m->cx, &m->cy);
    m->scale = 256.0 * pow(2.0, v->zoom);
    m->c_lat = v->lat; m->c_lon = v->lon; m->c_zoom = v->zoom;
    m->c_w = v->w; m->c_h = v->h;
    m->c_valid = 1;
}

int mv_project(const MapView *v, double lat, double lon, int *x, int *y)
{
    mv_prepare(v);
    MapView *m = (MapView *)v;
    /* ★ 这里不用 nm_mercator：那个 sin+log 版本在每帧几十万次调用的场合
       是真花钱的（z9 一帧 30 ms）。墨卡托的 x 本来就只跟经度线性相关，
       y 只跟纬度相关且足够光滑 → 走查表插值版，见 geo.c 的误差账。 */
    double wx = (lon + 180.0) / 360.0;
    double wy = nm_mercator_y_fast(lat);
    double px = (wx - m->cx) * m->scale + v->w / 2.0;
    double py = (wy - m->cy) * m->scale + v->h / 2.0;
    *x = (int)lround(px);
    *y = (int)lround(py);
    return (*x < 0 || *y < 0 || *x >= v->w || *y >= v->h) ? 1 : 0;
}

void mv_proj_begin(const MapView *v, MvProj *p)
{
    mv_prepare(v);                 /* 与 mv_project 同一份缓存，值完全一致 */
    MapView *m = (MapView *)v;
    p->cx = m->cx; p->cy = m->cy; p->scale = m->scale;
    p->hw = v->w / 2.0; p->hh = v->h / 2.0;
}

void mv_unproject(const MapView *v, int x, int y, double *lat, double *lon)
{
    mv_prepare(v);
    MapView *m = (MapView *)v;
    double wx = m->cx + (x - v->w / 2.0) / m->scale;
    double wy = m->cy + (y - v->h / 2.0) / m->scale;
    nm_mercator_inv(wx, wy, lat, lon);
}

void mv_visible_bbox(const MapView *v, double bbox[4])
{
    double la0, lo0, la1, lo1;
    mv_unproject(v, 0, v->h, &la0, &lo0);          /* 左下 */
    mv_unproject(v, v->w, 0, &la1, &lo1);          /* 右上 */
    bbox[0] = la0; bbox[1] = lo0; bbox[2] = la1; bbox[3] = lo1;
}

void mv_pan_pixels(MapView *v, int dx, int dy)
{
    double cx, cy;
    nm_mercator(v->lat, v->lon, &cx, &cy);
    double s = mv_scale(v);
    double wx = cx - (double)dx / s;
    double wy = cy - (double)dy / s;
    if (wy < 0) wy = 0; else if (wy > 1) wy = 1;
    nm_mercator_inv(wx, wy, &v->lat, &v->lon);
    if (v->lon < -180) v->lon += 360;
    if (v->lon >  180) v->lon -= 360;
}

void mv_zoom_at(MapView *v, double dzoom, int px, int py)
{
    /* 锚点：缩放前后该屏幕点对应的经纬度不变 */
    double alat, alon;
    mv_unproject(v, px, py, &alat, &alon);
    v->zoom += dzoom;
    double ax, ay, cx, cy;
    nm_mercator(alat, alon, &ax, &ay);
    nm_mercator(v->lat, v->lon, &cx, &cy);
    double s = mv_scale(v);
    double wx = ax - (px - v->w / 2.0) / s;
    double wy = ay - (py - v->h / 2.0) / s;
    if (wy < 0) wy = 0; else if (wy > 1) wy = 1;
    nm_mercator_inv(wx, wy, &v->lat, &v->lon);
}

void mv_clamp_zoom(MapView *v, double zmin, double zmax)
{
    if (v->zoom < zmin) v->zoom = zmin;
    if (v->zoom > zmax) v->zoom = zmax;
}

void mv_fit_bbox(MapView *v, double lat0, double lon0,
                 double lat1, double lon1, double margin_px)
{
    double x0, y0, x1, y1;
    nm_mercator(lat0, lon0, &x0, &y0);
    nm_mercator(lat1, lon1, &x1, &y1);
    double dx = fabs(x1 - x0), dy = fabs(y1 - y0);
    if (dx < 1e-9) dx = 1e-9;
    if (dy < 1e-9) dy = 1e-9;

    double aw = (double)v->w - 2.0 * margin_px;
    double ah = (double)v->h - 2.0 * margin_px;
    if (aw < 32) aw = 32;
    if (ah < 32) ah = 32;
    double s = fmin(aw / dx, ah / dy);      /* 像素/世界单位 = 256 * 2^zoom */
    v->zoom = log2(s / 256.0);

    /* 中心 = 墨卡托矩形的中点（不是经纬度中点，否则高纬会偏） */
    nm_mercator_inv((x0 + x1) / 2.0, (y0 + y1) / 2.0, &v->lat, &v->lon);
}
