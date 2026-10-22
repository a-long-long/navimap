#ifndef NAVIMAP_NAVSIM_H
#define NAVIMAP_NAVSIM_H

#include <stdint.h>
#include "route/route.h"
#include "route/maneuver.h"

/* ---------------------------------------------------------------------------
 * 模拟定位 / 导航推进（阶段 9 导航层）
 *
 * 这一层是**纯计算**：不碰 SDL、不碰 Router、不碰线程。理由和项目里其它
 * 逻辑模块一样——GUI 无法无头验证，能断言的东西必须能在 CLI 里跑。
 * GUI 只负责"每隔 16 ms 调一次 navsim_advance 然后把结果画出来"。
 *
 * 三个能力：
 *   1. 沿路线推进（按当前段的限速走，倍速可调）→ 车头位置 / 朝向 / 已走里程
 *   2. 把任意经纬度投影到路线上 → 已走里程 + 横向偏移 off_m（偏航判定）
 *   3. 剩余里程 / 剩余 ETA（按剩余各段自己的限速求和，不是按总时长摊）
 *
 * ★ "GPS 点"与"车头"是两回事：
 *     car_*  = 沿路线插值的理想车头（推进用）
 *     gps_*  = 注入的定位点（真实 GNSS 的替身）。偏航只信 gps_*。
 *     没注入过 gps 时两者相同（模拟车永远在路线上，不会自己偏航）。
 * ------------------------------------------------------------------------- */

typedef struct {
    const RouteResult *rr;      /* 不拥有：路线来自 App/调用方 */
    uint32_t seg;               /* 当前段下标 [0, n-2] */
    double   seg_pos;           /* 在当前段内已走米数 */
    double   traveled_m;        /* 累计里程（= 前 seg 段之和 + seg_pos） */
    double   traveled_s;        /* 累计耗时（秒，按段限速） */
    double   speed_mul;         /* 倍速：1 = 真实速度 */
    int      arrived;

    double   car_lat, car_lon;  /* 车头（沿路线插值） */
    double   heading_deg;       /* 车头朝向：正北 0°，顺时针 */

    int      has_gps;           /* 是否注入过定位点 */
    double   gps_lat, gps_lon;
    double   off_m;             /* gps 到路线的横向偏移（米） */
    double   proj_m;            /* gps 投影点在路线上的里程（米） */
    uint32_t proj_seg;
} NavSim;

/* 绑定一条路线并把车头放到起点。rr 必须 ok 且 n>=2。 */
int  navsim_start(NavSim *s, const RouteResult *rr);

/* 推进 dt_real 秒（真实时间）；实际走 dt_real * speed_mul 秒的路。
 * 返回：0 正常；1 本步到达终点（再调也不会往前走）。 */
int  navsim_advance(NavSim *s, double dt_real);

/* 注入一个定位点（模拟 GNSS）。会同时算出投影里程与横向偏移。
 * 返回 0。off_m 用来判偏航，proj_m 用来在偏航后把"已走里程"对齐到实际位置。 */
int  navsim_locate(NavSim *s, double lat, double lon);

/* 当前段的限速（m/s）。段长度为 0 或耗时记录缺失时退回 50 km/h。 */
double navsim_speed_ms(const NavSim *s);

/* 剩余里程（米）：从车头（有 gps 时用投影点）到终点。 */
double navsim_remaining_m(const NavSim *s);
/* 剩余时间（秒）：剩余各段按自己的限速求和。★ 不是 total_s * 剩余比例。 */
double navsim_remaining_s(const NavSim *s);

/* 偏航判定。threshold_m <= 0 时用默认 60 m。
 * 只认 gps：没注入过定位点就永远不算偏航（车头是沿路线插值的，不可能偏）。 */
int  navsim_offroute(const NavSim *s, double threshold_m);
#define NAVSIM_OFFROUTE_DEFAULT_M 60.0

/* 当前该播报的指令（下一条）：ml 里第一条累计里程超过当前已走里程的那条。
 * 返回下标，-1 = 已过完（快到了）。
 * *to_go_m（可空）返回距该指令还有多少米；*prev 返回刚刚经过的那条（-1 无）。 */
int  navsim_next_maneuver(const NavSim *s, const ManeuverList *ml,
                          double *to_go_m, int *prev);

/* 已走里程在整条路线里的比例 [0,1]（画"已走过"那段用） */
double navsim_progress(const NavSim *s);

/* 从 (lat,lon) 沿航向的**右手**方向平移 d 米。
 * 两个用处：CLI 里造"走错了路"的定位点；GUI 里按一下键就模拟一次偏航。
 * ★ 法线是 (cos h, -sin h)（东, 北）；写成 (-sin h, cos h) 得到的是航向
 *   本身，注入的点会落回路线上，偏航永远测不出来（实测踩过一次）。 */
void navsim_offset_point(double lat, double lon, double heading_deg,
                         double right_m, double *olat, double *olon);

#endif
