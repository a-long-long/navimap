#include "route/navsim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "utils/geo.h"

/* ---------------------------------------------------------------------------
 * 局部平面投影：把经纬度换算成以 (lat0,lon0) 为原点的米坐标。
 * 只在几公里内用（点到线段的垂距），用球面公式反而没必要：
 * 这里 1 度的纬度差固定 111.2 km，经度差乘 cos(lat)，误差 <0.1%，
 * 对"偏航 60 m 就算偏"这种阈值完全够。
 * ------------------------------------------------------------------------- */
static void to_local(double lat, double lon, double lat0, double lon0,
                     double *x, double *y)
{
    const double R = NM_EARTH_R_M;
    double p = lat0 * M_PI / 180.0;
    *x = (lon - lon0) * M_PI / 180.0 * R * cos(p);
    *y = (lat - lat0) * M_PI / 180.0 * R;
}

static double bearing_deg(double la0, double lo0, double la1, double lo1)
{
    double p1 = la0 * M_PI / 180.0, p2 = la1 * M_PI / 180.0;
    double dl = (lo1 - lo0) * M_PI / 180.0;
    double y = sin(dl) * cos(p2);
    double x = cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dl);
    double b = atan2(y, x) * 180.0 / M_PI;
    if (b < 0) b += 360.0;
    return b;
}

/* 车头沿当前段插值放到 (seg, seg_pos) */
static void car_place(NavSim *s)
{
    const RouteResult *rr = s->rr;
    if (!rr || rr->n < 2) return;
    uint32_t i = s->seg;
    if (i + 1 >= rr->n) i = rr->n - 2;
    double lm = rr->seg_m[i];
    double t = lm > 0.0 ? s->seg_pos / lm : 0.0;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    double la0 = rr->lat_e7[i] / 1e7, lo0 = rr->lon_e7[i] / 1e7;
    double la1 = rr->lat_e7[i + 1] / 1e7, lo1 = rr->lon_e7[i + 1] / 1e7;
    s->car_lat = la0 + (la1 - la0) * t;
    s->car_lon = lo0 + (lo1 - lo0) * t;
    s->heading_deg = bearing_deg(la0, lo0, la1, lo1);
}

int navsim_start(NavSim *s, const RouteResult *rr)
{
    if (!s || !rr || !rr->ok || rr->n < 2) return -1;
    memset(s, 0, sizeof(*s));
    s->rr = rr;
    s->seg = 0;
    s->seg_pos = 0;
    s->speed_mul = 1;
    car_place(s);
    return 0;
}

double navsim_speed_ms(const NavSim *s)
{
    if (!s->rr || s->rr->n < 2) return 13.9;
    uint32_t i = s->seg;
    if (i + 1 >= s->rr->n) i = s->rr->n - 2;
    double lm = s->rr->seg_m[i];
    double ls = s->rr->seg_ds ? s->rr->seg_ds[i] / 10.0 : 0.0;
    /* 段长为 0 或没记耗时（老数据/接缝）时退回 50 km/h，别除出 inf */
    if (lm <= 0.0 || ls <= 0.0) return 50.0 * 1000.0 / 3600.0;
    double v = lm / ls;
    if (v < 1.0) v = 1.0;          /* 步行道 5 km/h 也行，但不能是 0 */
    if (v > 55.0) v = 55.0;        /* 200 km/h：限速字段偶尔有脏数据 */
    return v;
}

int navsim_advance(NavSim *s, double dt_real)
{
    if (!s || !s->rr) return -1;
    if (s->arrived) return 1;
    if (dt_real <= 0) return 0;

    double mul = s->speed_mul > 0 ? s->speed_mul : 1.0;
    double dist = navsim_speed_ms(s) * dt_real * mul;
    s->traveled_s += dt_real * mul;      /* 模拟世界里经过的时间 */

    uint32_t nseg = s->rr->n - 1;
    while (dist > 0) {
        if (s->seg >= nseg) { s->arrived = 1; break; }
        double lm = s->rr->seg_m[s->seg];
        double remain = lm - s->seg_pos;
        if (remain < 0) remain = 0;
        if (dist < remain) {
            s->seg_pos += dist;
            s->traveled_m += dist;
            dist = 0;
        } else {
            dist -= remain;
            s->traveled_m += remain;
            s->seg_pos = 0;
            s->seg++;
            if (s->seg >= nseg) {        /* 走完最后一段 */
                s->seg = nseg - 1;
                s->seg_pos = s->rr->seg_m[s->seg];
                s->arrived = 1;
                s->traveled_m = s->rr->total_m;
                dist = 0;
            }
        }
    }
    car_place(s);
    return s->arrived ? 1 : 0;
}

/* 点到折线的最近点。
 * 先看当前段附近的一段窗口（车不会瞬移），窗口边界命中就退化为全扫——
 * 长途路线几万段，每帧全扫太浪费，但偏航瞬移时必须能找到真最近点。 */
static void project(const NavSim *s, double lat, double lon,
                    double *off_m, double *proj_m, uint32_t *proj_seg)
{
    const RouteResult *rr = s->rr;
    uint32_t nseg = rr->n - 1;
    double best_d2 = 1e300, best_m = 0, best_t = 0;
    uint32_t best_i = 0;

    uint32_t lo = 0, hi = nseg;
    int windowed = 0;
    if (nseg > 512) {
        lo = s->seg > 64 ? s->seg - 64 : 0;
        hi = s->seg + 256 < nseg ? s->seg + 256 : nseg;
        windowed = 1;
    }

    for (int pass = 0; pass < 2; pass++) {
        best_d2 = 1e300;
        if (pass == 1) { lo = 0; hi = nseg; windowed = 0; }
        for (uint32_t i = lo; i < hi; i++) {
            double a_la = rr->lat_e7[i] / 1e7, a_lo = rr->lon_e7[i] / 1e7;
            double b_la = rr->lat_e7[i + 1] / 1e7, b_lo = rr->lon_e7[i + 1] / 1e7;
            double ax, ay, bx, by;
            to_local(a_la, a_lo, lat, lon, &ax, &ay);
            to_local(b_la, b_lo, lat, lon, &bx, &by);
            double dx = bx - ax, dy = by - ay;
            double L2 = dx * dx + dy * dy;
            double t = 0;
            if (L2 > 1e-9) {
                t = -(ax * dx + ay * dy) / L2;
                if (t < 0) t = 0; else if (t > 1) t = 1;
            }
            double cx = ax + dx * t, cy = ay + dy * t;
            double d2 = cx * cx + cy * cy;
            if (d2 < best_d2) {
                best_d2 = d2;
                best_i = i;
                best_t = t;
            }
        }
        /* 最近点落在窗口边界上：真实最近点可能在窗口外，全扫一次 */
        if (!windowed) break;
        if (best_i != lo && best_i + 1 != hi) break;
    }

    /* 投影点的里程 = 前面整段之和 + 本段内比例 */
    double acc = 0;
    for (uint32_t i = 0; i < best_i; i++) acc += rr->seg_m[i];
    best_m = acc + rr->seg_m[best_i] * best_t;

    if (off_m)   *off_m = sqrt(best_d2);
    if (proj_m)  *proj_m = best_m;
    if (proj_seg)*proj_seg = best_i;
}

int navsim_locate(NavSim *s, double lat, double lon)
{
    if (!s || !s->rr || s->rr->n < 2) return -1;
    s->has_gps = 1;
    s->gps_lat = lat;
    s->gps_lon = lon;
    project(s, lat, lon, &s->off_m, &s->proj_m, &s->proj_seg);
    return 0;
}

/* 从起点走到里程 m 处所花的时间（秒）：按各段自己的限速累加。 */
static double time_at_m(const NavSim *s, double m)
{
    const RouteResult *rr = s->rr;
    uint32_t nseg = rr->n - 1;
    double acc_m = 0, acc_s = 0;
    for (uint32_t i = 0; i < nseg; i++) {
        double lm = rr->seg_m[i];
        double ls = rr->seg_ds ? rr->seg_ds[i] / 10.0 : 0.0;
        if (m <= acc_m + lm) {
            double f = lm > 0 ? (m - acc_m) / lm : 0;
            return acc_s + ls * f;
        }
        acc_m += lm;
        acc_s += ls;
    }
    return acc_s;
}

static double base_m(const NavSim *s)
{
    /* 有定位点时以投影里程为准：车头是插值出来的，永远贴着路线；
       真实的"我走到哪了"要看定位点（偏航后尤其如此）。 */
    return s->has_gps ? s->proj_m : s->traveled_m;
}

double navsim_remaining_m(const NavSim *s)
{
    if (!s || !s->rr) return 0;
    double r = s->rr->total_m - base_m(s);
    return r > 0 ? r : 0;
}

double navsim_remaining_s(const NavSim *s)
{
    if (!s || !s->rr) return 0;
    double r = s->rr->total_s - time_at_m(s, base_m(s));
    return r > 0 ? r : 0;
}

int navsim_offroute(const NavSim *s, double threshold_m)
{
    if (!s || !s->has_gps) return 0;      /* 没定位点 = 无从判断 */
    if (threshold_m <= 0) threshold_m = NAVSIM_OFFROUTE_DEFAULT_M;
    return s->off_m > threshold_m;
}

int navsim_next_maneuver(const NavSim *s, const ManeuverList *ml,
                         double *to_go_m, int *prev)
{
    if (prev) *prev = -1;
    if (to_go_m) *to_go_m = 0;
    if (!s || !ml || ml->n <= 0) return -1;

    double here = base_m(s);
    double cum = 0;
    for (int i = 0; i < ml->n; i++) {
        cum += ml->v[i].dist_m;
        if (cum > here + 0.5) {
            if (to_go_m) *to_go_m = cum - here;
            if (prev) *prev = i - 1;
            return i;
        }
    }
    if (prev) *prev = ml->n - 1;
    return -1;
}

void navsim_offset_point(double lat, double lon, double heading_deg,
                         double right_m, double *olat, double *olon)
{
    const double R = NM_EARTH_R_M;
    double h = heading_deg * M_PI / 180.0;
    double dx =  cos(h) * right_m;        /* 东 */
    double dy = -sin(h) * right_m;        /* 北 */
    *olat = lat + dy / (R * M_PI / 180.0);
    *olon = lon + dx / (R * M_PI / 180.0 * cos(lat * M_PI / 180.0));
}

double navsim_progress(const NavSim *s)
{
    if (!s || !s->rr || s->rr->total_m <= 0) return 0;
    double p = base_m(s) / s->rr->total_m;
    if (p < 0) p = 0;
    if (p > 1) p = 1;
    return p;
}
