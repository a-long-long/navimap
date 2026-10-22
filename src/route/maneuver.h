#ifndef NAVIMAP_MANEUVER_H
#define NAVIMAP_MANEUVER_H

#include <stdint.h>
#include <stddef.h>
#include "route/route.h"
#include "map/mapset.h"

/* ---------------------------------------------------------------------------
 * 转向指令（阶段 9 导航）
 *
 * 输入只有 RouteResult 的几何 + 每段边的标志位，不需要额外数据：
 *   转角 = "身后一段路的方位" 与 "前方一段路的方位" 之差。
 * ★ 前后都要"看远一点"（默认 45 m）再算方位：OSM 的一条 way 会被路口切成
 *   很多小段，逐段算方位会在弯曲的路上每 10 米就报一次"右转"。
 *   取一小段累积窗口后，只有真正的路口转向才会超过阈值。
 * 环岛靠边的 EF_ROUNDABOUT 标志识别（几何分辨不出环岛，靠标志位最省事），
 *   出口序号 = 环内每个路口上"非环岛的出边"计数，含我们最后驶出的那一个。
 * ------------------------------------------------------------------------- */

enum {
    MAN_START = 0,       /* 起点：沿 X 出发 */
    MAN_STRAIGHT,        /* 继续直行（路名变了但方向没变，也用它） */
    MAN_SLIGHT_LEFT,     /* 靠左 */
    MAN_LEFT,            /* 左转 */
    MAN_SHARP_LEFT,      /* 向左急转 */
    MAN_SLIGHT_RIGHT,    /* 靠右 */
    MAN_RIGHT,           /* 右转 */
    MAN_SHARP_RIGHT,     /* 向右急转 */
    MAN_UTURN,           /* 掉头 */
    MAN_ROUNDABOUT,      /* 进入环岛，第 N 个出口驶出 */
    MAN_ARRIVE           /* 到达目的地 */
};

typedef struct {
    uint32_t idx;        /* 对应 RouteResult 的点下标（动作发生的那个路口） */
    int      type;       /* MAN_* */
    double   dist_m;     /* 距上一条指令的路程（米）；起点为 0 */
    double   turn_deg;   /* 转角：右正、左负，范围 (-180,180] */
    char     road[64];   /* 转入道路名（可能为空串） */
    uint8_t  cls;        /* 转入道路等级 RC_* */
    int      exit_no;    /* 环岛第几出口（MAN_ROUNDABOUT 有效，其余为 0） */
} Maneuver;

typedef struct {
    Maneuver *v;
    int       n, cap;
    double    sum_m;     /* Σ dist_m，应等于 rr->total_m（自检用） */
} ManeuverList;

/* 生成指令。rr 必须 ok 且 n>=2。返回指令条数（>=2：至少起点 + 到达）。 */
int  maneuver_build(const RouteResult *rr, const MapSet *ms, ManeuverList *out);
void maneuver_free(ManeuverList *ml);

/* 指令的中文短名（"左转"/"靠右"/"环岛第3出口"…），用于面板与日志 */
const char *maneuver_kind_text(const Maneuver *m);
/* 完整一句话："300 米后右转进入建国路" / "沿京藏高速行驶 12.4 公里" */
void maneuver_text(const Maneuver *m, char *buf, size_t n);
/* 距离格式化：<1000 m → "850 米"，否则 "12.4 公里" */
void fmt_distance(double meters, char *buf, size_t n);

#endif
